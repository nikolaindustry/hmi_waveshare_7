/**
 * @file hyperwisor_ota.h
 * @brief Over-the-air firmware update handling
 *
 * When CONFIG_HYPERWISOR_ENABLE_OTA is set, the library automatically
 * registers an OTA_UPDATE command handler. The Hyperwisor dashboard
 * can push firmware updates — zero user code required.
 *
 * Key behaviours:
 *   - Downloads firmware from the URL sent by the dashboard
 *   - Sends progress feedback (10% steps) to the dashboard
 *   - Verifies the new image and reboots
 *   - Auto-rolls back if the new image fails within 30 seconds
 *   - Saves firmware version to NVS after confirmed good boot
 */

#ifndef HYPERWISOR_OTA_H
#define HYPERWISOR_OTA_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Perform an OTA update from the given URL
 * @param url HTTPS URL of the firmware binary
 * @return ESP_OK on success (device will reboot), ESP_FAIL on error
 *
 * Note: this is called automatically by the OTA_UPDATE command handler.
 * You normally never call this directly.
 */
esp_err_t hyperwisor_ota_perform(const char *url);

/**
 * @brief Get current running firmware version string
 * @param out_buf Buffer to write version string
 * @param buf_len Buffer length
 */
void hyperwisor_ota_get_version(char *out_buf, size_t buf_len);

/**
 * @brief Verify firmware signature (if secure boot is enabled)
 * @return ESP_OK on success
 *
 * When CONFIG_SECURE_BOOT_V2_ENABLED is set, calls the real
 * verification. Otherwise logs a warning and returns ESP_OK.
 */
esp_err_t hyperwisor_ota_verify_signature(void);

/**
 * @brief Auto-register OTA command handlers (OTA + OTA_UPDATE)
 *
 * Called by hyperwisor_core when CONFIG_HYPERWISOR_ENABLE_OTA is set.
 */
void hyperwisor_ota_auto_register(void);

/**
 * @brief Returns true if an OTA download is in progress.
 *
 * Used by hyperwisor_core to suppress WebSocket reconnect attempts
 * while the OTA HTTPS download is running (so the WS client doesn't
 * fight for TLS/TCP resources with the download).
 */
bool hyperwisor_ota_is_in_progress(void);

/**
 * @brief Check if running app is marked valid (cancel rollback)
 *
 * Call this once at startup after hyperwisor_init(). If the app
 * was updated via OTA and is running fine, this marks it as valid
 * so the bootloader won't roll back on the next reboot.
 *
 * @return true if an OTA update was just confirmed (caller should
 *         notify the cloud once WebSocket is connected), false otherwise
 */
bool hyperwisor_ota_confirm_good_boot(void);

#ifdef __cplusplus
}
#endif

#endif /* HYPERWISOR_OTA_H */
