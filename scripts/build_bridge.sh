#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE_SOURCE="${DLSS5_BRIDGE_SOURCE:-${ROOT}/third_party/dlss5-linux-bridge}"

if [[ ! -x "${BRIDGE_SOURCE}/build.sh" ]]; then
  echo "No se encontró dlss5-linux-bridge en ${BRIDGE_SOURCE}." >&2
  echo "Clonalo desde https://github.com/ccoredesenvolvimento/dlss5-linux-bridge" >&2
  exit 2
fi
if [[ -z "${NGX_SDK_DIR:-}" ]]; then
  echo "NGX_SDK_DIR debe apuntar a headers DLSS/NGX obtenidos legalmente." >&2
  exit 2
fi

export CXX="${CXX:-x86_64-w64-mingw32-g++}"
export OUT_DIR="${OUT_DIR:-${ROOT}/build/proton}"
cd "${BRIDGE_SOURCE}"
PATCH_FILES=(
  "${ROOT}/patches/dlss5-linux-bridge-transport-probe.patch"
  "${ROOT}/patches/dlss5-linux-bridge-fd-probe.patch"
  "${ROOT}/patches/dlss5-linux-bridge-eval-fallback.patch"
  "${ROOT}/patches/dlss5-linux-bridge-fence-probe.patch"
  "${ROOT}/patches/dlss5-linux-bridge-host-resource-registry.patch"
  "${ROOT}/patches/dlss5-linux-bridge-resource-fd-probe.patch"
  "${ROOT}/patches/dlss5-linux-bridge-resource-fd-worker.patch"
)
for patch_file in "${PATCH_FILES[@]}"; do
  PATCH_APPLY_ARGS=()
  if [[ "${patch_file}" == *resource-fd-probe.patch ]]; then
    # This optional insertion-only extension targets multiple upstream patch
    # layouts; zero-context application keeps it portable across those layouts.
    PATCH_APPLY_ARGS=(--unidiff-zero)
  fi
  if git apply "${PATCH_APPLY_ARGS[@]}" --check "${patch_file}" >/dev/null 2>&1; then
    git apply "${PATCH_APPLY_ARGS[@]}" "${patch_file}"
  elif [[ "${patch_file}" == *transport-probe.patch ]] &&
       rg -q 'transport_probe|GetVulkanResourceInfo1' src/core_proxy.cpp; then
    echo "El probe de transporte ya está aplicado; se conserva y se continúa." >&2
  elif [[ "${patch_file}" == *dlss5-linux-bridge-fd-probe.patch ]] &&
       rg -q 'fd_probe|ExportVulkanHeapFd' src/core_proxy.cpp; then
    echo "El probe FD ya está aplicado; se conserva y se continúa." >&2
  elif [[ "${patch_file}" == *eval-fallback.patch ]] &&
       rg -q 'MGPU_DLSSNR_FALLBACK|NormalizeDlssEvaluationParameters' src/core_proxy.cpp; then
    echo "El fallback de evaluación ya está aplicado; se conserva y se continúa." >&2
  elif [[ "${patch_file}" == *fence-probe.patch ]] &&
       rg -q 'ProbeFenceFd|fence_probe_done' src/core_proxy.cpp; then
    echo "El probe de fences ya está aplicado; se conserva y se continúa." >&2
  elif [[ "${patch_file}" == *host-resource-registry.patch ]] &&
       rg -q 'NVSDK_NGX_Compat_GetD3D12Resource' src/core_proxy.cpp; then
    echo "El registro de recursos del host ya está aplicado; se conserva y se continúa." >&2
  elif [[ "${patch_file}" == *resource-fd-probe.patch ]] &&
       rg -q 'TransportResourceFdProbeEnabled|ExportVulkanResourceFd|resource_fd_probe_done' src/core_proxy.cpp; then
    echo "El probe de exportación directa de recursos ya está aplicado; se conserva y se continúa." >&2
  elif [[ "${patch_file}" == *resource-fd-worker.patch ]] &&
       rg -q 'TransportResourceFdWorkerEnabled|resource_fd_worker_socket|--source-daemon' src/core_proxy.cpp; then
    echo "El worker persistente de resource-FD ya está aplicado; se conserva y se continúa." >&2
  else
    echo "No se pudo aplicar el parche del bridge: ${patch_file}" >&2
    exit 3
  fi
done
exec ./build.sh
