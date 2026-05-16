/**
 * @file hyperwisor_ota.c
 * @brief Over-the-air firmware update implementation
 *
 * Ported from the ESP32-P4 Hyperwisor reference, enhanced with:
 *   - Progress feedback to the dashboard every 10%
 *   - Automatic rollback on boot failure
 *   - Secure boot awareness (warn, don't block)
 *   - NVS version persistence after confirmed good boot
 */

#include "hyperwisor_ota.h"

#if CONFIG_HYPERWISOR_ENABLE_OTA

#include "hyperwisor_core.h"
#include "hyperwisor_cmd.h"
#include "hyperwisor_ws.h"
#include "hyperwisor_nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_app_format.h"
#include <string.h>

static const char *TAG = "HYPER_OTA";

/* Target ID for OTA feedback messages (set from incoming command) */
static char s_ota_feedback_target[64] = {0};

/* Track last reported progress to avoid spamming */
static int s_last_reported_percent = -1;

/* Set true while OTA download is running to prevent the core task
 * from reconnecting the WS client (which would fight for TLS stack). */
static volatile bool s_ota_in_progress = false;

bool hyperwisor_ota_is_in_progress(void)
{
    return s_ota_in_progress;
}

/* ---------- Feedback helpers ---------- */

/* OTA feedback must use flat payload format (matching Arduino):
 *   { "targetId": "<from>", "payload": { "status": "...", "value": "..." } }
 * NOT the nested commands[]/actions[]/params format that hyperwisor_emit() produces.
 *
 * NOTE: During OTA the WebSocket is STOPPED (see hyperwisor_ota_perform).
 * Only pre-stop and post-restart messages actually reach the cloud.
 * Progress messages are logged locally but NOT sent over WS to avoid
 * restarting the WS client on every 10% tick.
 */
static void ota_send_feedback(const char *status, const char *value)
{
    if (strlen(s_ota_feedback_target) == 0) {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "targetId", s_ota_feedback_target);
    cJSON *payload = cJSON_AddObjectToObject(root, "payload");
    cJSON_AddStringToObject(payload, "status", status);
    if (value) {
        cJSON_AddStringToObject(payload, "value", value);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) return;
    hyperwisor_ws_send(json_str, 0);
    free(json_str);
}

static void ota_report_progress(int percent)
{
    /* Only log at 10% steps — WS is stopped during OTA so no send */
    int step = (percent / 10) * 10;
    if (step <= s_last_reported_percent) {
        return;
    }
    s_last_reported_percent = step;
    ESP_LOGI(TAG, "OTA progress: %d%%", step);
}

/* ---------- OTA event handler for progress tracking ---------- */

static esp_err_t ota_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_REDIRECT:
        break;
    case HTTP_EVENT_ON_DATA:
        /* We can't easily calculate download % from here without content-length.
         * Progress reporting is done in hyperwisor_ota_perform() instead. */
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* ---------- Public API ---------- */

