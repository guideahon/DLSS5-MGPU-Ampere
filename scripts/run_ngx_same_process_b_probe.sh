#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KEEP_TEMP="${MGPU_NGX_KEEP_TEMP:-0}"
PROBE_DIR="${MGPU_NGX_B_PROBE_DIR:-$(mktemp -d /tmp/dlss5-ngx-b-probe.XXXXXX)}"
LOG="${PROBE_DIR}/run.log"

# The B-first probe is also useful on hosts where the official NVIDIA demo is
# unavailable but the project already has a clean, user-provided runtime
# profile. Prefer that profile automatically; explicit environment overrides
# still win. This does not fetch or modify proprietary binaries.
PROFILE_DIR="${MGPU_NGX_PROFILE_DIR:-${ROOT_DIR}/build/proton-resource-pair-worker-experimental}"
if [[ -z "${NGX_BRIDGE_DIR:-}" &&
      -f "${PROFILE_DIR}/bridge-nvngx.dll" &&
      -f "${PROFILE_DIR}/_nvngx.dll" ]]; then
  export NGX_BRIDGE_DIR="${PROFILE_DIR}"
fi
if [[ -z "${MGPU_NGX_CORE_DLL:-}" &&
      -f "${PROFILE_DIR}/_nvngx_real.dll" ]]; then
  export MGPU_NGX_CORE_DLL="${PROFILE_DIR}/_nvngx_real.dll"
fi
if [[ -z "${DLSS_RUNTIME_DLL:-}" &&
      -f "${PROFILE_DIR}/nvngx_dlss_real.dll" ]]; then
  export DLSS_RUNTIME_DLL="${PROFILE_DIR}/nvngx_dlss_real.dll"
fi
if [[ -z "${DLSS_NR_DLL:-}" &&
      -f "${PROFILE_DIR}/nvngx_dlssnr.dll" ]]; then
  export DLSS_NR_DLL="${PROFILE_DIR}/nvngx_dlssnr.dll"
fi
SDK_CACHE_DIR="${XDG_CACHE_HOME:-${HOME}/.cache}/dlss5-sdk/DLSS"
if [[ -z "${NGX_SDK_DIR:-}" &&
      -f "${SDK_CACHE_DIR}/include/nvsdk_ngx.h" ]]; then
  export NGX_SDK_DIR="${SDK_CACHE_DIR}"
fi
if [[ -z "${DLSS_DEMO_DIR:-}" &&
      -f "${MGPU_NGX_CORE_DLL:-}" ]]; then
  export MGPU_NGX_SKIP_OFFICIAL_DEMO="${MGPU_NGX_SKIP_OFFICIAL_DEMO:-1}"
fi

if [[ -z "${MGPU_NGX_B_PROBE_DIR:-}" && "${KEEP_TEMP}" != "1" ]]; then
  trap 'find "${PROBE_DIR}" -depth -delete 2>/dev/null || true' EXIT
fi
mkdir -p "${PROBE_DIR}"

if [[ -z "${PROTON:-}" || ! -x "${PROTON}" ]]; then
  echo "PROTON debe apuntar al launcher Proton ejecutable." >&2
  exit 2
fi
if [[ -z "${VKD3D_DLL_DIR:-}" || ! -f "${VKD3D_DLL_DIR}/d3d12.dll" ||
      ! -f "${VKD3D_DLL_DIR}/d3d12core.dll" ]]; then
  echo "VKD3D_DLL_DIR debe contener d3d12.dll y d3d12core.dll." >&2
  exit 2
fi

set +e
env -u VKD3D_DUPLICATE_LUID_INDEX \
  VKD3D_DUPLICATE_LUID_ADAPTERS=1 \
  MGPU_NGX_SECOND_DEVICE_TEST=1 \
  MGPU_NGX_SECOND_DEVICE_FIRST=1 \
  MGPU_NGX_EVALUATE_SECOND_DEVICE=1 \
  WINEPREFIX="${PROBE_DIR}/wine-prefix" \
  DLSS5_PROTON_PREFIX="${PROBE_DIR}/proton-prefix" \
  "${ROOT_DIR}/scripts/run_ngx_test.sh" >"${LOG}" 2>&1
RUN_RC=$?
set -e

main_a=false
second_b=false
ngx_b=false
queue_b=false
readback_b=false
global_guard=false
if rg -q 'main_device physical_identity hr=0x00000000 .*pci=0:1:0.0' "${LOG}"; then main_a=true; fi
if rg -q 'second_device physical_identity hr=0x00000000 .*pci=0:3:0.0' "${LOG}"; then second_b=true; fi
if rg -q 'Second device EvaluateFeature: 0x00000001 resources=color/output/motion/depth' "${LOG}"; then ngx_b=true; fi
if rg -q 'Second device command submission: close=0x00000000 execute=0x00000000 wait=0x00000000' "${LOG}"; then queue_b=true; fi
if rg -q 'Second device output readback: map=0x00000000 bytes=[0-9]+ nonzero=[1-9][0-9]*' "${LOG}"; then readback_b=true; fi
if rg -q 'NVSDK_NGX_D3D12_CreateFeature: 0xbad00007 handle=null' "${LOG}"; then global_guard=true; fi

printf '{"return_code":%s,"main_gpu_a":%s,"second_gpu_b":%s,"ngx_b_evaluate":%s,"queue_b_cpu_fence":%s,"readback_b_nonzero":%s,"ngx_global_guard":%s,"log":"%s"}\n' \
  "${RUN_RC}" "${main_a}" "${second_b}" "${ngx_b}" "${queue_b}" \
  "${readback_b}" "${global_guard}" "${LOG}"

if [[ "${RUN_RC}" -ne 0 || "${main_a}" != true || "${second_b}" != true ||
      "${ngx_b}" != true || "${queue_b}" != true ||
      "${readback_b}" != true || "${global_guard}" != true ]]; then
  echo "El probe B-first no pasó todos los gates; revisar ${LOG}." >&2
  exit 5
fi
