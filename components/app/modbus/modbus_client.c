#include "modbus_client.h"
#include "bsp_pins.h"

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "modbus";

#define MB_RX_BUF_SIZE 256
#define MB_TX_TIMEOUT_MS 200
#define MB_RESP_TIMEOUT_MS 250
#define MB_INTER_FRAME_MS 10   /* 3.5 char times: ~4ms @9600, ~0.3ms @115200;
                                  10ms is safe for both bus speeds */

static SemaphoreHandle_t s_lock;
static bool s_inited = false;

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

esp_err_t modbus_client_init(uint32_t baud)
{
    if (s_inited) return ESP_OK;

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    const uart_config_t cfg = {
        .baud_rate = (int)baud,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(BSP_RS485_UART_NUM,
                                        MB_RX_BUF_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK) { ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(err)); return err; }

    err = uart_param_config(BSP_RS485_UART_NUM, &cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "uart_param_config: %s", esp_err_to_name(err)); return err; }

    err = uart_set_pin(BSP_RS485_UART_NUM,
                       BSP_RS485_IO_TX, BSP_RS485_IO_RX,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) { ESP_LOGE(TAG, "uart_set_pin: %s", esp_err_to_name(err)); return err; }

    /* Transceiver handles DE/RE automatically; plain UART mode is fine. */
    s_inited = true;
    ESP_LOGI(TAG, "master started, UART%d %u 8N1 on TX=%d RX=%d (auto-DE)",
             BSP_RS485_UART_NUM, (unsigned)baud,
             BSP_RS485_IO_TX, BSP_RS485_IO_RX);
    return ESP_OK;
}

/* Send `tx_len` bytes, wait for exactly `want_rx` bytes (or timeout).
 * Caller owns the lock. */
static esp_err_t txn(const uint8_t *tx, size_t tx_len,
                     uint8_t *rx, size_t want_rx)
{
    /* Clear any stale bytes */
    uart_flush_input(BSP_RS485_UART_NUM);

    ESP_LOGI(TAG, "TX %u bytes:", (unsigned)tx_len);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, tx, tx_len, ESP_LOG_INFO);

    int written = uart_write_bytes(BSP_RS485_UART_NUM, (const char *)tx, tx_len);
    if (written != (int)tx_len) {
        ESP_LOGW(TAG, "uart_write_bytes returned %d / %u", written, (unsigned)tx_len);
        return ESP_FAIL;
    }
    /* Make sure the stop bit of the last byte has left the shifter before
     * the transceiver flips back to receive. */
    uart_wait_tx_done(BSP_RS485_UART_NUM, pdMS_TO_TICKS(MB_TX_TIMEOUT_MS));

    size_t got = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(MB_RESP_TIMEOUT_MS);
    while (got < want_rx) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            if (got == 0) {
                ESP_LOGW(TAG, "rx timeout: 0/%u bytes (no reply from slave)",
                         (unsigned)want_rx);
            } else {
                ESP_LOG_BUFFER_HEX_LEVEL(TAG, rx, got, ESP_LOG_WARN);
                ESP_LOGW(TAG, "rx timeout: partial %u/%u bytes (see hex above)",
                         (unsigned)got, (unsigned)want_rx);
            }
            return ESP_ERR_TIMEOUT;
        }
        int n = uart_read_bytes(BSP_RS485_UART_NUM, rx + got,
                                want_rx - got, deadline - now);
        if (n < 0) return ESP_FAIL;
        got += (size_t)n;
    }
    /* Allow inter-frame gap so the next transaction sees a clean line. */
    vTaskDelay(pdMS_TO_TICKS(MB_INTER_FRAME_MS));
    ESP_LOGI(TAG, "RX %u bytes:", (unsigned)got);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, rx, got, ESP_LOG_INFO);
    return ESP_OK;
}

