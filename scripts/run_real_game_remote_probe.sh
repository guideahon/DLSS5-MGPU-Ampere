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
SEED_CYBERPUNK_DLSS=0
FORCE_SYSTEM32_NGX=0
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
    [--timeout-seconds 90] [--output-dir /tmp/salida] \
    [--seed-cyberpunk-dlss] [--force-system32-ngx] [-- argumento-del-juego ...]

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
    --seed-cyberpunk-dlss)
      SEED_CYBERPUNK_DLSS=1; shift ;;
    --force-system32-ngx)
      FORCE_SYSTEM32_NGX=1; shift ;;
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
command -v setsid >/dev/null 2>&1 || {
  echo "Falta setsid; no se ejecuta una inyección sin guardian de restauración." >&2
  exit 2
}

if [[ "$SEED_CYBERPUNK_DLSS" -eq 1 ]]; then
  python3 "$ROOT_DIR/scripts/seed_cyberpunk_dlss.py" --prefix "$PREFIX" \
    >/dev/null
fi

if [[ -z "$OUTPUT_DIR" ]]; then
  OUTPUT_DIR="$(mktemp -d /tmp/dlss5-real-game.XXXXXX)"
else
  mkdir -p "$OUTPUT_DIR"
fi

ORIGINAL_RUNNER="$RUNNER"
ORIGINAL_PROTON_ROOT="$(cd "$(dirname "$ORIGINAL_RUNNER")/.." && pwd)"
if [[ "$FORCE_SYSTEM32_NGX" -eq 1 ]]; then
  # GE-Proton unconditionally copies the host driver's _nvngx.dll during
  # setup_prefix.  Use a private entrypoint copy that gates that operation;
  # every other Proton asset remains a symlink to the original installation.
  RUNNER="$(python3 "$ROOT_DIR/scripts/prepare_proton_mgpu_runner.py" \
    --runner "$ORIGINAL_RUNNER" \
    --output-root "$OUTPUT_DIR/proton-mgpu-runner")"
  export MGPU_PROTON_COPY_NVIDIA_NGX=0
fi

GAME_DIR="$(cd "$(dirname "$GAME_DLL")" && pwd)"
BACKUP_DIR="$(mktemp -d /tmp/dlss5-real-game-backup.XXXXXX)"
BACKUP_DLL="$BACKUP_DIR/nvngx_dlss.dll"
RESTORE_STATE="$OUTPUT_DIR/game-dll-restore.state"
GAME_LOG="$GAME_DIR/dlssnr-proxy.log"
ORIGINAL_LOG=0
if [[ -e "$GAME_LOG" ]]; then ORIGINAL_LOG=1; fi

cp -p "$GAME_DLL" "$BACKUP_DLL"
ORIGINAL_HASH="$(sha256sum "$BACKUP_DLL" | awk '{print $1}')"
printf 'pending\n' > "$RESTORE_STATE"

RESTORED=0
INJECTED_HASH="not-injected"
SYSTEM32_NGX_TARGET="$PREFIX/pfx/drive_c/windows/system32/_nvngx.dll"
SYSTEM32_NGX_FILES=(
  _nvngx.dll
  _nvngx_real.dll
  bridge-nvngx.dll
  nvngx_dlss_real.dll
  nvngx_dlssnr.dll
)
SYSTEM32_NGX_INSTALLED=0
restore_system32_ngx() {
  if [[ "$SYSTEM32_NGX_INSTALLED" -eq 0 ]]; then return; fi
  local name target backup
  for name in "${SYSTEM32_NGX_FILES[@]}"; do
    target="$PREFIX/pfx/drive_c/windows/system32/$name"
    backup="$OUTPUT_DIR/system32-nvngx-original-$name"
    if [[ -f "$backup" ]]; then
      cp -p "$backup" "$target"
    else
      rm -f "$target"
    fi
  done
  SYSTEM32_NGX_INSTALLED=0
}
restore_game_dll() {
  if [[ "$RESTORED" -eq 1 ]]; then return; fi
  if [[ -f "$RESTORE_STATE" ]] && grep -qx 'restored' "$RESTORE_STATE"; then
    RESTORED=1
    return
  fi
  if [[ ! -f "$BACKUP_DLL" ]]; then
    echo "ERROR: falta el backup; no se puede restaurar $GAME_DLL." >&2
    return 1
  fi
  local restore_tmp="$GAME_DLL.dlss5-restore.$$"
  cp -p "$BACKUP_DLL" "$restore_tmp"
  mv -f "$restore_tmp" "$GAME_DLL"
  local restored_hash
  restored_hash="$(sha256sum "$GAME_DLL" | awk '{print $1}')"
  if [[ "$restored_hash" != "$ORIGINAL_HASH" ]]; then
    echo "ERROR: el hash restaurado no coincide con el backup." >&2
    return 1
  fi
  RESTORED=1
  printf 'restored\n' > "$RESTORE_STATE"
  echo "game_dll_restored=true original_sha256=$ORIGINAL_HASH injected_sha256=$INJECTED_HASH"
  find "$BACKUP_DIR" -depth -delete
}

