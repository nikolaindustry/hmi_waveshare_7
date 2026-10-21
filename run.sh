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

# ---- ESP-IDF v5.5 -------------------------------------------------------
# This project must build on ESP-IDF 5.x. The Hyperwisor HSC v1 security
# module uses the legacy mbedTLS 3.x ECC API (ecdsa.h/ecp.h), removed in
# mbedTLS 4.x (shipped with ESP-IDF 6.0). Do not switch to 6.x without
# first porting hyperwisor_hsc.c to the PSA crypto API.
IDF_ACTIVATE="$HOME/.espressif/tools/activate_idf_v5.5.sh"
if [ ! -f "$IDF_ACTIVATE" ]; then
    echo "ERROR: ESP-IDF 5.5 not found at $IDF_ACTIVATE" >&2
    echo "       Re-run the Espressif Installation Manager (eim.app)." >&2
    exit 1
fi

# The activation script cannot be sourced from a script FILE. It decides
# whether it was sourced by testing $0 against shell names (bash/sh/...),
# and here $0 is this script, so it prints "should be sourced, not
# executed" and calls `exit` -- which, being sourced, takes us down with
# it. That is silent, because its output is normally redirected.
#
# Its -e mode prints the same environment as plain KEY=VALUE lines, so
# import those instead. SYSTEM_PATH is the PATH it captured at install
# time; ours is fresher, so IDF's PATH is prepended to the live one.
idf_import_env() {
    local line idf_path=""
    while IFS= read -r line; do
        case "$line" in
            PATH=*)        idf_path="${line#PATH=}" ;;
            SYSTEM_PATH=*) ;;
            *=*)           export "$line" ;;
        esac
    done < <("$IDF_ACTIVATE" -e)
    [ -n "$idf_path" ] || return 1
    export PATH="$idf_path:$PATH"
}

if ! idf_import_env; then
    echo "ERROR: could not read the ESP-IDF environment from" >&2
    echo "       $IDF_ACTIVATE -e" >&2
    exit 1
fi

# Normally idf is a shell function the activation script defines, so
# call the underlying script the same way it would.
if [ ! -x "${IDF_PYTHON_ENV_PATH:-}/bin/python" ]; then
    echo "ERROR: ESP-IDF python env missing at ${IDF_PYTHON_ENV_PATH:-unset}" >&2
    exit 1
fi
idf() { "$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py" "$@"; }

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
        idf build
        ;;
    flash)
        idf -p "$PORT" -b 460800 flash
        ;;
    monitor)
        idf -p "$PORT" monitor
        ;;
    all)
        idf -p "$PORT" -b 460800 flash monitor
        ;;
    *)
        echo "Unknown action: $ACTION (use: build | flash | monitor | all)" >&2
        exit 1
        ;;
esac
