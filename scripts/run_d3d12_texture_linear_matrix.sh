#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROTON="${PROTON:-}"
VKD3D_DLL_DIR="${VKD3D_DLL_DIR:-${ROOT_DIR}/build/proton}"
BASE_TMP="$(mktemp -d /tmp/dlss5-d3d12-linear-matrix.XXXXXX)"
trap 'rm -rf "${BASE_TMP}"' EXIT

if [[ -z "${PROTON}" || ! -x "${PROTON}" ]]; then
  echo "PROTON debe apuntar al launcher Proton ejecutable." >&2
  exit 2
fi

run_direction() {
  local source="$1"
  local destination="$2"
  local log="${BASE_TMP}/${source}-${destination}.log"
  local rc=0
  set +e
  VKD3D_DUPLICATE_LUID_ADAPTERS=1 \
  VKD3D_DUPLICATE_LUID_INDEX="${source}" \
  MGPU_D3D12_ADAPTER_INDEX="${source}" \
  MGPU_CUDA_SOURCE_ORDINAL="${source}" \
  MGPU_CUDA_DESTINATION_ORDINAL="${destination}" \
  PROTON="${PROTON}" VKD3D_DLL_DIR="${VKD3D_DLL_DIR}" \
  WINEPREFIX="${BASE_TMP}/prefix-${source}-${destination}" \
    "${ROOT_DIR}/scripts/run_d3d12_texture_linear_smoke.sh" >"${log}" 2>&1
  rc=$?
  set -e
  local exported=false
  local validated=false
  if rg -q 'd3d12_linear_export hr=0x00000000' "${log}"; then exported=true; fi
  if rg -q 'cuda_readback_validation=ok' "${log}"; then validated=true; fi
  printf '{"source":%s,"destination":%s,"launcher_return":%s,"heap_export":%s,"readback_validation":%s}\n' \
    "${source}" "${destination}" "${rc}" "${exported}" "${validated}"
  [[ "${rc}" -eq 0 && "${exported}" == true && "${validated}" == true ]]
}

run_direction 0 1
run_direction 1 0
