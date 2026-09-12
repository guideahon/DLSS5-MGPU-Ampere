#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_DIR="${WINE_SOURCE_DIR:-/tmp/dlss5-wine}"
BUILD_DIR="${WINE_BUILD_DIR:-/tmp/dlss5-wine-build}"
JOBS="${JOBS:-8}"

PATCH_FILES=(
  "${ROOT_DIR}/patches/wine-linux-fd-semaphore.patch"
  "${ROOT_DIR}/patches/winevulkan-expose-external-memory-fd.patch"
  "${ROOT_DIR}/patches/winevulkan-expose-external-semaphore-fd.patch"
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
    echo "Filtro VK_KHR_external_semaphore_fd ya removido; se continúa." >&2
  elif [[ "${patch_file}" == *expose-external-memory-fd.patch ]] &&
       ! rg -q '^[[:space:]]*"VK_KHR_external_memory_fd",' \
       "${SOURCE_DIR}/dlls/winevulkan/make_vulkan"; then
    echo "Filtro VK_KHR_external_memory_fd ya removido; se continúa." >&2
  elif [[ "${patch_file}" == *win32u-import-memory-fd.patch ]] &&
       rg -q 'case VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR:[[:space:]]*break;' \
       "${SOURCE_DIR}/dlls/win32u/vulkan.c"; then
    echo "win32u ya acepta VkImportMemoryFdInfoKHR; se continúa." >&2
  elif [[ "${patch_file}" == *wine-linux-fd-semaphore.patch ]] &&
       ! rg -q '^[[:space:]]*"VK_KHR_external_semaphore_fd",' \
       "${SOURCE_DIR}/dlls/winevulkan/make_vulkan" &&
       rg -q 'VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT' \
       "${SOURCE_DIR}/dlls/win32u/vulkan.c"; then
    echo "El parche de semáforos FD de Wine ya está aplicado; se continúa." >&2
  else
    echo "No se pudo aplicar el parche: ${patch_file}" >&2
    exit 3
  fi
done

python3 "${SOURCE_DIR}/dlls/winevulkan/make_vulkan"

if [[ ! -f "${BUILD_DIR}/Makefile" ]]; then
  mkdir -p "${BUILD_DIR}"
  configure_args=(--enable-win64 --disable-tests)
  if [[ "${WINE_WITHOUT_X:-0}" == "1" ]]; then
    configure_args+=(--without-x)
  fi
  (
    cd "${BUILD_DIR}"
    "${SOURCE_DIR}/configure" "${configure_args[@]}"
  )
fi

# El runtime debe construirse completo. Un build selectivo puede generar un
# ntdll/loader no autoconsistente y fallar antes de cargar Vulkan.
make -C "${BUILD_DIR}" -j"${JOBS}"

required=(
  "${BUILD_DIR}/loader/wine"
  "${BUILD_DIR}/server/wineserver"
  "${BUILD_DIR}/dlls/winevulkan/x86_64-windows/winevulkan.dll"
  "${BUILD_DIR}/dlls/winevulkan/winevulkan.so"
  "${BUILD_DIR}/dlls/win32u/x86_64-windows/win32u.dll"
  "${BUILD_DIR}/dlls/win32u/win32u.so"
  "${BUILD_DIR}/dlls/cryptbase/x86_64-windows/cryptbase.dll"
)
if [[ "${WINE_WITHOUT_X:-0}" != "1" ]]; then
  required+=(
    "${BUILD_DIR}/dlls/winex11.drv/x86_64-windows/winex11.drv"
    "${BUILD_DIR}/dlls/winex11.drv/winex11.so"
  )
fi
for artifact in "${required[@]}"; do
  [[ -f "${artifact}" ]] || {
    echo "Falta artefacto del runtime Wine: ${artifact}" >&2
    exit 4
  }
done

echo "Runtime Wine experimental completo en: ${BUILD_DIR}"
echo "Loader: ${BUILD_DIR}/loader/wine"
echo "Server: ${BUILD_DIR}/server/wineserver"
