/**
 * @file hyperwisor_http.c
 * @brief HTTP client for database, SMS, auth, onboarding (Supabase backend)
 */

#include "hyperwisor_http.h"
#include "hyperwisor_core.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG __attribute__((unused)) = "HYPER_HTTP";

/* Helper: perform HTTP request and return response as cJSON */
static cJSON *http_request(const char *method, const char *url,
                            const char *post_data, int *http_code)
{
    hyperwisor_state_t *state = hyperwisor_get_state();
    cJSON *result = cJSON_CreateObject();

    if (strlen(state->api_key) == 0 || strlen(state->secret_key) == 0) {
        cJSON_AddBoolToObject(result, "success", false);
        cJSON_AddStringToObject(result, "error", "API keys not set");
        return result;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = strcmp(method, "GET") == 0 ? HTTP_METHOD_GET :
                  strcmp(method, "POST") == 0 ? HTTP_METHOD_POST :
                  strcmp(method, "PUT") == 0 ? HTTP_METHOD_PUT :
                  strcmp(method, "DELETE") == 0 ? HTTP_METHOD_DELETE : HTTP_METHOD_GET,
        .timeout_ms = HYPERWISOR_HTTP_TIMEOUT_MS,
        .skip_cert_common_name_check = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        cJSON_AddBoolToObject(result, "success", false);
        cJSON_AddStringToObject(result, "error", "HTTP client init failed");
        return result;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "x-api-key", state->api_key);
    esp_http_client_set_header(client, "x-secret-key", state->secret_key);

    if (post_data) {
        esp_http_client_set_post_field(client, post_data, strlen(post_data));
    }

    esp_err_t err = esp_http_client_perform(client);
    int code = esp_http_client_get_status_code(client);
    if (http_code) *http_code = code;

    if (err == ESP_OK && code > 0) {
        int content_len = esp_http_client_get_content_length(client);
        if (content_len > 0) {
            char *buf = malloc(content_len + 1);
            if (buf) {
                int read_len = esp_http_client_read_response(client, buf, content_len);
                buf[read_len] = '\0';
                cJSON_AddBoolToObject(result, "success", true);
                cJSON_AddNumberToObject(result, "http_response_code", code);
                cJSON *parsed = cJSON_Parse(buf);
                if (parsed) {
                    cJSON_AddItemToObject(result, "data", parsed);
                } else {
                    cJSON_AddStringToObject(result, "raw_response", buf);
                }
                free(buf);
            }
        } else {
            cJSON_AddBoolToObject(result, "success", true);
            cJSON_AddNumberToObject(result, "http_response_code", code);
        }
    } else {
        cJSON_AddBoolToObject(result, "success", false);
        cJSON_AddNumberToObject(result, "http_response_code", code);
        cJSON_AddStringToObject(result, "error", "HTTP request failed");
    }

    esp_http_client_cleanup(client);
    return result;
}

/* ==================== Database Operations ==================== */

esp_err_t hyperwisor_db_insert(const char *product_id, const char *device_id, const char *table_name,
                                void (*data_builder)(cJSON *data))
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "product_id", product_id);
    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddStringToObject(root, "table_name", table_name);
    cJSON *data_payload = cJSON_AddObjectToObject(root, "data_payload");
    data_builder(data_payload);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char url[256];
    snprintf(url, sizeof(url), "%s/manufacturer-api/database/runtime-data", HYPERWISOR_HTTP_BASE_URL);

    cJSON *resp = http_request("POST", url, json_str, NULL);
    bool success = cJSON_IsTrue(cJSON_GetObjectItem(resp, "success"));
    cJSON_Delete(resp);
    free(json_str);

    return success ? ESP_OK : ESP_FAIL;
}

cJSON *hyperwisor_db_insert_with_response(const char *product_id, const char *device_id, const char *table_name,
                                            void (*data_builder)(cJSON *data))
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "product_id", product_id);
    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddStringToObject(root, "table_name", table_name);
    cJSON *data_payload = cJSON_AddObjectToObject(root, "data_payload");
    data_builder(data_payload);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char url[256];
    snprintf(url, sizeof(url), "%s/manufacturer-api/database/runtime-data", HYPERWISOR_HTTP_BASE_URL);

    cJSON *resp = http_request("POST", url, json_str, NULL);
    free(json_str);
    return resp;
}

esp_err_t hyperwisor_db_get(const char *product_id, const char *table_name, int limit)
{
    char url[512];
    snprintf(url, sizeof(url), "%s/manufacturer-api/database/runtime-data?product_id=%s&table_name=%s&limit=%d",
             HYPERWISOR_HTTP_BASE_URL, product_id, table_name, limit);

    cJSON *resp = http_request("GET", url, NULL, NULL);
    bool success = cJSON_IsTrue(cJSON_GetObjectItem(resp, "success"));
    cJSON_Delete(resp);
    return success ? ESP_OK : ESP_FAIL;
}

cJSON *hyperwisor_db_get_with_response(const char *product_id, const char *table_name, int limit)
{
    char url[512];
    snprintf(url, sizeof(url), "%s/manufacturer-api/database/runtime-data?product_id=%s&table_name=%s&limit=%d",
             HYPERWISOR_HTTP_BASE_URL, product_id, table_name, limit);

    return http_request("GET", url, NULL, NULL);
}

