/* Desktop stand-in for ESP-IDF's esp_err.h. */
#pragma once
#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK                     0
#define ESP_FAIL                  -1
#define ESP_ERR_NO_MEM             0x101
#define ESP_ERR_INVALID_ARG        0x102
#define ESP_ERR_INVALID_STATE      0x103
#define ESP_ERR_INVALID_SIZE       0x104
#define ESP_ERR_NOT_FOUND          0x105
#define ESP_ERR_NOT_SUPPORTED      0x106
#define ESP_ERR_TIMEOUT            0x107
#define ESP_ERR_INVALID_RESPONSE   0x108
#define ESP_ERR_INVALID_CRC        0x109

#define ESP_ERR_NVS_BASE           0x1100
#define ESP_ERR_NVS_NOT_FOUND          (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_NO_FREE_PAGES      (ESP_ERR_NVS_BASE + 0x0d)
#define ESP_ERR_NVS_NEW_VERSION_FOUND  (ESP_ERR_NVS_BASE + 0x10)

const char *esp_err_to_name(esp_err_t err);

#define ESP_ERROR_CHECK(x) do { (void)(x); } while (0)

#define ESP_RETURN_ON_ERROR(x, tag, fmt, ...) \
    do { esp_err_t _e = (x); if (_e != ESP_OK) return _e; } while (0)
#define ESP_RETURN_ON_FALSE(a, err, tag, fmt, ...) \
    do { if (!(a)) return (err); } while (0)
#define ESP_GOTO_ON_ERROR(x, lbl, tag, fmt, ...) \
    do { esp_err_t _e = (x); if (_e != ESP_OK) goto lbl; } while (0)
