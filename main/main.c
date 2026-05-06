#include "esp_log.h"
#include "bsp.h"
#include "bsp_pins.h"
#include "app.h"
#include "ui_theme.h"
#include "owner_name.h"
#include "tile_names.h"
#include "modbus_client.h"
#include "modbus_task.h"
#include "modbus_server.h"
#include "hvac_controller.h"
#include "bmp280.h"
#include "hmi_role.h"
#include "hmi_sync.h"
#include "bus_config.h"
#include "hyperwisor.h"
#include "hyperwisor_app.h"
#include "display_brightness.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Hyperwisor S3 booting");

    /* 1) NVS must be initialised BEFORE the RGB LCD panel starts.
     *    The RGB DMA EOF ISR reads the PSRAM bounce buffer, but NVS
     *    (flash) reads temporarily disable the flash/PSRAM cache, so
     *    an ISR firing mid-read would crash with "Cache disabled but
     *    cached memory region accessed". Doing it here keeps flash
     *    I/O entirely before any periodic DMA kicks in. */
    owner_name_init();

    /* Role (PRIMARY default) + bus baud (9600 default) both persisted in NVS.
     * Read before any UART / UI brings up so later branches see the final
     * effective values. */
    hmi_role_init();
    bus_config_init();

    /* Load active colour-theme preset from NVS. Must run before any
     * UI build so the first paint uses the right palette. NVS has
     * already been initialised inside owner_name_init(). */
    ui_theme_init();

    /* Load persisted Reading Lights / Switches display names into
     * the ctrl_state arrays. NVS is already initialised above, so
     * this is a straight read. Kept in app_main alongside
     * owner_name_init() so all flash reads happen before the RGB
     * panel's DMA EOF ISR starts firing. */
    tile_names_init();

    /* Load persisted LCD brightness level. The value is applied right
     * after bsp_init() below (LVGL must be up first). All flash reads
     * stay before the RGB panel's DMA EOF ISR comes online. */
    display_brightness_init();

    /* 2) Bring up board: I2C -> IO expander -> RGB panel -> GT911 touch -> LVGL */
    bsp_init();

    /* Apply the loaded brightness level as soon as LVGL is ready. This
     * paints the dimming overlay on lv_layer_sys() and flips the
     * backlight through CH422G EXIO2. A brief ~one-frame flash at full
     * brightness is expected between bsp_init's backlight-on and here. */
    display_brightness_apply();

    /* 3) Role-branched boot.
     *
     *    PRIMARY : owns the bus -- master talks to relay+RGB+HVAC+secondary,
     *              runs BMP280 + HVAC controller locally.
     *    SECONDARY: headless logic-wise; no master, no BMP280, no HVAC.
     *              Its UART is owned by modbus_server (slave 0x10) which
     *              only answers FC 0x03 / 0x10 from the primary.
     *
     *    Both roles boot the same LVGL UI; widgets read from ac_state /
     *    ctrl_state which the primary drives from local events and the
     *    secondary drives from mirror pushes. */
    hmi_role_t role  = hmi_role_get();
    uint32_t   baud  = bus_config_get_baud();
    ESP_LOGI(TAG, "role=%s, bus=%lu baud",
             hmi_role_str(role), (unsigned long)baud);

    if (role == HMI_ROLE_PRIMARY) {
        /* 3a) Bring up BMP280 on the shared I2C bus (header pins SDA=GPIO8
         *     SCL=GPIO9). Non-fatal: if no sensor is connected the UI still
         *     works and the Climate tab shows "--". */
        if (bmp280_init() == ESP_OK) {
            bmp280_task_start();
        }

        /* 3b) Bring up Modbus RTU master over RS-485 (UART1, GPIO15/16).
         *     Baud comes from bus_config (user-configurable in Maintenance). */
        if (modbus_client_init(baud) == ESP_OK) {
            modbus_task_start();

            /* HVAC controller drives coils 8..15 on the same relay board.
             * Safe to start here: it uses modbus_task_request() (the same
             * queue the UI uses) and reads BMP280 via bmp280_get() which is
             * already running. Relay wiring + thresholds live in hvac_config.h. */
            hvac_controller_start();
        }

        /* 3c) Hyperwisor IoT cloud: native WiFi STA/AP + websocket client.
         *     Only the PRIMARY pushes state to the cloud; the SECONDARY is
         *     headless logic-wise and has no independent view of the bus.
         *     Order matters: core first (creates NVS keys + WiFi netif),
         *     then app layer registers command handlers + loads widget
         *     bindings, then start kicks off the state machine. */
        if (hyperwisor_init() == ESP_OK) {
            hyperwisor_app_init();
            hyperwisor_start();
        } else {
            ESP_LOGW(TAG, "hyperwisor_init failed - cloud features disabled");
        }
    } else {
        /* SECONDARY: install the UART driver only, then run the slave.
         * modbus_client_init() is still called because it owns the UART
         * driver install -- the master worker is simply never started. */
        if (modbus_client_init(baud) == ESP_OK) {
            modbus_server_start(HMI_SECONDARY_SLAVE_ID);
        }
    }

    /* 4) Hand control to application (LVGL UI + Hyperwisor IoT logic) */
    app_start();
}
