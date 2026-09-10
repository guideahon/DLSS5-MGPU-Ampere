#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

if [[ ! -x "${BUILD_DIR}/mgpu-p2p-probe" ]]; then
  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
  cmake --build "${BUILD_DIR}" -j"$(nproc)"
fi

echo "=== nvidia-smi ==="
nvidia-smi
echo
echo "=== topology ==="
nvidia-smi topo -m
echo
echo "=== CUDA P2P ==="
"${BUILD_DIR}/mgpu-p2p-probe" "$@"
