#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WINE_BUILD_DIR="${WINE_BUILD_DIR:-/tmp/dlss5-wine-build-x}"
VKD3D_DLL_DIR="${VKD3D_DLL_DIR:-/tmp/dlss5-vkd3d-install/bin}"
WINE_PREFIX="${WINEPREFIX:-/tmp/dlss5-cross-adapter-fence-prefix}"
OUT_DIR="${OUT_DIR:-/tmp/dlss5-cross-adapter-fence-out}"
WINE_LOADER="${WINE_LOADER:-${WINE_BUILD_DIR}/loader/wine}"
WINE_SERVER="${WINE_SERVER:-${WINE_BUILD_DIR}/server/wineserver}"
CUDA_FENCE_WAIT="${MGPU_FENCE_CUDA_WAIT:-0}"
CUDA_FENCE_WAIT_HELPER="${MGPU_FENCE_CUDA_WAIT_HELPER:-${ROOT_DIR}/build/cuda_external_semaphore_wait_helper}"
CUDA_FENCE_WAIT_LOG="${MGPU_FENCE_CUDA_WAIT_LOG:-${OUT_DIR}/cuda-fence-wait.log}"
CUDA_FENCE_WAIT_ORDINAL="${MGPU_FENCE_CUDA_WAIT_ORDINAL:-1}"
FD_INHERIT_SHIM="${MGPU_FD_INHERIT_SHIM:-${ROOT_DIR}/build/libmgpu_fd_inherit_shim.so}"

for required in "${WINE_LOADER}" "${WINE_SERVER}" \
    "${VKD3D_DLL_DIR}/d3d12.dll" "${VKD3D_DLL_DIR}/d3d12core.dll"; do
  if [[ ! -e "${required}" ]]; then
    echo "Falta artefacto experimental: ${required}" >&2
    exit 2
  fi
done

if [[ "${CUDA_FENCE_WAIT}" == "1" && ! -x "${CUDA_FENCE_WAIT_HELPER}" ]]; then
  SEMAPHORE_WAIT_OUT="${CUDA_FENCE_WAIT_HELPER}" \
    "${ROOT_DIR}/scripts/build_cuda_external_import_helper.sh" >/dev/null
fi
if [[ "${CUDA_FENCE_WAIT}" == "1" && ! -x "${CUDA_FENCE_WAIT_HELPER}" ]]; then
  echo "No se pudo construir el helper CUDA de espera externa: ${CUDA_FENCE_WAIT_HELPER}" >&2
  exit 2
fi
if [[ ! -f "${FD_INHERIT_SHIM}" ]]; then
  OUT="${FD_INHERIT_SHIM}" "${ROOT_DIR}/scripts/build_fd_inherit_shim.sh" >/dev/null
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
# The custom Wine loader does not recursively search the build tree for
# builtin PE modules.  Keep the temporary harness self-contained so the
# system prefix can provide the registry/metadata while this build provides
# the matching cryptbase implementation.
cp "${WINE_BUILD_DIR}/dlls/cryptbase/x86_64-windows/cryptbase.dll" \
  "${OUT_DIR}/cryptbase.dll"
if [[ -d "${WINE_PREFIX}/drive_c/windows/system32" ]]; then
  cp "${WINE_BUILD_DIR}/dlls/cryptbase/x86_64-windows/cryptbase.dll" \
    "${WINE_PREFIX}/drive_c/windows/system32/cryptbase.dll"
  cp "${WINE_BUILD_DIR}/dlls/winex11.drv/x86_64-windows/winex11.drv" \
    "${WINE_PREFIX}/drive_c/windows/system32/winex11.drv"
fi
x86_64-w64-mingw32-g++ -O2 -std=c++17 \
  "${ROOT_DIR}/tests/vkd3d_cross_adapter_fence_smoke.cpp" \
  -I/usr/include/vulkan -idirafter /usr/include \
  -static-libgcc -static-libstdc++ \
  -o "${OUT_DIR}/vkd3d_cross_adapter_fence_smoke.exe" -ld3d12

export WINEPREFIX="${WINE_PREFIX}"
export WINEARCH=win64
export WINEBUILDDIR="${WINE_BUILD_DIR}"
export WINESERVER="${WINE_SERVER}"
export WINELOADERNOEXEC="${WINELOADERNOEXEC:-1}"
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEDLLPATH="${OUT_DIR}:${WINE_BUILD_DIR}/dlls:${WINE_BUILD_DIR}/dlls/cryptbase/x86_64-windows:${WINE_BUILD_DIR}/dlls/winex11.drv"
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-d3d12=n,b;d3d12core=n,b}"
export VKD3D_EXPORT_FENCE_FD=1
export VKD3D_DUPLICATE_LUID_ADAPTERS=1
if [[ "${CUDA_FENCE_WAIT}" == "1" ]]; then
  export LD_PRELOAD="${FD_INHERIT_SHIM}${LD_PRELOAD:+:${LD_PRELOAD}}"
  export MGPU_FENCE_CUDA_WAIT_HELPER="${CUDA_FENCE_WAIT_HELPER}"
  export MGPU_FENCE_CUDA_WAIT_LOG="${CUDA_FENCE_WAIT_LOG}"
  export MGPU_FENCE_CUDA_WAIT_ORDINAL="${CUDA_FENCE_WAIT_ORDINAL}"
fi
unset VKD3D_DUPLICATE_LUID_INDEX
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-${WINE_BUILD_DIR}/dlls/winevulkan:${WINE_BUILD_DIR}/dlls/ntdll:${WINE_BUILD_DIR}/dlls/win32u:${WINE_BUILD_DIR}/dlls/unixlib:${WINE_BUILD_DIR}/libs/wine}"

cd "${OUT_DIR}"
exec "${WINE_LOADER}" ./vkd3d_cross_adapter_fence_smoke.exe
