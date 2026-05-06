#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * Minimal Modbus RTU SLAVE server for the secondary HMI.
 *
 * Shares the UART1 driver installed by modbus_client_init() (only
 * one role drives the bus at a time: secondary never runs the master
 * worker, primary never runs this server). The slave filters on
 * address HMI_SECONDARY_SLAVE_ID (0x10) and handles just two FCs:
 *
 *   0x03  Read Holding Registers    - primary reads intent ring
 *   0x10  Write Multiple Registers  - primary writes state mirror
 *
 * All other FCs reply with an Illegal Function exception (0x01).
 * Runs on core 1 at priority 4 (one below the master worker).
 * ------------------------------------------------------------------ */

/* Start the server task. Safe to call once. modbus_client_init() MUST
 * have been called first so the UART driver exists. */
esp_err_t modbus_server_start(uint8_t slave_addr);

/* True once the server has successfully parsed and responded to at
 * least one Modbus frame from the primary. Used by the Maintenance
 * tab's "peer seen" indicator. */
bool modbus_server_peer_seen(void);

/* Monotonic counter bumped each time a frame is serviced. UI polls
 * this to decide when to repaint. */
uint32_t modbus_server_rx_count(void);

#ifdef __cplusplus
}
#endif
