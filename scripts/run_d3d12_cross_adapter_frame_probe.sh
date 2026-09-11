#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROTON="${PROTON:-}"
OUT_DIR="${OUT_DIR:-${ROOT_DIR}/build/proton}"
VKD3D_DLL_DIR="${VKD3D_DLL_DIR:-${OUT_DIR}}"
HELPER="${MGPU_CUDA_P2P_COPY_HELPER:-${ROOT_DIR}/build/mgpu-cuda-external-p2p-copy-helper}"
FENCED_HELPER="${MGPU_FENCED_P2P_HELPER:-${ROOT_DIR}/build/cuda_external_fenced_p2p_helper}"
SHIM="${MGPU_FD_INHERIT_SHIM:-${ROOT_DIR}/build/libmgpu_fd_inherit_shim.so}"
KEEP_TEMP="${MGPU_KEEP_TEMP:-0}"
NGX_CROSS_ADAPTER="${MGPU_NGX_CROSS_ADAPTER:-1}"
RESOURCE_FD_MODE="${MGPU_CROSS_ADAPTER_RESOURCE_FD:-0}"
PERSISTENT_FRAMES="${MGPU_CROSS_ADAPTER_PERSISTENT_FRAMES:-1}"
NGX_FRAME_COUNT="${MGPU_NGX_FRAME_COUNT:-1}"
PRESENTATION="${MGPU_CROSS_ADAPTER_PRESENT:-0}"
PRESENTATION_FRAMES="${MGPU_PRESENT_FRAMES:-1}"
PRESENTATION_AUTO="${MGPU_CROSS_ADAPTER_PRESENT_AUTO:-1}"
RASTER="${MGPU_CROSS_ADAPTER_RASTER:-0}"
GPU_NATIVE="${MGPU_CROSS_ADAPTER_GPU_NATIVE:-0}"
GPU_NATIVE_FRAMES="${MGPU_CROSS_ADAPTER_GPU_NATIVE_FRAMES:-3}"
GPU_NATIVE_OUT="${MGPU_CROSS_ADAPTER_GPU_NATIVE_OUT:-${OUT_DIR}}"
PROBE_TIMEOUT_SECONDS="${MGPU_CROSS_ADAPTER_TIMEOUT_SECONDS:-60}"
REQUIRE_DISTINCT_IDENTITY="${MGPU_CROSS_ADAPTER_REQUIRE_DISTINCT_IDENTITY:-1}"
DXC="${MGPU_DXC:-}"
RESOURCE_EXPORT_FD="${VKD3D_EXPORT_RESOURCE_FD:-0}"
if [[ "${RESOURCE_FD_MODE}" == "1" ]]; then
  RESOURCE_EXPORT_FD=1
fi
if [[ "${GPU_NATIVE}" == "1" ]]; then
  RESOURCE_FD_MODE=1
  RESOURCE_EXPORT_FD=1
fi
FENCE_EXPORT_FD="${VKD3D_EXPORT_FENCE_FD:-0}"
if [[ "${GPU_NATIVE}" == "1" ]]; then
  FENCE_EXPORT_FD=1
fi
REVERSE_DIRECTION="${MGPU_CROSS_ADAPTER_REVERSE:-0}"
CORE_DLL="${MGPU_NGX_CORE_DLL:-}"
CORE_PREFIX="${MGPU_NGX_CORE_PREFIX:-/tmp/dlss5-cross-adapter-core}"
CORE_PREFIX_OWNED=0

if [[ -z "${PROTON}" || ! -x "${PROTON}" ]]; then
  echo "PROTON debe apuntar al launcher Proton ejecutable." >&2
  exit 2
fi
if [[ ! -x "${HELPER}" ]]; then
  cmake --build "${ROOT_DIR}/build" --target mgpu-cuda-external-p2p-copy-helper -j2
fi
if [[ "${GPU_NATIVE}" == "1" &&
      ( ! -x "${FENCED_HELPER}" ||
        "${ROOT_DIR}/tests/cuda_external_fenced_p2p_helper.cpp" -nt "${FENCED_HELPER}" ) ]]; then
  FENCED_P2P_OUT="${FENCED_HELPER}" \
    "${ROOT_DIR}/scripts/build_cuda_external_import_helper.sh" >/dev/null
