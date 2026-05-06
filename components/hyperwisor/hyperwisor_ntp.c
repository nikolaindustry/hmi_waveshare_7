/**
 * @file hyperwisor_ntp.c
 * @brief NTP time synchronization
 */

#include "hyperwisor_ntp.h"
#include "hyperwisor_core.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "HYPER_NTP";

static char s_timezone[32] = "UTC0";

esp_err_t hyperwisor_ntp_init(const char *timezone)
{
    if (timezone) {
        strncpy(s_timezone, timezone, sizeof(s_timezone) - 1);
        s_timezone[sizeof(s_timezone) - 1] = '\0';
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.nist.gov");
    esp_sntp_init();

    setenv("TZ", s_timezone, 1);
    tzset();

    hyperwisor_state_t *state = hyperwisor_get_state();
    state->ntp_initialized = true;

    ESP_LOGI(TAG, "NTP initialized with timezone: %s", s_timezone);
    return ESP_OK;
}

esp_err_t hyperwisor_ntp_set_timezone(const char *timezone)
{
    if (timezone) {
        strncpy(s_timezone, timezone, sizeof(s_timezone) - 1);
        s_timezone[sizeof(s_timezone) - 1] = '\0';
    }
    setenv("TZ", s_timezone, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to: %s", s_timezone);
    return ESP_OK;
}

esp_err_t hyperwisor_ntp_get_time(char *out_buf, size_t buf_len)
{
    if (buf_len < 9) {
        return ESP_ERR_INVALID_ARG;
    }

    time_t now = time(NULL);
    if (now < 1000000000L) {
        return ESP_ERR_NOT_FINISHED;
    }

    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(out_buf, buf_len, "%H:%M:%S", &timeinfo);
    return ESP_OK;
}

esp_err_t hyperwisor_ntp_get_date(char *out_buf, size_t buf_len)
{
    if (buf_len < 11) {
        return ESP_ERR_INVALID_ARG;
    }

    time_t now = time(NULL);
    if (now < 1000000000L) {
        return ESP_ERR_NOT_FINISHED;
    }

    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(out_buf, buf_len, "%Y-%m-%d", &timeinfo);
    return ESP_OK;
}

esp_err_t hyperwisor_ntp_get_datetime(char *out_buf, size_t buf_len)
{
    if (buf_len < 20) {
        return ESP_ERR_INVALID_ARG;
    }

    time_t now = time(NULL);
    if (now < 1000000000L) {
        return ESP_ERR_NOT_FINISHED;
    }

    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(out_buf, buf_len, "%Y-%m-%d %H:%M:%S", &timeinfo);
    return ESP_OK;
}
