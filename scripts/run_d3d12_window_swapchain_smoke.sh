#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROTON="${PROTON:-}"
PREFIX="${WINEPREFIX:-/tmp/dlss5-d3d12-window-swapchain-smoke}"
OUT_DIR="${OUT_DIR:-${ROOT_DIR}/build/proton}"
VKD3D_DLL_DIR="${VKD3D_DLL_DIR:-}"
NGX_RUNTIME_DLL="${NGX_RUNTIME_DLL:-}"
TIMEOUT_SECONDS="${MGPU_SWAPCHAIN_TIMEOUT_SECONDS:-20}"

if [[ -z "${PROTON}" || ! -x "${PROTON}" ]]; then
  echo "PROTON debe apuntar al launcher Proton ejecutable." >&2
  exit 2
fi
mkdir -p "${OUT_DIR}" "${PREFIX}"

x86_64-w64-mingw32-g++ -O2 -std=c++17 \
  -static-libgcc -static-libstdc++ \
  "${ROOT_DIR}/tests/d3d12_window_swapchain_smoke.cpp" \
  -o "${OUT_DIR}/d3d12_window_swapchain_smoke.exe" \
  -ld3d12 -ldxgi -luser32

copy_if_different() {
  local source="$1" destination="$2"
  if [[ "$(readlink -f "${source}")" != "$(readlink -f "${destination}" 2>/dev/null || true)" ]]; then
    cp "${source}" "${destination}"
  fi
}

if [[ -n "${VKD3D_DLL_DIR}" ]]; then
  [[ -f "${VKD3D_DLL_DIR}/d3d12.dll" && -f "${VKD3D_DLL_DIR}/d3d12core.dll" ]] || {
    echo "VKD3D_DLL_DIR debe contener d3d12.dll y d3d12core.dll." >&2
    exit 2
  }
  copy_if_different "${VKD3D_DLL_DIR}/d3d12.dll" "${OUT_DIR}/d3d12.dll"
  copy_if_different "${VKD3D_DLL_DIR}/d3d12core.dll" "${OUT_DIR}/d3d12core.dll"
  export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-d3d12=n,b;d3d12core=n,b}"
fi
if [[ -n "${NGX_RUNTIME_DLL}" ]]; then
  [[ -f "${NGX_RUNTIME_DLL}" ]] || {
    echo "NGX_RUNTIME_DLL no existe: ${NGX_RUNTIME_DLL}" >&2
    exit 2
  }
  cp "${NGX_RUNTIME_DLL}" "${OUT_DIR}/nvngx_dlss.dll"
fi

PROTON_ROOT="$(cd "$(dirname "${PROTON}")" && pwd)"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="${STEAM_COMPAT_CLIENT_INSTALL_PATH:-${PROTON_ROOT}}"
export STEAM_COMPAT_DATA_PATH="${STEAM_COMPAT_DATA_PATH:-${PREFIX}}"
export UMU_ID="${UMU_ID:-dlss5-d3d12-window-swapchain-smoke}"
export UMU_USE_STEAM="${UMU_USE_STEAM:-0}"
export PROTON_USE_XALIA="${PROTON_USE_XALIA:-0}"
export WINEDEBUG="${WINEDEBUG:--all}"

cd "${OUT_DIR}"
rm_result="${OUT_DIR}/d3d12_window_swapchain_smoke.result.txt"
if [[ -f "${rm_result}" ]]; then
  find "${rm_result}" -depth -delete
fi

set +e
setsid timeout --signal=TERM --kill-after=3s "${TIMEOUT_SECONDS}s" \
  env MGPU_SWAPCHAIN_NGX="${MGPU_SWAPCHAIN_NGX:-}" \
  MGPU_SWAPCHAIN_OFFICIAL_PROFILE="${MGPU_SWAPCHAIN_OFFICIAL_PROFILE:-}" \
  MGPU_SWAPCHAIN_SHOW="${MGPU_SWAPCHAIN_SHOW:-}" \
  MGPU_D3D12_ADAPTER_INDEX="${MGPU_D3D12_ADAPTER_INDEX:-0}" \
  "${PROTON}" run ./d3d12_window_swapchain_smoke.exe
rc=$?
set -e

if [[ -f "${rm_result}" ]]; then
  sed -n '1,240p' "${rm_result}"
fi
echo "d3d12_window_swapchain_return_code=${rc} timeout_seconds=${TIMEOUT_SECONDS}"
exit "${rc}"
