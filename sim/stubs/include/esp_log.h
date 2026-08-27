/* Desktop stand-in for ESP-IDF's esp_log.h -- logs go to stdout so the
 * simulator's terminal shows the same ESP_LOGx lines the device serial
 * console would. */
#pragma once
#include <stdio.h>

typedef enum {
    ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN,
    ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE,
} esp_log_level_t;

static inline void esp_log_level_set(const char *tag, esp_log_level_t l)
{ (void)tag; (void)l; }

#define ESP_LOGE(tag, fmt, ...) printf("E (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) printf("I (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) do { (void)(tag); } while (0)
#define ESP_LOGV(tag, fmt, ...) do { (void)(tag); } while (0)

#define ESP_LOG_BUFFER_HEX_LEVEL(tag, buf, len, lvl) do { (void)(buf); } while (0)
#define ESP_LOG_BUFFER_HEX(tag, buf, len)            do { (void)(buf); } while (0)
