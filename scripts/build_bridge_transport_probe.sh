#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE_SOURCE="${DLSS5_BRIDGE_SOURCE:-/tmp/dlss5-linux-bridge-clean}"
PATCH_FILES=(
  "${ROOT}/patches/dlss5-linux-bridge-transport-probe.patch"
  "${ROOT}/patches/dlss5-linux-bridge-fd-probe.patch"
)
OUT_DIR="${OUT_DIR:-${ROOT}/build/proton-transport-probe}"

if [[ ! -f "${BRIDGE_SOURCE}/build.sh" ]]; then
  echo "No se encontró el fuente del bridge en ${BRIDGE_SOURCE}." >&2
  exit 2
fi
if [[ -z "${NGX_SDK_DIR:-}" || ! -f "${NGX_SDK_DIR}/include/nvsdk_ngx.h" ]]; then
  echo "NGX_SDK_DIR debe apuntar a headers válidos." >&2
  exit 2
fi

WORK_DIR="$(mktemp -d /tmp/dlss5-linux-bridge-probe.XXXXXX)"
cp -a "${BRIDGE_SOURCE}/." "${WORK_DIR}/"
for patch_file in "${PATCH_FILES[@]}"; do
  git -C "${WORK_DIR}" apply --check "${patch_file}"
  git -C "${WORK_DIR}" apply "${patch_file}"
done
mkdir -p "${OUT_DIR}"
(
  cd "${WORK_DIR}"
  NGX_SDK_DIR="${NGX_SDK_DIR}" OUT_DIR="${OUT_DIR}" \
    CXX="${CXX:-x86_64-w64-mingw32-g++}" ./build.sh
)
echo "Patched bridge build ready: ${OUT_DIR}"
