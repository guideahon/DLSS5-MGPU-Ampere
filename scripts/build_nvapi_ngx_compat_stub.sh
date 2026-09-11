#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${1:-${ROOT_DIR}/build/diagnostics}"
mkdir -p "${OUT_DIR}"

command -v x86_64-w64-mingw32-gcc >/dev/null
x86_64-w64-mingw32-gcc -shared -O2 -Wall -Wextra \
  -o "${OUT_DIR}/nvapi64.dll" \
  "${ROOT_DIR}/tests/nvapi_ngx_compat_stub.c"

printf '%s\n' "${OUT_DIR}/nvapi64.dll"
