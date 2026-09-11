#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_DIR="${VKD3D_SOURCE_DIR:-/tmp/dlss5-vkd3d-proton}"
BUILD_DIR="${VKD3D_BUILD_DIR:-/tmp/dlss5-vkd3d-build}"
INSTALL_DIR="${VKD3D_INSTALL_DIR:-/tmp/dlss5-vkd3d-install}"
PATCH_FILES=(
  "${ROOT_DIR}/patches/vkd3d-duplicate-luid-adapters.patch"
  "${ROOT_DIR}/patches/vkd3d-duplicate-luid-strict-identity.patch"
  "${ROOT_DIR}/patches/vkd3d-duplicate-luid-per-device.patch"
  "${ROOT_DIR}/patches/vkd3d-mingw-pathcch-compat.patch"
  "${ROOT_DIR}/patches/vkd3d-export-opaque-fd-memory.patch"
  "${ROOT_DIR}/patches/vkd3d-fd-diagnostics.patch"
  "${ROOT_DIR}/patches/vkd3d-export-heap-fd-spi.patch"
  "${ROOT_DIR}/patches/vkd3d-export-fence-fd-spi.patch"
  "${ROOT_DIR}/patches/vkd3d-fence-capability-diagnostics.patch"
  "${ROOT_DIR}/patches/vkd3d-export-resource-fd-spi.patch"
  "${ROOT_DIR}/patches/vkd3d-import-resource-fd-current.patch"
  "${ROOT_DIR}/patches/vkd3d-command-list-queue-spi.patch"
)

if [[ "${VKD3D_FENCE_ONLY:-0}" == "1" ]]; then
  PATCH_FILES=("${PATCH_FILES[@]:0:7}")
  echo "Modo fence-only: se omiten los parches de exportación/importación de recursos." >&2
fi

if [[ ! -e "${SOURCE_DIR}/.git" ]]; then
  echo "VKD3D_SOURCE_DIR no apunta a un checkout Git de VKD3D-Proton: ${SOURCE_DIR}" >&2
  exit 2
