#!/usr/bin/env bash
# Build and launch the desktop HMI simulator.
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S sim -B sim/build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build sim/build -j"$(sysctl -n hw.ncpu)"
exec ./sim/build/hmi_sim
