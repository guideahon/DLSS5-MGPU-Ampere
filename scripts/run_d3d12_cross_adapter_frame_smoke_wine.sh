#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WINE_BUILD_DIR="${WINE_BUILD_DIR:-/tmp/dlss5-wine-build-fence}"
VKD3D_DLL_DIR="${VKD3D_DLL_DIR:-/tmp/dlss5-vkd3d-install-resource-gpu/bin}"
WINE_PREFIX="${WINEPREFIX:-/tmp/dlss5-frame-smoke-wine-prefix}"
OUT_DIR="${OUT_DIR:-/tmp/dlss5-frame-smoke-wine-out}"
WINE_LOADER="${WINE_LOADER:-${WINE_BUILD_DIR}/loader/wine}"
WINE_SERVER="${WINE_SERVER:-${WINE_BUILD_DIR}/server/wineserver}"
SHIM="${MGPU_FD_INHERIT_SHIM:-${ROOT_DIR}/build/libmgpu_fd_inherit_shim.so}"
COPY_HELPER="${MGPU_CUDA_P2P_COPY_HELPER:-${ROOT_DIR}/build/mgpu-cuda-external-p2p-copy-helper}"
FENCED_HELPER="${MGPU_FENCED_P2P_HELPER:-${ROOT_DIR}/build/cuda_external_fenced_p2p_helper}"
SDK_DIR="${NGX_SDK_DIR:-/home/cristian/.cache/dlss5-sdk/DLSS}"
GPU_NATIVE="${MGPU_CROSS_ADAPTER_GPU_NATIVE:-0}"
GPU_NATIVE_FRAMES="${MGPU_CROSS_ADAPTER_GPU_NATIVE_FRAMES:-3}"
REVERSE="${MGPU_CROSS_ADAPTER_REVERSE:-0}"
NGX_MODE="${MGPU_NGX_CROSS_ADAPTER:-0}"
NGX_BRIDGE_DIR="${NGX_BRIDGE_DIR:-${ROOT_DIR}/build/proton}"
NGX_PROXY_DLL="${MGPU_NGX_PROXY_DLL:-${NGX_BRIDGE_DIR}/_nvngx.dll}"
NGX_BRIDGE_DLL="${MGPU_NGX_BRIDGE_DLL:-${NGX_BRIDGE_DIR}/bridge-nvngx.dll}"
NGX_CORE_DLL="${MGPU_NGX_CORE_DLL:-${NGX_BRIDGE_DIR}/_nvngx_real.dll}"
NGX_RUNTIME_DLL="${DLSS_RUNTIME_DLL:-${SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll}"
NGX_NR_DLL="${DLSS_NR_DLL:-${NGX_BRIDGE_DIR}/nvngx_dlssnr.dll}"
NGX_COMPAT_DLL_DIR="${MGPU_NGX_COMPAT_DLL_DIR:-}"
NGX_COMPAT_UNIX_DIR="${MGPU_NGX_COMPAT_UNIX_DIR:-}"
DXVK_DIR="${MGPU_DXVK_DIR:-}"
DXVK_NVAPI_DIR="${MGPU_DXVK_NVAPI_DIR:-}"

for required in "${WINE_LOADER}" "${WINE_SERVER}" \
    "${VKD3D_DLL_DIR}/d3d12.dll" "${VKD3D_DLL_DIR}/d3d12core.dll" \
    "${SDK_DIR}/include/nvsdk_ngx.h"; do
  if [[ ! -e "${required}" ]]; then
    echo "Falta artefacto experimental: ${required}" >&2
    exit 2
  fi
done

if [[ ! -x "${COPY_HELPER}" ]]; then
  cmake --build "${ROOT_DIR}/build" --target mgpu-cuda-external-p2p-copy-helper -j2
fi
if [[ "${ROOT_DIR}/tests/cuda_external_fenced_p2p_helper.cpp" -nt "${FENCED_HELPER}" ||
      ! -x "${FENCED_HELPER}" ]]; then
  FENCED_P2P_OUT="${FENCED_HELPER}" \
    "${ROOT_DIR}/scripts/build_cuda_external_import_helper.sh" >/dev/null
fi
if [[ ! -f "${SHIM}" ]]; then
  OUT="${SHIM}" "${ROOT_DIR}/scripts/build_fd_inherit_shim.sh" >/dev/null
fi

