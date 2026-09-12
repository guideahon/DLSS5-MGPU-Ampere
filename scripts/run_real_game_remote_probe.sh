#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXE=""
GAME_DLL=""
STREAMLINE_DIR_ARG=""
RUNNER="${PROTON:-}"
PREFIX=""
BRIDGE_DIR="${MGPU_REMOTE_PROFILE:-${ROOT_DIR}/build/proton-resource-pair-worker-experimental}"
TIMEOUT_SECONDS="${MGPU_REAL_GAME_TIMEOUT_SECONDS:-90}"
PREWARM_TIMEOUT_SECONDS="${MGPU_PROTON_PREWARM_TIMEOUT_SECONDS:-30}"
PREWARM="${MGPU_REAL_GAME_PREWARM:-1}"
OUTPUT_DIR="${MGPU_REAL_GAME_OUTPUT_DIR:-}"
SEED_CYBERPUNK_DLSS=0
FORCE_SYSTEM32_NGX=0
PATCH_STREAMLINE_SIGNATURE=0
AUDIT_LOADER=0
GAME_ARGS=()

usage() {
  cat >&2 <<'EOF'
Uso:
  scripts/run_real_game_remote_probe.sh \
    --exe /ruta/Game-Win64-Shipping.exe \
    --game-dll /ruta/nvngx_dlss.dll \
    --runner /ruta/GE-Proton/proton \
    --prefix /ruta/compat-data \
    [--streamline-dir /ruta/a/Streamline] \
    [--bridge-dir /ruta/build/proton-resource-pair-worker-experimental] \
    [--timeout-seconds 90] [--output-dir /tmp/salida] \
    [--seed-cyberpunk-dlss] [--force-system32-ngx] \
    [--patch-streamline-signature] [--audit-loader] \
    [-- argumento-del-juego ...]

Los DLL del juego y Streamline se reemplazan sólo durante el proceso cuando se
solicita el modo de desarrollo. El backup se restaura con trap incluso si el
proceso termina por timeout o señal.
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
    --streamline-dir)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      STREAMLINE_DIR_ARG="$2"; shift 2 ;;
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
    --patch-streamline-signature)
      PATCH_STREAMLINE_SIGNATURE=1; shift ;;
    --audit-loader)
      AUDIT_LOADER=1; shift ;;
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
command -v timeout >/dev/null 2>&1 || {
  echo "Falta timeout; no se ejecuta el precalentamiento Proton sin watchdog." >&2
  exit 2
}

