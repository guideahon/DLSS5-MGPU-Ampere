#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEMO_DIR="${DLSS_DEMO_DIR:-}"
BRIDGE_DIR="${NGX_BRIDGE_DIR:-${ROOT_DIR}/build/proton}"
RUNTIME_DLL="${DLSS_RUNTIME_DLL:-}"
DLSS_NR_DLL="${DLSS_NR_DLL:-}"
PROTON="${PROTON:-}"
VKD3D_DLL_DIR="${VKD3D_DLL_DIR:-}"
SCENE_FILE="${MGPU_OFFICIAL_HOST_SCENE:-}"
TIMEOUT_SECONDS="${MGPU_OFFICIAL_HOST_TIMEOUT_SECONDS:-45}"
HOST_TMP="${MGPU_OFFICIAL_HOST_DIR:-$(mktemp -d /tmp/dlss5-official-host-probe.XXXXXX)}"
KEEP_HOST="${MGPU_OFFICIAL_HOST_KEEP:-0}"

require_file() {
  local path="$1"
  local label="$2"
  if [[ ! -f "${path}" ]]; then
    echo "Falta ${label}: ${path}" >&2
    exit 2
  fi
}

if [[ -z "${DEMO_DIR}" ]]; then
  echo "DLSS_DEMO_DIR debe apuntar al directorio bin/ngx_dlss_demo." >&2
  exit 2
fi
require_file "${DEMO_DIR}/ngx_dlss_demo.exe" "ngx_dlss_demo.exe"
if [[ -z "${PROTON}" || ! -x "${PROTON}" ]]; then
  echo "PROTON debe apuntar al launcher Proton ejecutable." >&2
  exit 2
fi
if [[ -z "${RUNTIME_DLL}" ]]; then
  RUNTIME_DLL="${DEMO_DIR}/nvngx_dlss.dll"
fi
require_file "${RUNTIME_DLL}" "runtime DLSS limpio"
require_file "${DLSS_NR_DLL}" "runtime DLSSNR"
require_file "${BRIDGE_DIR}/_nvngx.dll" "proxy _nvngx.dll"
require_file "${BRIDGE_DIR}/bridge-nvngx.dll" "bridge-nvngx.dll"
require_file "${VKD3D_DLL_DIR}/d3d12.dll" "d3d12.dll experimental"
require_file "${VKD3D_DLL_DIR}/d3d12core.dll" "d3d12core.dll experimental"
if [[ -n "${SCENE_FILE}" ]]; then
  require_file "${SCENE_FILE}" "escena alternativa"
fi

mkdir -p "${HOST_TMP}/prefix"
cp -a "${DEMO_DIR}/." "${HOST_TMP}/"
if [[ -d "${DEMO_DIR}/../../media" ]]; then
  cp -a "${DEMO_DIR}/../../media" "${HOST_TMP}/media"
fi
cp "${BRIDGE_DIR}/_nvngx.dll" "${HOST_TMP}/nvngx_dlss.dll"
cp "${BRIDGE_DIR}/bridge-nvngx.dll" "${HOST_TMP}/bridge-nvngx.dll"
cp "${RUNTIME_DLL}" "${HOST_TMP}/_nvngx_real.dll"
cp "${RUNTIME_DLL}" "${HOST_TMP}/nvngx_dlss_real.dll"
cp "${DLSS_NR_DLL}" "${HOST_TMP}/nvngx_dlssnr.dll"
cp "${VKD3D_DLL_DIR}/d3d12.dll" "${HOST_TMP}/d3d12.dll"
cp "${VKD3D_DLL_DIR}/d3d12core.dll" "${HOST_TMP}/d3d12core.dll"
if [[ -n "${SCENE_FILE}" ]]; then
  cp "${SCENE_FILE}" "${HOST_TMP}/media/sponza.json"
fi