if [[ "$FORCE_SYSTEM32_NGX" -eq 1 ]]; then
  # Complete the prefix setup first, then install the project proxy.  The
  # isolated Proton entrypoint keeps this replacement on the next invocation.
  STEAM_COMPAT_DATA_PATH="$PREFIX" \
  STEAM_COMPAT_CLIENT_INSTALL_PATH="$ORIGINAL_PROTON_ROOT" \
    PROTON_ENABLE_NVAPI="${PROTON_ENABLE_NVAPI:-1}" \
    "$RUNNER" run cmd.exe /c exit >/dev/null 2>&1 || true
  SYSTEM32_NGX_INSTALLED=1
  mkdir -p "$(dirname "$SYSTEM32_NGX_TARGET")"
  for name in "${SYSTEM32_NGX_FILES[@]}"; do
    target="$PREFIX/pfx/drive_c/windows/system32/$name"
    backup="$OUTPUT_DIR/system32-nvngx-original-$name"
    source="$BRIDGE_DIR/$name"
    if [[ -e "$target" ]]; then
      cp -p "$target" "$backup"
    fi
    SYSTEM32_NGX_TMP="$target.dlss5-install.$$"
    cp "$source" "$SYSTEM32_NGX_TMP"
    mv -f "$SYSTEM32_NGX_TMP" "$target"
  done
fi

# EXIT/INT/TERM/HUP cover normal shell teardown.  The detached guardian covers
# SIGKILL and an external launcher that kills this shell before EXIT runs.
RUNNER_PID="$$"
RUNNER_START_TICKS="$(awk '{print $22}' "/proc/$$/stat")"
setsid python3 - "$GAME_DLL" "$BACKUP_DLL" "$RESTORE_STATE" \
  "$RUNNER_PID" "$RUNNER_START_TICKS" "$ORIGINAL_HASH" <<'PY' >/dev/null 2>&1 &
import hashlib
import os
import shutil
import sys
import time

game, backup, state, owner, expected_start, expected = sys.argv[1:]
owner_pid = int(owner)

def state_value():
    try:
        with open(state, "r", encoding="utf-8") as stream:
            return stream.read().strip()
    except OSError:
        return ""

def owner_alive():
    try:
        os.kill(owner_pid, 0)
        with open("/proc/%d/stat" % owner_pid, "r", encoding="utf-8") as stream:
            stat = stream.read()
        fields = stat[stat.rfind(") ") + 2:].split()
        # After comm, fields[0] is state (field 3) and fields[19] is
        # starttime (field 22).  This also treats a zombie as dead.
        return fields[0] != "Z" and fields[19] == expected_start
    except (OSError, IndexError, ValueError):
        return False

while state_value() != "restored" and owner_alive():
    time.sleep(0.10)

if state_value() == "restored":
    raise SystemExit(0)
if not os.path.isfile(backup):
    raise SystemExit("dlss5 restore guardian: backup missing")

temporary = game + ".dlss5-guardian-restore.%d" % os.getpid()
shutil.copy2(backup, temporary)
os.replace(temporary, game)
digest = hashlib.sha256()
with open(game, "rb") as stream:
    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
        digest.update(chunk)
if digest.hexdigest() != expected:
    raise SystemExit("dlss5 restore guardian: hash mismatch")
with open(state, "w", encoding="utf-8") as stream:
    stream.write("restored\n")
try:
    os.unlink(backup)
    os.rmdir(os.path.dirname(backup))
except OSError:
    pass
PY
GUARDIAN_PID=$!
restore_all() {
  restore_system32_ngx
  restore_game_dll
}
trap restore_all EXIT INT TERM HUP

# The replacement is atomic within the game directory.  If the launcher is
# killed between backup and injection, the guardian still restores the full
# original file because it was started before this operation.
INJECT_TMP="$GAME_DLL.dlss5-inject.$$"
cp "$BRIDGE_DIR/_nvngx.dll" "$INJECT_TMP"
mv -f "$INJECT_TMP" "$GAME_DLL"
INJECTED_HASH="$(sha256sum "$GAME_DLL" | awk '{print $1}')"
printf 'injected\n' > "$RESTORE_STATE"

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

if [[ "$FORCE_SYSTEM32_NGX" -eq 1 && -f "$SYSTEM32_NGX_TARGET" ]]; then
  : > "$OUTPUT_DIR/system32-nvngx-runtime.sha256"
  for name in "${SYSTEM32_NGX_FILES[@]}"; do
    target="$PREFIX/pfx/drive_c/windows/system32/$name"
    if [[ -f "$target" ]]; then
      sha256sum "$target" >> "$OUTPUT_DIR/system32-nvngx-runtime.sha256"
    fi
  done
fi

if [[ "$RUN_RC" -eq 0 ]]; then
  echo "real_game_probe=finished output_dir=$OUTPUT_DIR"
else
  echo "real_game_probe_return_code=$RUN_RC output_dir=$OUTPUT_DIR"
fi
exit "$RUN_RC"
