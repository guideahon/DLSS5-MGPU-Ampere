#!/usr/bin/env bash
set -euo pipefail

# Conservative preflight for the optional D3D12/VKD3D GPU-native path.
# This never enables GPU-native synchronization in the MVP; it only reports
# whether the actual Proton/Wine process exposes the Vulkan semaphore-FD path
# that the VKD3D SPI needs.
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROTON="${PROTON:-}"
VKD3D_DLL_DIR="${VKD3D_DLL_DIR:-${ROOT_DIR}/build/proton-resource-pair-worker-experimental}"
OUT_DIR_INPUT="${OUT_DIR:-}"
PREFIX_INPUT="${WINEPREFIX:-}"
OWN_ROOT=""

if [[ -z "${OUT_DIR_INPUT}" || -z "${PREFIX_INPUT}" ]]; then
  OWN_ROOT="$(mktemp -d /tmp/dlss5-vkd3d-fence-preflight.XXXXXX)"
fi
OUT_DIR="${OUT_DIR_INPUT:-${OWN_ROOT}/out}"
PREFIX="${PREFIX_INPUT:-${OWN_ROOT}/prefix}"
LOG_FILE="${OUT_DIR}/vkd3d-fence-capability.log"

cleanup_owned_temp() {
  if [[ -n "${OWN_ROOT}" ]]; then
    find "${OWN_ROOT}" -depth -delete 2>/dev/null || true
  fi
}
trap cleanup_owned_temp EXIT

if [[ -z "${PROTON}" || ! -x "${PROTON}" ]]; then
  echo "PROTON debe apuntar al launcher Proton ejecutable." >&2
  exit 2
fi
for required in "${VKD3D_DLL_DIR}/d3d12.dll" \
                "${VKD3D_DLL_DIR}/d3d12core.dll"; do
  if [[ ! -f "${required}" ]]; then
    echo "Falta artefacto VKD3D experimental: ${required}" >&2
    exit 2
  fi
done

mkdir -p "${OUT_DIR}" "${PREFIX}"
set +e
env \
  PROTON="${PROTON}" \
  VKD3D_DLL_DIR="${VKD3D_DLL_DIR}" \
  OUT_DIR="${OUT_DIR}" \
  WINEPREFIX="${PREFIX}" \
  VKD3D_EXPORT_FENCE_FD=1 \
  VKD3D_EXPORT_OPAQUE_FD_MEMORY=1 \
  VKD3D_DUPLICATE_LUID_ADAPTERS=1 \
  VKD3D_INTEROP_REQUIRE_DISTINCT=1 \
  WINEDEBUG="${WINEDEBUG:--all}" \
  "${ROOT_DIR}/scripts/run_vkd3d_interop_probe.sh" >"${LOG_FILE}" 2>&1
PROBE_RC=$?
set -e

extension_line="$(rg -m1 'device_extensions count=' "${LOG_FILE}" | tr -d '\r' || true)"
fence_line="$(rg -m1 'vkd3d_fence_fd_exported=' "${LOG_FILE}" | tr -d '\r' || true)"
identity_line="$(rg -m1 'physical_identity_spi=' "${LOG_FILE}" | tr -d '\r' || true)"

ready=false
status="PROBE_FAILED"
reason="El probe VKD3D no terminó correctamente."
if [[ "${PROBE_RC}" -eq 0 && -n "${extension_line}" ]]; then
  if [[ "${extension_line}" == *"external_semaphore_fd=yes"* &&
        "${extension_line}" == *"external_fence_fd=yes"* &&
        "${fence_line}" == *"vkd3d_fence_fd_exported=yes"* ]]; then
    ready=true
    status="GPU_NATIVE_FENCE_READY"
    reason="VKD3D expone semaphore/fence FD y exportó un FD válido."
  else
    status="GPU_NATIVE_FENCE_BLOCKED"
    reason="El proceso Proton/VKD3D no expone VK_KHR_external_semaphore_fd/fence_fd o no exporta un FD válido."
  fi
elif [[ "${PROBE_RC}" -eq 0 ]]; then
  status="GPU_NATIVE_FENCE_UNVERIFIED"
  reason="El probe terminó, pero no produjo el inventario de extensiones VKD3D esperado."
fi

# Los campos derivados son deliberadamente simples y estables para que los
# runners Python/CI puedan consumirlos sin depender del texto completo del log.
printf '{"status":"%s","ready":%s,"probe_rc":%d,"extension_line":"%s","fence_line":"%s","identity_line":"%s","reason":"%s"}\n' \
  "${status}" "${ready}" "${PROBE_RC}" \
  "${extension_line//\\/\\\\}" "${fence_line//\\/\\\\}" \
  "${identity_line//\\/\\\\}" "${reason//\"/\\\"}"

if [[ "${ready}" == "true" ]]; then
  exit 0
fi
exit 1
