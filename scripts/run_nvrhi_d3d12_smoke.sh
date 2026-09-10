#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROTON="${PROTON:-}"
VKD3D_DLL_DIR="${VKD3D_DLL_DIR:-}"
NVRHI_SRC_ROOT="${NVRHI_SRC_ROOT:-/home/cristian/Juegos/DLSS5-NVIDIA-DLSS-310.9.1/DLSS_Sample_App/donut/nvrhi}"
OUT_DIR="${OUT_DIR:-${ROOT_DIR}/build/proton}"
PREFIX="${WINEPREFIX:-/tmp/dlss5-nvrhi-d3d12-smoke}"
TIMEOUT_SECONDS="${MGPU_NVRHI_TIMEOUT_SECONDS:-20}"

if [[ -z "${PROTON}" || ! -x "${PROTON}" ]]; then
  echo "PROTON debe apuntar al launcher Proton ejecutable." >&2
  exit 2
fi
if [[ ! -f "${NVRHI_SRC_ROOT}/include/nvrhi/d3d12/d3d12.h" ||
      ! -f "${NVRHI_SRC_ROOT}/src/d3d12/d3d12.cpp" ]]; then
  echo "NVRHI_SRC_ROOT no contiene el snapshot NVRHI esperado: ${NVRHI_SRC_ROOT}" >&2
  exit 2
fi
mkdir -p "${OUT_DIR}" "${PREFIX}"

x86_64-w64-mingw32-g++ -O2 -std=c++17 -msse4.2 \
  -DUSE_DX=1 -DUSE_DX11=0 -DUSE_DX12=1 -DUSE_VK=0 \
  -DNVRHI_D3D12_WITH_NVAPI=0 -DNVRHI_WITH_DXR=0 -DNOMINMAX -D_Inout_= \
  -include cstdint -include cstring \
  -include "${ROOT_DIR}/tests/fixtures/nvrhi_compat/nvrhi_compat.h" \
  -I"${ROOT_DIR}/tests/fixtures/nvrhi_compat" \
  -I"${NVRHI_SRC_ROOT}/include" \
  "${ROOT_DIR}/tests/nvrhi_d3d12_smoke.cpp" \
  "${NVRHI_SRC_ROOT}/src/common/alloc.cpp" \
  "${NVRHI_SRC_ROOT}/src/common/crc.cpp" \
  "${NVRHI_SRC_ROOT}/src/common/shader-blob.cpp" \
  "${NVRHI_SRC_ROOT}/src/common/utils.cpp" \
  "${NVRHI_SRC_ROOT}/src/validation/device.cpp" \
  "${NVRHI_SRC_ROOT}/src/validation/commandlist.cpp" \
  "${NVRHI_SRC_ROOT}/src/d3d12/d3d12.cpp" \
  "${NVRHI_SRC_ROOT}/src/d3d12/commandlist.cpp" \
  "${NVRHI_SRC_ROOT}/src/d3d12/dxr.cpp" \
  -o "${OUT_DIR}/nvrhi_d3d12_smoke.exe" \
  -ld3d12 -ldxgi -ldxguid -lole32 -luser32 \
  -static-libgcc -static-libstdc++

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

PROTON_ROOT="$(cd "$(dirname "${PROTON}")" && pwd)"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="${STEAM_COMPAT_CLIENT_INSTALL_PATH:-${PROTON_ROOT}}"
export STEAM_COMPAT_DATA_PATH="${STEAM_COMPAT_DATA_PATH:-${PREFIX}}"
export UMU_ID="${UMU_ID:-dlss5-nvrhi-d3d12-smoke}"
export UMU_USE_STEAM="${UMU_USE_STEAM:-0}"
export PROTON_USE_XALIA="${PROTON_USE_XALIA:-0}"
export WINEDEBUG="${WINEDEBUG:--all}"

cd "${OUT_DIR}"
result_file="${OUT_DIR}/nvrhi_d3d12_smoke.result.txt"
if [[ -f "${result_file}" ]]; then find "${result_file}" -depth -delete; fi
set +e
setsid timeout --signal=TERM --kill-after=3s "${TIMEOUT_SECONDS}s" \
  env MGPU_D3D12_ADAPTER_INDEX="${MGPU_D3D12_ADAPTER_INDEX:-0}" \
  MGPU_NVRHI_SHOW_AFTER="${MGPU_NVRHI_SHOW_AFTER:-}" \
  "${PROTON}" run ./nvrhi_d3d12_smoke.exe
rc=$?
set -e
if [[ -f "${result_file}" ]]; then sed -n '1,240p' "${result_file}"; fi
echo "nvrhi_d3d12_return_code=${rc} timeout_seconds=${TIMEOUT_SECONDS}"
exit "${rc}"