kill_prefix_processes() {
  local proc pid env_dump
  local -a pids=()
  for proc in /proc/[0-9]*; do
    pid="${proc##*/}"
    [[ "$pid" == "$$" ]] && continue
    # Put the /proc read in an external command so hidepid/permission races
    # cannot make Bash print an error for unrelated system processes.
    env_dump="$(cat "$proc/environ" 2>/dev/null | tr '\0' '\n' || true)"
    if printf '%s\n' "$env_dump" | rg -Fxq \
      -e "STEAM_COMPAT_DATA_PATH=$PREFIX" \
      -e "WINEPREFIX=$PREFIX/pfx"; then
      pids+=("$pid")
    fi
  done
  if ((${#pids[@]})); then
    kill -TERM "${pids[@]}" 2>/dev/null || true
    sleep 1
    for pid in "${pids[@]}"; do
      if kill -0 "$pid" 2>/dev/null; then
        kill -KILL "$pid" 2>/dev/null || true
      fi
    done
  fi
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

GAME_DIR="$(cd "$(dirname "$GAME_DLL")" && pwd)"
STREAMLINE_DIR="${STREAMLINE_DIR_ARG:-$GAME_DIR}"
STREAMLINE_DIR="$(cd "$STREAMLINE_DIR" && pwd)"
STREAMLINE_DEV_DIR=""
if [[ "$PATCH_STREAMLINE_SIGNATURE" -eq 1 ]]; then
  STREAMLINE_DEV_DIR="$OUTPUT_DIR/streamline-dev"
  mkdir -p "$STREAMLINE_DEV_DIR"
  for name in sl.common.dll sl.interposer.dll; do
    [[ -f "$STREAMLINE_DIR/$name" ]] || {
      echo "Falta $STREAMLINE_DIR/$name; no se puede aplicar el parche Streamline." >&2
      exit 2
    }
    python3 "$ROOT_DIR/scripts/patch_streamline_signature.py" \
      --input "$STREAMLINE_DIR/$name" \
      --output "$STREAMLINE_DEV_DIR/$name" \
      > "$OUTPUT_DIR/streamline-signature-$name.json"
  done
  {
    echo "mode=development-copy"
    echo "source_dir=$STREAMLINE_DIR"
    echo "patched_dir=$STREAMLINE_DEV_DIR"
    sha256sum "$STREAMLINE_DIR/sl.common.dll" "$STREAMLINE_DEV_DIR/sl.common.dll"
    sha256sum "$STREAMLINE_DIR/sl.interposer.dll" "$STREAMLINE_DEV_DIR/sl.interposer.dll"
  } > "$OUTPUT_DIR/streamline-signature-patch.sha256"
fi

ORIGINAL_RUNNER="$RUNNER"
ORIGINAL_PROTON_ROOT="$(cd "$(dirname "$ORIGINAL_RUNNER")" && pwd)"
if [[ "$FORCE_SYSTEM32_NGX" -eq 1 ]]; then
  # GE-Proton unconditionally copies the host driver's _nvngx.dll during
  # setup_prefix.  Use a private entrypoint copy that gates that operation;
  # every other Proton asset remains a symlink to the original installation.
  RUNNER="$(python3 "$ROOT_DIR/scripts/prepare_proton_mgpu_runner.py" \
    --runner "$ORIGINAL_RUNNER" \
    --output-root "$OUTPUT_DIR/proton-mgpu-runner")"
  export MGPU_PROTON_COPY_NVIDIA_NGX=0
fi

BACKUP_DIR="$(mktemp -d /tmp/dlss5-real-game-backup.XXXXXX)"
BACKUP_DLL="$BACKUP_DIR/nvngx_dlss.dll"
RESTORE_STATE="$OUTPUT_DIR/game-dll-restore.state"
STREAMLINE_RESTORE_STATE="$OUTPUT_DIR/streamline-dll-restore.state"
STREAMLINE_PATCH_NAMES=(sl.common.dll sl.interposer.dll)
GAME_LOG="$GAME_DIR/dlssnr-proxy.log"
ORIGINAL_LOG=0
if [[ -e "$GAME_LOG" ]]; then ORIGINAL_LOG=1; fi

cp -p "$GAME_DLL" "$BACKUP_DLL"
ORIGINAL_HASH="$(sha256sum "$BACKUP_DLL" | awk '{print $1}')"
printf 'pending\n' > "$RESTORE_STATE"
if [[ -n "$STREAMLINE_DEV_DIR" ]]; then
  for name in "${STREAMLINE_PATCH_NAMES[@]}"; do
    cp -p "$STREAMLINE_DIR/$name" "$BACKUP_DIR/$name"
  done
  printf 'pending\n' > "$STREAMLINE_RESTORE_STATE"
fi

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
# GE-Proton's prefix lock is created inside STEAM_COMPAT_DATA_PATH.  A fresh
# direct-game probe must create that parent first; otherwise setup_prefix can
# fail before Proton gets a chance to initialize the isolated prefix.
mkdir -p "$PREFIX"
prewarm_prefix() {
  [[ "$PREWARM" -eq 1 ]] || return 0
  set +e
  timeout --signal=TERM --kill-after=10s "${PREWARM_TIMEOUT_SECONDS}s" \
    env STEAM_COMPAT_DATA_PATH="$PREFIX" \
    STEAM_COMPAT_CLIENT_INSTALL_PATH="$ORIGINAL_PROTON_ROOT" \
      UMU_ID="${UMU_ID:-dlss5-real-game-prewarm}" \
      UMU_USE_STEAM="${UMU_USE_STEAM:-0}" \
      PROTON_ENABLE_NVAPI="${PROTON_ENABLE_NVAPI:-1}" \
      WINEDEBUG="${WINEDEBUG:--all}" \
      "$RUNNER" run cmd.exe /c exit \
      >"$OUTPUT_DIR/proton-prewarm.log" 2>&1
  local prewarm_rc=$?
  set -e
  if [[ "$prewarm_rc" -ne 0 ]]; then
    if [[ "$prewarm_rc" -eq 124 || "$prewarm_rc" -eq 137 ]]; then
      echo "El precalentamiento Proton excedió ${PREWARM_TIMEOUT_SECONDS}s; se aborta de forma segura." >&2
    else
      echo "El precalentamiento Proton falló con código ${prewarm_rc}; se aborta de forma segura." >&2
    fi
    kill_prefix_processes
    return "$prewarm_rc"
  fi
}
if prewarm_prefix; then
  :
else
  prewarm_rc=$?
  exit "$prewarm_rc"
fi
restore_streamline_dlls() {
  if [[ -z "$STREAMLINE_DEV_DIR" ]]; then return; fi
  if [[ -f "$STREAMLINE_RESTORE_STATE" ]] &&
     grep -qx 'restored' "$STREAMLINE_RESTORE_STATE"; then
    return
  fi
  local name target backup temporary expected actual
  for name in "${STREAMLINE_PATCH_NAMES[@]}"; do
    target="$STREAMLINE_DIR/$name"
    backup="$BACKUP_DIR/$name"
    if [[ ! -f "$backup" ]]; then
      echo "ERROR: falta el backup de $name; no se puede restaurar." >&2
      return 1
    fi
    temporary="$target.dlss5-restore.$$"
    cp -p "$backup" "$temporary"
    mv -f "$temporary" "$target"
    expected="$(sha256sum "$backup" | awk '{print $1}')"
    actual="$(sha256sum "$target" | awk '{print $1}')"
    if [[ "$actual" != "$expected" ]]; then
      echo "ERROR: el hash restaurado de $name no coincide." >&2
      return 1
    fi
  done
  printf 'restored\n' > "$STREAMLINE_RESTORE_STATE"
}
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
}

if [[ "$FORCE_SYSTEM32_NGX" -eq 1 ]]; then
  # The prefix is already initialized above; install the proxy only after the
  # prewarm so Proton cannot overwrite it during setup_prefix.
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
GUARDIAN_ARGS=(
  "$GAME_DLL" "$BACKUP_DLL" "$RESTORE_STATE"
  "$RUNNER_PID" "$RUNNER_START_TICKS" "$ORIGINAL_HASH"
)
if [[ -n "$STREAMLINE_DEV_DIR" ]]; then
  for name in "${STREAMLINE_PATCH_NAMES[@]}"; do
    GUARDIAN_ARGS+=("$STREAMLINE_DIR/$name" "$BACKUP_DIR/$name")
  done
fi
setsid python3 - "${GUARDIAN_ARGS[@]}" <<'PY' >/dev/null 2>&1 &
import hashlib
import os
import shutil
import sys
import time

game, backup, state, owner, expected_start, expected, *extra = sys.argv[1:]
owner_pid = int(owner)
if len(extra) % 2:
    raise SystemExit("dlss5 restore guardian: invalid extra file pairs")
pairs = [(game, backup)] + list(zip(extra[::2], extra[1::2]))

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
for target, backup_path in pairs:
    if not os.path.isfile(backup_path):
        raise SystemExit("dlss5 restore guardian: backup missing")
    temporary = target + ".dlss5-guardian-restore.%d" % os.getpid()
    shutil.copy2(backup_path, temporary)
    os.replace(temporary, target)
    digest = hashlib.sha256()
    with open(target, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    expected_digest = hashlib.sha256()
    with open(backup_path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            expected_digest.update(chunk)
    if digest.hexdigest() != expected_digest.hexdigest():
        raise SystemExit("dlss5 restore guardian: hash mismatch")
with open(state, "w", encoding="utf-8") as stream:
    stream.write("restored\n")
for _, backup_path in pairs:
    try:
        os.unlink(backup_path)
    except OSError:
        pass
try:
    os.rmdir(os.path.dirname(backup))
except OSError:
    pass
PY
GUARDIAN_PID=$!
cleanup_done=0
restore_all() {
  if [[ "$cleanup_done" -eq 1 ]]; then return; fi
  cleanup_done=1
  # On normal exit the shell owns cleanup.  Stop the detached guardian first
  # so it cannot race with the atomic restores below.  If this shell is killed
  # with SIGKILL, this function is never entered and the guardian remains live.
  if [[ -n "${GUARDIAN_PID:-}" ]] && kill -0 "$GUARDIAN_PID" 2>/dev/null; then
    kill -TERM "$GUARDIAN_PID" 2>/dev/null || true
    wait "$GUARDIAN_PID" 2>/dev/null || true
  fi
  local cleanup_rc=0
  restore_system32_ngx || cleanup_rc=1
  restore_streamline_dlls || cleanup_rc=1
  restore_game_dll || cleanup_rc=1
  find "$BACKUP_DIR" -depth -delete 2>/dev/null || true
  return "$cleanup_rc"
}
trap restore_all EXIT INT TERM HUP

# Streamline resolves its modules from the game directory before consulting
# WINEDLLPATH.  In the explicit development mode, replace only these two
# files after the guardian is live; restore_all (or the guardian after SIGKILL)
# puts the original bytes back.
if [[ -n "$STREAMLINE_DEV_DIR" ]]; then
  for name in "${STREAMLINE_PATCH_NAMES[@]}"; do
    target="$STREAMLINE_DIR/$name"
    temporary="$target.dlss5-streamline-install.$$"
    cp "$STREAMLINE_DEV_DIR/$name" "$temporary"
    mv -f "$temporary" "$target"
  done
  printf 'installed\n' > "$STREAMLINE_RESTORE_STATE"
fi

# The replacement is atomic within the game directory.  If the launcher is
# killed between backup and injection, the guardian still restores the full
# original file because it was started before this operation.
INJECT_TMP="$GAME_DLL.dlss5-inject.$$"
cp "$BRIDGE_DIR/_nvngx.dll" "$INJECT_TMP"
mv -f "$INJECT_TMP" "$GAME_DLL"
INJECTED_HASH="$(sha256sum "$GAME_DLL" | awk '{print $1}')"
printf 'injected\n' > "$RESTORE_STATE"

export MGPU_NGX_CROSS_ADAPTER=1
export MGPU_NGX_PROXY_INJECTED=1
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
if [[ -n "$STREAMLINE_DEV_DIR" ]]; then
  export MGPU_STREAMLINE_DEV_DLL_DIR="$STREAMLINE_DEV_DIR"
  export WINEDLLPATH="$STREAMLINE_DEV_DIR:$BRIDGE_DIR${WINEDLLPATH:+:$WINEDLLPATH}"
  export WINEDLLOVERRIDES="sl.interposer=n,b;sl.common=n,b;${WINEDLLOVERRIDES:-}"
else
  export WINEDLLPATH="$BRIDGE_DIR${WINEDLLPATH:+:$WINEDLLPATH}"
fi
export WINEDEBUG="${WINEDEBUG:--all}"
if [[ "$AUDIT_LOADER" -eq 1 ]]; then
  # Keep normal runs quiet, but make this diagnostic mode self-contained.
  if [[ "$WINEDEBUG" == "-all" ]]; then
    export WINEDEBUG='-all,+loaddll'
  elif [[ "$WINEDEBUG" != *'+loaddll'* ]]; then
    export WINEDEBUG="${WINEDEBUG},+loaddll"
  fi
  export PROTON_LOG=1
  export PROTON_LOG_DIR="$OUTPUT_DIR/proton-log"
  mkdir -p "$PROTON_LOG_DIR"
fi

RESULT_FILE="$OUTPUT_DIR/mgpu-auto-result.json"
BRIDGE_LOG_COPY="$OUTPUT_DIR/dlssnr-proxy.log"
export MGPU_LAUNCH_OUTPUT_LOG="$OUTPUT_DIR/proton-launch.log"
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

if [[ "$AUDIT_LOADER" -eq 1 ]]; then
  # The child output is kept separate so RESULT_FILE remains valid JSON. Audit
  # the machine result, captured launch output, and Proton's per-app logs.
  AUDIT_SOURCES=("$RESULT_FILE")
  [[ -f "$OUTPUT_DIR/proton-launch.log" ]] &&
    AUDIT_SOURCES+=("$OUTPUT_DIR/proton-launch.log")
  [[ -d "$OUTPUT_DIR/proton-log" ]] &&
    AUDIT_SOURCES+=("$OUTPUT_DIR/proton-log")
  if ((${#AUDIT_SOURCES[@]})); then
    rg -i 'loaddll:' "${AUDIT_SOURCES[@]}" \
      > "$OUTPUT_DIR/loader-audit.log" || : > "$OUTPUT_DIR/loader-audit.log"
  else
    : > "$OUTPUT_DIR/loader-audit.log"
  fi
  if rg -qi 'loaddll:.*(nvngx|sl\.(common|interposer|dlss))' \
      "$OUTPUT_DIR/loader-audit.log"; then
    printf 'ngx_or_streamline_load_observed\n' > "$OUTPUT_DIR/loader-audit.status"
  elif rg -qi 'loaddll:' "$OUTPUT_DIR/loader-audit.log"; then
    printf 'loader_trace_without_ngx\n' > "$OUTPUT_DIR/loader-audit.status"
  else
    printf 'no_loader_trace_observed\n' > "$OUTPUT_DIR/loader-audit.status"
  fi
fi

# Xbox/GDK launches can terminate in xalia before the shipping executable
# reaches D3D12. Keep that cause separate from the loader audit so a missing
# NGX trace is not misread as a DLL rejection.
LAUNCHER_SOURCES=("$RESULT_FILE")
[[ -f "$OUTPUT_DIR/proton-launch.log" ]] &&
  LAUNCHER_SOURCES+=("$OUTPUT_DIR/proton-launch.log")
[[ -d "$OUTPUT_DIR/proton-log" ]] &&
  LAUNCHER_SOURCES+=("$OUTPUT_DIR/proton-log")
if rg -qi 'Game: xalia\.exe|xalia\.exe' "${LAUNCHER_SOURCES[@]}" 2>/dev/null; then
  printf 'xalia_launcher_observed\n' > "$OUTPUT_DIR/launcher-gate.status"
else
  printf 'no_xalia_launcher_observed\n' > "$OUTPUT_DIR/launcher-gate.status"
fi

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
