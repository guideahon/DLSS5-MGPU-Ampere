#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CUDA_ROOT="${CUDA_ROOT:-/usr/local/cuda-12.8}"
OUT="${OUT:-${ROOT_DIR}/build/cuda_external_import_helper}"

g++ -O2 -std=c++17 \
  "${ROOT_DIR}/tests/cuda_external_import_helper.cpp" \
  -o "${OUT}" \
  -I"${CUDA_ROOT}/targets/x86_64-linux/include" \
  -L/usr/lib/x86_64-linux-gnu -lcuda

echo "Helper CUDA nativo: ${OUT}"