if [[ "${NGX_MODE}" == "1" ]]; then
  for required_ngx in "${NGX_PROXY_DLL}" "${NGX_BRIDGE_DLL}" \
      "${NGX_CORE_DLL}" "${NGX_RUNTIME_DLL}" "${NGX_NR_DLL}"; do
    if [[ ! -f "${required_ngx}" ]]; then
      echo "Falta artefacto NGX explícito: ${required_ngx}" >&2
      echo "Usá MGPU_NGX_CORE_DLL/DLSS_RUNTIME_DLL/DLSS_NR_DLL o desactivá MGPU_NGX_CROSS_ADAPTER." >&2
      exit 2
    fi
  done
fi

if [[ -n "${DXVK_DIR}" && ! -f "${DXVK_DIR}/dxgi.dll" ]]; then
  echo "Falta DXVK x64: ${DXVK_DIR}/dxgi.dll" >&2
  exit 2
fi
if [[ -n "${DXVK_NVAPI_DIR}" ]]; then
  for required_dxvk_nvapi in "${DXVK_NVAPI_DIR}/nvapi64.dll" \
      "${DXVK_NVAPI_DIR}/nvofapi64.dll"; do
    if [[ ! -f "${required_dxvk_nvapi}" ]]; then
      echo "Falta DXVK-NVAPI x64: ${required_dxvk_nvapi}" >&2
      exit 2
    fi
  done
fi

mkdir -p "${OUT_DIR}" "${WINE_PREFIX}"
if [[ ! -f "${WINE_PREFIX}/system.reg" && "${WINE_BOOTSTRAP_PREFIX:-1}" == "1" ]]; then
  WINEBOOT="${WINEBOOT:-$(command -v wineboot || true)}"
  if [[ -z "${WINEBOOT}" || ! -x "${WINEBOOT}" ]]; then
    echo "No se encontró wineboot para inicializar el prefix temporal." >&2
    exit 2
  fi
  env -u WINESERVER -u WINELOADER \
    WINEPREFIX="${WINE_PREFIX}" WINEARCH=win64 WINEDEBUG=-all \
    "${WINEBOOT}" -u
