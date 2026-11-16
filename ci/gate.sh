#!/usr/bin/env bash
# Gate do Core em Linux/WSL (motor + dominio, sem a app JUCE): configura, compila, corre os testes "merge" e escreve
# o resumo (ci/resumo.cmake: ramo, SHA, contagem por label, duracoes, inventario contra ci/testes.txt e main).
#   ci/gate.sh [build-dir]            build incremental
#   CLEAN=1 ci/gate.sh [build-dir]    apaga a pasta de build antes (build limpa: a que conta para uma entrega)
# Codigo de saida: 0 = VERDE. O resumo fica em <build>/gate-resumo.txt.
set -uo pipefail
src="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$src/../core-build-gate}"
clean="nao"
if [ "${CLEAN:-0}" = "1" ]; then
  if [ -d "$build" ] && [ -n "$(ls -A "$build" 2>/dev/null)" ] && [ ! -f "$build/CMakeCache.txt" ]; then
    echo "gate: $build nao e uma pasta de build (sem CMakeCache.txt); nao apago" >&2; exit 1
  fi
  rm -rf "$build"; clean="sim"
elif [ ! -f "$build/CMakeCache.txt" ]; then
  clean="sim"
fi
t0=$(date +%s)
rc=0
cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCORE_BUILD_APP=OFF || rc=1
[ $rc -eq 0 ] && { cmake --build "$build" --parallel "$(nproc)" || rc=1; }
ctest_rc=1
if [ $rc -eq 0 ]; then
  rm -f "$build/gate-junit.xml"
  ctest --test-dir "$build" -L merge --no-tests=error --output-on-failure --timeout 600 --output-junit "$build/gate-junit.xml"
  ctest_rc=$?
fi
secs=$(( $(date +%s) - t0 ))
cmake -DSRC="$src" -DBUILD="$build" -DJUNIT="$build/gate-junit.xml" -DCTEST_RC=$ctest_rc -DSECS=$secs -DCLEAN=$clean \
      -DAPP=OFF -DBASE="${BASE:-main}" -DOUT="$build/gate-resumo.txt" -P "$src/ci/resumo.cmake" || rc=1
[ $ctest_rc -eq 0 ] || rc=1
exit $rc
