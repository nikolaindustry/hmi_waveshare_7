#include "bmp280.h"
#include "hyperwisor_app.h"

#include <string.h>

#include "bsp.h"
#include "bsp_pins.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bmp280";

/* ===== BMP280 register map (datasheet section 4.2) ===== */
#define BMP280_REG_CALIB_START  0x88
#define BMP280_REG_CALIB_LEN    24      /* 0x88..0x9F (T1..P9, 12 x u16) */
#define BMP280_REG_ID           0xD0
#define BMP280_REG_RESET        0xE0
#define BMP280_REG_STATUS       0xF3
#define BMP280_REG_CTRL_MEAS    0xF4
#define BMP280_REG_CONFIG       0xF5
#define BMP280_REG_PRESS_MSB    0xF7    /* burst-read 6 bytes: P MSB/LSB/XLSB, T MSB/LSB/XLSB */

#define BMP280_CHIP_ID_BMP280   0x58
#define BMP280_CHIP_ID_BME280   0x60    /* accept BME280 too - register-compatible for T+P */

/* ctrl_meas: osrs_t=x2, osrs_p=x16, mode=normal */
#define BMP280_CTRL_MEAS_VAL    ((0x02 << 5) | (0x05 << 2) | 0x03)
/* config: t_sb=500ms, filter=x16, spi3w_en=0 */
#define BMP280_CONFIG_VAL       ((0x04 << 5) | (0x04 << 2) | 0x00)

/* ===== state ===== */
static i2c_master_dev_handle_t s_dev = NULL;
static SemaphoreHandle_t       s_mtx = NULL;
static bool                    s_ready = false;

/* Bosch calibration coefficients (datasheet section 3.11) */
static uint16_t dig_T1;
static int16_t  dig_T2, dig_T3;
static uint16_t dig_P1;
static int16_t  dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;

/* Latest compensated sample */
static float s_temp_c = 0.0f;
static float s_press_hpa = 0.0f;

/* ===== low-level I2C helpers ===== */
static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len,
                                       BSP_I2C_TIMEOUT_MS);
}

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { reg, val };
    return i2c_master_transmit(s_dev, tx, 2, BSP_I2C_TIMEOUT_MS);
}

/* ===== address probe ===== */
static esp_err_t probe_addr(uint8_t addr)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_bus_handle();
    if (!bus) return ESP_ERR_INVALID_STATE;

    /* Is anything ACK-ing on that address? */
    if (i2c_master_probe(bus, addr, 50) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = BSP_I2C_FREQ_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &dev);
    if (err != ESP_OK) return err;

    /* Read chip id to confirm it's a BMP280 / BME280 */
    uint8_t reg = BMP280_REG_ID;
    uint8_t id  = 0;
    err = i2c_master_transmit_receive(dev, &reg, 1, &id, 1,
                                      BSP_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        i2c_master_bus_rm_device(dev);
        return err;
    }
    if (id != BMP280_CHIP_ID_BMP280 && id != BMP280_CHIP_ID_BME280) {
        ESP_LOGW(TAG, "addr 0x%02X responded with id 0x%02X (not BMP280)",
                 addr, id);
        i2c_master_bus_rm_device(dev);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "found chip id 0x%02X at 0x%02X", id, addr);
    s_dev = dev;
    return ESP_OK;
}

/* ===== calibration ===== */
static esp_err_t load_calibration(void)
{
    uint8_t raw[BMP280_REG_CALIB_LEN];
    esp_err_t err = read_regs(BMP280_REG_CALIB_START, raw, sizeof(raw));
    if (err != ESP_OK) return err;

    dig_T1 = (uint16_t)(raw[0]  | (raw[1]  << 8));
    dig_T2 = (int16_t) (raw[2]  | (raw[3]  << 8));
    dig_T3 = (int16_t) (raw[4]  | (raw[5]  << 8));
    dig_P1 = (uint16_t)(raw[6]  | (raw[7]  << 8));
    dig_P2 = (int16_t) (raw[8]  | (raw[9]  << 8));
    dig_P3 = (int16_t) (raw[10] | (raw[11] << 8));
    dig_P4 = (int16_t) (raw[12] | (raw[13] << 8));
    dig_P5 = (int16_t) (raw[14] | (raw[15] << 8));
    dig_P6 = (int16_t) (raw[16] | (raw[17] << 8));
    dig_P7 = (int16_t) (raw[18] | (raw[19] << 8));
    dig_P8 = (int16_t) (raw[20] | (raw[21] << 8));
    dig_P9 = (int16_t) (raw[22] | (raw[23] << 8));
    return ESP_OK;
}

