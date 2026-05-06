#include "driver/i2c_master.h"
#include "esp_log.h"
#include "bsp.h"
#include "bsp_pins.h"

static const char *TAG = "bsp_i2c";
static i2c_master_bus_handle_t s_bus = NULL;

esp_err_t bsp_i2c_init(void)
{
    if (s_bus) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = BSP_I2C_NUM,
        .sda_io_num        = BSP_I2C_SDA,
        .scl_io_num        = BSP_I2C_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "I2C%d up (SDA=%d SCL=%d %dHz)",
             BSP_I2C_NUM, BSP_I2C_SDA, BSP_I2C_SCL, BSP_I2C_FREQ_HZ);
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_bus_handle(void)
{
    return s_bus;
}
