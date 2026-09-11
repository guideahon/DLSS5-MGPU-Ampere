#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROTON="${PROTON:-}"
PREFIX="${WINEPREFIX:-/tmp/dlss5-vkd3d-interop-probe}"
OUT_DIR="${OUT_DIR:-${ROOT_DIR}/build/proton}"
VKD3D_DLL_DIR="${VKD3D_DLL_DIR:-}"
FD_INHERIT_SHIM="${MGPU_FD_INHERIT_SHIM:-${ROOT_DIR}/build/libmgpu_fd_inherit_shim.so}"

copy_if_different() {
  if [[ "$(readlink -f "$1")" != "$(readlink -f "$2")" ]]; then
    cp "$1" "$2"
  fi
}

if [[ -z "${PROTON}" || ! -x "${PROTON}" ]]; then
  echo "PROTON debe apuntar al launcher Proton ejecutable de la prueba." >&2
  exit 2
fi

mkdir -p "${OUT_DIR}" "${PREFIX}"
if [[ -n "${VKD3D_DLL_DIR}" &&
      ( ! -f "${VKD3D_DLL_DIR}/d3d12.dll" || ! -f "${VKD3D_DLL_DIR}/d3d12core.dll" ) ]]; then
  echo "VKD3D_DLL_DIR debe contener d3d12.dll y d3d12core.dll." >&2
  exit 2
fi
x86_64-w64-mingw32-g++ -O2 -std=c++17 \
  "${ROOT_DIR}/tests/vkd3d_interop_probe.cpp" \
  -o "${OUT_DIR}/vkd3d_interop_probe.exe" -ld3d12 -ldxgi
x86_64-w64-mingw32-g++ -O2 -std=c++17 \
  "${ROOT_DIR}/tests/d3d12_external_fd_import_worker.cpp" \
  -o "${OUT_DIR}/d3d12_external_fd_import_worker.exe" -ld3d12 -ldxgi
if [[ -n "${VKD3D_DLL_DIR}" ]]; then
  copy_if_different "${VKD3D_DLL_DIR}/d3d12.dll" "${OUT_DIR}/d3d12.dll"
  copy_if_different "${VKD3D_DLL_DIR}/d3d12core.dll" "${OUT_DIR}/d3d12core.dll"
  export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-d3d12=n,b;d3d12core=n,b}"
fi
if [[ -n "${MGPU_CUDA_IMPORT_HELPER:-}" ||
      -n "${MGPU_D3D12_FD_IMPORT_WORKER:-}" ]]; then
  if [[ ! -f "${FD_INHERIT_SHIM}" ]]; then
    "${ROOT_DIR}/scripts/build_fd_inherit_shim.sh"
  fi
  export LD_PRELOAD="${FD_INHERIT_SHIM}${LD_PRELOAD:+:${LD_PRELOAD}}"
fi

PROTON_ROOT="$(cd "$(dirname "${PROTON}")" && pwd)"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="${STEAM_COMPAT_CLIENT_INSTALL_PATH:-${PROTON_ROOT}}"
export STEAM_COMPAT_DATA_PATH="${STEAM_COMPAT_DATA_PATH:-${PREFIX}}"
export UMU_ID="${UMU_ID:-dlss5-vkd3d-interop-probe}"
export UMU_USE_STEAM="${UMU_USE_STEAM:-0}"
export WINEDEBUG="${WINEDEBUG:-=-all}"
export VKD3D_INTEROP_REQUIRE_DISTINCT="${VKD3D_INTEROP_REQUIRE_DISTINCT:-}"
# This probe creates two D3D12 devices in one process. When an explicit base
# index is supplied, opt into the per-device sequence so A/B do not inherit
# the same Vulkan physical device. Multi-process launchers keep this unset.
export VKD3D_DUPLICATE_LUID_INDEX_PER_DEVICE="${VKD3D_DUPLICATE_LUID_INDEX_PER_DEVICE:-1}"

cd "${OUT_DIR}"
exec "${PROTON}" run ./vkd3d_interop_probe.exe
