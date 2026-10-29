#!/usr/bin/env bash
# Build + flash + monitor helper for the Waveshare ESP32-S3-Touch-LCD-7 (macOS/Linux).
#
# Usage:
#   ./run.sh              build, flash, then open serial monitor
#   ./run.sh build        build only
#   ./run.sh flash        build + flash (no monitor)
#   ./run.sh monitor      open serial monitor only
#   PORT=/dev/cu.xxxx ./run.sh   override the auto-detected port
#
# Ctrl+] quits the monitor.

set -euo pipefail

# ---- Activate ESP-IDF v5.5 (installed via Espressif Installation Manager) ----
# NOTE: this project must build on ESP-IDF 5.x. The Hyperwisor HSC v1 security
# module uses the legacy mbedTLS 3.x ECC API (ecdsa.h/ecp.h), which was removed
# in mbedTLS 4.x (shipped with ESP-IDF 6.0). Do not switch to 6.x without first
# porting hyperwisor_hsc.c to the PSA crypto API.
IDF_ACTIVATE="$HOME/.espressif/tools/activate_idf_v5.5.sh"
if [ ! -f "$IDF_ACTIVATE" ]; then
    echo "ERROR: ESP-IDF activation script not found at $IDF_ACTIVATE" >&2
    echo "Re-run the Espressif Installation Manager (eim.app) or adjust this path." >&2
    exit 1
fi
# shellcheck disable=SC1090
. "$IDF_ACTIVATE" >/dev/null 2>&1

cd "$(dirname "$0")"

# ---- Auto-detect the board's serial port unless PORT is set ----
if [ -z "${PORT:-}" ]; then
    PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
fi
if [ -z "${PORT:-}" ]; then
    echo "WARNING: no /dev/cu.usbmodem* port found. Plug in the board's native USB-C port." >&2
    echo "         (DIP switch #15 must be in the UART2 position — flashing is via native USB only.)" >&2
fi

ACTION="${1:-all}"

case "$ACTION" in
    build)
        idf.py build
        ;;
    flash)
        idf.py -p "$PORT" -b 460800 flash
        ;;
    monitor)
        idf.py -p "$PORT" monitor
        ;;
    all)
        idf.py -p "$PORT" -b 460800 flash monitor
        ;;
    *)
        echo "Unknown action: $ACTION (use: build | flash | monitor | all)" >&2
        exit 1
        ;;
esac
