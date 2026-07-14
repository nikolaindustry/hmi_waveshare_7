# Changelog

All notable changes to the Hyperwisor IoT component will be documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [0.2.0] — 2026-07-03

### Added — HSC v1 (Hyperwisor Secure Channel)
Opt-in cryptographic device authentication so devices can't be impersonated or
hijacked. Existing firmware is unaffected unless it calls `hyperwisor_enable_security()`.

- `hyperwisor_hsc.{c,h}`: on-chip P-256 keypair (mbedTLS) generated at first boot
  and stored in NVS — the private key never leaves the device.
  `hyperwisor_hsc_get_public_key_b64()`, `hyperwisor_hsc_sign()`, and
  `hyperwisor_hsc_register()` (registers the public key via `relay-register-device`).
- WebSocket transport does the HSC challenge–response on connect and only reports
  connected after `auth_ok`.
- `hyperwisor_enable_security()` + `hyperwisor_get_public_key_b64()` in core.
- Point the device at your secured relay via `CONFIG_HYPERWISOR_WS_HOST`.
- `secure_hyperwisor` example.

## [0.1.0] — 2026-05-03

Initial public release. Extracted from the `hyperwisor_s3` reference project
into a standalone, registry-publishable ESP-IDF component.

### Added
- Umbrella header `hyperwisor.h` and Kconfig feature gates
  (`SYSTEM`, `NTP`, `HTTP`, `WIDGET`, `OTA`, `GPIO`, `DISPLAY`).
- WiFi STA + SoftAP fallback (`NIKOLAINDUSTRY_Setup-XXXX`).
- Secure WebSocket transport with cJSON command framing.
- Built-in commands: `SYSTEM`, `DEVICE_STATUS`, `OTA_UPDATE`, `GPIO_MANAGEMENT`.
- Board-port abstraction (`hyperwisor_port_t`) for GPIO whitelist + display.
- Bundled OTA partition CSVs for 4/8/16 MB flash.
- `hello_hyperwisor` example.

### Reliability
- WebSocket client is stopped during HTTPS OTA download to avoid
  TLS-stack contention; a `s_ota_in_progress` flag prevents the core
  task from racing the disconnect.
- OTA download progress is logged locally only (no WS sends mid-download).
- Failures are persisted in NVS (`ota_failed_reason`) and reported to
  the cloud once WebSocket reconnects.

### Targets
- ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6, ESP32-P4
- IDF `>= 5.5, < 7.0`
