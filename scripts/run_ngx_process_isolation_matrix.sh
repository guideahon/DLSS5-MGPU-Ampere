#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE_TMP="${MGPU_NGX_ISOLATION_DIR:-$(mktemp -d /tmp/dlss5-ngx-process-isolation.XXXXXX)}"
KEEP_LOGS="${MGPU_NGX_KEEP_LOGS:-0}"
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
  if rg -q 'NVSDK_NGX_D3D12_EvaluateFeature: 0x00000001' "${log}"; then
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
