#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEMO_DIR="${DLSS_DEMO_DIR:-}"
BRIDGE_DIR="${NGX_BRIDGE_DIR:-${ROOT_DIR}/build/proton}"
RUNTIME_DLL="${DLSS_RUNTIME_DLL:-}"
DLSS_NR_DLL="${DLSS_NR_DLL:-}"
CORE_DLL="${MGPU_OFFICIAL_HOST_CORE_DLL:-}"
PROTON="${PROTON:-}"
VKD3D_DLL_DIR="${VKD3D_DLL_DIR:-}"
SCENE_FILE="${MGPU_OFFICIAL_HOST_SCENE:-}"
SHADER_DIR="${MGPU_OFFICIAL_HOST_SHADER_DIR:-}"
MINGW_RUNTIME_DIR="${MGPU_OFFICIAL_HOST_MINGW_RUNTIME_DIR:-}"
TIMEOUT_SECONDS="${MGPU_OFFICIAL_HOST_TIMEOUT_SECONDS:-45}"
HOST_TMP="${MGPU_OFFICIAL_HOST_DIR:-$(mktemp -d /tmp/dlss5-official-host-probe.XXXXXX)}"
KEEP_HOST="${MGPU_OFFICIAL_HOST_KEEP:-0}"
HOST_TRACE_NGX="${MGPU_OFFICIAL_HOST_TRACE_NGX:-}"
HOST_BYPASS_DLSS_AVAILABLE="${MGPU_OFFICIAL_HOST_BYPASS_DLSS_AVAILABLE:-}"
HOST_TRUST_NGX_INIT="${MGPU_OFFICIAL_HOST_TRUST_NGX_INIT:-}"
HOST_MINIMAL_EVAL="${MGPU_OFFICIAL_HOST_MINIMAL_EVAL:-}"
DLSSNR_TRANSPORT="${MGPU_DLSSNR_TRANSPORT:-}"
DLSSNR_PARAMETER_PROBE="${MGPU_DLSSNR_PARAMETER_PROBE:-}"
DLSSNR_FENCE_PROBE="${MGPU_DLSSNR_FENCE_PROBE:-}"
CUDA_IMPORT_HELPER="${MGPU_CUDA_IMPORT_HELPER:-}"
VULKAN_IMAGE_IMPORT_HELPER="${MGPU_VULKAN_IMAGE_IMPORT_HELPER:-}"
CUDA_SOURCE_ORDINAL="${MGPU_CUDA_SOURCE_ORDINAL:-}"
CUDA_DESTINATION_ORDINAL="${MGPU_CUDA_DESTINATION_ORDINAL:-}"
EXPORT_OPAQUE_FD_MEMORY="${VKD3D_EXPORT_OPAQUE_FD_MEMORY:-}"
EXPORT_HEAP_FD="${VKD3D_EXPORT_HEAP_FD:-}"
EXPORT_FENCE_FD="${VKD3D_EXPORT_FENCE_FD:-}"
CUDA_HELPER_LOG="${MGPU_CUDA_HELPER_LOG:-}"
HOST_LD_PRELOAD="${MGPU_OFFICIAL_HOST_LD_PRELOAD:-}"

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

if [[ -z "${SHADER_DIR}" ]]; then
  for candidate_shader_dir in \
      "${DEMO_DIR}/../donut/shaders" \
      "${DEMO_DIR}/../../donut/shaders"; do
    if [[ -d "${candidate_shader_dir}" ]]; then
      SHADER_DIR="${candidate_shader_dir}"
      break
    fi
  done
fi
if [[ -n "${MGPU_OFFICIAL_HOST_SHADER_DIR:-}" && ! -d "${SHADER_DIR}" ]]; then
  echo "MGPU_OFFICIAL_HOST_SHADER_DIR no existe: ${SHADER_DIR}" >&2
  exit 2
fi

if LC_ALL=C grep -a -Eq '_nvngx_real\.dll|bridge-nvngx\.dll' "${RUNTIME_DLL}"; then
  echo "DLSS_RUNTIME_DLL es un proxy/bridge; debe ser el runtime limpio nvngx_dlss.dll." >&2
  exit 2
fi

PROTON_ROOT="$(cd "$(dirname "${PROTON}")" && pwd)"
CORE_PREFIX="${HOST_TMP}/core-prefix"
if [[ -z "${CORE_DLL}" ]]; then
  CORE_DLL="${CORE_PREFIX}/pfx/drive_c/windows/system32/_nvngx.dll"
fi
if [[ ! -f "${CORE_DLL}" ]]; then
  mkdir -p "${CORE_PREFIX}"
  set +e
  timeout "${MGPU_OFFICIAL_HOST_CORE_TIMEOUT_SECONDS:-12}s" env \
    STEAM_COMPAT_CLIENT_INSTALL_PATH="${PROTON_ROOT}" \
    STEAM_COMPAT_DATA_PATH="${CORE_PREFIX}" \
    UMU_ID=dlss5officialhostbootstrap UMU_USE_STEAM=0 \
    MGPU_OFFICIAL_HOST_SKIP_HIGH_LEVEL="${MGPU_OFFICIAL_HOST_SKIP_HIGH_LEVEL:-}" \
    WINEDEBUG="${MGPU_OFFICIAL_HOST_CORE_WINEDEBUG:--all}" \
    "${PROTON}" run "${DEMO_DIR}/ngx_dlss_demo.exe" -d3d12 -width 320 -height 180 \
    >"${CORE_PREFIX}/bootstrap.log" 2>&1
  set -e
fi
require_file "${CORE_DLL}" "core _nvngx.dll generado por Proton"

