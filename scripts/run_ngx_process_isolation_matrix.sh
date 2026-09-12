#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE_TMP="${MGPU_NGX_ISOLATION_DIR:-$(mktemp -d /tmp/dlss5-ngx-process-isolation.XXXXXX)}"
KEEP_LOGS="${MGPU_NGX_KEEP_LOGS:-0}"

# Reuse an existing project profile when the official NVIDIA demo is not
# installed. Explicit variables remain authoritative; this block only fills
# missing values and never downloads or edits proprietary files.
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
mkdir -p "${BASE_TMP}"
if [[ "${MGPU_NGX_ISOLATION_DIR:-}" == "" && "${KEEP_LOGS}" != "1" ]]; then
  trap 'rm -rf "${BASE_TMP}"' EXIT
fi

if [[ -z "${PROTON:-}" || ! -x "${PROTON}" ]]; then
  echo "PROTON debe apuntar al launcher Proton ejecutable." >&2
  exit 2
fi
if [[ -z "${VKD3D_DLL_DIR:-}" ]]; then
  echo "VKD3D_DLL_DIR debe apuntar al build experimental con d3d12.dll/d3d12core.dll." >&2
  exit 2
fi
if [[ ! -f "${VKD3D_DLL_DIR}/d3d12.dll" || ! -f "${VKD3D_DLL_DIR}/d3d12core.dll" ]]; then
  echo "VKD3D_DLL_DIR no contiene d3d12.dll y d3d12core.dll: ${VKD3D_DLL_DIR}" >&2
  exit 2
fi

run_gpu() {
  local gpu="$1"
  local expected_pci="$2"
  local log="${BASE_TMP}/gpu-${gpu}.log"
  local prefix="${BASE_TMP}/proton-${gpu}"
  local wineprefix="${BASE_TMP}/wine-${gpu}"
  local rc=0

  set +e
  VKD3D_DUPLICATE_LUID_ADAPTERS=1 \
  VKD3D_DUPLICATE_LUID_INDEX="${gpu}" \
  VKD3D_VULKAN_DEVICE="${gpu}" \
  MGPU_NGX_SECOND_DEVICE_TEST= \
  DLSS5_PROTON_PREFIX="${prefix}" \
  WINEPREFIX="${wineprefix}" \
    "${ROOT_DIR}/scripts/run_ngx_test.sh" >"${log}" 2>&1
  rc=$?
  set -e

  local identity=false
  local ngx_eval=false
  local nr_eval=false
  local positive=false
  if rg -q "main_device physical_identity hr=0x00000000 .*pci=${expected_pci}" "${log}"; then
    identity=true
  fi
  # Depending on the selected runtime, the probe reports the standard NGX
  # call as either the raw export or the bridge's structured line. Require a
  # successful HRESULT in one of those forms; NR success remains a separate
  # gate below.
  if rg -q 'NVSDK_NGX_D3D12_EvaluateFeature: 0x00000001|DLSS standard EvaluateFeature result=0x00000001|Second device EvaluateFeature: 0x00000001' "${log}"; then
    ngx_eval=true
  fi
  if rg -q 'DLSSNR Evaluate result=0x00000001' "${log}"; then
    nr_eval=true
  fi
  if rg -q 'positive_d3d12_smoke_return_code=0([[:space:]]|$)' "${log}"; then
    positive=true
  fi
  printf '{"gpu":%s,"expected_pci":"%s","return_code":%s,"physical_identity":%s,"ngx_evaluate":%s,"nr_evaluate":%s,"positive_process":%s,"log":"%s"}\n' \
    "${gpu}" "${expected_pci}" "${rc}" "${identity}" "${ngx_eval}" \
    "${nr_eval}" "${positive}" "${log}"
  [[ "${rc}" -eq 0 && "${identity}" == true && "${ngx_eval}" == true &&
     "${nr_eval}" == true && "${positive}" == true ]]
}

run_gpu 0 "0:1:0.0"
run_gpu 1 "0:3:0.0"
echo "process_isolation_matrix=passed logs=${BASE_TMP}" 