fi
if [[ "${GPU_NATIVE}" == "1" && ! -x "${FENCED_HELPER}" ]]; then
  echo "No se pudo construir el helper CUDA con fences: ${FENCED_HELPER}" >&2
  exit 2
fi
if [[ ! -f "${SHIM}" ]]; then
  "${ROOT_DIR}/scripts/build_fd_inherit_shim.sh" >/dev/null
fi
if [[ ! -f "${VKD3D_DLL_DIR}/d3d12.dll" || ! -f "${VKD3D_DLL_DIR}/d3d12core.dll" ]]; then
  echo "VKD3D_DLL_DIR debe contener d3d12.dll y d3d12core.dll." >&2
  exit 2
fi
if [[ -z "${NGX_SDK_DIR:-}" || ! -f "${NGX_SDK_DIR}/include/nvsdk_ngx.h" ]]; then
  echo "NGX_SDK_DIR debe apuntar a headers válidos para compilar el smoke." >&2
  exit 2
fi
if [[ "${NGX_CROSS_ADAPTER}" == "1" ]]; then
  if [[ -z "${DLSS_RUNTIME_DLL:-}" || ! -f "${DLSS_RUNTIME_DLL}" ]]; then
    echo "El modo NGX combinado requiere DLSS_RUNTIME_DLL apuntando al runtime DLSS real." >&2
    exit 2
  fi
  if [[ -z "${DLSS_NR_DLL:-}" || ! -f "${DLSS_NR_DLL}" ]]; then
    echo "El modo NGX combinado requiere DLSS_NR_DLL apuntando a nvngx_dlssnr.dll." >&2
    exit 2
  fi
  if [[ -z "${NGX_BRIDGE_DIR:-}" ||
        ! -f "${NGX_BRIDGE_DIR}/_nvngx.dll" ||
        ! -f "${NGX_BRIDGE_DIR}/bridge-nvngx.dll" ]]; then
    echo "El modo NGX combinado requiere _nvngx.dll y bridge-nvngx.dll en NGX_BRIDGE_DIR." >&2
    exit 2
  fi
  if [[ -z "${CORE_DLL}" ]]; then
    CORE_DLL="${CORE_PREFIX}/pfx/drive_c/windows/system32/_nvngx.dll"
    if [[ ! -f "${CORE_DLL}" ]]; then
      if [[ -z "${DLSS_DEMO_DIR:-}" || ! -f "${DLSS_DEMO_DIR}/ngx_dlss_demo.exe" ]]; then
        echo "Para generar automáticamente el core NGX indicá DLSS_DEMO_DIR con ngx_dlss_demo.exe." >&2
        exit 2
      fi
      CORE_PREFIX_OWNED=1
      PROTON_ROOT_BOOTSTRAP="$(cd "$(dirname "${PROTON}")" && pwd)"
      mkdir -p "${CORE_PREFIX}"
      set +e
      timeout 10s env \
        STEAM_COMPAT_CLIENT_INSTALL_PATH="${PROTON_ROOT_BOOTSTRAP}" \
        STEAM_COMPAT_DATA_PATH="${CORE_PREFIX}" \
        UMU_ID=dlss5crossadapterbootstrap UMU_USE_STEAM=0 WINEDEBUG=-all \
        "${PROTON}" run "${DLSS_DEMO_DIR}/ngx_dlss_demo.exe" -d3d12 \
        >/dev/null 2>&1
      BOOTSTRAP_RC=$?
      set -e
      if [[ ! -f "${CORE_DLL}" ]]; then
        echo "GE-Proton no generó el core NGX (${CORE_DLL}), rc=${BOOTSTRAP_RC}." >&2
        exit 2
      fi
    fi
  fi
fi
if [[ "${REVERSE_DIRECTION}" == "1" ]]; then
  SOURCE_ORDINAL="${MGPU_CUDA_SOURCE_ORDINAL:-1}"
  DESTINATION_ORDINAL="${MGPU_CUDA_DESTINATION_ORDINAL:-0}"
else
  SOURCE_ORDINAL="${MGPU_CUDA_SOURCE_ORDINAL:-0}"
  DESTINATION_ORDINAL="${MGPU_CUDA_DESTINATION_ORDINAL:-1}"
fi

