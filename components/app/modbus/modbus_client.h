#pragma once

/* Minimal Modbus RTU master.
 *
 * Function codes implemented:
 *   FC 0x01  Read Coils                (Waveshare relay board poll)
 *   FC 0x05  Write Single Coil         (Waveshare relay board writes)
 *   FC 0x10  Write Multiple Registers  (RGB slave colour update)
 *
 * Implementation sits directly on the ESP-IDF UART driver, so there is
 * no external dependency on esp-modbus. That keeps the port clean with
 * ESP-IDF 6.0.1 and avoids the 1.x / 2.x API split.
 *
 * The RS-485 transceiver on the ESP32-S3-Touch-LCD-7 has automatic
 * direction control, so no DE/RE GPIO is required.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise UART1 on the RS-485 pins at the given baud (typically 9600).
 * Safe to call once at boot. Re-entry is a no-op. */
esp_err_t modbus_client_init(uint32_t baud);

/* FC 0x05 Write Single Coil.
 *   slave  - 1..247 (Waveshare default 0x01)
 *   coil   - 0..15 for Relay 1..16
 *   on     - true = 0xFF00, false = 0x0000
 * Returns ESP_OK on CRC-valid echo response, ESP_ERR_TIMEOUT otherwise. */
esp_err_t modbus_client_write_coil(uint8_t slave, uint16_t coil, bool on);

/* FC 0x01 Read Coils.
 *   slave  - 1..247
 *   start  - first coil
 *   count  - number of coils to read (1..2000)
 *   mask   - out: packed bitmask, LSB=first coil. Must be big enough.
 * Returns ESP_OK on CRC-valid response. */
esp_err_t modbus_client_read_coils(uint8_t slave, uint16_t start,
                                   uint16_t count, uint16_t *mask);

/* FC 0x10 Write Multiple Holding Registers.
 *   slave  - 1..247
 *   start  - first register address
 *   count  - number of registers to write (1..123 per spec; we cap at 64
 *            to fit the dual-HMI mirror block in one transaction)
 *   regs   - pointer to `count` 16-bit values in host byte order; they
 *            get serialised big-endian on the wire by this function.
 * Returns ESP_OK on CRC-valid response whose start/count fields match
 * the request, ESP_ERR_TIMEOUT on no reply. */
esp_err_t modbus_client_write_hregs(uint8_t slave, uint16_t start,
                                    uint16_t count, const uint16_t *regs);

/* FC 0x06 Write Single Register.
 *   slave - 1..247 (0 = Modbus broadcast, fire-and-forget)
 *   addr  - holding register address
 *   value - 16-bit value, host byte order
 * Returns ESP_OK on byte-for-byte echo (non-broadcast), or ESP_OK with no
 * wait when slave == 0 (broadcast). ESP_ERR_TIMEOUT on no reply. */
esp_err_t modbus_client_write_hreg_single(uint8_t slave, uint16_t addr,
                                          uint16_t value);

/* FC 0x03 Read Holding Registers.
 *   slave  - 1..247
 *   start  - first register address
 *   count  - number of registers to read (1..64; bounded by our rx buffer)
 *   regs   - out: `count` 16-bit values in host byte order.
 * Returns ESP_OK on CRC-valid response, ESP_ERR_TIMEOUT on no reply. */
esp_err_t modbus_client_read_hregs(uint8_t slave, uint16_t start,
                                   uint16_t count, uint16_t *regs);

#ifdef __cplusplus
}
#endif
