# Hyperwisor S3 — Waveshare ESP32-S3-Touch-LCD-7

ESP-IDF firmware for the **Waveshare ESP32-S3-Touch-LCD-7** board, providing a touch HMI with cloud connectivity, Modbus RTU master, HVAC control, and LVGL-based UI.

## Hardware

| Item | Value |
|---|---|
| MCU | ESP32-S3-N16R8 (16 MB flash, 8 MB PSRAM octal) |
| Display | 7″ 800×480 RGB LCD |
| Touch | GT911 capacitive (I²C) |
| IO expander | CH422G (backlight, DISP, TP RST, SD CS) |
| RS-485 | Auto-direction transceiver on UART1 (GPIO16 TX / GPIO15 RX) |
| Sensor | BMP280 on shared I²C header (GPIO8 SDA / GPIO9 SCL) |

## Prerequisites

1. **ESP-IDF v6.0.x** (or v5.5+ — see `main/idf_component.yml` for the exact range).
2. Python 3.11+ with the ESP-IDF toolchain installed (`install.bat` / `export.bat`).
3. A USB-C cable connected to the **native USB-JTAG-SERIAL** port (GPIO19/20) for flashing and serial monitor.

> **Board switch note:** DIP switch #15 must be in the **UART2** position for the RS-485 transceiver to work. This disconnects the CH340 USB-UART bridge, so flashing is only possible via the native USB port.

## Clone & Build

```bash
git clone https://github.com/nikolaindustry/hmi_waveshare_7.git
cd hmi_waveshare_7

# Set up ESP-IDF environment (adjust path to your installation)
# Windows (PowerShell):
. C:\esp\v6.0.1\esp-idf\export.ps1
# Linux / macOS:
. $HOME/esp/v6.0.1/esp-idf/install.sh && . $HOME/esp/v6.0.1/esp-idf/export.sh

# Build
idf.py build
```

### First-time managed components

On the first build, ESP-IDF automatically downloads the managed components listed in `main/idf_component.yml` and `components/app/idf_component.yml` (cJSON, esp_websocket_client, etc.) into `managed_components/`. This directory is gitignored.

## Flash

```bash
# Detect your serial port (common: COM4 on Windows, /dev/ttyACM0 on Linux)
idf.py -p COM4 flash
```

Or use the included PowerShell helper that builds, flashes, and captures 35 s of serial log:

```powershell
.\_run.ps1
```

> Edit `_run.ps1` line 4 (`Set-Location`) and line 17 (`-p COM4`) if your port or project path differs.

## Monitor

```bash
idf.py -p COM4 monitor
# Ctrl+] to quit
```

## Project Structure

```
├── main/                     # app_main() — boot sequence, role branching
├── components/
│   ├── app/                  # Application layer
│   │   ├── ui/               # LVGL UI tabs (overview, control, climate, entertainment, maintenance)
│   │   ├── modbus/           # Modbus RTU master + slave server
│   │   ├── hvac/             # HVAC controller (coil-driven relay logic)
│   │   ├── sensor/           # BMP280 temperature / pressure
│   │   ├── hmi_role/         # PRIMARY / SECONDARY role (NVS-persisted)
│   │   ├── hmi_sync/         # Cross-HMI state sync (Modbus mirror)
│   │   ├── bus_config/       # RS-485 baud rate (NVS-persisted)
│   │   └── cloud/            # Hyperwisor IoT cloud command handlers
│   ├── bsp_s3_rgb/           # Board support: I2C, CH422G, RGB panel, GT911, LVGL port
│   ├── hyperwisor/           # Hyperwisor IoT core: WiFi, WebSocket, NVS, NTP, commands
│   ├── esp_lcd_touch/        # ESP LCD touch abstraction
│   ├── esp_lcd_touch_gt911/  # GT911 driver
│   └── lvgl/                 # LVGL v8.3 (vendor copy with Kconfig)
├── slave_rgb_arduino/        # Arduino sketch for the ATmega RGB slave (addr 0x20)
├── assets/                   # Source images converted to C arrays
├── scripts/                  # Logo conversion utility
├── partitions.csv            # 16 MB partition table (4 MB app, 2 MB SPIFFS)
└── sdkconfig.defaults        # Baseline Kconfig overrides
```

## Key Features

| Feature | Details |
|---|---|
| **LVGL Touch UI** | 5 tabs: Overview, Control, Climate, Entertainment, Maintenance |
| **Cloud (Hyperwisor IoT)** | TLS WebSocket to `nikolaindustry-realtime.onrender.com`; handles `DEVICE_STATUS`, `rgb_control` commands |
| **Display Brightness** | Software dim via `lv_layer_sys()` overlay (5–100% slider in Maintenance tab); NVS-persisted |
| **Modbus RTU Master** | Polls relay boards + RGB slave at 9600–115200 baud (user-configurable) |
| **HVAC Controller** | Auto mode drives coils 8–15 based on BMP280 readings and thresholds |
| **RGB Zones** | Roof & Floor colour control via ATmega slave (Modbus registers 0–5 / 6–11) |
| **Role System** | PRIMARY (bus master + cloud) or SECONDARY (Modbus slave 0x10, headless) |
| **Theme Engine** | Multiple colour presets, NVS-persisted, applied on boot |

## Configuration

All persistent settings are managed through the **Maintenance** tab on the HMI itself — no re-flashing required:

- **Owner Info** — display name
- **Reading Lights / Switches** — custom tile labels
- **HMI Role** — PRIMARY or SECONDARY
- **Bus Setup** — RS-485 baud rate (reprograms slaves automatically)
- **Theme** — colour preset
- **Display** — brightness slider (5–100%)
- **Cloud** — WiFi credentials, device/user ID binding

## NVS Keys

The firmware uses two NVS namespaces:

| Namespace | Keys | Purpose |
|---|---|---|
| `cfg` | `owner`, `ssid`, `pass`, `hmi_role`, `bus_baud`, `theme`, tile names | General device config |
| `disp` | `bri` | Display brightness (0–100, default 100) |

The Hyperwisor IoT core also uses the `hyperwisor` namespace for cloud credentials (`deviceid`, `userid`, `email`, `productid`, `apikey`, `secretkey`, `firmware`).

## License

Proprietary — NikolaIndustry Pvt. Ltd.