fi
for patch_file in "${PATCH_FILES[@]}"; do
  if [[ ! -f "${patch_file}" ]]; then
    echo "No se encontró el parche experimental: ${patch_file}" >&2
    exit 2
  fi

  if git -C "${SOURCE_DIR}" apply --check "${patch_file}" >/dev/null 2>&1; then
    git -C "${SOURCE_DIR}" apply "${patch_file}"
  elif [[ "${patch_file}" == *duplicate-luid-adapters.patch ]] &&
      ! git -C "${SOURCE_DIR}" diff --quiet -- libs/d3d12core/main.c; then
    echo "El parche de LUID ya está aplicado; se conserva y se continúa." >&2
  elif [[ "${patch_file}" == *duplicate-luid-strict-identity.patch ]] &&
      rg -q 'Could not select a distinct Vulkan physical device' \
        "${SOURCE_DIR}/libs/d3d12core/main.c"; then
    echo "El guard estricto de identidad física ya está aplicado; se conserva y se continúa." >&2
  elif [[ "${patch_file}" == *duplicate-luid-per-device.patch ]] &&
      rg -q 'VKD3D_DUPLICATE_LUID_INDEX_PER_DEVICE' \
        "${SOURCE_DIR}/libs/d3d12core/main.c"; then
    echo "El selector por dispositivo ya está aplicado; se conserva y se continúa." >&2
  elif [[ "${patch_file}" == *mingw-pathcch-compat.patch ]] &&
      rg -q '#define PATHCCH_NONE' "${SOURCE_DIR}/libs/vkd3d-common/platform.c"; then
    echo "La compatibilidad PATHCCH_NONE ya está aplicada; se conserva y se continúa." >&2
  elif [[ "${patch_file}" == *export-opaque-fd-memory.patch ]] &&
      ! git -C "${SOURCE_DIR}" diff --quiet -- libs/vkd3d/device.c libs/vkd3d/memory.c libs/vkd3d/vkd3d_private.h; then
    echo "El parche de memoria exportable ya está aplicado; se conserva y se continúa." >&2
  elif [[ "${patch_file}" == *fd-diagnostics.patch ]] &&
      ! git -C "${SOURCE_DIR}" diff --quiet -- libs/vkd3d/memory.c libs/vkd3d/vulkan_procs.h; then
    echo "El parche de diagnóstico FD ya está aplicado; se conserva y se continúa." >&2
  elif [[ "${patch_file}" == *export-heap-fd-spi.patch ]] &&
      rg -q 'ID3D12DXVKInteropDevice4|ExportVulkanHeapFd' \
        "${SOURCE_DIR}/include/vkd3d_device_vkd3d_ext.idl" \
        "${SOURCE_DIR}/libs/vkd3d/device_vkd3d_ext.c"; then
    echo "La SPI de exportación de heap ya está aplicada; se conserva y se continúa." >&2
  elif [[ "${patch_file}" == *export-fence-fd-spi.patch ]] &&
      rg -q 'ID3D12DXVKInteropDevice5|ExportVulkanFenceFd|GetVulkanPhysicalDeviceIdentity' \
        "${SOURCE_DIR}/include/vkd3d_device_vkd3d_ext.idl" \
        "${SOURCE_DIR}/libs/vkd3d/device_vkd3d_ext.c"; then
    echo "La SPI de fence/identidad ya está aplicada; se conserva y se continúa." >&2
  elif [[ "${patch_file}" == *fence-capability-diagnostics.patch ]] &&
      rg -q 'KHR_external_semaphore_fd|fence semaphore capability' \
        "${SOURCE_DIR}/libs/vkd3d/vkd3d_private.h" \
        "${SOURCE_DIR}/libs/vkd3d/command.c"; then
    echo "El diagnóstico de capacidad de fence ya está aplicado; se conserva y se continúa." >&2
  elif [[ "${patch_file}" == *export-resource-fd-spi.patch ]] &&
      rg -q 'ID3D12DXVKInteropDevice6|ExportVulkanResourceFd|VKD3D_EXPORT_RESOURCE_FD' \
        "${SOURCE_DIR}/include/vkd3d_device_vkd3d_ext.idl" \
        "${SOURCE_DIR}/libs/vkd3d/device_vkd3d_ext.c" \
        "${SOURCE_DIR}/libs/vkd3d/memory.c"; then
    echo "La SPI de exportación de recursos ya está aplicada; se conserva y se continúa." >&2
  elif [[ "${patch_file}" == *import-resource-fd-current.patch ]] &&
      rg -q 'ID3D12DeviceExt6|CreateResourceFromExternalFd|VKD3D_IMPORT_RESOURCE_FD' \
        "${SOURCE_DIR}/include/vkd3d_device_vkd3d_ext.idl" \
        "${SOURCE_DIR}/libs/vkd3d/device_vkd3d_ext.c" \
        "${SOURCE_DIR}/libs/vkd3d/resource.c"; then
    echo "El runtime actual de importación FD ya está aplicado; se conserva y se continúa." >&2
  elif [[ "${patch_file}" == *command-list-queue-spi.patch ]] &&
      rg -q 'ID3D12DXVKInteropDevice7|GetCommandListQueue|last_submit_queue' \
        "${SOURCE_DIR}/include/vkd3d_device_vkd3d_ext.idl" \
        "${SOURCE_DIR}/libs/vkd3d/device_vkd3d_ext.c" \
        "${SOURCE_DIR}/libs/vkd3d/vkd3d_private.h"; then
    echo "La SPI de queue asociada al command list ya está aplicada; se conserva y se continúa." >&2
  else
    echo "No se pudo aplicar el parche experimental al checkout de VKD3D: ${patch_file}" >&2
    exit 3
  fi
done

if [[ ! -f "${SOURCE_DIR}/khronos/Vulkan-Headers/include/vulkan/vulkan.h" ]]; then
  echo "Inicializando submódulos Vulkan/DXIL de VKD3D-Proton..." >&2
  git -C "${SOURCE_DIR}" submodule update --init --recursive
fi

cd "${SOURCE_DIR}"
meson setup --reconfigure --cross-file build-win64.txt \
  --buildtype release --prefix "${INSTALL_DIR}" "${BUILD_DIR}"
ninja -C "${BUILD_DIR}" -j"${JOBS:-2}"
ninja -C "${BUILD_DIR}" install

echo "Build experimental instalado en: ${INSTALL_DIR}/bin"
echo "Activación opt-in: VKD3D_DUPLICATE_LUID_ADAPTERS=1"
echo "Memoria FD opt-in: VKD3D_EXPORT_OPAQUE_FD_MEMORY=1"
echo "SPI heap FD opt-in: VKD3D_EXPORT_HEAP_FD=1"
echo "SPI fence FD opt-in: VKD3D_EXPORT_FENCE_FD=1"
