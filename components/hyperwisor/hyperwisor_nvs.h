/**
 * @file hyperwisor_nvs.h
 * @brief NVS/ persistent storage abstraction for Hyperwisor
 */

#ifndef HYPERWISOR_NVS_H
#define HYPERWISOR_NVS_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t hyperwisor_nvs_init(void);
esp_err_t hyperwisor_nvs_get_str(const char *key, char *out_buf, size_t buf_len);
esp_err_t hyperwisor_nvs_set_str(const char *key, const char *value);
esp_err_t hyperwisor_nvs_get_i32(const char *key, int32_t *out_value);
esp_err_t hyperwisor_nvs_set_i32(const char *key, int32_t value);
esp_err_t hyperwisor_nvs_commit(void);

#ifdef __cplusplus
}
#endif

#endif /* HYPERWISOR_NVS_H */
