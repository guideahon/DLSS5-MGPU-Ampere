#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXE=""
GAME_DLL=""
RUNNER="${PROTON:-}"
PREFIX=""
BRIDGE_DIR="${MGPU_REMOTE_PROFILE:-${ROOT_DIR}/build/proton-resource-pair-worker-experimental}"
TIMEOUT_SECONDS="${MGPU_REAL_GAME_TIMEOUT_SECONDS:-90}"
OUTPUT_DIR="${MGPU_REAL_GAME_OUTPUT_DIR:-}"
GAME_ARGS=()

usage() {
  cat >&2 <<'EOF'
Uso:
  scripts/run_real_game_remote_probe.sh \
    --exe /ruta/Game-Win64-Shipping.exe \
    --game-dll /ruta/nvngx_dlss.dll \
    --runner /ruta/GE-Proton/proton \
    --prefix /ruta/compat-data \
    [--bridge-dir /ruta/build/proton-resource-pair-worker-experimental] \
    [--timeout-seconds 90] [--output-dir /tmp/salida] [-- argumento-del-juego ...]

El DLL del juego se reemplaza sólo durante el proceso. El backup se restaura
con trap incluso si el proceso termina por timeout o señal.
EOF
}

while (($#)); do
  case "$1" in
    --exe)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      EXE="$2"; shift 2 ;;
    --game-dll)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      GAME_DLL="$2"; shift 2 ;;
    --runner)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      RUNNER="$2"; shift 2 ;;
    --prefix)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      PREFIX="$2"; shift 2 ;;
    --bridge-dir)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      BRIDGE_DIR="$2"; shift 2 ;;
    --timeout-seconds)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      TIMEOUT_SECONDS="$2"; shift 2 ;;
    --output-dir)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      OUTPUT_DIR="$2"; shift 2 ;;
    --)
      shift
      GAME_ARGS+=("$@")
      break ;;
    -h|--help)
      usage 2>&1; exit 0 ;;
    *)
      echo "Argumento desconocido: $1" >&2
      usage
      exit 2 ;;
  esac
done

[[ -n "$EXE" && -f "$EXE" ]] || { echo "--exe debe apuntar a un ejecutable existente." >&2; exit 2; }
[[ -n "$GAME_DLL" && -f "$GAME_DLL" ]] || { echo "--game-dll debe apuntar a un DLL existente." >&2; exit 2; }
[[ -n "$RUNNER" && -x "$RUNNER" ]] || { echo "--runner debe ser ejecutable." >&2; exit 2; }
[[ -n "$PREFIX" ]] || { echo "--prefix es obligatorio para aislar el juego." >&2; exit 2; }
[[ -f "$BRIDGE_DIR/_nvngx.dll" && -f "$BRIDGE_DIR/bridge-nvngx.dll" ]] || {
  echo "Faltan _nvngx.dll/bridge-nvngx.dll en $BRIDGE_DIR." >&2
  exit 2
}
[[ -f "$BRIDGE_DIR/_nvngx_real.dll" &&
   -f "$BRIDGE_DIR/nvngx_dlss_real.dll" &&
   -f "$BRIDGE_DIR/nvngx_dlssnr.dll" ]] || {
  echo "Faltan los runtimes NGX reales en $BRIDGE_DIR." >&2
  exit 2
}

if [[ -z "$OUTPUT_DIR" ]]; then
  OUTPUT_DIR="$(mktemp -d /tmp/dlss5-real-game.XXXXXX)"
else
  mkdir -p "$OUTPUT_DIR"
fi

GAME_DIR="$(cd "$(dirname "$GAME_DLL")" && pwd)"
BACKUP_DIR="$(mktemp -d /tmp/dlss5-real-game-backup.XXXXXX)"
BACKUP_DLL="$BACKUP_DIR/nvngx_dlss.dll"
GAME_LOG="$GAME_DIR/dlssnr-proxy.log"
ORIGINAL_LOG=0
if [[ -e "$GAME_LOG" ]]; then ORIGINAL_LOG=1; fi

