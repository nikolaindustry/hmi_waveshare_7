#include "esp_log.h"
#include "esp_spiffs.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "bsp.h"
#include "app.h"
#include "splash.h"
#include "ui/ui_shell.h"
#include "hds.h"
#include "cloud/hds_sink.h"

#include <sys/stat.h>

static const char *TAG = "app";

/* =====================================================================
 *  Dashboard or shell
 *
 *  The `storage` SPIFFS partition may hold a transcoded Hyperwisor
 *  dashboard (see tools/hds_transcode.py). When it does, the panel
 *  renders that dashboard; when it does not, the hand-built ui_shell
 *  comes up exactly as before. Nothing else in the boot path changes,
 *  so a board can be moved between the two by reflashing the partition.
 * ===================================================================== */

#define DASH_MOUNT  "/spiffs"
#define DASH_SPEC   DASH_MOUNT "/dashboard.json"
#define DASH_ASSETS DASH_MOUNT "/assets"

static bool s_dashboard_ready;

static void dashboard_probe(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = DASH_MOUNT,
        .partition_label        = "storage",
        .max_files              = 4,
        .format_if_mount_failed = false,   /* never wipe the partition */
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        /* An unformatted or absent partition is a normal state for a
         * board that has only ever run the shell. */
        ESP_LOGI(TAG, "no dashboard storage (%s); using shell UI",
                 esp_err_to_name(err));
        return;
    }

    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "storage mounted: %u/%u bytes used",
                 (unsigned)used, (unsigned)total);
    }

    struct stat st;
    if (stat(DASH_SPEC, &st) != 0) {
        ESP_LOGI(TAG, "storage holds no %s; using shell UI", DASH_SPEC);
        esp_vfs_spiffs_unregister("storage");
        return;
    }

    /* Parse before the splash finishes rather than on the LVGL callback,
     * so a malformed spec falls back to the shell instead of leaving a
     * half-built screen up. */
    if (hds_init(DASH_SPEC, DASH_ASSETS) != ESP_OK) {
        ESP_LOGE(TAG, "dashboard spec failed to load; using shell UI");
        esp_vfs_spiffs_unregister("storage");
        return;
    }

    hds_set_emit(hds_sink_emit, NULL);
    s_dashboard_ready = true;
    ESP_LOGI(TAG, "dashboard ready (%ld bytes): %s", (long)st.st_size,
             hds_spec()->name ? hds_spec()->name : "unnamed");
}

/* Called by splash when it finishes. The LVGL task is holding the port
 * mutex here, so we build straight onto the target screen supplied by
 * the splash animation pipeline. */
static void on_splash_done(lv_obj_t *target_scr)
{
    if (s_dashboard_ready && hds_build(target_scr)) {
        ESP_LOGI(TAG, "dashboard UI ready");
        return;
    }
    ui_shell_build(target_scr);
    ESP_LOGI(TAG, "shell UI ready");
}

void app_start(void)
{
    ESP_LOGI(TAG, "LVGL app starting");

    /* owner_name_init() runs in app_main BEFORE bsp_init() so NVS flash
     * reads don't race with the RGB panel's DMA EOF ISR. */

    /* Probe before taking the LVGL lock: this touches flash and parses
     * JSON, neither of which should happen under the port mutex. */
    dashboard_probe();

    /* All LVGL calls must be wrapped with the port mutex */
    if (!lvgl_port_lock(-1)) {
        ESP_LOGE(TAG, "LVGL lock timeout");
        return;
    }
    splash_show(3000, on_splash_done);
    lvgl_port_unlock();
}
