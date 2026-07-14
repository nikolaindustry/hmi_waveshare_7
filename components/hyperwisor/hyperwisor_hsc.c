/**
 * @file hyperwisor_hsc.c
 * @brief HSC v1 device identity — on-chip P-256 keygen, challenge signing,
 *        and public-key registration. Built on ESP-IDF's mbedTLS.
 */

/* mbedTLS 3.x (ESP-IDF 5.x) hides struct members behind MBEDTLS_PRIVATE.
 * Allow direct access so the same code compiles on 2.x and 3.x. */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

#include "hyperwisor_hsc.h"
#include "hyperwisor_nvs.h"
#include "hyperwisor_core.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "mbedtls/ecp.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/bignum.h"
#include "mbedtls/sha256.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/base64.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "HYPER_HSC";

#define HSC_NVS_KEY   "hsc_priv"          /* base64 of the 32-byte private scalar */
#define HSC_DOMAIN    "HYPERWISOR-HSC-v1"
#define HSC_US        "\x1f"              /* unit separator */

static bool    s_ready = false;
static uint8_t s_priv[32];               /* private scalar d */
static uint8_t s_pub[65];                /* 0x04 || X || Y */

/* --- helpers -------------------------------------------------------------- */

static int seed_rng(mbedtls_ctr_drbg_context *ctr, mbedtls_entropy_context *ent)
{
    mbedtls_entropy_init(ent);
    mbedtls_ctr_drbg_init(ctr);
    const char *pers = "hyperwisor-hsc-v1";
    return mbedtls_ctr_drbg_seed(ctr, mbedtls_entropy_func, ent,
                                 (const unsigned char *)pers, strlen(pers));
}

/* base64-encode into a caller buffer. Returns ESP_OK on success. */
static esp_err_t b64_encode(const uint8_t *data, size_t len, char *out, size_t out_len)
{
    size_t olen = 0;
    int rc = mbedtls_base64_encode((unsigned char *)out, out_len, &olen, data, len);
    if (rc != 0) return ESP_FAIL;
    out[olen] = '\0';
    return ESP_OK;
}

/* Derive the public key Q = d*G from the stored private scalar. */
static esp_err_t derive_public(void)
{
    mbedtls_ecp_group grp; mbedtls_mpi d; mbedtls_ecp_point Q;
    mbedtls_ctr_drbg_context ctr; mbedtls_entropy_context ent;
    mbedtls_ecp_group_init(&grp); mbedtls_mpi_init(&d); mbedtls_ecp_point_init(&Q);

    esp_err_t ret = ESP_FAIL;
    if (seed_rng(&ctr, &ent) == 0 &&
        mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
        mbedtls_mpi_read_binary(&d, s_priv, 32) == 0 &&
        mbedtls_ecp_mul(&grp, &Q, &d, &grp.G, mbedtls_ctr_drbg_random, &ctr) == 0) {
        size_t olen = 0;
        if (mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                           &olen, s_pub, sizeof(s_pub)) == 0 && olen == 65) {
            ret = ESP_OK;
        }
    }

    mbedtls_ctr_drbg_free(&ctr); mbedtls_entropy_free(&ent);
    mbedtls_ecp_point_free(&Q); mbedtls_mpi_free(&d); mbedtls_ecp_group_free(&grp);
    return ret;
}

static esp_err_t load_from_nvs(void)
{
    char b64[80];
    if (hyperwisor_nvs_get_str(HSC_NVS_KEY, b64, sizeof(b64)) != ESP_OK) {
        return ESP_FAIL;
    }
    size_t olen = 0;
    if (mbedtls_base64_decode(s_priv, sizeof(s_priv), &olen,
                              (const unsigned char *)b64, strlen(b64)) != 0 || olen != 32) {
        return ESP_FAIL;
    }
    return derive_public();
}

static esp_err_t generate_and_store(void)
{
    mbedtls_ctr_drbg_context ctr; mbedtls_entropy_context ent;
    if (seed_rng(&ctr, &ent) != 0) {
        mbedtls_ctr_drbg_free(&ctr); mbedtls_entropy_free(&ent);
        return ESP_FAIL;
    }

    mbedtls_ecp_keypair key;
    mbedtls_ecp_keypair_init(&key);
    esp_err_t ret = ESP_FAIL;

    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, &key,
                            mbedtls_ctr_drbg_random, &ctr) == 0) {
        size_t olen = 0;
        if (mbedtls_mpi_write_binary(&key.d, s_priv, 32) == 0 &&
            mbedtls_ecp_point_write_binary(&key.grp, &key.Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                           &olen, s_pub, sizeof(s_pub)) == 0 && olen == 65) {
            char b64[80];
            if (b64_encode(s_priv, 32, b64, sizeof(b64)) == ESP_OK &&
                hyperwisor_nvs_set_str(HSC_NVS_KEY, b64) == ESP_OK) {
                ret = ESP_OK;
            }
        }
    }

    mbedtls_ecp_keypair_free(&key);
    mbedtls_ctr_drbg_free(&ctr); mbedtls_entropy_free(&ent);
    return ret;
}

