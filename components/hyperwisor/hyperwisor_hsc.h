/**
 * @file hyperwisor_hsc.h
 * @brief HSC v1 — Hyperwisor Secure Channel (device side, ESP-IDF).
 *
 * On-chip P-256 identity. The private key is generated on the chip at first
 * boot and stored in NVS; it never leaves the device. The device answers the
 * relay's connection challenge by signing it, proving it is genuine without
 * transmitting any secret.
 *
 * Wire formats match the relay (utils/hsc.js) and the Arduino library
 * byte-for-byte:
 *   - public key : raw uncompressed point 0x04||X||Y (65 bytes), base64
 *   - signature  : raw r||s (IEEE-P1363, 64 bytes), base64
 *   - message    : "HYPERWISOR-HSC-v1" \x1f role \x1f deviceId \x1f channelId
 *                  \x1f nonce \x1f ts   (UTF-8, then SHA-256, then ECDSA)
 */

#ifndef HYPERWISOR_HSC_H
#define HYPERWISOR_HSC_H

#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Load the device keypair from NVS, or generate + persist one on first boot. */
esp_err_t hyperwisor_hsc_init(void);

/** True once a usable keypair is loaded/generated. */
bool hyperwisor_hsc_ready(void);

/** Write the base64 raw public key (register this during onboarding) into out. */
esp_err_t hyperwisor_hsc_get_public_key_b64(char *out, size_t out_len);

/**
 * Sign a relay challenge. Writes base64 of the raw 64-byte signature into `out`.
 * role is "device"; channelId equals device_id for the device leg.
 */
esp_err_t hyperwisor_hsc_sign(const char *device_id, const char *nonce,
                              const char *ts, char *out, size_t out_len);

/**
 * Register this device's public key with the platform (relay-register-device).
 * functions_base_url e.g. "https://<project>.supabase.co/functions/v1".
 * user_auth_token is the claiming user's Supabase access token. Returns ESP_OK
 * on HTTP 200.
 */
esp_err_t hyperwisor_hsc_register(const char *functions_base_url,
                                  const char *user_auth_token);

/** Erase the stored keypair (factory reset — creates a new identity next boot). */
void hyperwisor_hsc_erase(void);

#ifdef __cplusplus
}
#endif

#endif /* HYPERWISOR_HSC_H */