mkdir -p "${OUT_DIR}"
SHADER_DEFINE=()
if [[ "${RASTER}" == "1" ]]; then
  if [[ -z "${DXC}" ]]; then
    DXC="$(command -v dxc || true)"
  fi
  if [[ -z "${DXC}" || ! -x "${DXC}" ]]; then
    echo "MGPU_CROSS_ADAPTER_RASTER=1 requiere MGPU_DXC apuntando al binario DXC oficial." >&2
    exit 2
  fi
  SHADER_HEADER="${OUT_DIR}/mgpu_triangle_dxil.inc"
  SHADER_TMP="$(mktemp -d /tmp/dlss5-cross-adapter-shaders.XXXXXX)"
  "${DXC}" -E VSMain -T vs_6_0 -HV 2021 \
    -Fo "${SHADER_TMP}/triangle_vs.cso" \
    "${ROOT_DIR}/tests/shaders/cross_adapter_triangle.hlsl"
  "${DXC}" -E PSMain -T ps_6_0 -HV 2021 \
    -Fo "${SHADER_TMP}/triangle_ps.cso" \
    "${ROOT_DIR}/tests/shaders/cross_adapter_triangle.hlsl"
  {
    xxd -i -n mgpu_triangle_vs "${SHADER_TMP}/triangle_vs.cso"
    xxd -i -n mgpu_triangle_ps "${SHADER_TMP}/triangle_ps.cso"
  } > "${SHADER_HEADER}"
  find "${SHADER_TMP}" -depth -delete 2>/dev/null || true
  SHADER_DEFINE=(-DMGPU_RASTER_SHADER=1)
fi
x86_64-w64-mingw32-g++ -O2 -std=c++17 \
  "${SHADER_DEFINE[@]}" \
  -I"${OUT_DIR}" \
  -I"${NGX_SDK_DIR}/include" \
  -static-libgcc -static-libstdc++ \
  "${ROOT_DIR}/tests/d3d12_cross_adapter_frame_smoke.cpp" \
  -o "${OUT_DIR}/d3d12_cross_adapter_frame_smoke.exe" \
  -ld3d12 -ldxgi
if [[ "$(readlink -f "${VKD3D_DLL_DIR}/d3d12.dll")" != "$(readlink -f "${OUT_DIR}/d3d12.dll")" ]]; then
  cp "${VKD3D_DLL_DIR}/d3d12.dll" "${OUT_DIR}/d3d12.dll"
fi
if [[ "$(readlink -f "${VKD3D_DLL_DIR}/d3d12core.dll")" != "$(readlink -f "${OUT_DIR}/d3d12core.dll")" ]]; then
  cp "${VKD3D_DLL_DIR}/d3d12core.dll" "${OUT_DIR}/d3d12core.dll"
fi
if [[ "${NGX_CROSS_ADAPTER}" == "1" ]]; then
  copy_unless_same() {
    local source="$1"
    local destination="$2"
    if [[ "$(readlink -f "${source}")" != "$(readlink -f "${destination}" 2>/dev/null || true)" ]]; then
      cp "${source}" "${destination}"
    fi
  }
  copy_unless_same "${NGX_BRIDGE_DIR}/_nvngx.dll" "${OUT_DIR}/nvngx_dlss.dll"
  copy_unless_same "${NGX_BRIDGE_DIR}/bridge-nvngx.dll" "${OUT_DIR}/bridge-nvngx.dll"
  copy_unless_same "${CORE_DLL}" "${OUT_DIR}/_nvngx_real.dll"
  copy_unless_same "${DLSS_RUNTIME_DLL}" "${OUT_DIR}/nvngx_dlss_real.dll"
  copy_unless_same "${DLSS_NR_DLL}" "${OUT_DIR}/nvngx_dlssnr.dll"
fi

OWN_ROOT=""
if [[ -z "${WINEPREFIX:-}" ]]; then
  OWN_ROOT="$(mktemp -d /tmp/dlss5-cross-adapter-frame.XXXXXX)"
  PREFIX="${OWN_ROOT}/prefix"
else
  PREFIX="${WINEPREFIX}"
fi
if [[ -n "${OWN_ROOT}" && "${KEEP_TEMP}" != "1" ]]; then
  trap 'find "${OWN_ROOT}" -depth -delete 2>/dev/null || true; if [[ "${CORE_PREFIX_OWNED}" == "1" ]]; then find "${CORE_PREFIX}" -depth -delete 2>/dev/null || true; fi' EXIT