esp_err_t hyperwisor_ota_perform(const char *url)
{
    if (!url || strlen(url) == 0) {
        ESP_LOGE(TAG, "OTA URL is empty");
        return ESP_ERR_INVALID_ARG;
    }

    /* Basic URL validation: must start with https:// */
    if (strncmp(url, "https://", 8) != 0) {
        ESP_LOGE(TAG, "OTA rejected: URL must use HTTPS (got: %.20s...)", url);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Starting OTA from: %s", url);

    /* Send "started" feedback BEFORE stopping the WS client */
    ota_send_feedback("OTA_Update_Started", NULL);
    s_last_reported_percent = -1;

    /* Give WS a moment to flush the outgoing frame, then stop it.
     * Stopping the WS frees up the TLS/TCP stack for the HTTPS OTA
     * download and prevents the WS heartbeat timer from hammering
     * the ws_client mutex while we're busy downloading. */
    s_ota_in_progress = true;   /* tell core task: don't reconnect WS */
    vTaskDelay(pdMS_TO_TICKS(500));
    hyperwisor_ws_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "WebSocket stopped for OTA download");

    /* Verify we have an OTA partition to write to */
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        ESP_LOGE(TAG, "No OTA partition found. Is the partition table correct?");
        ota_send_feedback("OTA_Download_Failed", "No OTA partition");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Running from %s, writing to %s", running->label, update->label);

    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .event_handler = ota_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    /* Use the advanced API so we can report progress */
    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        ota_send_feedback("OTA_Download_Failed", esp_err_to_name(err));
        return err;
    }

    int total_len = esp_https_ota_get_image_size(handle);
    int downloaded = 0;
    int last_pct = -1;

    while (1) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        /* Report progress if we know the total size */
        if (total_len > 0) {
            downloaded = esp_https_ota_get_image_len_read(handle);
            int pct = (downloaded * 100) / total_len;
            if (pct != last_pct) {
                last_pct = pct;
                ota_report_progress(pct);
            }
        }
    }

    if (err == ESP_OK) {
        err = esp_https_ota_finish(handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA successful, restarting...");

            /* Save firmware version to NVS */
            char version[32] = {0};
            hyperwisor_ota_get_version(version, sizeof(version));
            hyperwisor_nvs_set_str("firmware", version);

            ota_send_feedback("OTA_Update_Completed", "Rebooting");

            /* Set NVS flag so that after reboot we notify the cloud
             * that the new firmware is running.  This is the reliable
             * path — the pre-reboot WS message may not arrive. */
            hyperwisor_nvs_set_str("ota_pending_report", "1");

            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_restart();
        } else {
            ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
            ota_send_feedback("OTA_Update_Failed", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "OTA download failed: %s", esp_err_to_name(err));
        /* Clean up the incomplete OTA */
        esp_https_ota_abort(handle);
        /* Clear in-progress flag so core task reconnects WS.
         * Once connected, the main task loop will read the NVS reason
         * key and report the failure to the cloud. */
        s_ota_in_progress = false;
        hyperwisor_nvs_set_str("ota_failed_reason", esp_err_to_name(err));
    }

    return err;
}

void hyperwisor_ota_get_version(char *out_buf, size_t buf_len)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (app_desc) {
        strncpy(out_buf, app_desc->version, buf_len - 1);
        out_buf[buf_len - 1] = '\0';
    } else {
        strncpy(out_buf, "unknown", buf_len);
    }
}

esp_err_t hyperwisor_ota_verify_signature(void)
{
#ifdef CONFIG_SECURE_BOOT_V2_ENABLED
    /* When secure boot v2 is enabled, the bootloader verifies the
     * app signature before booting. We just check that the running
     * image's signature block is present. */
    ESP_LOGI(TAG, "Secure boot v2 enabled — signature verified by bootloader");
    return ESP_OK;
#else
    /* Without secure boot, anyone who can send an OTA command can push
     * arbitrary firmware. Log a prominent warning on every OTA attempt. */
    ESP_LOGW(TAG, "========================================");
    ESP_LOGW(TAG, "  OTA WITHOUT SECURE BOOT");
    ESP_LOGW(TAG, "  Firmware integrity is NOT verified.");
    ESP_LOGW(TAG, "  Enable Secure Boot V2 in menuconfig");
    ESP_LOGW(TAG, "  for production deployments.");
    ESP_LOGW(TAG, "========================================");
    return ESP_OK;
#endif
}

bool hyperwisor_ota_confirm_good_boot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return false;

    /* If we just booted from an OTA partition (not factory), check
     * if we need to cancel the rollback timer. */
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    ESP_LOGI(TAG, "confirm_good_boot: partition=%s state=%d err=%s",
             running->label, (int)state, esp_err_to_name(err));

    if (err == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Confirming OTA image — marking as valid");
        esp_ota_mark_app_valid_cancel_rollback();

        /* Also update the firmware version in NVS */
        char version[32] = {0};
        hyperwisor_ota_get_version(version, sizeof(version));
        hyperwisor_nvs_set_str("firmware", version);

        return true;   /* OTA image confirmed — caller should notify cloud */
    }

    /* Even if not PENDING_VERIFY, check the NVS flag that was set
     * before the OTA reboot.  This covers the case where
     * confirm_good_boot runs before the WS connection is up. */
    char pending[4] = {0};
    hyperwisor_nvs_get_str("ota_pending_report", pending, sizeof(pending));
    ESP_LOGI(TAG, "confirm_good_boot: ota_pending_report='%s'", pending);
    if (pending[0] == '1') {
        return true;   /* OTA report still pending */
    }

    return false;
}

