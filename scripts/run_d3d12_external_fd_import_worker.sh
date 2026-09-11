#!/usr/bin/env bash
set -euo pipefail

PROTON="${MGPU_D3D12_FD_IMPORT_WORKER_PROTON:-${PROTON:-}}"
WORKER_PE="${MGPU_D3D12_FD_IMPORT_WORKER_PE:-}"
if [[ -z "${PROTON}" || ! -x "${PROTON}" || -z "${WORKER_PE}" || ! -f "${WORKER_PE}" ]]; then
  echo "worker requiere MGPU_D3D12_FD_IMPORT_WORKER_PROTON/PE" >&2
  exit 2
fi

if [[ -n "${MGPU_D3D12_FD_IMPORT_WORKER_LOG:-}" ]]; then
  exec "${PROTON}" run "${WORKER_PE}" "$@" >"${MGPU_D3D12_FD_IMPORT_WORKER_LOG}" 2>&1
fi
exec "${PROTON}" run "${WORKER_PE}" "$@"