mkdir -p "${HOST_TMP}/prefix"
cp -a "${DEMO_DIR}/." "${HOST_TMP}/"
if [[ -d "${DEMO_DIR}/../../media" ]]; then
  cp -a "${DEMO_DIR}/../../media" "${HOST_TMP}/media"
fi
cp "${BRIDGE_DIR}/_nvngx.dll" "${HOST_TMP}/nvngx_dlss.dll"
cp "${BRIDGE_DIR}/bridge-nvngx.dll" "${HOST_TMP}/bridge-nvngx.dll"
cp "${CORE_DLL}" "${HOST_TMP}/_nvngx_real.dll"
cp "${RUNTIME_DLL}" "${HOST_TMP}/nvngx_dlss_real.dll"
cp "${DLSS_NR_DLL}" "${HOST_TMP}/nvngx_dlssnr.dll"
cp "${VKD3D_DLL_DIR}/d3d12.dll" "${HOST_TMP}/d3d12.dll"
cp "${VKD3D_DLL_DIR}/d3d12core.dll" "${HOST_TMP}/d3d12core.dll"
if [[ -n "${SHADER_DIR}" && -d "${SHADER_DIR}" ]]; then
  mkdir -p "${HOST_TMP}/donut/shaders"
  cp -a "${SHADER_DIR}/." "${HOST_TMP}/donut/shaders/"
fi

# A host cross-built with MinGW may import the dynamic C++ runtime.  Stage it
# automatically when available; this is harmless for an MSVC/native host and
# avoids a misleading Wine exit code before the first diagnostic stage.
if [[ -n "${MINGW_RUNTIME_DIR}" ]]; then
  for runtime_dll in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
    require_file "${MINGW_RUNTIME_DIR}/${runtime_dll}" "runtime MinGW ${runtime_dll}"
    cp "${MINGW_RUNTIME_DIR}/${runtime_dll}" "${HOST_TMP}/${runtime_dll}"
  done
elif command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
  for runtime_dll in libgcc_s_seh-1.dll libstdc++-6.dll; do
    runtime_path="$(x86_64-w64-mingw32-g++ -print-file-name="${runtime_dll}")"
    if [[ -f "${runtime_path}" ]]; then
      cp "${runtime_path}" "${HOST_TMP}/${runtime_dll}"
    fi
  done
  runtime_path="/usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll"
  if [[ -f "${runtime_path}" ]]; then
    cp "${runtime_path}" "${HOST_TMP}/libwinpthread-1.dll"
  fi
fi
if [[ -n "${SCENE_FILE}" ]]; then
  cp "${SCENE_FILE}" "${HOST_TMP}/media/sponza.json"
fi

if [[ -n "${HOST_LD_PRELOAD}" ]]; then
  export LD_PRELOAD="${HOST_LD_PRELOAD}${LD_PRELOAD:+:${LD_PRELOAD}}"
fi

HOST_LOG="${HOST_TMP}/host.log"
set +e
setsid bash -c 'cd "$1"; shift; exec "$@"' bash "${HOST_TMP}" env \
  STEAM_COMPAT_CLIENT_INSTALL_PATH="${PROTON_ROOT}" \
  STEAM_COMPAT_DATA_PATH="${HOST_TMP}/prefix" \
  UMU_ID=dlss5officialhostprobe UMU_USE_STEAM=0 \
  VKD3D_DUPLICATE_LUID_ADAPTERS=1 \
  VKD3D_DUPLICATE_LUID_INDEX="${VKD3D_DUPLICATE_LUID_INDEX:-0}" \
  VKD3D_VULKAN_DEVICE="${VKD3D_VULKAN_DEVICE:-0}" \
  MGPU_OFFICIAL_HOST_SKIP_HIGH_LEVEL="${MGPU_OFFICIAL_HOST_SKIP_HIGH_LEVEL:-}" \
  MGPU_OFFICIAL_HOST_TRACE_NGX="${HOST_TRACE_NGX}" \
  MGPU_OFFICIAL_HOST_BYPASS_DLSS_AVAILABLE="${HOST_BYPASS_DLSS_AVAILABLE}" \
  MGPU_OFFICIAL_HOST_TRUST_NGX_INIT="${HOST_TRUST_NGX_INIT}" \
  MGPU_OFFICIAL_HOST_MINIMAL_EVAL="${HOST_MINIMAL_EVAL}" \
  MGPU_DLSSNR_TRANSPORT="${DLSSNR_TRANSPORT}" \
  MGPU_DLSSNR_PARAMETER_PROBE="${DLSSNR_PARAMETER_PROBE}" \
  MGPU_DLSSNR_FENCE_PROBE="${DLSSNR_FENCE_PROBE}" \
  MGPU_CUDA_IMPORT_HELPER="${CUDA_IMPORT_HELPER}" \
  MGPU_VULKAN_IMAGE_IMPORT_HELPER="${VULKAN_IMAGE_IMPORT_HELPER}" \
  MGPU_CUDA_SOURCE_ORDINAL="${CUDA_SOURCE_ORDINAL}" \
  MGPU_CUDA_DESTINATION_ORDINAL="${CUDA_DESTINATION_ORDINAL}" \
  VKD3D_EXPORT_OPAQUE_FD_MEMORY="${EXPORT_OPAQUE_FD_MEMORY}" \
  VKD3D_EXPORT_HEAP_FD="${EXPORT_HEAP_FD}" \
  VKD3D_EXPORT_FENCE_FD="${EXPORT_FENCE_FD}" \
  MGPU_CUDA_HELPER_LOG="${CUDA_HELPER_LOG}" \
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
  # WINEDEBUG=-all intentionally hides DLL loader lines; a bridge log means
  # the NGX proxy was loaded even when host.log contains no DLL name.
  ngx_loaded=true
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
