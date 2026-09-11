#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_DIR="${WINE_SOURCE_DIR:-/tmp/dlss5-wine}"
BUILD_DIR="${WINE_BUILD_DIR:-/tmp/dlss5-wine-build}"
PATCH_FILE="${ROOT_DIR}/patches/winevulkan-expose-external-semaphore-fd.patch"

if [[ ! -f "${SOURCE_DIR}/dlls/winevulkan/make_vulkan" ]]; then
  echo "WINE_SOURCE_DIR no apunta a un checkout de Wine: ${SOURCE_DIR}" >&2
  exit 2
fi

if git -C "${SOURCE_DIR}" apply --check "${PATCH_FILE}" >/dev/null 2>&1; then
  git -C "${SOURCE_DIR}" apply "${PATCH_FILE}"
elif rg -q '^[[:space:]]*"VK_KHR_external_semaphore_fd",' \
    "${SOURCE_DIR}/dlls/winevulkan/make_vulkan"; then
  echo "No se pudo aplicar el parche de winevulkan." >&2
  exit 3
else
  echo "El filtro VK_KHR_external_semaphore_fd ya está removido; se continúa." >&2
fi

python3 "${SOURCE_DIR}/dlls/winevulkan/make_vulkan"

if [[ ! -f "${BUILD_DIR}/Makefile" ]]; then
  mkdir -p "${BUILD_DIR}"
  (
    cd "${BUILD_DIR}"
    "${SOURCE_DIR}/configure" --enable-win64 --disable-tests --without-x
  )
fi

make -C "${BUILD_DIR}/dlls/winevulkan" -j"${JOBS:-2}" \
  x86_64-windows/winevulkan.dll winevulkan.so

echo "PE:   ${BUILD_DIR}/dlls/winevulkan/x86_64-windows/winevulkan.dll"
echo "Unix: ${BUILD_DIR}/dlls/winevulkan/winevulkan.so"
echo "Nota: GE-Proton debe cargar ambos artefactos desde un loader compatible."
