#include "modbus_server.h"
#include "hmi_sync.h"
#include "bsp_pins.h"

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <string.h>

static const char *TAG = "mb_srv";

/* Secondary never runs the master worker, so this server owns the bus.
 * The UART driver itself is installed by modbus_client_init() — we only
 * read/write bytes here. */

#define MBS_MAX_FRAME       256    /* FC 0x10 with 64 regs = 135 bytes; cap 256 */
#define MBS_INTER_FRAME_MS  10     /* matches master-side MB_INTER_FRAME_MS */
#define MBS_READ_GAP_MS     3      /* 3.5-char gap detector @9600 is ~4ms */

#define MBS_EXC_ILLEGAL_FUNC   0x01
#define MBS_EXC_ILLEGAL_ADDR   0x02
#define MBS_EXC_ILLEGAL_VALUE  0x03

static uint8_t  s_slave_addr = 0;
static bool     s_started    = false;
static uint32_t s_rx_count   = 0;
static int64_t  s_last_seen_us = 0;

/* ---- CRC16 (Modbus, poly 0xA001, init 0xFFFF, LSB first) ---- */
static uint16_t crc16_modbus(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001;
            else              crc >>= 1;
        }
    }
    return crc;
}

/* Receive one frame. A frame ends when the UART stops producing bytes
 * for MBS_READ_GAP_MS — the standard Modbus RTU 3.5-character idle
 * separator. Returns the byte count, or 0 on empty. */
static size_t receive_frame(uint8_t *buf, size_t cap)
{
    size_t got = 0;

    /* Block for the first byte; no deadline here. */
    int n = uart_read_bytes(BSP_RS485_UART_NUM, buf, 1, portMAX_DELAY);
    if (n <= 0) return 0;
    got = 1;

    /* Then read everything still in the pipe, draining until a silent gap. */
    for (;;) {
        n = uart_read_bytes(BSP_RS485_UART_NUM, buf + got,
                            cap - got, pdMS_TO_TICKS(MBS_READ_GAP_MS));
        if (n <= 0) break;
        got += (size_t)n;
        if (got >= cap) break;
    }
    return got;
}

static void send_frame(const uint8_t *buf, size_t len)
{
    uart_write_bytes(BSP_RS485_UART_NUM, (const char *)buf, len);
    uart_wait_tx_done(BSP_RS485_UART_NUM, pdMS_TO_TICKS(100));
}

static void send_exception(uint8_t fc, uint8_t code)
{
    uint8_t resp[5];
    resp[0] = s_slave_addr;
    resp[1] = fc | 0x80;
    resp[2] = code;
    uint16_t crc = crc16_modbus(resp, 3);
    resp[3] = (uint8_t)(crc & 0xFF);
    resp[4] = (uint8_t)(crc >> 8);
    send_frame(resp, sizeof(resp));
}

/* FC 0x03 Read Holding Registers
 * request : [addr][0x03][start_hi][start_lo][cnt_hi][cnt_lo][crc_lo][crc_hi]
 * response: [addr][0x03][byte_cnt][data...][crc_lo][crc_hi] */
static void handle_read_hregs(const uint8_t *req, size_t req_len)
{
    if (req_len != 8) {
        send_exception(0x03, MBS_EXC_ILLEGAL_VALUE);
        return;
    }
    uint16_t start = ((uint16_t)req[2] << 8) | req[3];
    uint16_t count = ((uint16_t)req[4] << 8) | req[5];

    /* Only the intent ring block (0x0000..0x0021) is readable. */
    if (count == 0 || count > HMI_INTENT_RING_REGS) {
        send_exception(0x03, MBS_EXC_ILLEGAL_VALUE);
        return;
    }
    /* Accept any window inside the intent ring -- the primary issues a
     * 2-reg head probe at 0x0000 AND a 32-reg slot read at 0x0002, so
     * requiring start == HMI_INTENT_BASE silently dropped the slot read
     * and intents from the secondary never got dispatched.
     * (HMI_INTENT_BASE is 0, so only the upper-bound check is meaningful.) */
    if ((uint32_t)start + count > (uint32_t)HMI_INTENT_BASE + HMI_INTENT_RING_REGS) {
        send_exception(0x03, MBS_EXC_ILLEGAL_ADDR);
        return;
    }

    uint16_t ring[HMI_INTENT_RING_REGS];
    hmi_sync_read_intent_block(ring);

    uint8_t resp[3 + 2 * HMI_INTENT_RING_REGS + 2];
    resp[0] = s_slave_addr;
    resp[1] = 0x03;
    resp[2] = (uint8_t)(count * 2);
    size_t i = 3;
    for (uint16_t k = 0; k < count; k++) {
        uint16_t v = ring[start - HMI_INTENT_BASE + k];
        resp[i++] = (uint8_t)(v >> 8);
        resp[i++] = (uint8_t)(v & 0xFF);
    }
    uint16_t crc = crc16_modbus(resp, i);
    resp[i++] = (uint8_t)(crc & 0xFF);
    resp[i++] = (uint8_t)(crc >> 8);
    send_frame(resp, i);
}

