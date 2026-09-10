#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

if [[ ! -x "${BUILD_DIR}/mgpu-vulkan-cuda-probe" ]]; then
  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
  cmake --build "${BUILD_DIR}" -j"$(nproc)"
fi

PAYLOAD="${1:-8294400}"

echo "=== Vulkan GPU 0 -> CUDA 0 -> CUDA 1 ==="
"${BUILD_DIR}/mgpu-vulkan-cuda-probe" \
  --vulkan-gpu 0 --cuda-source 0 --cuda-destination 1 --bytes "${PAYLOAD}"

echo
echo "=== Vulkan GPU 1 -> CUDA 1 -> CUDA 0 ==="
"${BUILD_DIR}/mgpu-vulkan-cuda-probe" \
  --vulkan-gpu 1 --cuda-source 1 --cuda-destination 0 --bytes "${PAYLOAD}"
