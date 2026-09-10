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
exec ./build.sh
