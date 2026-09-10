#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROTON="${PROTON:-}"
PREFIX="${WINEPREFIX:-/tmp/dlss5-d3d12-linear-smoke}"
OUT_DIR="${OUT_DIR:-${ROOT_DIR}/build/proton}"
VKD3D_DLL_DIR="${VKD3D_DLL_DIR:-}"
HELPER="${MGPU_CUDA_READBACK_HELPER:-${ROOT_DIR}/build/cuda_external_readback_helper}"
SHIM="${MGPU_FD_INHERIT_SHIM:-${ROOT_DIR}/build/libmgpu_fd_inherit_shim.so}"

if [[ -z "${PROTON}" || ! -x "${PROTON}" ]]; then
  echo "PROTON debe apuntar al launcher Proton ejecutable." >&2
  exit 2
fi
if [[ ! -x "${HELPER}" ]]; then
  "${ROOT_DIR}/scripts/build_cuda_external_import_helper.sh" >/dev/null
fi
if [[ ! -f "${SHIM}" ]]; then
  "${ROOT_DIR}/scripts/build_fd_inherit_shim.sh" >/dev/null
fi

mkdir -p "${OUT_DIR}" "${PREFIX}"
x86_64-w64-mingw32-g++ -O2 -std=c++17 \
  -static-libgcc -static-libstdc++ \
  "${ROOT_DIR}/tests/vkd3d_d3d12_texture_linear_smoke.cpp" \
  -o "${OUT_DIR}/vkd3d_d3d12_texture_linear_smoke.exe" \
  -ld3d12 -ldxgi

if [[ -n "${VKD3D_DLL_DIR}" ]]; then
  if [[ "$(readlink -f "${VKD3D_DLL_DIR}/d3d12.dll")" != "$(readlink -f "${OUT_DIR}/d3d12.dll")" ]]; then
    cp "${VKD3D_DLL_DIR}/d3d12.dll" "${OUT_DIR}/d3d12.dll"
  fi
  if [[ "$(readlink -f "${VKD3D_DLL_DIR}/d3d12core.dll")" != "$(readlink -f "${OUT_DIR}/d3d12core.dll")" ]]; then
    cp "${VKD3D_DLL_DIR}/d3d12core.dll" "${OUT_DIR}/d3d12core.dll"
  fi
  export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-d3d12=n,b;d3d12core=n,b}"
fi

PROTON_ROOT="$(cd "$(dirname "${PROTON}")" && pwd)"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="${STEAM_COMPAT_CLIENT_INSTALL_PATH:-${PROTON_ROOT}}"
export STEAM_COMPAT_DATA_PATH="${STEAM_COMPAT_DATA_PATH:-${PREFIX}}"
export UMU_ID="${UMU_ID:-dlss5-d3d12-texture-linear-smoke}"
export UMU_USE_STEAM="${UMU_USE_STEAM:-0}"
export WINEDEBUG="${WINEDEBUG:--all}"

cd "${OUT_DIR}"
export LD_PRELOAD="${SHIM}${LD_PRELOAD:+:${LD_PRELOAD}}"
exec env MGPU_CUDA_READBACK_HELPER="${HELPER}" \
  MGPU_CUDA_SOURCE_ORDINAL="${MGPU_CUDA_SOURCE_ORDINAL:-0}" \
  MGPU_CUDA_DESTINATION_ORDINAL="${MGPU_CUDA_DESTINATION_ORDINAL:-1}" \
  VKD3D_EXPORT_OPAQUE_FD_MEMORY=1 \
  VKD3D_EXPORT_HEAP_FD=1 \
  "${PROTON}" run ./vkd3d_d3d12_texture_linear_smoke.exe