fi
mkdir -p "${PREFIX}"
PROTON_ROOT="$(cd "$(dirname "${PROTON}")" && pwd)"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="${STEAM_COMPAT_CLIENT_INSTALL_PATH:-${PROTON_ROOT}}"
export STEAM_COMPAT_DATA_PATH="${STEAM_COMPAT_DATA_PATH:-${PREFIX}}"
export UMU_ID="${UMU_ID:-dlss5-cross-adapter-frame}"
export UMU_USE_STEAM="${UMU_USE_STEAM:-0}"
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-d3d12=n,b;d3d12core=n,b}"

cd "${OUT_DIR}"
export LD_PRELOAD="${SHIM}${LD_PRELOAD:+:${LD_PRELOAD}}"
run_probe() {
  env -u VKD3D_DUPLICATE_LUID_INDEX \
    VKD3D_DUPLICATE_LUID_ADAPTERS=1 \
    VKD3D_EXPORT_OPAQUE_FD_MEMORY=1 \
    VKD3D_EXPORT_HEAP_FD=1 \
    MGPU_CUDA_P2P_COPY_HELPER="${HELPER}" \
    MGPU_CUDA_WORKER_HELPER="${MGPU_CUDA_WORKER_HELPER:-${HELPER}}" \
    MGPU_CUDA_SOURCE_ORDINAL="${SOURCE_ORDINAL}" \
    MGPU_CUDA_DESTINATION_ORDINAL="${DESTINATION_ORDINAL}" \
    MGPU_CROSS_ADAPTER_REVERSE="${REVERSE_DIRECTION}" \
    MGPU_CROSS_ADAPTER_RESOURCE_FD="${RESOURCE_FD_MODE}" \
    MGPU_CROSS_ADAPTER_PERSISTENT_FRAMES="${PERSISTENT_FRAMES}" \
    MGPU_CROSS_ADAPTER_GPU_NATIVE="${GPU_NATIVE}" \
    MGPU_CROSS_ADAPTER_GPU_NATIVE_FRAMES="${GPU_NATIVE_FRAMES}" \
    MGPU_CROSS_ADAPTER_GPU_NATIVE_OUT="${GPU_NATIVE_OUT}" \
    MGPU_CROSS_ADAPTER_REQUIRE_DISTINCT_IDENTITY="${REQUIRE_DISTINCT_IDENTITY}" \
    MGPU_FENCED_P2P_HELPER="${FENCED_HELPER}" \
    MGPU_NGX_FRAME_COUNT="${NGX_FRAME_COUNT}" \
    MGPU_CROSS_ADAPTER_PRESENT="${PRESENTATION}" \
    MGPU_PRESENT_FRAMES="${PRESENTATION_FRAMES}" \
    MGPU_CROSS_ADAPTER_PRESENT_AUTO="${PRESENTATION_AUTO}" \
    MGPU_CROSS_ADAPTER_RASTER="${RASTER}" \
    MGPU_NGX_CROSS_ADAPTER="${NGX_CROSS_ADAPTER}" \
    VKD3D_EXPORT_FENCE_FD="${FENCE_EXPORT_FD}" \
    VKD3D_EXPORT_RESOURCE_FD="${RESOURCE_EXPORT_FD}" \
    setsid timeout --signal=TERM --kill-after=5s \
      "${PROBE_TIMEOUT_SECONDS}s" \
      "${PROTON}" run ./d3d12_cross_adapter_frame_smoke.exe
}

if [[ "${PRESENTATION}" == "1" && "${PRESENTATION_AUTO}" == "1" &&
      "${REVERSE_DIRECTION}" == "0" ]]; then
  RETRY_LOG="$(mktemp /tmp/dlss5-cross-adapter-presentation.XXXXXX)"
  set +e
  run_probe 2>&1 | tee "${RETRY_LOG}"
  PROBE_RC=${PIPESTATUS[0]}
  set -e
  if [[ "${PROBE_RC}" -ne 0 ]] &&
     rg -q '"presentation_success":false' "${RETRY_LOG}"; then
    echo "cross_adapter_presentation_auto_retry=reverse" >&2
    REVERSE_DIRECTION=1
    SOURCE_ORDINAL=1
    DESTINATION_ORDINAL=0
    set +e
    run_probe
    PROBE_RC=$?
    set -e
  fi
  find "${RETRY_LOG}" -maxdepth 0 -delete 2>/dev/null || true
  exit "${PROBE_RC}"
fi

run_probe
