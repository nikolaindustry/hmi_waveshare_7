/**
 * @file hyperwisor_ntp.h
 * @brief NTP time synchronization functions
 */

#ifndef HYPERWISOR_NTP_H
#define HYPERWISOR_NTP_H

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t hyperwisor_ntp_init(const char *timezone);
esp_err_t hyperwisor_ntp_set_timezone(const char *timezone);
esp_err_t hyperwisor_ntp_get_time(char *out_buf, size_t buf_len);
esp_err_t hyperwisor_ntp_get_date(char *out_buf, size_t buf_len);
esp_err_t hyperwisor_ntp_get_datetime(char *out_buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* HYPERWISOR_NTP_H */