/* --- public API ----------------------------------------------------------- */

esp_err_t hyperwisor_hsc_init(void)
{
    if (s_ready) return ESP_OK;

    if (load_from_nvs() == ESP_OK) {
        s_ready = true;
        ESP_LOGI(TAG, "device key loaded from NVS");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "no key found — generating on-chip keypair...");
    if (generate_and_store() == ESP_OK) {
        s_ready = true;
        ESP_LOGI(TAG, "keypair generated & stored (private key stays on-device)");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "keypair generation failed");
    return ESP_FAIL;
}

bool hyperwisor_hsc_ready(void) { return s_ready; }

esp_err_t hyperwisor_hsc_get_public_key_b64(char *out, size_t out_len)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    return b64_encode(s_pub, 65, out, out_len);
}

esp_err_t hyperwisor_hsc_sign(const char *device_id, const char *nonce,
                              const char *ts, char *out, size_t out_len)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    /* Build canonical message: DOMAIN|role|deviceId|channelId|nonce|ts */
    char msg[512];
    int n = snprintf(msg, sizeof(msg), "%s%s%s%s%s%s%s%s%s%s%s",
                     HSC_DOMAIN, HSC_US, "device", HSC_US, device_id, HSC_US,
                     device_id, HSC_US, nonce, HSC_US, ts);
    if (n <= 0 || n >= (int)sizeof(msg)) return ESP_FAIL;

    /* hash = SHA-256(msg) */
    uint8_t hash[32];
    {
        mbedtls_sha256_context sha;
        mbedtls_sha256_init(&sha);
        mbedtls_sha256_starts(&sha, 0); /* 0 => SHA-256 */
        mbedtls_sha256_update(&sha, (const unsigned char *)msg, n);
        mbedtls_sha256_finish(&sha, hash);
        mbedtls_sha256_free(&sha);
    }

    mbedtls_ctr_drbg_context ctr; mbedtls_entropy_context ent;
    if (seed_rng(&ctr, &ent) != 0) {
        mbedtls_ctr_drbg_free(&ctr); mbedtls_entropy_free(&ent);
        return ESP_FAIL;
    }

    mbedtls_ecp_group grp; mbedtls_mpi d, r, s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d); mbedtls_mpi_init(&r); mbedtls_mpi_init(&s);

    esp_err_t ret = ESP_FAIL;
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
        mbedtls_mpi_read_binary(&d, s_priv, 32) == 0 &&
        mbedtls_ecdsa_sign(&grp, &r, &s, &d, hash, 32,
                           mbedtls_ctr_drbg_random, &ctr) == 0) {
        uint8_t sig[64];
        if (mbedtls_mpi_write_binary(&r, sig, 32) == 0 &&
            mbedtls_mpi_write_binary(&s, sig + 32, 32) == 0) {
            ret = b64_encode(sig, 64, out, out_len);
        }
    }

    mbedtls_mpi_free(&d); mbedtls_mpi_free(&r); mbedtls_mpi_free(&s);
    mbedtls_ecp_group_free(&grp);
    mbedtls_ctr_drbg_free(&ctr); mbedtls_entropy_free(&ent);
    return ret;
}

esp_err_t hyperwisor_hsc_register(const char *functions_base_url,
                                  const char *user_auth_token)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    char pub[128];
    if (hyperwisor_hsc_get_public_key_b64(pub, sizeof(pub)) != ESP_OK) return ESP_FAIL;

    char device_id[HYPERWISOR_DEVICE_ID_LEN] = {0};
    hyperwisor_nvs_get_str("deviceid", device_id, sizeof(device_id));

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "device_id", device_id);
    cJSON_AddStringToObject(body, "public_key", pub);
    cJSON_AddStringToObject(body, "algo", "p256");
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) return ESP_FAIL;

    char url[256];
    snprintf(url, sizeof(url), "%s/relay-register-device", functions_base_url);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(json); return ESP_FAIL; }

    char auth[600];
    snprintf(auth, sizeof(auth), "Bearer %s", user_auth_token);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, json, strlen(json));

    esp_err_t err = esp_http_client_perform(client);
    int code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(json);

    if (err == ESP_OK && code == 200) {
        ESP_LOGI(TAG, "public key registered with platform");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "registration failed (err=%s, http=%d)", esp_err_to_name(err), code);
    return ESP_FAIL;
}

void hyperwisor_hsc_erase(void)
{
    /* Overwrite the stored key with an empty string (best-effort factory reset). */
    hyperwisor_nvs_set_str(HSC_NVS_KEY, "");
    s_ready = false;
    ESP_LOGI(TAG, "keypair erased (a new identity will be generated next boot)");
}
