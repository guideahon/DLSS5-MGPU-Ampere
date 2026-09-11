#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CUDA_ROOT="${CUDA_ROOT:-/usr/local/cuda-12.8}"
OUT="${OUT:-${ROOT_DIR}/build/cuda_external_import_helper}"
READBACK_OUT="${READBACK_OUT:-${ROOT_DIR}/build/cuda_external_readback_helper}"
SEMAPHORE_WAIT_OUT="${SEMAPHORE_WAIT_OUT:-${ROOT_DIR}/build/cuda_external_semaphore_wait_helper}"
FENCED_P2P_OUT="${FENCED_P2P_OUT:-${ROOT_DIR}/build/cuda_external_fenced_p2p_helper}"

g++ -O2 -std=c++17 \
  "${ROOT_DIR}/tests/cuda_external_import_helper.cpp" \
  -o "${OUT}" \
  -I"${CUDA_ROOT}/targets/x86_64-linux/include" \
  -L/usr/lib/x86_64-linux-gnu -lcuda

echo "Helper CUDA nativo: ${OUT}"

g++ -O2 -std=c++17 \
  "${ROOT_DIR}/tests/cuda_external_readback_helper.cpp" \
  -o "${READBACK_OUT}" \
  -I"${CUDA_ROOT}/targets/x86_64-linux/include" \
  -L/usr/lib/x86_64-linux-gnu -lcuda

echo "Helper CUDA readback: ${READBACK_OUT}"

g++ -O2 -std=c++17 \
  "${ROOT_DIR}/tests/cuda_external_semaphore_wait_helper.cpp" \
  -o "${SEMAPHORE_WAIT_OUT}" \
  -I"${CUDA_ROOT}/targets/x86_64-linux/include" \
  -L/usr/lib/x86_64-linux-gnu -lcuda

echo "Helper CUDA semaphore wait: ${SEMAPHORE_WAIT_OUT}"

g++ -O2 -std=c++17 \
  "${ROOT_DIR}/tests/cuda_external_fenced_p2p_helper.cpp" \
  -o "${FENCED_P2P_OUT}" \
  -I"${CUDA_ROOT}/targets/x86_64-linux/include" \
  -L/usr/lib/x86_64-linux-gnu -lcuda

echo "Helper CUDA fenced P2P: ${FENCED_P2P_OUT}"
