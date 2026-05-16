# Hyperwisor IoT for ESP-IDF

Drop-in IoT client for ESP-IDF: WiFi provisioning, secure WebSocket transport,
cloud command runtime, dashboard widgets, NTP, NVS helpers, and optional
OTA + GPIO management — all behind one umbrella header.

* **Targets:** ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6, ESP32-P4
* **IDF:** `>= 5.5, < 7.0`
* **License:** Apache-2.0
* **UI:** framework-agnostic (LVGL/raw/none — your call)

---

## Install

Add to your project's `main/idf_component.yml`:

```yaml
dependencies:
  nikolaindustry/hyperwisor: "^0.1.0"
```

Then `idf.py reconfigure`. The dependency manager pulls
`espressif/esp_websocket_client` and `espressif/cjson` automatically.

## Quick start

```c
#include "hyperwisor.h"

void app_main(void)
{
    hyperwisor_init();

    /* First-boot provisioning. On subsequent boots these values are
     * loaded from NVS automatically — call only when you need to override. */
    if (!hyperwisor_has_credentials()) {
        hyperwisor_set_credentials("MySSID", "MyPassword",
                                   "my-device-id", "my-user-id");
    }

    hyperwisor_start();   /* spawns the core task */
}
```

The device auto-connects to WiFi, syncs NTP, opens a WebSocket to the
configured Hyperwisor server, and starts accepting cloud commands. If no
credentials are present it falls back to SoftAP provisioning mode
(`NIKOLAINDUSTRY_Setup-XXXX` / pass `0123456789`).

## Features (Kconfig toggles)

| Symbol | Default | Purpose |
| --- | --- | --- |
| `HYPERWISOR_ENABLE_SYSTEM` | y | Built-in `restart`, `status` commands |
| `HYPERWISOR_ENABLE_NTP`    | y | SNTP time sync after WiFi up |
| `HYPERWISOR_ENABLE_HTTP`   | y | `hyperwisor_http_get/post` helpers |
| `HYPERWISOR_ENABLE_WIDGET` | y | Push live values to dashboard widgets |
| `HYPERWISOR_ENABLE_OTA`    | n | OTA via `OTA_UPDATE` cloud command |
| `HYPERWISOR_ENABLE_GPIO`   | n | Cloud-driven GPIO read/write |
| `HYPERWISOR_ENABLE_DISPLAY`| n | Backlight control via board port |

## Configuration

```kconfig
CONFIG_HYPERWISOR_HTTP_BASE_URL="https://your.api/functions/v1"
CONFIG_HYPERWISOR_WS_HOST="your.realtime.host"
CONFIG_HYPERWISOR_WS_PORT=443
```

Defaults point at the official Hyperwisor cloud
(`hyperwisor.nikolaindustry.workers.dev` / `nikolaindustry-realtime.onrender.com`).
Override these for self-hosting.

## Enabling OTA

1. In `sdkconfig.defaults`:
   ```
   CONFIG_HYPERWISOR_ENABLE_OTA=y
   CONFIG_PARTITION_TABLE_CUSTOM=y
   CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_16mb_ota.csv"
   ```
2. Copy the matching CSV from `partitions/` into your project root
   (4 MB / 8 MB / 16 MB variants are bundled).
3. Build and flash. The dashboard's "Push Update" button now works.

> **Security:** without Secure Boot V2, OTA is **unauthenticated**. Anyone
> who can reach your device's WS session can push arbitrary firmware.
> Enable Secure Boot V2 + Flash Encryption for production.

## Registering custom commands

```c
static void cmd_blink(const char *from, cJSON *payload)
{
    gpio_set_level(GPIO_NUM_2, 1);

    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "status", "ok");
    hyperwisor_send_response(from, reply);
    cJSON_Delete(reply);
}

hyperwisor_register_cmd_handler("BLINK", cmd_blink);
```

## Board port (optional)

Provide a `hyperwisor_port_t` struct of function pointers to integrate
with a specific board's GPIO whitelist, display backlight, or sensor APIs.
See [`hyperwisor_port.h`](hyperwisor_port.h) for the full interface.

## Project links

* Cloud dashboard: <https://hyperwisor.nikolaindustry.workers.dev>
* Issue tracker: <https://github.com/nikolaindustry/hyperwisor-esp-idf/issues>
* Changelog: see [CHANGELOG.md](CHANGELOG.md)

## License

Apache License 2.0 — see [LICENSE](LICENSE).