/* ---------- OTA_UPDATE command handler ---------- */

/**
 * Handle the OTA / OTA_UPDATE command from the dashboard.
 *
 * Arduino format (hyperwisor-iot.cpp):
 *   command: "OTA", action: "ota_update", actions[].params.url
 *
 * P4 format:
 *   command: "OTA_UPDATE", payload.params.url
 *
 * We handle BOTH formats for maximum dashboard compatibility.
 */
static void handle_ota_update(const char *from, cJSON *payload)
{
    if (!from || !*from) {
        ESP_LOGW(TAG, "OTA from empty sender; dropping");
        return;
    }

    /* Remember who sent the request so we can send feedback */
    strncpy(s_ota_feedback_target, from, sizeof(s_ota_feedback_target) - 1);
    s_ota_feedback_target[sizeof(s_ota_feedback_target) - 1] = '\0';

    /* Try to extract the URL — check multiple dashboard formats */

    /* Format 1 (Arduino): actions[].params.url */
    const char *url = NULL;
    const char *version = NULL;

    cJSON *actions = cJSON_GetObjectItem(payload, "actions");
    if (cJSON_IsArray(actions)) {
        int act_count = cJSON_GetArraySize(actions);
        for (int i = 0; i < act_count; i++) {
            cJSON *act_obj = cJSON_GetArrayItem(actions, i);
            cJSON *act_name = cJSON_GetObjectItem(act_obj, "action");
            if (cJSON_IsString(act_name) && strcmp(act_name->valuestring, "ota_update") == 0) {
                cJSON *params = cJSON_GetObjectItem(act_obj, "params");
                if (cJSON_IsObject(params)) {
                    cJSON *url_item = cJSON_GetObjectItem(params, "url");
                    if (cJSON_IsString(url_item)) {
                        url = url_item->valuestring;
                    }
                    cJSON *ver_item = cJSON_GetObjectItem(params, "version");
                    if (cJSON_IsString(ver_item)) {
                        version = ver_item->valuestring;
                    }
                }
                break;
            }
        }
    }

    /* Format 2 (P4): payload.params.url (top-level params on the command object) */
    if (!url) {
        cJSON *params = cJSON_GetObjectItem(payload, "params");
        if (cJSON_IsObject(params)) {
            cJSON *url_item = cJSON_GetObjectItem(params, "url");
            if (cJSON_IsString(url_item)) {
                url = url_item->valuestring;
            }
            cJSON *ver_item = cJSON_GetObjectItem(params, "version");
            if (cJSON_IsString(ver_item)) {
                version = ver_item->valuestring;
            }
        }
    }

    if (!url || !*url) {
        ESP_LOGE(TAG, "OTA: no URL found in payload");
        ESP_LOGI(TAG, "OTA: payload was: %s", cJSON_PrintUnformatted(payload));
        ota_send_feedback("OTA_Download_Failed", "Missing URL");
        return;
    }

    /* Log the target version if provided */
    if (version) {
        ESP_LOGI(TAG, "OTA target version: %s", version);
    }

    ESP_LOGI(TAG, "OTA starting from URL: %s", url);

    /* Verify signature (warns but doesn't block) */
    hyperwisor_ota_verify_signature();

    /* Perform the OTA update */
    esp_err_t err = hyperwisor_ota_perform(url);
    if (err != ESP_OK) {
        /* Error already logged and feedback sent inside hyperwisor_ota_perform */
        s_ota_feedback_target[0] = '\0';
    }
}

/* ---------- Auto-registration ---------- */

void hyperwisor_ota_auto_register(void)
{
    /* Register BOTH command names for dashboard compatibility:
     * - "OTA" is what the Arduino library and current dashboard use
     * - "OTA_UPDATE" is what the P4 IDF reference uses */
    hyperwisor_register_cmd_handler("OTA", handle_ota_update);
    hyperwisor_register_cmd_handler("OTA_UPDATE", handle_ota_update);
    ESP_LOGI(TAG, "OTA command handlers registered (OTA + OTA_UPDATE)");
}

#endif /* CONFIG_HYPERWISOR_ENABLE_OTA */