esp_err_t hyperwisor_db_update(const char *data_id, void (*data_builder)(cJSON *data))
{
    cJSON *root = cJSON_CreateObject();
    cJSON *data_payload = cJSON_AddObjectToObject(root, "data_payload");
    data_builder(data_payload);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char url[256];
    snprintf(url, sizeof(url), "%s/manufacturer-api/database/runtime-data/%s", HYPERWISOR_HTTP_BASE_URL, data_id);

    cJSON *resp = http_request("PUT", url, json_str, NULL);
    bool success = cJSON_IsTrue(cJSON_GetObjectItem(resp, "success"));
    cJSON_Delete(resp);
    free(json_str);
    return success ? ESP_OK : ESP_FAIL;
}

cJSON *hyperwisor_db_update_with_response(const char *data_id, void (*data_builder)(cJSON *data))
{
    cJSON *root = cJSON_CreateObject();
    cJSON *data_payload = cJSON_AddObjectToObject(root, "data_payload");
    data_builder(data_payload);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char url[256];
    snprintf(url, sizeof(url), "%s/manufacturer-api/database/runtime-data/%s", HYPERWISOR_HTTP_BASE_URL, data_id);

    cJSON *resp = http_request("PUT", url, json_str, NULL);
    free(json_str);
    return resp;
}

esp_err_t hyperwisor_db_delete(const char *data_id)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/manufacturer-api/database/runtime-data/%s", HYPERWISOR_HTTP_BASE_URL, data_id);

    cJSON *resp = http_request("DELETE", url, NULL, NULL);
    bool success = cJSON_IsTrue(cJSON_GetObjectItem(resp, "success"));
    cJSON_Delete(resp);
    return success ? ESP_OK : ESP_FAIL;
}

cJSON *hyperwisor_db_delete_with_response(const char *data_id)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/manufacturer-api/database/runtime-data/%s", HYPERWISOR_HTTP_BASE_URL, data_id);

    return http_request("DELETE", url, NULL, NULL);
}

/* ==================== Device Onboarding ==================== */

esp_err_t hyperwisor_onboard_device(const char *product_id, const char *user_id,
                                     const char *device_name, const char *device_identifier)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "product_id", product_id);
    cJSON_AddStringToObject(root, "user_id", user_id);
    cJSON_AddStringToObject(root, "device_name", device_name);
    cJSON_AddStringToObject(root, "device_identifier", device_identifier);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char url[256];
    snprintf(url, sizeof(url), "%s/manufacturer-api/onboarding/device", HYPERWISOR_HTTP_BASE_URL);

    cJSON *resp = http_request("POST", url, json_str, NULL);
    bool success = cJSON_IsTrue(cJSON_GetObjectItem(resp, "success"));
    cJSON_Delete(resp);
    free(json_str);
    return success ? ESP_OK : ESP_FAIL;
}

cJSON *hyperwisor_onboard_device_with_response(const char *product_id, const char *user_id,
                                                const char *device_name, const char *device_identifier)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "product_id", product_id);
    cJSON_AddStringToObject(root, "user_id", user_id);
    cJSON_AddStringToObject(root, "device_name", device_name);
    cJSON_AddStringToObject(root, "device_identifier", device_identifier);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char url[256];
    snprintf(url, sizeof(url), "%s/manufacturer-api/onboarding/device", HYPERWISOR_HTTP_BASE_URL);

    cJSON *resp = http_request("POST", url, json_str, NULL);
    free(json_str);
    return resp;
}

/* ==================== SMS ==================== */

esp_err_t hyperwisor_send_sms(const char *product_id, const char *to, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "productId", product_id);
    cJSON_AddStringToObject(root, "to", to);
    cJSON_AddStringToObject(root, "message", message);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char url[256];
    snprintf(url, sizeof(url), "%s/sms-service", HYPERWISOR_HTTP_BASE_URL);

    cJSON *resp = http_request("POST", url, json_str, NULL);
    bool success = cJSON_IsTrue(cJSON_GetObjectItem(resp, "success"));
    cJSON_Delete(resp);
    free(json_str);
    return success ? ESP_OK : ESP_FAIL;
}

cJSON *hyperwisor_send_sms_with_response(const char *product_id, const char *to, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "productId", product_id);
    cJSON_AddStringToObject(root, "to", to);
    cJSON_AddStringToObject(root, "message", message);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char url[256];
    snprintf(url, sizeof(url), "%s/sms-service", HYPERWISOR_HTTP_BASE_URL);

    cJSON *resp = http_request("POST", url, json_str, NULL);
    free(json_str);
    return resp;
}

/* ==================== Authentication ==================== */

esp_err_t hyperwisor_authenticate_user(const char *email, const char *password)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "email", email);
    cJSON_AddStringToObject(root, "password", password);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char url[256];
    snprintf(url, sizeof(url), "%s/manufacturer-api/auth/signin", HYPERWISOR_HTTP_BASE_URL);

    cJSON *resp = http_request("POST", url, json_str, NULL);
    bool success = cJSON_IsTrue(cJSON_GetObjectItem(resp, "success"));
    cJSON_Delete(resp);
    free(json_str);
    return success ? ESP_OK : ESP_FAIL;
}

cJSON *hyperwisor_authenticate_user_with_response(const char *email, const char *password)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "email", email);
    cJSON_AddStringToObject(root, "password", password);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char url[256];
    snprintf(url, sizeof(url), "%s/manufacturer-api/auth/signin", HYPERWISOR_HTTP_BASE_URL);

    cJSON *resp = http_request("POST", url, json_str, NULL);
    free(json_str);
    return resp;
}
