#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${OUT:-${ROOT_DIR}/build/libmgpu_fd_inherit_shim.so}"

mkdir -p "$(dirname "${OUT}")"
gcc -O2 -fPIC -shared \
  "${ROOT_DIR}/tests/fd_inherit_shim.c" \
  -o "${OUT}" -ldl
echo "FD inheritance shim: ${OUT}"
