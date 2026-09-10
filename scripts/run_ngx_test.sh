#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
DEMO_DIR="${DLSS_DEMO_DIR:-}"
PREFIX="${WINEPREFIX:-/tmp/dlss5-wine64-final}"
TIMEOUT_SECONDS="${NGX_TEST_TIMEOUT_SECONDS:-20}"
VKD3D_DLL_DIR="${VKD3D_DLL_DIR:-}"

if [[ -z "${DEMO_DIR}" || ! -f "${DEMO_DIR}/ngx_dlss_demo" || ! -f "${DEMO_DIR}/nvngx_dlss.dll" ]]; then
  echo "DLSS_DEMO_DIR debe apuntar a bin/ngx_dlss_demo del release oficial de NVIDIA." >&2
  exit 2
fi
if [[ ! -f "${BUILD_DIR}/proton/_nvngx.dll" || ! -f "${BUILD_DIR}/proton/bridge-nvngx.dll" ]]; then
  echo "Faltan los DLL del bridge. Ejecutá scripts/build_bridge.sh primero." >&2
  exit 2
fi
if [[ -n "${VKD3D_DLL_DIR}" &&
      ( ! -f "${VKD3D_DLL_DIR}/d3d12.dll" || ! -f "${VKD3D_DLL_DIR}/d3d12core.dll" ) ]]; then
  echo "VKD3D_DLL_DIR debe contener d3d12.dll y d3d12core.dll." >&2
  exit 2
fi
if ! command -v wine >/dev/null 2>&1; then
  echo "No se encontró Wine." >&2
  exit 2
fi
mkdir -p "${PREFIX}"

echo "=== NGX Linux oficial ==="
LINUX_LOG="$(mktemp /tmp/dlss5-ngx-linux.XXXXXX.log)"
set +e
timeout "${TIMEOUT_SECONDS}s" env \
  DISPLAY="${DISPLAY:-:0}" \
  XAUTHORITY="${XAUTHORITY:-/var/run/lightdm/root/:0}" \
  LD_LIBRARY_PATH="${DEMO_DIR}:${LD_LIBRARY_PATH:-}" \
  "${DEMO_DIR}/ngx_dlss_demo" -w 1280 -h 720 >"${LINUX_LOG}" 2>&1
LINUX_RC=$?
set -e
echo "return_code=${LINUX_RC} log=${LINUX_LOG}"
rg -m 8 'Minimum driver|GetFeatureRequirements|VULKAN_GetFeature|warning|error' "${LINUX_LOG}" || true

echo
echo "=== Carga del proxy Windows bajo Wine ==="
TEST_DIR="$(mktemp -d /tmp/dlss5-ngx-bridge.XXXXXX)"
cp -a "${DEMO_DIR}/." "${TEST_DIR}/"
cp "${BUILD_DIR}/proton/_nvngx.dll" "${TEST_DIR}/nvngx_dlss.dll"
cp "${BUILD_DIR}/proton/bridge-nvngx.dll" "${TEST_DIR}/bridge-nvngx.dll"
cp "${DEMO_DIR}/nvngx_dlss.dll" "${TEST_DIR}/_nvngx_real.dll"
cp "${DEMO_DIR}/nvngx_dlss.dll" "${TEST_DIR}/nvngx_dlss_real.dll"
if [[ -n "${VKD3D_DLL_DIR}" ]]; then
  cp "${VKD3D_DLL_DIR}/d3d12.dll" "${TEST_DIR}/d3d12.dll"
  cp "${VKD3D_DLL_DIR}/d3d12core.dll" "${TEST_DIR}/d3d12core.dll"
fi

if [[ -z "${NGX_SDK_DIR:-}" || ! -f "${NGX_SDK_DIR}/include/nvsdk_ngx.h" ]]; then
  echo "NGX_SDK_DIR no apunta a headers válidos; se omite la compilación del smoke D3D12." >&2
  exit 2
fi

x86_64-w64-mingw32-gcc -O2 "${ROOT_DIR}/tests/ngx_loader_smoke.c" \
  -o "${TEST_DIR}/ngx_loader_smoke.exe"
x86_64-w64-mingw32-g++ -O2 -std=c++17 -I"${NGX_SDK_DIR}/include" \
  "${ROOT_DIR}/tests/ngx_d3d12_smoke.cpp" \
  -o "${TEST_DIR}/ngx_d3d12_smoke.exe" -ld3d12 -ldxgi

(
  cd "${TEST_DIR}"
  env DISPLAY="${DISPLAY:-:0}" \
    XAUTHORITY="${XAUTHORITY:-/var/run/lightdm/root/:0}" \
    WINEPREFIX="${PREFIX}" WINEDEBUG=-all \
    wine ./ngx_loader_smoke.exe
)

set +e
(
  cd "${TEST_DIR}"
  timeout "${TIMEOUT_SECONDS}s" env DISPLAY="${DISPLAY:-:0}" \
    XAUTHORITY="${XAUTHORITY:-/var/run/lightdm/root/:0}" \
    WINEPREFIX="${PREFIX}" VKD3D_VULKAN_DEVICE="${VKD3D_VULKAN_DEVICE:-0}" \
    WINEDEBUG=-all wine ./ngx_d3d12_smoke.exe
)
D3D12_RC=$?
set -e
echo "d3d12_smoke_return_code=${D3D12_RC} test_dir=${TEST_DIR}"
if [[ -f "${TEST_DIR}/dlssnr-proxy.log" ]]; then
  sed -n '1,120p' "${TEST_DIR}/dlssnr-proxy.log"
