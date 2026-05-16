/**
 * @file hyperwisor_http.h
 * @brief HTTP client APIs for database, SMS, auth, onboarding
 */

#ifndef HYPERWISOR_HTTP_H
#define HYPERWISOR_HTTP_H

#include "esp_err.h"
#include "cJSON.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configurable via Kconfig → HYPERWISOR_HTTP_BASE_URL.
 * Defaults to https://hyperwisor.nikolaindustry.workers.dev/functions/v1 */
#define HYPERWISOR_HTTP_BASE_URL     CONFIG_HYPERWISOR_HTTP_BASE_URL
#define HYPERWISOR_HTTP_TIMEOUT_MS   10000

esp_err_t hyperwisor_db_insert(const char *product_id, const char *device_id, const char *table_name,
                                void (*data_builder)(cJSON *data));
cJSON *hyperwisor_db_insert_with_response(const char *product_id, const char *device_id, const char *table_name,
                                            void (*data_builder)(cJSON *data));
esp_err_t hyperwisor_db_get(const char *product_id, const char *table_name, int limit);
cJSON *hyperwisor_db_get_with_response(const char *product_id, const char *table_name, int limit);
esp_err_t hyperwisor_db_update(const char *data_id, void (*data_builder)(cJSON *data));
cJSON *hyperwisor_db_update_with_response(const char *data_id, void (*data_builder)(cJSON *data));
esp_err_t hyperwisor_db_delete(const char *data_id);
cJSON *hyperwisor_db_delete_with_response(const char *data_id);

esp_err_t hyperwisor_onboard_device(const char *product_id, const char *user_id,
                                     const char *device_name, const char *device_identifier);
cJSON *hyperwisor_onboard_device_with_response(const char *product_id, const char *user_id,
                                                const char *device_name, const char *device_identifier);

esp_err_t hyperwisor_send_sms(const char *product_id, const char *to, const char *message);
cJSON *hyperwisor_send_sms_with_response(const char *product_id, const char *to, const char *message);

esp_err_t hyperwisor_authenticate_user(const char *email, const char *password);
cJSON *hyperwisor_authenticate_user_with_response(const char *email, const char *password);

#ifdef __cplusplus
}
#endif

#endif /* HYPERWISOR_HTTP_H */
