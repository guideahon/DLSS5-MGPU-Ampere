#!/usr/bin/env bash
# Non-invasive real-game gate: no game DLL replacement and no DLSS/NR claim.
set -eo pipefail
EXE=""
RUNNER="$PROTON"
PREFIX=""
VKD3D_DIR="$VKD3D_DLL_DIR"
OUTPUT_DIR="$MGPU_D3D12_HOST_OUTPUT_DIR"
TIMEOUT_SECONDS="$MGPU_D3D12_HOST_TIMEOUT_SECONDS"
PREWARM="$MGPU_D3D12_HOST_PREWARM"
GPU_INDEX="$VKD3D_VULKAN_DEVICE"
GAME_LOG=""
GAME_ARGS=()
[[ -n "$TIMEOUT_SECONDS" ]] || TIMEOUT_SECONDS=30
[[ -n "$PREWARM" ]] || PREWARM=1
[[ -n "$GPU_INDEX" ]] || GPU_INDEX=1
usage() {
  cat >&2 <<'EOF'
Uso:
  scripts/run_real_d3d12_host_probe.sh --exe /ruta/Game.exe --runner /ruta/GE-Proton/proton --prefix /ruta/compatdata [--vkd3d-dir /ruta/build/vkd3d] [--game-log /ruta/Player.log] [--output-dir build/real-game-dx12] [--timeout-seconds 30] [--gpu-index 1] [-- --force-d3d12]
El timeout posterior a la creación del device sigue siendo evidencia válida.
EOF
}
while (($#)); do
  case "$1" in
    --exe) [[ $# -ge 2 ]] || { usage; exit 2; }; EXE="$2"; shift 2 ;;
    --runner) [[ $# -ge 2 ]] || { usage; exit 2; }; RUNNER="$2"; shift 2 ;;
    --prefix) [[ $# -ge 2 ]] || { usage; exit 2; }; PREFIX="$2"; shift 2 ;;
    --vkd3d-dir) [[ $# -ge 2 ]] || { usage; exit 2; }; VKD3D_DIR="$2"; shift 2 ;;
    --output-dir) [[ $# -ge 2 ]] || { usage; exit 2; }; OUTPUT_DIR="$2"; shift 2 ;;
    --timeout-seconds) [[ $# -ge 2 ]] || { usage; exit 2; }; TIMEOUT_SECONDS="$2"; shift 2 ;;
    --gpu-index) [[ $# -ge 2 ]] || { usage; exit 2; }; GPU_INDEX="$2"; shift 2 ;;
    --game-log) [[ $# -ge 2 ]] || { usage; exit 2; }; GAME_LOG="$2"; shift 2 ;;
    --) shift; GAME_ARGS=("$@"); break ;;
    -h|--help) usage 2>&1; exit 0 ;;
    *) echo "Argumento desconocido: $1" >&2; usage; exit 2 ;;
  esac
done
[[ -f "$EXE" ]] || { echo "No existe --exe: $EXE" >&2; exit 2; }
[[ -x "$RUNNER" ]] || { echo "--runner no es ejecutable: $RUNNER" >&2; exit 2; }
[[ -d "$PREFIX" ]] || { echo "No existe --prefix: $PREFIX" >&2; exit 2; }
if [[ -n "$VKD3D_DIR" ]]; then
  [[ -f "$VKD3D_DIR/d3d12.dll" && -f "$VKD3D_DIR/d3d12core.dll" ]] || { echo "--vkd3d-dir no contiene d3d12.dll y d3d12core.dll." >&2; exit 2; }
fi
command -v setsid >/dev/null || { echo "Falta setsid." >&2; exit 2; }
if [[ -z "$OUTPUT_DIR" ]]; then OUTPUT_DIR="$(mktemp -d /tmp/dlss5-real-d3d12-host.XXXXXX)"; else mkdir -p "$OUTPUT_DIR"; fi
mkdir -p "$OUTPUT_DIR/proton-log"
RUNNER_ROOT="$(cd "$(dirname "$RUNNER")" && pwd)"
if [[ "$PREWARM" == 1 ]]; then
  set +e
  timeout --signal=TERM --kill-after=10s 30s env STEAM_COMPAT_DATA_PATH="$PREFIX" STEAM_COMPAT_CLIENT_INSTALL_PATH="$RUNNER_ROOT" UMU_ID=dlss5-d3d12-host-prewarm UMU_USE_STEAM=0 WINEDEBUG=-all "$RUNNER" run cmd.exe /c exit >"$OUTPUT_DIR/proton-prewarm.log" 2>&1
  prewarm_rc=$?
  set -e
  if [[ "$prewarm_rc" -ne 0 ]]; then echo "El precalentamiento Proton falló con código $prewarm_rc." >&2; exit "$prewarm_rc"; fi
fi
export STEAM_COMPAT_DATA_PATH="$PREFIX"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$RUNNER_ROOT"
export UMU_ID=dlss5-d3d12-host
export UMU_USE_STEAM=0
export VKD3D_VULKAN_DEVICE="$GPU_INDEX"
export VKD3D_DUPLICATE_LUID_ADAPTERS="${VKD3D_DUPLICATE_LUID_ADAPTERS:-1}"
export PROTON_LOG=1
export PROTON_LOG_DIR="$OUTPUT_DIR/proton-log"
export WINEDEBUG=-all,+loaddll
if [[ -n "$VKD3D_DIR" ]]; then export VKD3D_DLL_DIR="$VKD3D_DIR"; export WINEDLLPATH="$VKD3D_DIR"; export WINEDLLOVERRIDES="d3d12=n,b;d3d12core=n,b"; fi
HOST_LOG="$OUTPUT_DIR/host.log"
kill_prefix_processes() {
  local proc pid env_dump
  local -a pids=()
  for proc in /proc/[0-9]*; do
    pid="${proc##*/}"
    [[ "$pid" == "$$" ]] && continue
    env_dump="$(cat "$proc/environ" 2>/dev/null | tr '\0' '\n' || true)"
    if printf '%s\n' "$env_dump" | rg -Fxq \
      -e "STEAM_COMPAT_DATA_PATH=$STEAM_COMPAT_DATA_PATH" \
      -e "WINEPREFIX=$STEAM_COMPAT_DATA_PATH/pfx"; then
      pids+=("$pid")
    fi
  done
  if ((${#pids[@]})); then
    kill -TERM "${pids[@]}" 2>/dev/null || true
    sleep 1
    for pid in "${pids[@]}"; do
      kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null || true
    done
  fi
}
set +e
setsid bash -c 'cd "$1"; shift; exec "$@"' bash "$(dirname "$EXE")" "$RUNNER" run "$EXE" "${GAME_ARGS[@]}" >"$HOST_LOG" 2>&1 &
HOST_PID=$!
HOST_PGID="$(ps -o pgid= -p "$HOST_PID" | tr -d ' ')"
HOST_RC=0
TIMED_OUT=0
DEADLINE=$((SECONDS + TIMEOUT_SECONDS))
while kill -0 "$HOST_PID" 2>/dev/null; do
  if (( SECONDS >= DEADLINE )); then HOST_RC=124; TIMED_OUT=1; break; fi
  sleep 1
done
if (( TIMED_OUT )); then
  if [[ -n "$HOST_PGID" && "$HOST_PGID" != 0 && "$HOST_PGID" != "$$" ]]; then kill -TERM -- "-$HOST_PGID" 2>/dev/null || true; sleep 2; kill -KILL -- "-$HOST_PGID" 2>/dev/null || true; else kill -TERM "$HOST_PID" 2>/dev/null || true; fi
fi
wait "$HOST_PID" 2>/dev/null
wait_rc=$?
kill_prefix_processes
set -e
if (( ! TIMED_OUT )); then HOST_RC=$wait_rc; fi
LOG_TEXT="$OUTPUT_DIR/combined-loader.log"
: > "$LOG_TEXT"
while IFS= read -r log; do cat "$log" >> "$LOG_TEXT"; done < <(find "$OUTPUT_DIR/proton-log" -maxdepth 1 -type f -name '*.log' -print 2>/dev/null)
[[ -f "$HOST_LOG" ]] && cat "$HOST_LOG" >> "$LOG_TEXT"
[[ -n "$GAME_LOG" && -f "$GAME_LOG" ]] && cat "$GAME_LOG" >> "$LOG_TEXT"
has() { rg -qi "$1" "$LOG_TEXT" 2>/dev/null; }
D3D12_LOADED=false; D3D12CORE_LOADED=false; VULKAN_LOADED=false; VKD3D_DEVICE=false; GAME_STARTED=false; GAME_LOG_PRESENT=false
has 'Loaded .*d3d12\.dll|Forcing GfxDevice: Direct3D 12|Direct3D 12|vkd3d-proton:.*d3d12_(device|physical|caps)|dxgi_vk_swap_chain_init' && D3D12_LOADED=true
has 'Loaded .*d3d12core\.dll' && D3D12CORE_LOADED=true
has 'Loaded .*winevulkan\.dll|Loaded .*vulkan-1\.dll|Vulkan:' && VULKAN_LOADED=true
has 'vkd3d-proton:.*d3d12|Direct3D:.*Version: Direct3D 12|VKD3D create device' && VKD3D_DEVICE=true
has 'Game: |UnityPlayer\.dll|Forcing GfxDevice|Direct3D 12' && GAME_STARTED=true
if [[ -n "$GAME_LOG" && -f "$GAME_LOG" ]]; then GAME_LOG_PRESENT=true; fi
python3 - "$OUTPUT_DIR/result.json" "$EXE" "$HOST_RC" "$TIMED_OUT" "$D3D12_LOADED" "$D3D12CORE_LOADED" "$VULKAN_LOADED" "$VKD3D_DEVICE" "$GAME_STARTED" "$GAME_LOG_PRESENT" "$OUTPUT_DIR" <<'PY'
import json
import pathlib
import sys
out, exe, rc, timed, d3d12, core, vulkan, device, started, game_log, directory = sys.argv[1:]
payload = {"probe": "real-d3d12-host", "executable": exe, "return_code": int(rc), "timed_out": timed == "1", "started": started == "true", "d3d12_loaded": d3d12 == "true", "d3d12core_loaded": core == "true", "vulkan_loaded": vulkan == "true", "vkd3d_device_created": device == "true", "game_log_present": game_log == "true", "host_dir": directory, "remote_ngx": False, "gpu_native_sync": "pending"}
payload["status"] = "validated" if payload["d3d12_loaded"] and payload["vkd3d_device_created"] else "not_reached"
pathlib.Path(out).write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
print(json.dumps(payload, indent=2))
PY
if [[ "$D3D12_LOADED" == true && "$VKD3D_DEVICE" == true ]]; then exit 0; fi
exit 5