fi
echo "Nota: la ausencia de nvngx_dlssnr.dll es intencional en esta prueba negativa."

if [[ -n "${PROTON:-}" ]]; then
  if [[ ! -x "${PROTON}" ]]; then
    echo "PROTON no apunta a un launcher ejecutable: ${PROTON}" >&2
    exit 2
  fi
  if [[ -z "${DLSS_NR_DLL:-}" || ! -f "${DLSS_NR_DLL}" ]]; then
    echo "La prueba positiva requiere DLSS_NR_DLL apuntando a un nvngx_dlssnr.dll local." >&2
    exit 2
  fi

  echo
  echo "=== Cadena positiva NGX bajo Proton/VKD3D-Proton ==="
  POSITIVE_DIR="$(mktemp -d /tmp/dlss5-ngx-positive.XXXXXX)"
  POSITIVE_PREFIX="${DLSS5_PROTON_PREFIX:-/tmp/dlss5-proton-ngx-positive}"
  PROTON_ROOT="$(cd "$(dirname "${PROTON}")" && pwd)"
  mkdir -p "${POSITIVE_PREFIX}"

  # Inicializa el prefix para que GE-Proton instale su _nvngx.dll core.
  # El ejecutable oficial sólo se usa como bootstrap aislado; no se modifica.
  if [[ ! -f "${POSITIVE_PREFIX}/pfx/drive_c/windows/system32/_nvngx.dll" ]]; then
    set +e
    timeout 8s env \
      STEAM_COMPAT_CLIENT_INSTALL_PATH="${PROTON_ROOT}" \
      STEAM_COMPAT_DATA_PATH="${POSITIVE_PREFIX}" \
      UMU_ID=dlss5ngxbootstrap UMU_USE_STEAM=0 \
      WINEDEBUG=-all \
      "${PROTON}" run "${DEMO_DIR}/ngx_dlss_demo.exe" -d3d12 >/dev/null 2>&1
    set -e
  fi

  CORE_DLL="${POSITIVE_PREFIX}/pfx/drive_c/windows/system32/_nvngx.dll"
  if [[ ! -f "${CORE_DLL}" ]]; then
    echo "GE-Proton no generó el core esperado: ${CORE_DLL}" >&2
    exit 2
  fi

  cp "${BUILD_DIR}/proton/_nvngx.dll" "${POSITIVE_DIR}/nvngx_dlss.dll"
  cp "${BUILD_DIR}/proton/bridge-nvngx.dll" "${POSITIVE_DIR}/bridge-nvngx.dll"
  cp "${CORE_DLL}" "${POSITIVE_DIR}/_nvngx_real.dll"
  cp "${DEMO_DIR}/nvngx_dlss.dll" "${POSITIVE_DIR}/nvngx_dlss_real.dll"
  cp "${DLSS_NR_DLL}" "${POSITIVE_DIR}/nvngx_dlssnr.dll"
  cp "${TEST_DIR}/ngx_d3d12_smoke.exe" "${POSITIVE_DIR}/ngx_d3d12_smoke.exe"
  if [[ -n "${VKD3D_DLL_DIR}" ]]; then
    cp "${VKD3D_DLL_DIR}/d3d12.dll" "${POSITIVE_DIR}/d3d12.dll"
    cp "${VKD3D_DLL_DIR}/d3d12core.dll" "${POSITIVE_DIR}/d3d12core.dll"
  fi

  set +e
  (
    cd "${POSITIVE_DIR}"
    env STEAM_COMPAT_CLIENT_INSTALL_PATH="${PROTON_ROOT}" \
      STEAM_COMPAT_DATA_PATH="${POSITIVE_PREFIX}" \
      UMU_ID=dlss5ngxpositive UMU_USE_STEAM=0 \
      NVIDIA_WINE_DLL_DIR="${POSITIVE_DIR}" \
      MGPU_NGX_SECOND_DEVICE_TEST="${MGPU_NGX_SECOND_DEVICE_TEST:-}" \
      VKD3D_DEBUG="${VKD3D_DEBUG:-none}" WINEDEBUG=-all \
      "${PROTON}" run ./ngx_d3d12_smoke.exe
  )
  POSITIVE_RC=$?
  set -e
  echo "positive_d3d12_smoke_return_code=${POSITIVE_RC} test_dir=${POSITIVE_DIR}"
  if [[ -f "${POSITIVE_DIR}/ngx_d3d12_smoke.result.txt" ]]; then
    sed -n '1,240p' "${POSITIVE_DIR}/ngx_d3d12_smoke.result.txt"
  fi
  if [[ -f "${POSITIVE_DIR}/dlssnr-proxy.log" ]]; then
    sed -n '1,240p' "${POSITIVE_DIR}/dlssnr-proxy.log"
  fi
fi