/* ===== Bosch compensation (datasheet section 8.1, float variant) ===== */
static void compensate(int32_t raw_p, int32_t raw_t,
                       float *out_t, float *out_p)
{
    /* Temperature */
    float var1 = (((float)raw_t) / 16384.0f - ((float)dig_T1) / 1024.0f)
                 * ((float)dig_T2);
    float var2 = ((((float)raw_t) / 131072.0f - ((float)dig_T1) / 8192.0f)
                  * (((float)raw_t) / 131072.0f - ((float)dig_T1) / 8192.0f))
                 * ((float)dig_T3);
    float t_fine = var1 + var2;
    float T = t_fine / 5120.0f;

    /* Pressure */
    var1 = (t_fine / 2.0f) - 64000.0f;
    var2 = var1 * var1 * ((float)dig_P6) / 32768.0f;
    var2 = var2 + var1 * ((float)dig_P5) * 2.0f;
    var2 = (var2 / 4.0f) + (((float)dig_P4) * 65536.0f);
    var1 = (((float)dig_P3) * var1 * var1 / 524288.0f
            + ((float)dig_P2) * var1) / 524288.0f;
    var1 = (1.0f + var1 / 32768.0f) * ((float)dig_P1);
    float P = 0.0f;
    if (var1 != 0.0f) {
        P = 1048576.0f - (float)raw_p;
        P = (P - (var2 / 4096.0f)) * 6250.0f / var1;
        var1 = ((float)dig_P9) * P * P / 2147483648.0f;
        var2 = P * ((float)dig_P8) / 32768.0f;
        P = P + (var1 + var2 + ((float)dig_P7)) / 16.0f;
        /* P is in Pa -> convert to hPa */
        P = P / 100.0f;
    }

    *out_t = T;
    *out_p = P;
}

/* ===== public API ===== */
esp_err_t bmp280_init(void)
{
    if (s_dev) return ESP_OK;

    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
        if (!s_mtx) return ESP_ERR_NO_MEM;
    }

    /* Try 0x76 first (SDO=GND, common on breakouts), then 0x77. */
    esp_err_t err = probe_addr(0x76);
    if (err != ESP_OK) err = probe_addr(0x77);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no BMP280 detected on I2C0 (tried 0x76, 0x77)");
        return ESP_ERR_NOT_FOUND;
    }

    err = load_calibration();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "calibration read failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Set filter + standby first, then enable normal-mode sampling */
    err = write_reg(BMP280_REG_CONFIG,    BMP280_CONFIG_VAL);
    if (err != ESP_OK) return err;
    err = write_reg(BMP280_REG_CTRL_MEAS, BMP280_CTRL_MEAS_VAL);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "BMP280 configured (osrs_t=x2, osrs_p=x16, filter=x16, 500ms)");
    return ESP_OK;
}

bool bmp280_is_ready(void)
{
    return s_ready;
}

bool bmp280_get(float *temp_c, float *pressure_hpa)
{
    if (!s_ready || !s_mtx) return false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (temp_c)       *temp_c       = s_temp_c;
    if (pressure_hpa) *pressure_hpa = s_press_hpa;
    xSemaphoreGive(s_mtx);
    return true;
}

/* ===== sampling task ===== */
static void sample_once(void)
{
    uint8_t raw[6];
    if (read_regs(BMP280_REG_PRESS_MSB, raw, sizeof(raw)) != ESP_OK) {
        return;
    }
    int32_t raw_p = ((int32_t)raw[0] << 12) | ((int32_t)raw[1] << 4) | (raw[2] >> 4);
    int32_t raw_t = ((int32_t)raw[3] << 12) | ((int32_t)raw[4] << 4) | (raw[5] >> 4);

    /* 0x80000 = sensor hasn't produced a sample yet (e.g. forced mode idle) */
    if (raw_p == 0x80000 || raw_t == 0x80000) return;

    float t, p;
    compensate(raw_p, raw_t, &t, &p);

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_temp_c    = t;
    s_press_hpa = p;
    s_ready     = true;
    xSemaphoreGive(s_mtx);

    /* Voluntary cloud push every 5th sample (~5s at 1 Hz sampling).
     * Internal to hyperwisor_app: no-op when the websocket is not
     * connected or the cloud bindings haven't been configured yet. */
    static int s_push_counter = 0;
    if (++s_push_counter >= 5) {
        s_push_counter = 0;
        hyperwisor_app_push_bmp280(t, p);
    }
}

static void bmp280_task(void *arg)
{
    (void)arg;
    /* First reading may take ~1 conversion cycle (~50 ms) to be valid
     * after wake - wait a bit so the first logged value is real. */
    vTaskDelay(pdMS_TO_TICKS(100));
    for (;;) {
        sample_once();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void bmp280_task_start(void)
{
    if (!s_dev) return;   /* no hardware -> no task */
    static bool started = false;
    if (started) return;
    started = true;
    /* Pin to core 1 to stay off the LVGL / main thread on core 0 */
    xTaskCreatePinnedToCore(bmp280_task, "bmp280", 3072, NULL,
                            tskIDLE_PRIORITY + 2, NULL, 1);
}