/* FC 0x10 Write Multiple Registers
 * request : [addr][0x10][start_hi][start_lo][cnt_hi][cnt_lo][byte_cnt][data...][crc_lo][crc_hi]
 * response: [addr][0x10][start_hi][start_lo][cnt_hi][cnt_lo][crc_lo][crc_hi] */
static void handle_write_hregs(const uint8_t *req, size_t req_len)
{
    if (req_len < 9) {
        send_exception(0x10, MBS_EXC_ILLEGAL_VALUE);
        return;
    }
    uint16_t start = ((uint16_t)req[2] << 8) | req[3];
    uint16_t count = ((uint16_t)req[4] << 8) | req[5];
    uint8_t  bytes = req[6];

    if (count == 0 || count > HMI_MIRROR_REGS || bytes != count * 2 ||
        req_len != (size_t)(7 + bytes + 2)) {
        send_exception(0x10, MBS_EXC_ILLEGAL_VALUE);
        return;
    }
    /* Only the full mirror block is writable, and only as one transaction. */
    if (start != HMI_MIRROR_BASE || count != HMI_MIRROR_REGS) {
        send_exception(0x10, MBS_EXC_ILLEGAL_ADDR);
        return;
    }

    uint16_t mirror[HMI_MIRROR_REGS];
    for (uint16_t k = 0; k < count; k++) {
        mirror[k] = ((uint16_t)req[7 + 2 * k] << 8) | req[7 + 2 * k + 1];
    }
    hmi_sync_write_mirror(mirror);

    uint8_t resp[8];
    resp[0] = s_slave_addr;
    resp[1] = 0x10;
    resp[2] = (uint8_t)(start >> 8);
    resp[3] = (uint8_t)(start & 0xFF);
    resp[4] = (uint8_t)(count >> 8);
    resp[5] = (uint8_t)(count & 0xFF);
    uint16_t crc = crc16_modbus(resp, 6);
    resp[6] = (uint8_t)(crc & 0xFF);
    resp[7] = (uint8_t)(crc >> 8);
    send_frame(resp, sizeof(resp));
}

static void process_frame(const uint8_t *frame, size_t len)
{
    if (len < 4) return;                   /* too short to contain addr+fc+crc */
    if (frame[0] != s_slave_addr) return;  /* not for us (no broadcast on this bus) */

    /* CRC check */
    uint16_t calc = crc16_modbus(frame, len - 2);
    uint16_t got  = (uint16_t)frame[len - 2] | ((uint16_t)frame[len - 1] << 8);
    if (calc != got) {
        ESP_LOGW(TAG, "crc mismatch: calc=%04x got=%04x len=%u",
                 calc, got, (unsigned)len);
        return;                            /* silent drop per spec */
    }

    uint8_t fc = frame[1];
    switch (fc) {
    case 0x03: handle_read_hregs (frame, len); break;
    case 0x10: handle_write_hregs(frame, len); break;
    default:
        ESP_LOGW(TAG, "unsupported FC 0x%02x", fc);
        send_exception(fc, MBS_EXC_ILLEGAL_FUNC);
        break;
    }

    s_rx_count++;
    s_last_seen_us = esp_timer_get_time();
}

static void server_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "slave server listening as 0x%02x", s_slave_addr);

    uint8_t frame[MBS_MAX_FRAME];
    for (;;) {
        size_t n = receive_frame(frame, sizeof(frame));
        if (n == 0) continue;
        process_frame(frame, n);
        /* Tiny pause so the next frame's 3.5-char gap is reliably detected. */
        vTaskDelay(pdMS_TO_TICKS(MBS_INTER_FRAME_MS));
    }
}

esp_err_t modbus_server_start(uint8_t slave_addr)
{
    if (s_started) return ESP_OK;
    if (slave_addr == 0 || slave_addr > 247) return ESP_ERR_INVALID_ARG;

    s_slave_addr = slave_addr;

    BaseType_t ok = xTaskCreatePinnedToCore(server_task, "mb_srv",
                                            4096, NULL, 4, NULL, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed");
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

bool     modbus_server_peer_seen(void) { return s_last_seen_us != 0; }
uint32_t modbus_server_rx_count(void)  { return s_rx_count;           }