esp_err_t modbus_client_write_coil(uint8_t slave, uint16_t coil, bool on)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    uint8_t  frame[8];
    frame[0] = slave;
    frame[1] = 0x05;
    frame[2] = (uint8_t)(coil >> 8);
    frame[3] = (uint8_t)(coil & 0xFF);
    frame[4] = on ? 0xFF : 0x00;
    frame[5] = 0x00;
    uint16_t crc = crc16_modbus(frame, 6);
    frame[6] = (uint8_t)(crc & 0xFF);   /* CRC LSB first */
    frame[7] = (uint8_t)(crc >> 8);

    uint8_t resp[8] = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = txn(frame, sizeof(frame), resp, sizeof(resp));
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) return err;

    /* FC 0x05 response is byte-for-byte echo of the request. */
    if (memcmp(frame, resp, 8) != 0) {
        ESP_LOGW(TAG, "write_coil: echo mismatch (coil=%u on=%d)", coil, on);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t modbus_client_read_coils(uint8_t slave, uint16_t start,
                                   uint16_t count, uint16_t *mask)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!mask || count == 0 || count > 16) return ESP_ERR_INVALID_ARG;

    uint8_t req[8];
    req[0] = slave;
    req[1] = 0x01;
    req[2] = (uint8_t)(start >> 8);
    req[3] = (uint8_t)(start & 0xFF);
    req[4] = (uint8_t)(count >> 8);
    req[5] = (uint8_t)(count & 0xFF);
    uint16_t crc = crc16_modbus(req, 6);
    req[6] = (uint8_t)(crc & 0xFF);
    req[7] = (uint8_t)(crc >> 8);

    /* Response: [addr][0x01][byte_count][data...][crc_lo][crc_hi]
     * byte_count = ceil(count / 8). For count<=16 that's 1 or 2 bytes. */
    size_t data_bytes = (count + 7) / 8;
    size_t want_rx    = 3 + data_bytes + 2;
    uint8_t resp[16]  = {0};
    if (want_rx > sizeof(resp)) return ESP_ERR_INVALID_SIZE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = txn(req, sizeof(req), resp, want_rx);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) return err;

    /* Validate header + CRC. */
    if (resp[0] != slave || resp[1] != 0x01 || resp[2] != data_bytes) {
        ESP_LOGW(TAG, "read_coils: bad header %02x %02x %02x",
                 resp[0], resp[1], resp[2]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint16_t calc = crc16_modbus(resp, 3 + data_bytes);
    uint16_t got  = (uint16_t)resp[3 + data_bytes] |
                    ((uint16_t)resp[3 + data_bytes + 1] << 8);
    if (calc != got) {
        ESP_LOGW(TAG, "read_coils: CRC %04x != %04x", calc, got);
        return ESP_ERR_INVALID_CRC;
    }

    uint16_t bits = resp[3];
    if (data_bytes == 2) bits |= ((uint16_t)resp[4]) << 8;
    *mask = bits;
    return ESP_OK;
}

esp_err_t modbus_client_write_hregs(uint8_t slave, uint16_t start,
                                    uint16_t count, const uint16_t *regs)
{
    if (!s_inited)                        return ESP_ERR_INVALID_STATE;
    if (!regs || count == 0 || count > 64) return ESP_ERR_INVALID_ARG;

    /* FC 0x10 request frame:
     *   [addr][0x10][start_hi][start_lo][cnt_hi][cnt_lo]
     *   [byte_count][data_hi[0]][data_lo[0]] ... [crc_lo][crc_hi]
     * 7 header bytes + 2*count data bytes + 2 CRC bytes. Worst case for
     * count=64 is 7 + 128 + 2 = 137 bytes -- fits comfortably in 160. */
    uint8_t frame[160];
    size_t  i = 0;
    frame[i++] = slave;
    frame[i++] = 0x10;
    frame[i++] = (uint8_t)(start >> 8);
    frame[i++] = (uint8_t)(start & 0xFF);
    frame[i++] = (uint8_t)(count >> 8);
    frame[i++] = (uint8_t)(count & 0xFF);
    frame[i++] = (uint8_t)(count * 2);
    for (uint16_t k = 0; k < count; k++) {
        frame[i++] = (uint8_t)(regs[k] >> 8);
        frame[i++] = (uint8_t)(regs[k] & 0xFF);
    }
    uint16_t crc = crc16_modbus(frame, i);
    frame[i++] = (uint8_t)(crc & 0xFF);
    frame[i++] = (uint8_t)(crc >> 8);

    /* FC 0x10 response is a fixed 8 bytes:
     *   [addr][0x10][start_hi][start_lo][cnt_hi][cnt_lo][crc_lo][crc_hi] */
    uint8_t resp[8] = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = txn(frame, i, resp, sizeof(resp));
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) return err;

    /* Validate: address, function code, echoed start, echoed count, CRC. */
    if (resp[0] != slave || resp[1] != 0x10 ||
        resp[2] != (uint8_t)(start >> 8) ||
        resp[3] != (uint8_t)(start & 0xFF) ||
        resp[4] != (uint8_t)(count >> 8) ||
        resp[5] != (uint8_t)(count & 0xFF)) {
        ESP_LOGW(TAG, "write_hregs: bad header %02x %02x %02x%02x %02x%02x",
                 resp[0], resp[1], resp[2], resp[3], resp[4], resp[5]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint16_t calc = crc16_modbus(resp, 6);
    uint16_t got  = (uint16_t)resp[6] | ((uint16_t)resp[7] << 8);
    if (calc != got) {
        ESP_LOGW(TAG, "write_hregs: CRC %04x != %04x", calc, got);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

esp_err_t modbus_client_write_hreg_single(uint8_t slave, uint16_t addr,
                                          uint16_t value)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    /* FC 0x06 request: [addr][0x06][reg_hi][reg_lo][val_hi][val_lo][crc_lo][crc_hi] */
    uint8_t frame[8];
    frame[0] = slave;
    frame[1] = 0x06;
    frame[2] = (uint8_t)(addr >> 8);
    frame[3] = (uint8_t)(addr & 0xFF);
    frame[4] = (uint8_t)(value >> 8);
    frame[5] = (uint8_t)(value & 0xFF);
    uint16_t crc = crc16_modbus(frame, 6);
    frame[6] = (uint8_t)(crc & 0xFF);
    frame[7] = (uint8_t)(crc >> 8);

    /* Broadcast (slave 0): Modbus spec says no response. Fire and forget. */
    if (slave == 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uart_flush_input(BSP_RS485_UART_NUM);
        int written = uart_write_bytes(BSP_RS485_UART_NUM,
                                       (const char *)frame, sizeof(frame));
        uart_wait_tx_done(BSP_RS485_UART_NUM,
                          pdMS_TO_TICKS(MB_TX_TIMEOUT_MS));
        vTaskDelay(pdMS_TO_TICKS(MB_INTER_FRAME_MS));
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "write_hreg_single broadcast addr=0x%04x val=0x%04x (%d bytes)",
                 addr, value, written);
        return (written == (int)sizeof(frame)) ? ESP_OK : ESP_FAIL;
    }

    /* FC 0x06 success response is byte-for-byte echo of the request.
     * Exception response is 5 bytes; txn() will time out waiting for 8. */
    uint8_t resp[8] = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = txn(frame, sizeof(frame), resp, sizeof(resp));
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) return err;

    if (memcmp(frame, resp, 8) != 0) {
        ESP_LOGW(TAG, "write_hreg_single: echo mismatch (slave=%u addr=0x%04x val=0x%04x)",
                 slave, addr, value);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t modbus_client_read_hregs(uint8_t slave, uint16_t start,
                                   uint16_t count, uint16_t *regs)
{
    if (!s_inited)                        return ESP_ERR_INVALID_STATE;
    if (!regs || count == 0 || count > 64) return ESP_ERR_INVALID_ARG;

    uint8_t req[8];
    req[0] = slave;
    req[1] = 0x03;
    req[2] = (uint8_t)(start >> 8);
    req[3] = (uint8_t)(start & 0xFF);
    req[4] = (uint8_t)(count >> 8);
    req[5] = (uint8_t)(count & 0xFF);
    uint16_t crc = crc16_modbus(req, 6);
    req[6] = (uint8_t)(crc & 0xFF);
    req[7] = (uint8_t)(crc >> 8);

    /* Response: [addr][0x03][byte_count][data...][crc_lo][crc_hi]
     *   byte_count = count * 2
     *   Worst case count=64 -> 3 + 128 + 2 = 133 bytes. */
    size_t  data_bytes = (size_t)count * 2;
    size_t  want_rx    = 3 + data_bytes + 2;
    uint8_t resp[160]  = {0};
    if (want_rx > sizeof(resp)) return ESP_ERR_INVALID_SIZE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = txn(req, sizeof(req), resp, want_rx);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) return err;

    /* Validate header + CRC. */
    if (resp[0] != slave || resp[1] != 0x03 || resp[2] != data_bytes) {
        /* An exception reply (fc|0x80) is 5 bytes -- not what we asked for. */
        ESP_LOGW(TAG, "read_hregs: bad header %02x %02x %02x",
                 resp[0], resp[1], resp[2]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint16_t calc_r = crc16_modbus(resp, 3 + data_bytes);
    uint16_t got_r  = (uint16_t)resp[3 + data_bytes] |
                      ((uint16_t)resp[3 + data_bytes + 1] << 8);
    if (calc_r != got_r) {
        ESP_LOGW(TAG, "read_hregs: CRC %04x != %04x", calc_r, got_r);
        return ESP_ERR_INVALID_CRC;
    }

    /* Unpack big-endian registers into host order. */
    for (uint16_t k = 0; k < count; k++) {
        regs[k] = ((uint16_t)resp[3 + 2 * k] << 8) | resp[3 + 2 * k + 1];
    }
    return ESP_OK;
}
