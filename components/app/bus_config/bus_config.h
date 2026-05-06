#pragma once

#include <stdint.h>
#include "esp_err.h"

/* NVS-backed RS-485 bus baud rate.
 *
 * Default (first boot, blank NVS) = BSP_RS485_DEFAULT_BAUD (9600) so the HMI
 * talks to an unconfigured Waveshare relay board out of the box. User upgrades
 * to 115200 from Maintenance -> Bus Setup on the HMI itself.
 *
 * Supported values: 9600 and 115200 only.
 */

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bus_config_init(void);               /* call once at boot */
uint32_t  bus_config_get_baud(void);            /* safe after init  */
esp_err_t bus_config_set_baud(uint32_t baud);   /* saves NVS, caller reboots */

#ifdef __cplusplus
}
#endif
