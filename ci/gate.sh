#!/usr/bin/env bash
# Gate do Core em Linux/WSL (motor + dominio, sem a app JUCE): configura, compila e corre os testes "merge".
#   ci/gate.sh [build-dir]
set -euo pipefail
src="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$src/../core-build-gate}"
cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCORE_BUILD_APP=OFF
cmake --build "$build" --parallel "$(nproc)"
ctest --test-dir "$build" -L merge --output-on-failure --timeout 600
