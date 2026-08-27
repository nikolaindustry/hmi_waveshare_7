#pragma once

/* Worker task that owns the Modbus RTU master link:
 *   - Serialises UART transactions on core 1 (keeps LCD refresh on core 0 clean).
 *   - Queues coil-write requests from the UI and sends them FC 0x05.
 *   - Polls coils 0..15 every 500 ms via FC 0x01 and updates the
 *     matching ctrl_toggle_t entries so the UI stays in sync even if
 *     a relay is commanded from another master.
 *
 * The UI side only needs three calls: start once, request toggles, and
 * read link status to drive the "LINK ONLINE/OFFLINE" pill.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the worker task. Safe to call once. Requires modbus_client_init()
 * to have been called first (main.c handles this order). */
esp_err_t modbus_task_start(void);

/* Enqueue a coil-write request. Non-blocking, drops if the queue is full
 * (UI must not wait on Modbus). Marks the matching ctrl_toggle_t as
 * pending=true so the UI can show a transient "in flight" border. */
void modbus_task_request(uint16_t coil, bool on);

/* Request a colour update on an RGB slave.
 *   slave     : Modbus RTU slave address of the RGB module (e.g. 0x20).
 *   start_reg : first holding register of the target zone's 4-reg block.
 *               The worker writes [R, G, B, MODE] starting at this
 *               register. A single slave can expose multiple zones by
 *               giving each one its own start_reg offset (e.g. 0 for
 *               Roof, 6 for Floor).
 *   r/g/b     : 0..255 channel intensities (pre-brightness scaled by UI).
 *   on        : false forces the zone into MODE_OFF (black), ignoring r/g/b.
 *
 * Uses a "latest value wins" slot per (slave, start_reg) pair, so a
 * 60 Hz UI drag collapses into at most one Modbus frame per worker
 * loop turn. Writes from different zones on the same slave are kept
 * independent. */
void modbus_task_request_rgb(uint8_t slave, uint16_t start_reg,
                             uint8_t r, uint8_t g, uint8_t b,
                             bool on);

/* True when the last Modbus poll succeeded (i.e. the relay board is
 * reachable). Used by the UI to paint the LINK pill. */
bool modbus_task_link_online(void);

/* Monotonic counter that increments whenever the worker confirms/updates
 * any toggle state. The UI polls this to decide when to repaint. */
uint32_t modbus_task_state_seq(void);

/* Force the state counter forward. For state that changed without going
 * through this module (e.g. hmi_sync applying a remote intent that only
 * touches ctrl_state), so open UI screens still notice and repaint. */
void modbus_task_bump_seq(void);

/* Halt the worker's polling loop. The call blocks (up to timeout_ms) until
 * the worker confirms it has reached a quiescent point, i.e. holds no UART
 * lock and is idling. After this returns ESP_OK the caller owns the bus
 * exclusively and can issue arbitrary modbus_client_* calls without
 * interleaving with poller traffic.
 *
 * Pair with modbus_task_resume(). Safe to call from the LVGL task. */
esp_err_t modbus_task_pause(uint32_t timeout_ms);

/* Resume the worker's polling loop after a prior pause. */
void modbus_task_resume(void);

#ifdef __cplusplus
}
#endif