cp -p "$GAME_DLL" "$BACKUP_DLL"
ORIGINAL_HASH="$(sha256sum "$BACKUP_DLL" | awk '{print $1}')"
cp "$BRIDGE_DIR/_nvngx.dll" "$GAME_DLL"
INJECTED_HASH="$(sha256sum "$GAME_DLL" | awk '{print $1}')"

RESTORED=0
restore_game_dll() {
  if [[ "$RESTORED" -eq 1 ]]; then return; fi
  cp -p "$BACKUP_DLL" "$GAME_DLL"
  local restored_hash
  restored_hash="$(sha256sum "$GAME_DLL" | awk '{print $1}')"
  if [[ "$restored_hash" != "$ORIGINAL_HASH" ]]; then
    echo "ERROR: el hash restaurado no coincide con el backup." >&2
    return 1
  fi
  RESTORED=1
  echo "game_dll_restored=true original_sha256=$ORIGINAL_HASH injected_sha256=$INJECTED_HASH"
}
trap restore_game_dll EXIT INT TERM

export MGPU_NGX_CROSS_ADAPTER=1
export MGPU_CROSS_ADAPTER_GPU_NATIVE=0
export MGPU_NGX_PRIME_SOURCE=0
export MGPU_DLSSNR_SKIP_LOCAL_NGX=1
export MGPU_DLSSNR_REMOTE_NGX_INIT_PROBE=1
export MGPU_DLSSNR_REMOTE_NGX_FEATURE=1
export MGPU_DLSSNR_REMOTE_NGX_PERSISTENT=1
export MGPU_DLSSNR_VALIDATE_REMOTE_OUTPUT=1
export MGPU_DLSSNR_TRANSPORT=resource-fd-pair-worker
export MGPU_REMOTE_TRANSPORT=resource-fd-pair-worker-remote-ngx
export MGPU_REMOTE_DIRECTIONS=forward
export MGPU_CROSS_ADAPTER_REQUIRE_DISTINCT_IDENTITY=1
export VKD3D_DUPLICATE_LUID_ADAPTERS=1
export VKD3D_DUPLICATE_LUID_INDEX_PER_DEVICE=1
export MGPU_NGX_CORE_DLL="$BRIDGE_DIR/_nvngx_real.dll"
export DLSS_RUNTIME_DLL="$BRIDGE_DIR/nvngx_dlss_real.dll"
export DLSS_NR_DLL="$BRIDGE_DIR/nvngx_dlssnr.dll"
export NGX_BRIDGE_DIR="$BRIDGE_DIR"
export VKD3D_DLL_DIR="${VKD3D_DLL_DIR:-$BRIDGE_DIR}"
export WINEDLLPATH="$BRIDGE_DIR${WINEDLLPATH:+:$WINEDLLPATH}"
export WINEDEBUG="${WINEDEBUG:--all}"

RESULT_FILE="$OUTPUT_DIR/mgpu-auto-result.json"
BRIDGE_LOG_COPY="$OUTPUT_DIR/dlssnr-proxy.log"
COMMAND=(python3 "$ROOT_DIR/scripts/mgpu_auto.py" run
  --exe "$EXE" --runner "$RUNNER" --prefix "$PREFIX"
  --timeout-seconds "$TIMEOUT_SECONDS" --enable-remote --json)
for argument in "${GAME_ARGS[@]}"; do
  COMMAND+=("--exe-arg=$argument")
done

set +e
"${COMMAND[@]}" 2>&1 | tee "$RESULT_FILE"
RUN_RC=${PIPESTATUS[0]}
set -e

if [[ -f "$GAME_LOG" ]]; then
  cp "$GAME_LOG" "$BRIDGE_LOG_COPY"
  if [[ "$ORIGINAL_LOG" -eq 0 ]]; then
    find "$GAME_LOG" -maxdepth 0 -delete
  fi
else
  : > "$OUTPUT_DIR/dlssnr-proxy.log.missing"
fi

if [[ "$RUN_RC" -eq 0 ]]; then
  echo "real_game_probe=finished output_dir=$OUTPUT_DIR"
else
  echo "real_game_probe_return_code=$RUN_RC output_dir=$OUTPUT_DIR"
fi
exit "$RUN_RC"
