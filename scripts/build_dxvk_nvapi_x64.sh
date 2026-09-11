#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "Uso: $0 <directorio-dxvk-nvapi> <salida> [build-type]" >&2
  exit 2
fi

SOURCE_DIR="$(cd "$1" && pwd)"
OUTPUT_DIR="$(mkdir -p "$2" && cd "$2" && pwd)"
BUILD_TYPE="${3:-release}"
BUILD_DIR="${OUTPUT_DIR}/build64"

for required in "${SOURCE_DIR}/meson.build" \
    "${SOURCE_DIR}/build-win64.txt"; do
  if [[ ! -e "${required}" ]]; then
    echo "Falta artefacto DXVK-NVAPI: ${required}" >&2
    exit 2
  fi
done
command -v meson >/dev/null || { echo "No se encontró meson en PATH" >&2; exit 2; }
command -v ninja >/dev/null || { echo "No se encontró ninja en PATH" >&2; exit 2; }

if [[ -e "${BUILD_DIR}/build.ninja" ]]; then
  meson setup --reconfigure \
    --cross-file "${SOURCE_DIR}/build-win64.txt" \
    --buildtype "${BUILD_TYPE}" \
    --prefix "${OUTPUT_DIR}" \
    --strip \
    --bindir x64 \
    --libdir x64 \
    -Denable_tests=false \
    "${BUILD_DIR}" "${SOURCE_DIR}"
else
  meson setup \
    --cross-file "${SOURCE_DIR}/build-win64.txt" \
    --buildtype "${BUILD_TYPE}" \
    --prefix "${OUTPUT_DIR}" \
    --strip \
    --bindir x64 \
    --libdir x64 \
    -Denable_tests=false \
    "${BUILD_DIR}" "${SOURCE_DIR}"
fi
ninja -C "${BUILD_DIR}" install

for required in "${OUTPUT_DIR}/x64/nvapi64.dll" \
    "${OUTPUT_DIR}/x64/nvofapi64.dll"; do
  if [[ ! -f "${required}" ]]; then
    echo "El build no produjo: ${required}" >&2
    exit 1
  fi
done

printf '%s\n' "${OUTPUT_DIR}/x64"
