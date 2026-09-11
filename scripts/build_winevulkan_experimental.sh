#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_DIR="${WINE_SOURCE_DIR:-/tmp/dlss5-wine}"
BUILD_DIR="${WINE_BUILD_DIR:-/tmp/dlss5-wine-build}"
PATCH_FILES=(
  "${ROOT_DIR}/patches/winevulkan-expose-external-semaphore-fd.patch"
  "${ROOT_DIR}/patches/winevulkan-expose-external-memory-fd.patch"
  "${ROOT_DIR}/patches/wine-win32u-import-memory-fd.patch"
)

if [[ ! -f "${SOURCE_DIR}/dlls/winevulkan/make_vulkan" ]]; then
  echo "WINE_SOURCE_DIR no apunta a un checkout de Wine: ${SOURCE_DIR}" >&2
  exit 2
fi

for patch_file in "${PATCH_FILES[@]}"; do
  if git -C "${SOURCE_DIR}" apply --check "${patch_file}" >/dev/null 2>&1; then
    git -C "${SOURCE_DIR}" apply "${patch_file}"
  elif [[ "${patch_file}" == *semaphore-fd.patch ]] &&
      ! rg -q '^[[:space:]]*"VK_KHR_external_semaphore_fd",' \
      "${SOURCE_DIR}/dlls/winevulkan/make_vulkan"; then
    echo "El filtro VK_KHR_external_semaphore_fd ya está removido; se continúa." >&2
  elif [[ "${patch_file}" == *winevulkan-expose-external-memory-fd.patch ]] &&
      ! rg -q '^[[:space:]]*"VK_KHR_external_memory_fd",' \
      "${SOURCE_DIR}/dlls/winevulkan/make_vulkan"; then
    echo "El filtro VK_KHR_external_memory_fd ya está removido; se continúa." >&2
  elif [[ "${patch_file}" == *win32u-import-memory-fd.patch ]] &&
      rg -q 'case VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR:[[:space:]]*break;' \
      "${SOURCE_DIR}/dlls/win32u/vulkan.c"; then
    echo "win32u ya acepta VkImportMemoryFdInfoKHR; se continúa." >&2
  else
    echo "No se pudo aplicar el parche de winevulkan: ${patch_file}" >&2
    exit 3
  fi
done

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