PROTON_ROOT="$(cd "$(dirname "${PROTON}")" && pwd)"
HOST_LOG="${HOST_TMP}/host.log"
set +e
setsid bash -c 'cd "$1"; shift; exec "$@"' bash "${HOST_TMP}" env \
  STEAM_COMPAT_CLIENT_INSTALL_PATH="${PROTON_ROOT}" \
  STEAM_COMPAT_DATA_PATH="${HOST_TMP}/prefix" \
  UMU_ID=dlss5officialhostprobe UMU_USE_STEAM=0 \
  VKD3D_DUPLICATE_LUID_ADAPTERS=1 \
  VKD3D_DUPLICATE_LUID_INDEX="${VKD3D_DUPLICATE_LUID_INDEX:-0}" \
  VKD3D_VULKAN_DEVICE="${VKD3D_VULKAN_DEVICE:-0}" \
  WINEDEBUG="${MGPU_OFFICIAL_HOST_WINEDEBUG:--all}" \
  "${PROTON}" run ./ngx_dlss_demo.exe -d3d12 -width 640 -height 360 \
  >"${HOST_LOG}" 2>&1 &
HOST_PID=$!
HOST_PGID="$(ps -o pgid= -p "${HOST_PID}" | tr -d ' ')"
HOST_RC=0
HOST_TIMED_OUT=0
HOST_DEADLINE=$((SECONDS + TIMEOUT_SECONDS))
while kill -0 "${HOST_PID}" 2>/dev/null; do
  if (( SECONDS >= HOST_DEADLINE )); then
    HOST_RC=124
    HOST_TIMED_OUT=1
    break
  fi
  sleep 1
done
if (( HOST_TIMED_OUT )); then
  if [[ -n "${HOST_PGID}" && "${HOST_PGID}" != "0" && "${HOST_PGID}" != "$$" ]]; then
    kill -TERM -- "-${HOST_PGID}" 2>/dev/null || true
    sleep 2
    kill -KILL -- "-${HOST_PGID}" 2>/dev/null || true
  else
    kill -TERM "${HOST_PID}" 2>/dev/null || true
  fi
fi
wait "${HOST_PID}" 2>/dev/null
WAIT_RC=$?
if (( ! HOST_TIMED_OUT )); then
  HOST_RC=${WAIT_RC}
fi
set -e

started=false
device_created=false
ngx_loaded=false
bridge_log=false
bridge_evaluated=false
if rg -q 'ngx_dlss_demo\.exe|Executable is inside wine prefix|VKD3D create device selected' "${HOST_LOG}"; then started=true; fi
if rg -q 'VKD3D create device selected' "${HOST_LOG}"; then device_created=true; fi
if rg -qi 'nvngx_dlss\.dll' "${HOST_LOG}"; then ngx_loaded=true; fi
if [[ -f "${HOST_TMP}/dlssnr-proxy.log" ]]; then
  bridge_log=true
  if rg -q 'DLSS standard EvaluateFeature result=0x00000001|DLSSNR Evaluate result=0x00000001' \
      "${HOST_TMP}/dlssnr-proxy.log"; then
    bridge_evaluated=true
  fi
fi

printf '{"return_code":%s,"started":%s,"device_created":%s,"ngx_loaded":%s,"bridge_log":%s,"bridge_evaluated":%s,"host_dir":"%s","host_log":"%s"}\n' \
  "${HOST_RC}" "${started}" "${device_created}" "${ngx_loaded}" \
  "${bridge_log}" "${bridge_evaluated}" "${HOST_TMP}" "${HOST_LOG}"

if [[ "${MGPU_OFFICIAL_HOST_REQUIRE_NGX:-0}" == "1" &&
      ( "${bridge_evaluated}" != true || "${HOST_RC}" -ne 0 ) ]]; then
  echo "El host oficial no alcanzó una evaluación NGX dentro del watchdog." >&2
  exit 5
fi

if [[ "${MGPU_OFFICIAL_HOST_DIR:-}" == "" && "${KEEP_HOST}" != "1" ]]; then
  rm -rf "${HOST_TMP}"
fi
