#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROTON="${PROTON:-}"
PREFIX="${WINEPREFIX:-/tmp/dlss5-vkd3d-vk-export-smoke}"
OUT_DIR="${OUT_DIR:-${ROOT_DIR}/build/proton}"
VKD3D_DLL_DIR="${VKD3D_DLL_DIR:-}"
VULKAN_INCLUDE_DIR="${VULKAN_INCLUDE_DIR:-/usr/include/vulkan}"
FD_INHERIT_SHIM="${MGPU_FD_INHERIT_SHIM:-${ROOT_DIR}/build/libmgpu_fd_inherit_shim.so}"

if [[ -z "${PROTON}" || ! -x "${PROTON}" ]]; then
  echo "PROTON debe apuntar al launcher Proton ejecutable." >&2
  exit 2
fi

mkdir -p "${OUT_DIR}" "${PREFIX}"
x86_64-w64-mingw32-g++ -O2 -std=c++17 \
  "${ROOT_DIR}/tests/vkd3d_vk_export_smoke.cpp" \
  -I"${VULKAN_INCLUDE_DIR}" -idirafter /usr/include \
  -o "${OUT_DIR}/vkd3d_vk_export_smoke.exe" -ld3d12 -ldxgi

if [[ -n "${VKD3D_DLL_DIR}" ]]; then
  cp "${VKD3D_DLL_DIR}/d3d12.dll" "${OUT_DIR}/d3d12.dll"
  cp "${VKD3D_DLL_DIR}/d3d12core.dll" "${OUT_DIR}/d3d12core.dll"
  export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-d3d12=n,b;d3d12core=n,b}"
fi

if [[ -n "${MGPU_CUDA_IMPORT_HELPER:-}" ]]; then
  if [[ ! -f "${FD_INHERIT_SHIM}" ]]; then
    "${ROOT_DIR}/scripts/build_fd_inherit_shim.sh"
  fi
  export LD_PRELOAD="${FD_INHERIT_SHIM}${LD_PRELOAD:+:${LD_PRELOAD}}"
fi

PROTON_ROOT="$(cd "$(dirname "${PROTON}")" && pwd)"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="${STEAM_COMPAT_CLIENT_INSTALL_PATH:-${PROTON_ROOT}}"
export STEAM_COMPAT_DATA_PATH="${STEAM_COMPAT_DATA_PATH:-${PREFIX}}"
export UMU_ID="${UMU_ID:-dlss5-vkd3d-vk-export-smoke}"
export UMU_USE_STEAM="${UMU_USE_STEAM:-0}"
export WINEDEBUG="${WINEDEBUG:--all}"

cd "${OUT_DIR}"
exec "${PROTON}" run ./vkd3d_vk_export_smoke.exe
