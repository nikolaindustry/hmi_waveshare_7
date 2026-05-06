#include "driver/i2c_master.h"
#include "esp_log.h"
#include "bsp.h"
#include "bsp_pins.h"

static const char *TAG = "ch422g";

/* CH422G: write 1 byte to fixed slave addresses.
 *   0x24 (WR_SET) -> system control byte. 0x01 = enable IO0..IO7 outputs.
 *   0x38 (WR_IO)  -> IO0..IO7 output latch.
 * Each address is a distinct device on the I2C bus (CH422G quirk).
 */
static i2c_master_dev_handle_t s_dev_wr_set = NULL;
static i2c_master_dev_handle_t s_dev_wr_io  = NULL;
static uint8_t s_shadow = 0;

static esp_err_t ch422g_add_dev(uint8_t addr, i2c_master_dev_handle_t *out)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = BSP_I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(bsp_i2c_get_bus_handle(), &dev_cfg, out);
}

static esp_err_t ch422g_write1_dev(i2c_master_dev_handle_t dev, uint8_t value)
{
    return i2c_master_transmit(dev, &value, 1, BSP_I2C_TIMEOUT_MS);
}

esp_err_t bsp_io_expander_init(void)
{
    esp_err_t err;

    if (!s_dev_wr_set) {
        err = ch422g_add_dev(CH422G_ADDR_WR_SET, &s_dev_wr_set);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "add_dev WR_SET: %s", esp_err_to_name(err));
            return err;
        }
    }
    if (!s_dev_wr_io) {
        err = ch422g_add_dev(CH422G_ADDR_WR_IO, &s_dev_wr_io);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "add_dev WR_IO: %s", esp_err_to_name(err));
            return err;
        }
    }

    /* Enable IO0..IO7 as outputs */
    if ((err = ch422g_write1_dev(s_dev_wr_set, 0x01)) != ESP_OK) {
        ESP_LOGE(TAG, "WR_SET (0x%02X) failed: %s - CH422G not responding?",
                 CH422G_ADDR_WR_SET, esp_err_to_name(err));
        return err;
    }

    /* Initial output: DISP high, SD_CS high, BL off, TP in reset */
    s_shadow = CH422G_IO_INIT_VAL;
    if ((err = ch422g_write1_dev(s_dev_wr_io, s_shadow)) != ESP_OK) {
        ESP_LOGE(TAG, "WR_IO init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "CH422G ready (shadow=0x%02X)", s_shadow);
    return ESP_OK;
}

esp_err_t bsp_io_expander_write_byte(uint8_t byte)
{
    s_shadow = byte;
    return ch422g_write1_dev(s_dev_wr_io, byte);
}

esp_err_t bsp_io_expander_set(uint8_t bit, bool level)
{
    if (bit > 7) {
        return ESP_ERR_INVALID_ARG;
    }
    if (level) s_shadow |=  (1u << bit);
    else       s_shadow &= ~(1u << bit);

    esp_err_t err = ch422g_write1_dev(s_dev_wr_io, s_shadow);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WR_IO bit%d=%d failed: %s", bit, level, esp_err_to_name(err));
    }
    return err;
}
