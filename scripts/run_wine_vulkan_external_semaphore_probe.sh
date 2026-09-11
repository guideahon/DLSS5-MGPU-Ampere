#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WINE_BUILD_DIR="${WINE_BUILD_DIR:-/tmp/dlss5-wine-build2}"
WINE_PREFIX="${WINE_PREFIX:-/tmp/dlss5-full-wine-prefix}"
WINE_LOADER="${WINE_LOADER:-${WINE_BUILD_DIR}/loader/wine}"
WINE_SERVER="${WINE_SERVER:-${WINE_BUILD_DIR}/server/wineserver}"
WINE_VULKAN_LIB="${WINE_VULKAN_LIB:-/usr/lib/x86_64-linux-gnu/wine/x86_64-windows/libvulkan-1.a}"
MINGW_CXX="${MINGW_CXX:-x86_64-w64-mingw32-g++}"
PROBE_EXE="${WINE_VULKAN_PROBE_EXE:-${TMPDIR:-/tmp}/dlss5-wine-vulkan-external-semaphore-probe.exe}"

for required in "${WINE_LOADER}" "${WINE_SERVER}" "${WINE_VULKAN_LIB}"; do
  if [[ ! -e "${required}" ]]; then
    echo "Falta artefacto Wine/Vulkan: ${required}" >&2
    exit 2
  fi
done

"${MINGW_CXX}" -O2 -std=c++17 -static-libgcc -static-libstdc++ \
  -idirafter /usr/include "${ROOT_DIR}/tests/wine_vulkan_external_semaphore_probe.cpp" \
  -o "${PROBE_EXE}" "${WINE_VULKAN_LIB}"

mkdir -p "${WINE_PREFIX}"
export WINEPREFIX="${WINE_PREFIX}"
export WINEARCH="win64"
export WINESERVER="${WINE_SERVER}"
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEDLLPATH="${WINEDLLPATH:-${WINE_BUILD_DIR}/dlls}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-${WINE_BUILD_DIR}/dlls/winevulkan:${WINE_BUILD_DIR}/dlls/ntdll:${WINE_BUILD_DIR}/dlls/win32u:${WINE_BUILD_DIR}/dlls/unixlib:${WINE_BUILD_DIR}/libs/wine}"

"${WINE_LOADER}" "${PROBE_EXE}"
