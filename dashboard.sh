#!/usr/bin/env bash
# Put a Hyperwisor dashboard export on the HMI.
#
#   ./dashboard.sh "La Vitis.json"          convert, build, flash
#   ./dashboard.sh "La Vitis.json" --sim    convert, open it on this Mac
#   ./dashboard.sh "La Vitis.json" --build  convert and build, don't flash
#   ./dashboard.sh --sim                    re-open the last converted one
#
#   PORT=/dev/cu.xxxx ./dashboard.sh x.json     override the detected port
#
# Converting rewrites assets/dashboard/, which is flashed to the
# `storage` partition alongside the app. With a dashboard there the
# panel renders it; with the directory emptied, the normal shell UI
# comes back.

set -euo pipefail
cd "$(dirname "$0")"

OUT="assets/dashboard"
INPUT=""
MODE="flash"

for arg in "$@"; do
    case "$arg" in
        --sim)   MODE="sim" ;;
        --build) MODE="build" ;;
        --help|-h) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "unknown option: $arg" >&2; exit 2 ;;
        *)  INPUT="$arg" ;;
    esac
done

# ---------------------------------------------------------------- convert
if [ -n "$INPUT" ]; then
    if [ ! -f "$INPUT" ]; then
        echo "ERROR: no such file: $INPUT" >&2
        exit 1
    fi
    echo "==> converting $(basename "$INPUT")"
    python3 -I tools/hds_transcode.py "$INPUT" -o "$OUT"
    echo
elif [ ! -f "$OUT/dashboard.json" ]; then
    echo "ERROR: no dashboard yet. Pass a JSON export:" >&2
    echo "         ./dashboard.sh my-dashboard.json" >&2
    exit 1
fi

# ------------------------------------------------------------------- sim
if [ "$MODE" = "sim" ]; then
    echo "==> building simulator"
    cmake -S sim -B sim/build -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build sim/build -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" >/dev/null
    echo "==> opening (H outlines hit areas, R reloads, 1-9 pages, ESC quits)"
    exec ./sim/build/hmi_sim --dashboard
fi

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

# Normally idf.py is a shell function the activation script defines, so
# call the underlying script the same way it would.
if [ ! -x "${IDF_PYTHON_ENV_PATH:-}/bin/python" ]; then
    echo "ERROR: ESP-IDF python env missing at ${IDF_PYTHON_ENV_PATH:-unset}" >&2
    exit 1
fi
idf() { "$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py" "$@"; }

echo "==> building firmware"
idf build

if [ "$MODE" = "build" ]; then
    echo
    echo "Built. Flash it with: ./dashboard.sh"
    exit 0
fi

# ----------------------------------------------------------------- flash
if [ -z "${PORT:-}" ]; then
    PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
fi
if [ -z "${PORT:-}" ]; then
    echo "ERROR: no board found on /dev/cu.usbmodem*." >&2
    echo "       Plug into the native USB-C port (DIP #15 in UART2)." >&2
    exit 1
fi

echo
echo "==> flashing $PORT"
idf -p "$PORT" -b 460800 flash

echo
echo "Done. The panel is showing your dashboard."