fi
if [[ "${WINE_BOOTSTRAP_PREFIX:-1}" == "1" && "${WINE_PREFIX}" == /tmp/* ]]; then
  printf 'disable\n' > "${WINE_PREFIX}/.update-timestamp"
fi

cp "${VKD3D_DLL_DIR}/d3d12.dll" "${OUT_DIR}/d3d12.dll"
cp "${VKD3D_DLL_DIR}/d3d12core.dll" "${OUT_DIR}/d3d12core.dll"
cp "${WINE_BUILD_DIR}/dlls/cryptbase/x86_64-windows/cryptbase.dll" \
  "${OUT_DIR}/cryptbase.dll"
if [[ -n "${DXVK_DIR}" ]]; then
  cp "${DXVK_DIR}/dxgi.dll" "${OUT_DIR}/dxgi.dll"
fi
if [[ -n "${DXVK_NVAPI_DIR}" ]]; then
  cp "${DXVK_NVAPI_DIR}/nvapi64.dll" "${OUT_DIR}/nvapi64.dll"
  cp "${DXVK_NVAPI_DIR}/nvofapi64.dll" "${OUT_DIR}/nvofapi64.dll"
fi
if [[ "${NGX_MODE}" == "1" ]]; then
  # Keep the NGX chain explicit. In particular, never use a proxy as the
  # real core/runtime: that creates recursive Init_Ext calls or 0xbad00002.
  cp "${NGX_PROXY_DLL}" "${OUT_DIR}/nvngx_dlss.dll"
  cp "${NGX_BRIDGE_DLL}" "${OUT_DIR}/bridge-nvngx.dll"
  cp "${NGX_CORE_DLL}" "${OUT_DIR}/_nvngx_real.dll"
  cp "${NGX_RUNTIME_DLL}" "${OUT_DIR}/nvngx_dlss_real.dll"
  cp "${NGX_NR_DLL}" "${OUT_DIR}/nvngx_dlssnr.dll"
fi
if [[ -d "${WINE_PREFIX}/drive_c/windows/system32" ]]; then
  cp "${WINE_BUILD_DIR}/dlls/cryptbase/x86_64-windows/cryptbase.dll" \
    "${WINE_PREFIX}/drive_c/windows/system32/cryptbase.dll"
  cp "${WINE_BUILD_DIR}/dlls/winex11.drv/x86_64-windows/winex11.drv" \
    "${WINE_PREFIX}/drive_c/windows/system32/winex11.drv"
  if [[ "${NGX_MODE}" == "1" && -n "${NGX_COMPAT_DLL_DIR}" ]]; then
    for compat_name in nvapi64.dll nvml.dll nvofapi64.dll; do
      if [[ -f "${NGX_COMPAT_DLL_DIR}/${compat_name}" ]]; then
        cp "${NGX_COMPAT_DLL_DIR}/${compat_name}" \
          "${WINE_PREFIX}/drive_c/windows/system32/${compat_name}"
      fi
    done
  fi
fi
x86_64-w64-mingw32-g++ -O2 -std=c++17 -static-libgcc -static-libstdc++ \
  -I"${SDK_DIR}/include" \
  "${ROOT_DIR}/tests/d3d12_cross_adapter_frame_smoke.cpp" \
  -o "${OUT_DIR}/d3d12_cross_adapter_frame_smoke.exe" -ld3d12 -ldxgi

if [[ "${REVERSE}" == "1" ]]; then
  SOURCE_ORDINAL=1
  DESTINATION_ORDINAL=0
else
  SOURCE_ORDINAL=0
  DESTINATION_ORDINAL=1
fi
FENCE_EXPORT_FD="${VKD3D_EXPORT_FENCE_FD:-0}"
if [[ "${GPU_NATIVE}" == "1" ]]; then
  FENCE_EXPORT_FD=1
fi

export WINEPREFIX="${WINE_PREFIX}"
export WINEARCH=win64
export WINEBUILDDIR="${WINE_BUILD_DIR}"
export WINESERVER="${WINE_SERVER}"
export WINELOADERNOEXEC="${WINELOADERNOEXEC:-1}"
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEDLLPATH="${OUT_DIR}:${NGX_COMPAT_UNIX_DIR:+${NGX_COMPAT_UNIX_DIR}:}${WINE_BUILD_DIR}/dlls:${WINE_BUILD_DIR}/dlls/cryptbase/x86_64-windows:${WINE_BUILD_DIR}/dlls/winex11.drv"
if [[ -n "${DXVK_DIR}" ]]; then
  export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-dxgi=n,b;d3d12=n,b;d3d12core=n,b}"
else
  export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-d3d12=n,b;d3d12core=n,b}"
fi
export VKD3D_DUPLICATE_LUID_ADAPTERS="${VKD3D_DUPLICATE_LUID_ADAPTERS:-1}"
export VKD3D_EXPORT_RESOURCE_FD=1
export VKD3D_EXPORT_FENCE_FD="${FENCE_EXPORT_FD}"
export MGPU_CROSS_ADAPTER_RESOURCE_FD=1
export MGPU_CROSS_ADAPTER_GPU_NATIVE="${GPU_NATIVE}"
export MGPU_CROSS_ADAPTER_GPU_NATIVE_FRAMES="${GPU_NATIVE_FRAMES}"
export MGPU_CROSS_ADAPTER_GPU_NATIVE_OUT="${OUT_DIR}"
export MGPU_FENCED_P2P_HELPER="${FENCED_HELPER}"
export MGPU_CUDA_P2P_COPY_HELPER="${COPY_HELPER}"
export MGPU_CUDA_SOURCE_ORDINAL="${SOURCE_ORDINAL}"
export MGPU_CUDA_DESTINATION_ORDINAL="${DESTINATION_ORDINAL}"
export MGPU_CROSS_ADAPTER_REVERSE="${REVERSE}"
export MGPU_NGX_CROSS_ADAPTER="${NGX_MODE}"
if [[ -n "${DXVK_NVAPI_DIR}" ]]; then
  export DXVK_ENABLE_NVAPI="${DXVK_ENABLE_NVAPI:-1}"
  export DXVK_CONFIG="${DXVK_CONFIG:-dxgi.customVendorId = 10de}"
fi
if [[ "${NGX_MODE}" == "1" ]]; then
  export NVIDIA_WINE_DLL_DIR="${NVIDIA_WINE_DLL_DIR:-${OUT_DIR}}"
fi
export LD_PRELOAD="${SHIM}${LD_PRELOAD:+:${LD_PRELOAD}}"
export LD_LIBRARY_PATH="${NGX_COMPAT_UNIX_DIR:+${NGX_COMPAT_UNIX_DIR}:}${LD_LIBRARY_PATH:-${WINE_BUILD_DIR}/dlls/winevulkan:${WINE_BUILD_DIR}/dlls/ntdll:${WINE_BUILD_DIR}/dlls/win32u:${WINE_BUILD_DIR}/dlls/unixlib:${WINE_BUILD_DIR}/libs/wine}"

cd "${OUT_DIR}"
exec "${WINE_LOADER}" ./d3d12_cross_adapter_frame_smoke.exe
