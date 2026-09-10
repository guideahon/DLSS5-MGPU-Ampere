#!/usr/bin/env bash
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROTON="${PROTON:-}"
VKD3D_DLL_DIR="${VKD3D_DLL_DIR:-}"
HELPER="${MGPU_CUDA_IMPORT_HELPER:-${ROOT_DIR}/build/cuda_external_import_helper}"

if [[ -z "${PROTON}" || ! -x "${PROTON}" ]]; then
  echo "PROTON debe apuntar al launcher Proton ejecutable." >&2
  exit 2
fi
if [[ -z "${VKD3D_DLL_DIR}" || ! -f "${VKD3D_DLL_DIR}/d3d12.dll" ||
      ! -f "${VKD3D_DLL_DIR}/d3d12core.dll" ]]; then
  echo "VKD3D_DLL_DIR debe contener d3d12.dll y d3d12core.dll." >&2
  exit 2
fi

"${ROOT_DIR}/scripts/build_cuda_external_import_helper.sh" >/dev/null || exit 3
"${ROOT_DIR}/scripts/build_fd_inherit_shim.sh" >/dev/null || exit 3

native_ok=1
for direction in "0 1" "1 0"; do
  read -r source destination <<<"${direction}"
  output="$(${ROOT_DIR}/build/mgpu-vulkan-cuda-probe \
      --vulkan-gpu "${source}" --cuda-source "${source}" \
      --cuda-destination "${destination}" --bytes 65536 2>&1)"
  rc=$?
  printf '%s\n' "${output}"
  if [[ ${rc} -ne 0 ]] || ! grep -q 'validation=ok' <<<"${output}"; then
    native_ok=0
  fi
done

set +e
proton_output="$(
  MGPU_D3D12_ADAPTER_INDEX=0 \
  VKD3D_DUPLICATE_LUID_INDEX=0 \
  VKD3D_DUPLICATE_LUID_ADAPTERS=1 \
  VKD3D_EXPORT_OPAQUE_FD_MEMORY=1 \
  MGPU_CUDA_IMPORT_HELPER="${HELPER}" \
  PROTON="${PROTON}" VKD3D_DLL_DIR="${VKD3D_DLL_DIR}" \
  WINEDLLOVERRIDES='d3d12=n,b;d3d12core=n,b' \
  "${ROOT_DIR}/scripts/run_vkd3d_vk_export_smoke.sh" 2>&1
)"
proton_rc=$?
set -e
printf '%s\n' "${proton_output}"

proton_ok=0
if [[ ${proton_rc} -eq 0 ]] && grep -q 'cuda_helper_p2p_validation=ok' <<<"${proton_output}"; then
  proton_ok=1
fi

if [[ ${native_ok} -eq 1 && ${proton_ok} -eq 1 ]]; then
  status="READY_REMOTE_TRANSPORT"
else
  status="READY_LOCAL_ONLY"
fi

printf '{"status":"%s","native_vulkan_cuda_p2p":%s,"proton_export_import_p2p":%s,"game_launch":"disabled"}\n' \
  "${status}" "${native_ok}" "${proton_ok}"
[[ "${status}" == "READY_REMOTE_TRANSPORT" ]]
