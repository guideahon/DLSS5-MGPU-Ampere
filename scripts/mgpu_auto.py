#!/usr/bin/env python3
"""Conservative automatic planner for the Linux dual-GPU MVP.

This tool performs discovery and self-tests, and can launch an explicitly
provided executable with a local fallback policy. It never downloads, copies,
or modifies NVIDIA runtimes or game prefixes.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import shutil
import subprocess
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
P2P_PROBE = BUILD / "mgpu-p2p-probe"
VULKAN_PROBE = BUILD / "mgpu-vulkan-cuda-probe"
VULKAN_CUDA_SEMAPHORE_PROBE = BUILD / "mgpu-vulkan-cuda-external-semaphore-probe"
CPU_SYNC_PROBE = BUILD / "mgpu-cpu-sync-p2p-probe"
FRAME_SYNC_PROBE = BUILD / "mgpu-cpu-sync-frame-probe"
CUDA_NATIVE_SYNC_PROBE = BUILD / "mgpu-cuda-native-sync-probe"
IMAGE_CUDA_P2P_PROBE = BUILD / "mgpu-vulkan-image-cuda-p2p-probe"
REMOTE_MVP_PROBE = ROOT / "scripts/run_d3d12_cross_adapter_frame_probe.sh"
REMOTE_NGX_PROFILES = (
    ROOT / "build/proton-resource-pair-worker-experimental",
    ROOT / "build/proton-resource-pair-worker",
    ROOT / "build/proton",
)

# Some Windows games use Steam identity variables to select their renderer
# even when Proton is launched directly. Keep these identifiers opt-in: the
# launcher still owns UMU/compat-data values and does not start Steam.
LAUNCHER_ENV_PASSTHROUGH = (
    "SteamAppId",
    "SteamGameId",
    "SteamClientLaunch",
    "SteamOverlayGameId",
    "PROTON_LOG",
    "PROTON_LOG_DIR",
    "SL_ENABLE_CONSOLE_LOGGING",
    "SL_LOG_LEVEL",
    "SL_LOG_NAME",
    "SL_LOG_PATH",
)


def apply_steam_runtime_context(environment: dict[str, str],
                                appid: str | None) -> str | None:
    """Opt into UMU/Steam API for a direct Proton launch.

    The default remains ``UMU_USE_STEAM=0``.  This opt-in is useful after the
    Steam client has been authenticated: Proton can then launch the shipping
    executable directly while its Steam API connects to the running client.
    """
    if os.environ.get("MGPU_USE_STEAM") != "1":
        return None
    selected = (appid or os.environ.get("MGPU_STEAM_APPID") or
                os.environ.get("SteamAppId") or "").strip()
    if not re.fullmatch(r"[0-9]+", selected):
        return "MGPU_USE_STEAM=1 requiere MGPU_STEAM_APPID numérico"
    environment["UMU_USE_STEAM"] = "1"
    environment["UMU_ID"] = f"umu-{selected}"
    environment["SteamAppId"] = selected
    environment["SteamGameId"] = selected
    return None


@dataclass
class Gpu:
    index: int
    name: str
    driver: str
    pci: str
    display_active: str
    utilization_percent: int
    memory_used_mib: int
    memory_total_mib: int

    @property
    def memory_free_mib(self) -> int:
        return self.memory_total_mib - self.memory_used_mib


@dataclass
class Game:
    appid: str
    name: str
    install_dir: str
    prefix: str
    executables: list[str]


def unescape_vdf(value: str) -> str:
    return value.replace('\\\\', '\\').replace('\\"', '"')


def read_vdf_value(text: str, key: str) -> str | None:
    match = re.search(rf'"{re.escape(key)}"\s+"([^"]*)"', text, re.IGNORECASE)
    return unescape_vdf(match.group(1)) if match else None


def steam_roots() -> list[Path]:
    home = Path.home()
    candidates = [
        home / ".steam/steam",
        home / ".steam/root",
        home / ".local/share/Steam",
        home / ".var/app/com.valvesoftware.Steam/data/Steam",
    ]
    return list(dict.fromkeys(path for path in candidates if path.exists()))


def steam_library_roots() -> list[Path]:
    libraries: list[Path] = []
    # Steam may not have written libraryfolders.vdf yet (for example before
    # the first login), while mounted libraries still contain valid ACF
    # manifests.  Keep this discovery bounded to explicit paths and the
    # conventional per-user mount locations; never recursively scan /media.
    configured = os.environ.get("MGPU_STEAM_LIBRARY_ROOTS", "")
    libraries.extend(
        Path(value).expanduser()
        for value in configured.split(os.pathsep)
        if value.strip()
    )
    home_name = Path.home().name
    for mount_base in (Path("/media") / home_name,
                       Path("/run/media") / home_name):
        if mount_base.is_dir():
            libraries.extend(sorted(mount_base.glob("*/SteamLibrary")))
    for root in steam_roots():
        libraries.append(root)
        library_file = root / "steamapps/libraryfolders.vdf"
        if not library_file.exists():
            continue
        text = library_file.read_text(encoding="utf-8", errors="replace")
        for match in re.finditer(r'"\d+"\s*\{[^{}]*?"path"\s+"([^"]+)"', text, re.DOTALL):
            libraries.append(Path(unescape_vdf(match.group(1))))
        for match in re.finditer(r'"path"\s+"([^"]+)"', text):
            libraries.append(Path(unescape_vdf(match.group(1))))
    return list(dict.fromkeys(path for path in libraries if path.exists()))


def game_executables(install_dir: Path) -> list[str]:
    results: list[str] = []
    if not install_dir.exists():
        return results
    for path in install_dir.rglob("*.exe"):
        if len(results) >= 32:
            break
        if any(part.lower() in {"redist", "redistributable", "support", "tools"}
               for part in path.parts):
            continue
        results.append(str(path))
    return results


def discover_games() -> list[Game]:
    games: list[Game] = []
    for library in steam_library_roots():
        steamapps = library / "steamapps"
        if not steamapps.exists():
            continue
        for manifest in sorted(steamapps.glob("appmanifest_*.acf")):
            text = manifest.read_text(encoding="utf-8", errors="replace")
            appid = read_vdf_value(text, "appid")
            name = read_vdf_value(text, "name")
            install_name = read_vdf_value(text, "installdir")
            if not appid or not name or not install_name:
                continue
            install_dir = steamapps / "common" / install_name
            prefix = steamapps / "compatdata" / appid / "pfx"
            games.append(Game(
                appid=appid,
                name=name,
                install_dir=str(install_dir),
                prefix=str(prefix),
                executables=game_executables(install_dir),
            ))
    unique: dict[str, Game] = {game.appid: game for game in games}
    return sorted(unique.values(), key=lambda game: game.name.lower())


def find_game(games: list[Game], query: str | None) -> Game | None:
    if not query:
        return None
    normalized = query.lower()
    exact = [game for game in games if game.appid == query]
    if exact:
        return exact[0]
    matches = [game for game in games if normalized in game.name.lower()]
    return matches[0] if len(matches) == 1 else None


def is_bridge_proxy(path: Path) -> bool:
    """Detect a bridge copied into the slot reserved for real DLSS."""
    markers = (b"_nvngx_real.dll", b"bridge-nvngx.dll")
    try:
        with path.open("rb") as stream:
            tail = b""
            while chunk := stream.read(1024 * 1024):
                data = tail + chunk
                if any(marker in data for marker in markers):
                    return True
                tail = data[-64:]
    except OSError:
        return False
    return False


def default_vkd3d_dir() -> Path | None:
    """Select the first complete project-owned VKD3D runtime."""
    for candidate in REMOTE_NGX_PROFILES:
        if ((candidate / "d3d12.dll").is_file()
                and (candidate / "d3d12core.dll").is_file()):
            return candidate
    return None


def runtime_status(game: Game | None, *, proton_override: str | None = None,
                   vkd3d_override: str | None = None) -> dict[str, Any]:
    if game is None:
        return {"game_selected": False, "available": False, "reason": "no se seleccionó juego"}
    roots = [Path(game.install_dir), Path(game.prefix) / "drive_c/windows/system32"]
    names = ("nvngx_dlssnr.dll", "nvngx_dlss.dll", "_nvngx.dll")
    found: dict[str, list[str]] = {name: [] for name in names}
    proxy_runtimes: list[str] = []
    searched: set[Path] = set()
    for root in roots:
        if not root.exists():
            continue
        candidates = [root / name for name in names]
        # Proton prefixes normally use system32, while community installers
        # may leave the DLLs beside the game executable or in a small subdir.
        # Keep the search bounded so a large Steam library cannot turn doctor
        # into an unbounded filesystem walk.
        try:
            for count, path in enumerate(root.rglob("*.dll")):
                if count >= 8192:
                    break
                if path.name.lower() in names:
                    candidates.append(path)
        except OSError:
            pass
        for candidate in candidates:
            normalized = candidate.resolve(strict=False)
            if normalized in searched:
                continue
            searched.add(normalized)
            if candidate.is_file() and candidate.name.lower() in names:
                name = candidate.name.lower()
                if name == "nvngx_dlss.dll" and is_bridge_proxy(candidate):
                    proxy_runtimes.append(str(candidate))
                else:
                    found[name].append(str(candidate))
    bridge_candidates = [
        Path(os.environ["NGX_BRIDGE_DIR"]) / "bridge-nvngx.dll"
        if os.environ.get("NGX_BRIDGE_DIR") else ROOT / "build/proton/bridge-nvngx.dll",
        ROOT / "build/proton-resource-pair-worker-experimental/bridge-nvngx.dll",
        ROOT / "build/proton-resource-pair-worker/bridge-nvngx.dll",
        ROOT / "build/proton/bridge-nvngx.dll",
        Path.home() / ".local/lib/dlss5-mgpu/bridge-nvngx.dll",
    ]
    bridge = [str(path) for path in bridge_candidates if path.exists()]
    profile_candidates = []
    if os.environ.get("NGX_BRIDGE_DIR"):
        profile_candidates.append(Path(os.environ["NGX_BRIDGE_DIR"]).expanduser())
    profile_candidates.extend([
        ROOT / "build/proton-resource-pair-worker-experimental",
        ROOT / "build/proton-resource-pair-worker",
        ROOT / "build/proton",
    ])
    remote_profile = next((profile for profile in profile_candidates if all(
        (profile / name).is_file() for name in (
            "_nvngx.dll", "bridge-nvngx.dll", "_nvngx_real.dll",
            "nvngx_dlss_real.dll", "nvngx_dlssnr.dll"))), None)
    # A real game normally supplies nvngx_dlss.dll itself, while the
    # experimental NR DLL lives in the project profile. Do not require the NR
    # DLL to be copied into every game directory before the remote profile can
    # be prepared.
    complete = bool(found["nvngx_dlss.dll"] and
                    (found["nvngx_dlssnr.dll"] or remote_profile) and bridge)
    if proxy_runtimes:
        reason = "nvngx_dlss.dll detectado como proxy; falta runtime DLSS real"
    else:
        reason = "bridge y runtimes encontrados" if complete \
            else "faltan bridge-nvngx.dll o runtimes NGX locales"
    proton_value = (proton_override if proton_override is not None
                    else os.environ.get("PROTON", ""))
    proton_path = Path(proton_value).expanduser() if proton_value else None
    if vkd3d_override is not None:
        vkd3d_value = vkd3d_override
    else:
        vkd3d_value = os.environ.get("VKD3D_DLL_DIR", "")
        if not vkd3d_value:
            default_vkd3d = default_vkd3d_dir()
            vkd3d_value = str(default_vkd3d) if default_vkd3d else ""
    vkd3d_path = Path(vkd3d_value).expanduser() if vkd3d_value else None
    helper = ROOT / "build/mgpu-cuda-external-p2p-copy-helper"
    proton_ok = bool(proton_path and proton_path.is_file() and
                     os.access(proton_path, os.X_OK))
    vkd3d_ok = bool(vkd3d_path and
                    (vkd3d_path / "d3d12.dll").is_file() and
                    (vkd3d_path / "d3d12core.dll").is_file())
    transport_available = complete and remote_profile is not None and proton_ok \
        and vkd3d_ok and helper.is_file()
    if not complete:
        transport_reason = "faltan runtimes NGX o bridge"
    elif remote_profile is None:
        transport_reason = "falta un perfil NGX completo para el worker remoto"
    elif not proton_ok:
        transport_reason = "PROTON no apunta a un launcher ejecutable"
    elif not vkd3d_ok:
        transport_reason = "VKD3D_DLL_DIR no contiene d3d12.dll y d3d12core.dll"
    elif not helper.is_file():
        transport_reason = "falta el helper CUDA P2P del proyecto"
    else:
        transport_reason = "Proton, VKD3D, bridge y helper remoto disponibles"
    return {
        "game_selected": True,
        "available": complete,
        "transport_available": transport_available,
        "transport_reason": transport_reason,
        "proton": str(proton_path) if proton_path else "",
        "vkd3d": str(vkd3d_path) if vkd3d_path else "",
        "helper": str(helper),
        "remote_profile": str(remote_profile) if remote_profile else "",
        "remote_runtime": {
            "core": str(remote_profile / "_nvngx_real.dll")
            if remote_profile else "",
            "dlss": str(remote_profile / "nvngx_dlss_real.dll")
            if remote_profile else "",
            "nr": str(remote_profile / "nvngx_dlssnr.dll")
            if remote_profile else "",
        },
        "bridge": bridge,
        "runtimes": found,
        "proxy_runtimes": proxy_runtimes,
        "reason": reason,
    }


def write_profile(game: Game, plan: dict[str, Any], runtime: dict[str, Any]) -> Path:
    profile_dir = Path.home() / ".config/dlss5-mgpu/games"
    profile_dir.mkdir(parents=True, exist_ok=True)
    profile_path = profile_dir / f"{game.appid}.toml"
    render_gpu = plan.get("render_gpu")
    neural_gpu = plan.get("neural_gpu")
    lines = [
        "[game]",
        f'name = {json.dumps(game.name, ensure_ascii=False)}',
        f'appid = {json.dumps(game.appid)}',
        f'install_dir = {json.dumps(game.install_dir)}',
        f'prefix = {json.dumps(game.prefix)}',
        "",
        "[gpu]",
        f"render = {json.dumps(str(render_gpu if render_gpu is not None else 'auto'))}",
        f"neural = {json.dumps(str(neural_gpu if neural_gpu is not None else 'auto'))}",
        "output = \"auto\"",
        "",
        "[neural]",
        "enabled = true",
        "frame_generation = false",
        "",
        "[safety]",
        "fallback_local = true",
        "anti_cheat = \"deny\"",
        "watchdog_seconds = 10",
        "",
        "[runtime]",
        f"status = {json.dumps(runtime.get('reason', 'unknown'))}",
        f"bridge = {json.dumps(runtime.get('bridge', []))}",
        "",
    ]
    profile_path.write_text("\n".join(lines), encoding="utf-8")
    return profile_path


def launch_preparation(game: Game | None, plan: dict[str, Any],
                       runtime: dict[str, Any]) -> dict[str, Any]:
    """Return a safe launch policy without starting Proton or touching a prefix."""
    if game is None:
        return {
            "ready": False,
            "mode": "none",
            "fallback_local": True,
            "reason": "no se seleccionó un juego Steam",
        }
    if plan.get("status") == "READY_REMOTE":
        executable = Path(game.executables[0]).resolve() if game.executables else None
        proton = runtime.get("proton", "")
        prefix = Path(game.prefix).resolve()
        bridge_dir = runtime.get("remote_profile", "") or (
            str(Path(runtime["bridge"][0]).resolve().parent)
            if runtime.get("bridge") else ""
        )
        render_gpu = plan.get("render_gpu")
        environment = {
            "STEAM_COMPAT_DATA_PATH": str(prefix),
            "STEAM_COMPAT_CLIENT_INSTALL_PATH": str(Path(proton).resolve().parent.parent)
            if proton else "",
            "UMU_ID": f"dlss5-mgpu-{game.appid}",
            "UMU_USE_STEAM": "0",
            "VKD3D_VULKAN_DEVICE": str(render_gpu if render_gpu is not None else 0),
            "VKD3D_DUPLICATE_LUID_ADAPTERS": "1",
            "VKD3D_EXPORT_RESOURCE_FD": "1",
            "VKD3D_EXPORT_HEAP_FD": "1",
            "MGPU_REMOTE_TRANSPORT": "resource-fd-pair-worker-remote-ngx",
            "MGPU_DLSSNR_TRANSPORT": "resource-fd-pair-worker",
            "MGPU_NGX_CROSS_ADAPTER": "1",
            "MGPU_REMOTE_DIRECTIONS": "forward",
            "MGPU_NGX_PRIME_SOURCE": "0",
            "MGPU_DLSSNR_SKIP_LOCAL_NGX": "1",
            "MGPU_DLSSNR_REMOTE_NGX_INIT_PROBE": "1",
            "MGPU_DLSSNR_REMOTE_NGX_FEATURE": "1",
            "MGPU_DLSSNR_REMOTE_QUEUE_PROBE": "1",
            "MGPU_DLSSNR_VALIDATE_REMOTE_OUTPUT": "1",
            "MGPU_CROSS_ADAPTER_REQUIRE_DISTINCT_IDENTITY": "1",
            "MGPU_CROSS_ADAPTER_GPU_NATIVE": "0",
            # Proton otherwise hides NVAPI from many Vulkan/D3D12 titles;
            # without it Streamline can load while DLSS stays unavailable.
            "PROTON_ENABLE_NVAPI": os.environ.get("PROTON_ENABLE_NVAPI", "1"),
            "MGPU_CUDA_WORKER_HELPER": str(runtime.get("helper", "")),
            "NGX_BRIDGE_DIR": bridge_dir,
            "MGPU_NGX_PROXY_DLL": str(Path(bridge_dir) / "_nvngx.dll")
            if bridge_dir else "",
            "MGPU_NGX_BRIDGE_DLL": str(Path(bridge_dir) / "bridge-nvngx.dll")
            if bridge_dir else "",
            "NVIDIA_WINE_DLL_DIR": bridge_dir,
            "WINEDLLPATH": os.pathsep.join(
                path for path in (
                    bridge_dir, str(runtime.get("vkd3d", "")),
                    os.environ.get("WINEDLLPATH", ""),
                ) if path
            ),
            "WINEDLLOVERRIDES": (
                "_nvngx=n,b;d3d12=n,b;d3d12core=n,b;"
                "nvngx_dlss=n;nvngx_dlssnr=n"
            ),
            "MGPU_NGX_CORE_DLL": runtime.get("remote_runtime", {}).get("core", ""),
            "DLSS_RUNTIME_DLL": runtime.get("remote_runtime", {}).get("dlss", ""),
            "DLSS_NR_DLL": runtime.get("remote_runtime", {}).get("nr", ""),
            "VKD3D_DLL_DIR": runtime.get("vkd3d", ""),
        }
        # Preserve opt-in VKD3D identity selectors in the generated policy.
        # Proton prefixes can otherwise hide these diagnostics when the
        # launcher replaces the environment with its explicit policy.
        for variable in (
                "VKD3D_DUPLICATE_LUID_INDEX",
                "VKD3D_DUPLICATE_LUID_INDEX_PER_DEVICE"):
            if os.environ.get(variable):
                environment[variable] = os.environ[variable]
        for variable in LAUNCHER_ENV_PASSTHROUGH:
            if os.environ.get(variable):
                environment[variable] = os.environ[variable]
        steam_error = apply_steam_runtime_context(environment, game.appid)
        if steam_error:
            environment["MGPU_STEAM_CONTEXT_ERROR"] = steam_error
        command = [proton, "run", str(executable)] if proton and executable else []
        return {
            "ready": True,
            "mode": "remote-neural",
            "fallback_local": True,
            "render_gpu": plan.get("render_gpu"),
            "neural_gpu": plan.get("neural_gpu"),
            "bridge": runtime.get("bridge", []),
            "command": command,
            "cwd": str(executable.parent) if executable else str(prefix),
            "env": environment,
            "reason": "bridge, Proton, VKD3D y helper verificados; lanzamiento remoto opt-in",
        }
    return {
        "ready": True,
        "mode": "local-fallback",
        "fallback_local": True,
        "render_gpu": plan.get("render_gpu"),
        "neural_gpu": plan.get("neural_gpu"),
        "reason": plan.get("reason", "modo remoto no aprobado"),
    }


def direct_launch_policy(executable: Path, runner: str, args: list[str],
                         prefix: Path | None, plan: dict[str, Any],
                         render_gpu: Gpu | None) -> dict[str, Any]:
    """Build a reproducible Wine/Proton command without mutating the prefix."""
    runner_path = Path(runner)
    is_proton = runner_path.name.lower() == "proton"
    command = ([runner, "run", str(executable), *args] if is_proton
               else [runner, str(executable), *args])
    env = {
        "VKD3D_VULKAN_DEVICE": str(render_gpu.index) if render_gpu else "0",
        "VKD3D_FILTER_DEVICE_NAME": render_gpu.name if render_gpu else "NVIDIA",
        "DXVK_FILTER_DEVICE_NAME": render_gpu.name if render_gpu else "NVIDIA",
        "DLSS5_MGPU_MODE": "local-fallback",
    }
    if prefix is not None:
        if is_proton:
            # Proton expects a compat-data root containing `pfx/`, while Wine
            # expects the WINEPREFIX itself. Keep the path explicit and isolated.
            env["STEAM_COMPAT_DATA_PATH"] = str(prefix)
            env["UMU_ID"] = "dlss5-mgpu-direct"
            env["UMU_USE_STEAM"] = "0"
            env["STEAM_COMPAT_CLIENT_INSTALL_PATH"] = str(
                runner_path.resolve().parent.parent)
        else:
            env["WINEPREFIX"] = str(prefix)
    return {
        "command": command,
        "cwd": str(executable.parent),
        "env": env,
        "mode": "local-fallback",
        "fallback_local": True,
        "render_gpu": render_gpu.index if render_gpu else None,
        "neural_gpu": plan.get("neural_gpu"),
        "reason": "bridge NGX remoto no disponible; se ejecuta sólo en la GPU render",
    }


def direct_remote_launch_policy(executable: Path, runner: str, args: list[str],
                                prefix: Path | None, plan: dict[str, Any],
                                runtime: dict[str, Any]) -> dict[str, Any]:
    """Build the opt-in remote policy for a non-Steam executable.

    The direct path deliberately uses the same ``launch_preparation`` policy
    as the Steam path.  It only changes the executable and compatibility-data
    identity; it does not invent a second transport or silently fall back to
    local NGX when the caller explicitly requested remote mode.
    """
    if prefix is None:
        return {
            "ready": False,
            "mode": "remote-neural",
            "fallback_local": False,
            "reason": "el modo remoto directo requiere --prefix explícito",
        }
    if plan.get("status") != "READY_REMOTE":
        return {
            "ready": False,
            "mode": "remote-neural",
            "fallback_local": False,
            "reason": plan.get("reason", "el plan remoto no está listo"),
        }
    synthetic_game = Game(
        appid="direct",
        name=executable.stem,
        install_dir=str(executable.parent),
        prefix=str(prefix),
        executables=[str(executable)],
    )
    policy = launch_preparation(synthetic_game, plan, runtime)
    if not policy.get("ready"):
        policy["fallback_local"] = False
        return policy
    policy["command"] = [str(runner), "run", str(executable), *args]
    policy["cwd"] = str(executable.parent)
    policy["env"]["STEAM_COMPAT_DATA_PATH"] = str(prefix)
    policy["env"]["UMU_ID"] = "dlss5-mgpu-direct"
    policy["env"]["UMU_USE_STEAM"] = "0"
    steam_error = apply_steam_runtime_context(policy["env"], None)
    if steam_error:
        policy["ready"] = False
        policy["fallback_local"] = False
        policy["reason"] = steam_error
        return policy
    policy["env"]["STEAM_COMPAT_CLIENT_INSTALL_PATH"] = str(
        Path(runner).resolve().parent.parent)
    policy["reason"] = (
        "ejecutable directo: bridge, Proton, VKD3D y helper verificados; "
        "transporte remoto CPU-gated opt-in"
    )
    return policy


def infer_direct_install_root(executable: Path) -> Path:
    """Find a bounded install root containing a game DLSS runtime.

    Unreal packages commonly put ``nvngx_dlss.dll`` below ``Binaries`` in a
    plugin directory rather than beside the shipping executable.  Walking a
    few ancestors keeps direct discovery useful without recursively scanning
    the whole filesystem.
    """
    for candidate in (executable.parent, *executable.parents[:8]):
        try:
            for path in candidate.rglob("nvngx_dlss.dll"):
                if path.is_file() and not is_bridge_proxy(path):
                    return candidate
        except OSError:
            continue
    return executable.parent


def owned_process_ids(tokens: list[str], exclude: set[int] | None = None) -> set[int]:
    """Find this launch's Wine children without matching unrelated Wine runs."""
    excluded = exclude or set()
    normalized = [token.lower() for token in tokens if token]
    windows = [token.replace("/", "\\").lower()
               for token in tokens if token and "/" in token]
    needles = normalized + windows
    result: set[int] = set()
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        pid = int(entry.name)
        if pid in excluded:
            continue
        try:
            cmdline = (entry / "cmdline").read_bytes().replace(b"\x00", b" ").lower()
            environ = (entry / "environ").read_bytes().replace(b"\x00", b" ").lower()
        except OSError:
            continue
        if any(needle.encode() in cmdline or needle.encode() in environ
               for needle in needles):
            result.add(pid)
    return result


def process_ancestor_ids(pid: int) -> set[int]:
    """Return ``pid`` and its live parent chain from procfs."""
    result: set[int] = set()
    current = pid
    while current > 1 and current not in result:
        result.add(current)
        try:
            stat = Path(f"/proc/{current}/stat").read_text(encoding="utf-8")
            fields = stat[stat.rfind(") ") + 2:].split()
            current = int(fields[1])
        except (OSError, IndexError, ValueError):
            break
    return result


def terminate_owned_processes(tokens: list[str], root_pid: int,
                              grace_seconds: float = 5.0) -> None:
    """Stop only processes carrying the explicit runner/executable identity."""
    # The invoking shell and its ancestors often contain the same --prefix/
    # --exe text in their command line.  Exclude the complete process chains
    # for both this cleanup process and the launched root, not just the
    # immediate parent; otherwise a matching outer shell could be terminated.
    excluded = process_ancestor_ids(os.getpid())
    excluded.update(process_ancestor_ids(root_pid))
    pids = owned_process_ids(tokens, excluded)
    for pid in pids:
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass
    deadline = time.monotonic() + grace_seconds
    while time.monotonic() < deadline:
        remaining = owned_process_ids(tokens, excluded)
        if not remaining:
            return
        time.sleep(0.05)
    for pid in owned_process_ids(tokens, excluded):
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass


def execute_direct(policy: dict[str, Any], timeout_seconds: int) -> dict[str, Any]:
    command = [str(item) for item in policy["command"]]
    env = os.environ.copy()
    env.update({str(key): str(value) for key, value in policy["env"].items()})
    # Proton creates the lock/pfx below compat-data itself, but it expects the
    # explicitly selected root to exist before opening that lock.  Creating
    # only this user-selected directory is part of launching; no DLLs or
    # existing prefix contents are overwritten here.
    for variable in ("STEAM_COMPAT_DATA_PATH", "WINEPREFIX"):
        value = env.get(variable)
        if value:
            Path(value).expanduser().mkdir(parents=True, exist_ok=True)
    process = subprocess.Popen(command, cwd=policy["cwd"], env=env,
                               start_new_session=True)
    cleanup_tokens = [
        token for token in command
        if token.lower().endswith((".exe", ".com"))
        or Path(token).name.lower() == "proton"
    ]
    cleanup_tokens.extend(env.get(variable, "")
                          for variable in ("STEAM_COMPAT_DATA_PATH", "WINEPREFIX"))
    try:
        return_code = process.wait(timeout=timeout_seconds or None)
        terminate_owned_processes(cleanup_tokens, process.pid)
        return {"started": True, "return_code": return_code, "timed_out": False}
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            return_code = process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            return_code = process.wait(timeout=5)
        terminate_owned_processes(cleanup_tokens, process.pid)
        return {"started": True, "return_code": return_code, "timed_out": True}


def run(command: list[str], check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=check)


def parse_device_uuid_maps(output: str) -> tuple[dict[int, str], dict[int, str]]:
    """Return Vulkan/CUDA index maps without assuming their enumeration order."""
    vulkan = {
        int(index): uuid.lower()
        for index, uuid in re.findall(
            r"Vulkan (\d+):.*?UUID=([0-9a-f:]+)", output, re.IGNORECASE)
    }
    cuda = {
        int(index): uuid.lower()
        for index, uuid in re.findall(
            r"CUDA (\d+):.*?UUID=([0-9a-f:]+)", output, re.IGNORECASE)
    }
    return vulkan, cuda


def parse_int(value: str) -> int:
    try:
        return int(value.strip())
    except ValueError:
        return 0


def discover_gpus() -> list[Gpu]:
    if shutil.which("nvidia-smi") is None:
        raise RuntimeError("nvidia-smi no está disponible")
    query = [
        "index,name,driver_version,pci.bus_id,display_active,"
        "utilization.gpu,memory.used,memory.total"
    ]
    result = run(["nvidia-smi", "--query-gpu=" + query[0],
                  "--format=csv,noheader,nounits"])
    gpus: list[Gpu] = []
    for line in result.stdout.splitlines():
        fields = [field.strip() for field in line.split(",")]
        if len(fields) != 8:
            continue
        gpus.append(Gpu(
            index=parse_int(fields[0]),
            name=fields[1],
            driver=fields[2],
            pci=fields[3],
            display_active=fields[4],
            utilization_percent=parse_int(fields[5]),
            memory_used_mib=parse_int(fields[6]),
            memory_total_mib=parse_int(fields[7]),
        ))
    return gpus


def p2p_report() -> dict[str, Any]:
    if not P2P_PROBE.exists():
        return {"available": False, "error": "build/mgpu-p2p-probe no existe"}
    result = run([str(P2P_PROBE), "--json", "--warmup", "5", "--iterations", "20"], check=False)
    if result.returncode != 0:
        return {"available": False, "error": result.stderr.strip() or result.stdout.strip()}
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        return {"available": False, "error": f"JSON P2P inválido: {error}"}
    return {"available": True, "report": payload}


def interop_report() -> dict[str, Any]:
    if not VULKAN_PROBE.exists():
        return {"available": False, "error": "build/mgpu-vulkan-cuda-probe no existe"}

    discovery = run([
        str(VULKAN_PROBE), "--vulkan-gpu", "0", "--cuda-source", "0",
        "--cuda-destination", "1", "--bytes", str(1920 * 1080 * 4),
    ], check=False)
    vulkan_uuids, cuda_uuids = parse_device_uuid_maps(discovery.stdout + discovery.stderr)
    cuda_by_uuid = {uuid: index for index, uuid in cuda_uuids.items()}
    if not vulkan_uuids or not cuda_by_uuid:
        return {
            "available": False,
            "error": "no se pudo construir el mapa Vulkan/CUDA por UUID",
            "discovery_output": discovery.stdout + discovery.stderr,
        }

    results = []
    for vulkan_gpu in (0, 1):
        source = cuda_by_uuid.get(vulkan_uuids.get(vulkan_gpu, ""))
        destinations = [index for index in cuda_uuids if index != source]
        if source is None or not destinations:
            results.append({
                "vulkan_gpu": vulkan_gpu,
                "cuda_source": source,
                "cuda_destination": None,
                "success": False,
                "output": "no hay correspondencia CUDA por UUID o GPU destino",
            })
            continue
        destination = destinations[0]
        result = run([
            str(VULKAN_PROBE), "--vulkan-gpu", str(vulkan_gpu),
            "--cuda-source", str(source), "--cuda-destination", str(destination),
            "--bytes", str(1920 * 1080 * 4),
        ], check=False)
        output = result.stdout + result.stderr
        results.append({
            "vulkan_gpu": vulkan_gpu,
            "cuda_source": source,
            "cuda_destination": destination,
            "success": result.returncode == 0 and "validation=ok" in output,
            "output": output,
        })
    return {
        "available": all(item["success"] for item in results),
        "directions": results,
        "mapping": {str(index): cuda_by_uuid.get(uuid)
                    for index, uuid in vulkan_uuids.items() if index in (0, 1)},
    }


def vulkan_cuda_external_semaphore_report() -> dict[str, Any]:
    """Validate native Vulkan<->CUDA opaque-FD semaphore handoff.

    This is deliberately reported separately from the D3D12/VKD3D native-sync
    check: passing here proves the driver path for a native Vulkan device, not
    that VKD3D can export an external fence/semaphore for a D3D12 resource.
    """
    if not VULKAN_CUDA_SEMAPHORE_PROBE.exists():
        return {
            "available": False,
            "error": "build/mgpu-vulkan-cuda-external-semaphore-probe no existe",
        }
    directions: list[dict[str, Any]] = []
    for vulkan_gpu, cuda_device in ((0, 1), (1, 0)):
        result = run([
            str(VULKAN_CUDA_SEMAPHORE_PROBE),
            "--vulkan-gpu", str(vulkan_gpu),
            "--cuda-device", str(cuda_device),
            "--json",
        ], check=False)
        output = result.stdout + result.stderr
        try:
            payload = json.loads(result.stdout)
        except json.JSONDecodeError as error:
            directions.append({
                "vulkan_gpu": vulkan_gpu,
                "cuda_device": cuda_device,
                "success": False,
                "error": f"JSON semáforo Vulkan/CUDA inválido: {error}",
                "output": output,
            })
            continue
        success = (result.returncode == 0
                   and payload.get("available", False)
                   and payload.get("vulkan_to_cuda", False)
                   and payload.get("cuda_to_vulkan", False))
        directions.append({
            "vulkan_gpu": vulkan_gpu,
            "cuda_device": cuda_device,
            "success": success,
            "report": payload,
            "output": output if not success else "",
        })
    return {
        "available": bool(directions) and all(item["success"] for item in directions),
        "directions": directions,
        "scope": "native Vulkan/CUDA only; D3D12/VKD3D GPU-native sync remains pending",
    }


def cpu_sync_report() -> dict[str, Any]:
    """Validate the CPU-gated P2P fallback with a bounded stall timeout."""
    if not CPU_SYNC_PROBE.exists():
        return {"available": False, "error": "build/mgpu-cpu-sync-p2p-probe no existe"}
    result = run([
        str(CPU_SYNC_PROBE), "--source", "0", "--destination", "1",
        "--frames", "120", "--timeout-ms", "5000", "--json",
    ], check=False)
    output = result.stdout + result.stderr
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        return {"available": False, "error": f"JSON CPU sync inválido: {error}",
                "output": output}
    return {
        "available": result.returncode == 0 and payload.get("validation_passed", False),
        "report": payload,
        "output": output if result.returncode != 0 else "",
    }


def frame_sync_report() -> dict[str, Any]:
    """Validate CPU-gated transfer of a synchronized color/motion/depth frame."""
    if not FRAME_SYNC_PROBE.exists():
        return {"available": False, "error": "build/mgpu-cpu-sync-frame-probe no existe"}
    result = run([
        str(FRAME_SYNC_PROBE), "--source", "0", "--destination", "1",
        "--frames", "120", "--timeout-ms", "5000", "--json",
    ], check=False)
    output = result.stdout + result.stderr
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        return {"available": False, "error": f"JSON frame sync inválido: {error}",
                "output": output}
    return {
        "available": result.returncode == 0 and payload.get("validation_passed", False),
        "report": payload,
        "output": output if result.returncode != 0 else "",
    }


def cuda_native_sync_report() -> dict[str, Any]:
    """Validate GPU-to-GPU CUDA event waits without CPU gating the copy."""
    if not CUDA_NATIVE_SYNC_PROBE.exists():
        return {"available": False, "error": "build/mgpu-cuda-native-sync-probe no existe"}
    result = run([
        str(CUDA_NATIVE_SYNC_PROBE), "--source", "0", "--destination", "1",
        "--frames", "120", "--timeout-ms", "5000", "--json",
    ], check=False)
    output = result.stdout + result.stderr
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        return {"available": False, "error": f"JSON CUDA native sync inválido: {error}",
                "output": output}
    available = (result.returncode == 0 and payload.get("validation_passed", False)
                 and payload.get("gpu_native_waits", False))
    return {
        "available": available,
        "report": payload,
        "output": output if result.returncode != 0 else "",
    }


def image_cuda_p2p_report() -> dict[str, Any]:
    """Validate the linear image-allocation fallback in both directions."""
    if not IMAGE_CUDA_P2P_PROBE.exists():
        return {"available": False,
                "error": "build/mgpu-vulkan-image-cuda-p2p-probe no existe"}
    directions: list[dict[str, Any]] = []
    for source, destination in ((0, 1), (1, 0)):
        result = run([str(IMAGE_CUDA_P2P_PROBE), str(source), str(destination)],
                     check=False)
        output = result.stdout + result.stderr
        try:
            payload = json.loads(result.stdout)
        except json.JSONDecodeError as error:
            directions.append({
                "source": source,
                "destination": destination,
                "success": False,
                "error": f"JSON imagen/CUDA inválido: {error}",
                "output": output,
            })
            continue
        directions.append({
            "source": source,
            "destination": destination,
            "success": result.returncode == 0
            and payload.get("cuda_image_allocation_p2p", False)
            and payload.get("readback_ok", False),
            "report": payload,
            "output": output if result.returncode != 0 else "",
        })
    return {"available": all(item["success"] for item in directions),
            "directions": directions}


def remote_mvp_report() -> dict[str, Any]:
    """Run the explicit CPU-gated cross-adapter NGX laboratory MVP."""
    transport_setting = os.environ.get("MGPU_REMOTE_TRANSPORT", "linear").lower()
    remote_ngx_transport = transport_setting in {
        "resource-fd-pair-worker-remote-ngx",
        "resource-fd-pair-worker-sequential-dual",
        "resource-fd-pair-worker-remote-ngx-persistent",
    }
    base_environment = os.environ.copy()
    if remote_ngx_transport:
        # The ordinary build/proton bridge only exercises local NGX. Prefer the
        # pair-worker profile as a coherent set so an automatic run cannot
        # silently validate the wrong bridge while still passing local gates.
        profile_files = {
            "NGX_BRIDGE_DIR": "bridge-nvngx.dll",
            "MGPU_NGX_CORE_DLL": "_nvngx_real.dll",
            "DLSS_RUNTIME_DLL": "nvngx_dlss_real.dll",
            "DLSS_NR_DLL": "nvngx_dlssnr.dll",
        }
        for profile in REMOTE_NGX_PROFILES:
            if all((profile / filename).is_file()
                   for filename in profile_files.values()):
                for variable, filename in profile_files.items():
                    base_environment.setdefault(variable, str(profile / filename)
                                               if variable != "NGX_BRIDGE_DIR"
                                               else str(profile))
                break
    required = ("PROTON", "NGX_SDK_DIR", "DLSS_RUNTIME_DLL", "DLSS_NR_DLL",
                "VKD3D_DLL_DIR")
    missing = [name for name in required if not base_environment.get(name)]
    if not base_environment.get("MGPU_NGX_CORE_DLL") and not base_environment.get("DLSS_DEMO_DIR"):
        missing.append("DLSS_DEMO_DIR o MGPU_NGX_CORE_DLL")
    if missing:
        return {
            "available": False,
            "error": "faltan variables requeridas: " + ", ".join(missing),
        }
    if not REMOTE_MVP_PROBE.is_file():
        return {"available": False, "error": "falta el probe MVP combinado"}

    direction_setting = base_environment.get("MGPU_REMOTE_DIRECTIONS", "forward").lower()
    if direction_setting not in {"forward", "reverse", "both"}:
        return {"available": False,
                "error": "MGPU_REMOTE_DIRECTIONS debe ser forward, reverse o both"}
    if transport_setting not in {"linear", "resource-fd", "resource-pair-daemon",
                                 "resource-fd-pair-worker",
                                 "resource-fd-pair-worker-remote-ngx",
                                 "resource-fd-pair-worker-sequential-dual",
                                 "resource-fd-pair-worker-remote-ngx-persistent"}:
        return {"available": False,
                "error": "MGPU_REMOTE_TRANSPORT debe ser linear, resource-fd, resource-pair-daemon, resource-fd-pair-worker, resource-fd-pair-worker-remote-ngx, resource-fd-pair-worker-sequential-dual o resource-fd-pair-worker-remote-ngx-persistent"}
    resource_fd_transport = transport_setting in {"resource-fd", "resource-pair-daemon",
                                                  "resource-fd-pair-worker",
                                                  "resource-fd-pair-worker-remote-ngx",
                                                  "resource-fd-pair-worker-sequential-dual",
                                                  "resource-fd-pair-worker-remote-ngx-persistent"}
    resource_daemon_transport = transport_setting == "resource-pair-daemon"
    bridge_pair_worker_transport = transport_setting in {
        "resource-fd-pair-worker", "resource-fd-pair-worker-remote-ngx",
        "resource-fd-pair-worker-sequential-dual",
        "resource-fd-pair-worker-remote-ngx-persistent"}
    sequential_dual_transport = (
        transport_setting == "resource-fd-pair-worker-sequential-dual")
    persistent_remote_transport = (
        transport_setting == "resource-fd-pair-worker-remote-ngx-persistent")
    presentation_requested = os.environ.get("MGPU_REMOTE_PRESENT", "0") == "1"
    raster_requested = os.environ.get("MGPU_REMOTE_RASTER", "0") == "1"
    frame_loop_requested = os.environ.get("MGPU_REMOTE_FRAME_LOOP", "0") == "1"
    frame_loop_frames = max(
        2, int(os.environ.get("MGPU_REMOTE_FRAME_LOOP_FRAMES", "3")))
    if presentation_requested and not remote_ngx_transport:
        return {
            "available": False,
            "error": "MGPU_REMOTE_PRESENT requiere un transporte remote-ngx",
        }
    presentation_frames = max(
        1, int(os.environ.get("MGPU_REMOTE_PRESENT_FRAMES", "3")))
    directions = (False, True) if direction_setting == "both" else (
        direction_setting == "reverse",
    )
    reports: list[dict[str, Any]] = []
    outputs: list[str] = []
    failures: list[str] = []
    for reverse in directions:
        environment = base_environment.copy()
        environment["MGPU_NGX_CROSS_ADAPTER"] = "1"
        environment["MGPU_CROSS_ADAPTER_REVERSE"] = "1" if reverse else "0"
        environment["MGPU_CROSS_ADAPTER_RESOURCE_FD"] = (
            "1" if resource_fd_transport else "0")
        environment["MGPU_CROSS_ADAPTER_RESOURCE_DAEMON"] = (
            "1" if resource_daemon_transport else "0")
        if bridge_pair_worker_transport:
            environment["MGPU_DLSSNR_TRANSPORT"] = "resource-fd-pair-worker"
            environment.setdefault(
                "MGPU_CUDA_WORKER_HELPER",
                str(ROOT / "build/mgpu-cuda-external-p2p-copy-helper"))
            environment.setdefault("MGPU_CUDA_PAIR_WORKER_PORT", "47951")
            if remote_ngx_transport:
                # NGX still keeps process-global state in the experimental
                # bridge. Prime the destination/remote adapter first; an
                # A-first probe makes the second adapter return 0xbad00007.
                environment["MGPU_NGX_PRIME_SOURCE"] = "0"
                environment["MGPU_DLSSNR_SKIP_LOCAL_NGX"] = "1"
                environment["MGPU_DLSSNR_REMOTE_NGX_INIT_PROBE"] = "1"
                environment["MGPU_DLSSNR_REMOTE_NGX_FEATURE"] = "1"
                environment["MGPU_DLSSNR_REMOTE_QUEUE_PROBE"] = "1"
                environment["MGPU_DLSSNR_VALIDATE_REMOTE_OUTPUT"] = "1"
                environment.setdefault("MGPU_CUDA_OUTPUT_WORKER_PORT", "47952")
                if sequential_dual_transport:
                    environment["MGPU_DLSSNR_PROBE_LOCAL_AFTER_REMOTE"] = "1"
                    environment["MGPU_DLSSNR_RESET_REMOTE_BEFORE_LOCAL"] = "1"
                if persistent_remote_transport:
                    environment["MGPU_DLSSNR_REMOTE_NGX_PERSISTENT"] = "1"
                    environment["MGPU_NGX_FRAME_COUNT"] = os.environ.get(
                        "MGPU_REMOTE_NGX_FRAMES", "3")
            environment.setdefault("MGPU_REMOTE_ADAPTER_INDEX", "0")
        # These are runner-level features, not pair-worker features.  Keeping
        # them outside the branch above is important for the plain resource-FD
        # transport: otherwise the automatic gate silently ignores a
        # requested frame loop (and can report a false failure even though the
        # direct runner supports it).
        if resource_fd_transport:
            if presentation_requested:
                environment["MGPU_CROSS_ADAPTER_PRESENT"] = "1"
                environment["MGPU_PRESENT_FRAMES"] = str(presentation_frames)
                environment["MGPU_CROSS_ADAPTER_PRESENT_AUTO"] = os.environ.get(
                    "MGPU_CROSS_ADAPTER_PRESENT_AUTO", "1")
            if raster_requested:
                environment["MGPU_CROSS_ADAPTER_RASTER"] = "1"
            if frame_loop_requested:
                environment["MGPU_CROSS_ADAPTER_FRAME_LOOP"] = "1"
                environment["MGPU_CROSS_ADAPTER_FRAME_COUNT"] = str(frame_loop_frames)
        remote_log_path = Path(environment.get(
            "OUT_DIR", str(ROOT / "build/proton"))) / "dlssnr-proxy.log"
        remote_log_offset = 0
        if remote_ngx_transport:
            try:
                remote_log_offset = remote_log_path.stat().st_size
            except OSError:
                remote_log_offset = 0
        if resource_daemon_transport:
            environment["MGPU_CROSS_ADAPTER_DAEMON_REPEAT"] = os.environ.get(
                "MGPU_REMOTE_DAEMON_REPEAT", "8")
        result = subprocess.run([str(REMOTE_MVP_PROBE)], text=True,
                                capture_output=True, check=False, env=environment)
        output = result.stdout + result.stderr
        outputs.append(output)
        remote_status = {
            "log": str(remote_log_path),
            "evaluate": False,
            "submit": False,
            "output_returned": False,
            "output_validation": False,
            "local_after_remote_init": False,
            "local_after_remote_create": False,
            "local_after_remote_evaluate": False,
            "persistent_frames": 0,
            "presentation_success": False,
            "presentation_frames_presented": 0,
        }
        if remote_ngx_transport:
            try:
                with remote_log_path.open("rb") as log_file:
                    log_file.seek(remote_log_offset)
                    remote_log = log_file.read().decode("utf-8", errors="replace")
            except OSError:
                remote_log = ""
            remote_status["evaluate"] = (
                "remote_ngx_evaluate result=0x00000001" in remote_log)
            remote_status["submit"] = bool(re.search(
                r"remote_ngx_submit result=0x00000000 "
                r"device_removed=0x00000000 .*completed=[1-9][0-9]* wait=0",
                remote_log))
            remote_status["output_returned"] = (
                "output_return_copy=ok" in remote_log)
            remote_status["output_validation"] = (
                "output_return_validation=ok" in remote_log)
            if sequential_dual_transport:
                remote_status["local_after_remote_init"] = (
                    "local_after_remote_init result=0x00000001" in remote_log)
                remote_status["local_after_remote_create"] = (
                    "local_after_remote_create result=0x00000001" in remote_log)
                remote_status["local_after_remote_evaluate"] = (
                    "DLSSNR Evaluate result=0x00000001" in remote_log)
            if persistent_remote_transport:
                match = re.search(
                    r'"ngx_b_frames_completed"\s*:\s*([0-9]+)', output)
                remote_status["persistent_frames"] = (
                    int(match.group(1)) if match else 0)
        payload: dict[str, Any] | None = None
        for line in reversed(output.splitlines()):
            candidate = line.strip()
            if not candidate.startswith("{"):
                continue
            try:
                decoded = json.loads(candidate)
            except json.JSONDecodeError:
                continue
            if isinstance(decoded, dict) and "gpu_a_to_b" in decoded:
                payload = decoded
                break
        if payload is None:
            failures.append("el probe no produjo JSON de resultado")
            continue
        if presentation_requested:
            remote_status["presentation_success"] = payload.get(
                "presentation_success", False)
            remote_status["presentation_frames_presented"] = payload.get(
                "presentation_frames_presented", 0)
        gates = (payload.get("gpu_a_to_b", False),
                 payload.get("helper_p2p", False),
                 payload.get("queue_a_cpu_fence", False),
                 payload.get("queue_b_cpu_fence", False),
                 payload.get("readback_validation", False),
                 payload.get("ngx_b_evaluate", False),
                 payload.get("ngx_b_readback", False))
        if resource_fd_transport:
            gates += (payload.get("resource_fd_mode", False),
                      payload.get("resource_planes_readback", False))
        if resource_daemon_transport:
            gates += (payload.get("resource_daemon_mode", False),
                      payload.get("remote_output_returned", False),
                      payload.get("remote_output_nonzero", 0) > 0)
        if remote_ngx_transport:
            gates += (remote_status["evaluate"], remote_status["submit"],
                      remote_status["output_returned"],
                      remote_status["output_validation"])
            if sequential_dual_transport:
                gates += (remote_status["local_after_remote_init"],
                          remote_status["local_after_remote_create"],
                          remote_status["local_after_remote_evaluate"])
        if persistent_remote_transport:
            requested_frames = int(environment.get("MGPU_NGX_FRAME_COUNT", "3"))
            gates += (remote_status["persistent_frames"] >= requested_frames,)
        if presentation_requested:
            gates += (
                payload.get("presentation_requested", False),
                payload.get("presentation_success", False),
                payload.get("presentation_frames_presented", 0) >= presentation_frames,
            )
        if raster_requested:
            gates += (
                payload.get("raster_requested", False),
                payload.get("raster_ready", False),
                payload.get("raster_submitted", False),
                payload.get("readback_nonzero", 0) > 0,
            )
        if frame_loop_requested:
            gates += (
                payload.get("frame_loop_requested", False),
                payload.get("frame_loop_success", False),
                payload.get("frame_loop_payload_varied", False),
                payload.get("frame_loop_frames_completed", 0) >= frame_loop_frames,
            )
        direction_fields = {"reverse_direction", "source_cuda_ordinal",
                            "destination_cuda_ordinal"}
        direction_metadata_present = direction_fields.issubset(payload)
        actual_reverse = payload.get("reverse_direction")
        allowed_reverse = {reverse}
        if presentation_requested and not reverse and environment.get(
                "MGPU_CROSS_ADAPTER_PRESENT_AUTO", "1") == "1":
            allowed_reverse = {False, True}
        actual_source = 1 if actual_reverse else 0
        actual_destination = 0 if actual_reverse else 1
        direction_ok = direction_metadata_present and actual_reverse in allowed_reverse and (
            payload.get("source_cuda_ordinal") == actual_source
            and payload.get("destination_cuda_ordinal") == actual_destination
        )
        if direction_setting != "both" and not direction_metadata_present:
            direction_ok = True
        report_entry = {"reverse": reverse, "returncode": result.returncode,
                        "passed": result.returncode == 0 and all(gates) and direction_ok,
                        "report": payload}
        if remote_ngx_transport:
            report_entry["remote_ngx"] = remote_status
        if presentation_requested:
            report_entry["presentation"] = {
                "requested": payload.get("presentation_requested", False),
                "success": payload.get("presentation_success", False),
                "frames_requested": payload.get("presentation_frames_requested", 0),
                "frames_presented": payload.get("presentation_frames_presented", 0),
                "auto_orientation": environment.get(
                    "MGPU_CROSS_ADAPTER_PRESENT_AUTO", "1") == "1",
            }
        reports.append(report_entry)
        if result.returncode != 0 or not all(gates) or not direction_ok:
            failures.append("gate fallido en " + ("B→A" if reverse else "A→B"))
    available = len(reports) == len(directions) and not failures and all(
        item["passed"] for item in reports)
    report: dict[str, Any] = {
        "available": available,
        "transport": transport_setting,
        "presentation_requested": presentation_requested,
        "raster_requested": raster_requested,
        "frame_loop_requested": frame_loop_requested,
        "directions": reports,
    }
    if direction_setting != "both" and reports:
        report["report"] = reports[0]["report"]
    if remote_ngx_transport:
        report["remote_ngx"] = [item["remote_ngx"] for item in reports
                                 if "remote_ngx" in item]
    if failures:
        report["error"] = "; ".join(failures)
    if not available:
        report["output"] = "\n".join(outputs)
    return report


def select_plan(gpus: list[Gpu], p2p: dict[str, Any], interop: dict[str, Any],
                runtime: dict[str, Any] | None = None,
                cpu_sync: dict[str, Any] | None = None,
                image_cuda_p2p: dict[str, Any] | None = None) -> dict[str, Any]:
    plan: dict[str, Any] = {
        "status": "READY_LOCAL_ONLY",
        "render_gpu": None,
        "neural_gpu": None,
        "reason": "",
        "cpu_sync_p2p_available": bool(cpu_sync and cpu_sync.get("available")),
        "cpu_sync_frame_p2p_available": bool(
            cpu_sync and cpu_sync.get("frame_available")),
        "gpu_native_sync": "pending",
        "image_cuda_p2p_available": bool(
            image_cuda_p2p and image_cuda_p2p.get("available")),
    }
    if len(gpus) < 2:
        plan["status"] = "P2P_UNAVAILABLE"
        plan["reason"] = "Se necesitan al menos dos GPUs NVIDIA"
        return plan
    if not p2p.get("available"):
        plan["status"] = "P2P_UNAVAILABLE"
        plan["reason"] = p2p.get("error", "self-test P2P fallido")
        return plan
    if not interop.get("available"):
        plan["status"] = "VULKAN_CUDA_MISMATCH"
        plan["reason"] = "falló la validación Vulkan→CUDA→P2P"
        return plan

    display = [gpu for gpu in gpus
               if gpu.display_active.lower() in {"on", "enabled", "yes", "active"}]
    neural_candidates = display or sorted(gpus, key=lambda gpu: gpu.memory_free_mib, reverse=True)
    neural = neural_candidates[0]
    render_candidates = [gpu for gpu in gpus if gpu.index != neural.index]
    render = min(render_candidates, key=lambda gpu: gpu.utilization_percent)

    plan["render_gpu"] = render.index
    plan["neural_gpu"] = neural.index
    if neural.memory_free_mib < 4096:
        plan["reason"] = "La GPU neural tiene menos de 4 GiB libres"
        return plan
    if runtime is not None and not runtime.get("available"):
        plan["reason"] = runtime.get("reason", "runtimes NGX incompletos")
        return plan
    if runtime is not None and not runtime.get("transport_available", False):
        plan["reason"] = runtime.get(
            "transport_reason", "transporte remoto no implementado")
        return plan
    plan["status"] = "READY_REMOTE"
    plan["reason"] = "P2P, Vulkan/CUDA y memoria libre aprobados"
    return plan


def doctor(game_query: str | None = None) -> dict[str, Any]:
    gpus = discover_gpus()
    p2p = p2p_report()
    interop = interop_report()
    cpu_sync = cpu_sync_report()
    frame_sync = frame_sync_report()
    cuda_native_sync = cuda_native_sync_report()
    vulkan_cuda_semaphore = vulkan_cuda_external_semaphore_report()
    image_cuda_p2p = image_cuda_p2p_report()
    cpu_sync["frame_available"] = frame_sync.get("available", False)
    games = discover_games()
    game = find_game(games, game_query)
    runtime = runtime_status(game)
    plan = select_plan(gpus, p2p, interop, runtime, cpu_sync, image_cuda_p2p)
    return {
        "project": "DLSS5-MGPU-Ampere",
        "root": str(ROOT),
        "gpus": [asdict(gpu) | {"memory_free_mib": gpu.memory_free_mib} for gpu in gpus],
        "p2p": p2p,
        "interop": interop,
        "cpu_sync": cpu_sync,
        "cpu_sync_frame": frame_sync,
        "cuda_native_sync": cuda_native_sync,
        "vulkan_cuda_external_semaphore": vulkan_cuda_semaphore,
        "image_cuda_p2p": image_cuda_p2p,
        "games_found": len(games),
        "game": asdict(game) if game else None,
        "runtime": runtime,
        "plan": plan,
        "launch": launch_preparation(game, plan, runtime),
    }


def main() -> int:
    parser = argparse.ArgumentParser(prog="mgpu-auto")
    parser.add_argument("command", choices=("doctor", "selftest", "remote-selftest",
                                              "plan", "games", "run"))
    parser.add_argument("--game", help="Steam AppID o parte exacta del nombre")
    parser.add_argument("--exe", help="ejecutable Windows directo para una prueba aislada")
    parser.add_argument("--prefix", help="WINEPREFIX/Proton prefix para --exe")
    parser.add_argument("--runner", default="wine",
                        help="runner para --exe: wine, proton o ruta absoluta")
    parser.add_argument("--exe-arg", action="append", default=[],
                        help="argumento del ejecutable; repetir para varios")
    parser.add_argument("--timeout-seconds", type=int, default=0,
                        help="watchdog de --exe; 0 espera hasta que termine")
    parser.add_argument("--json", action="store_true", help="emit JSON")
    parser.add_argument("--write-profile", action="store_true",
                        help="escribe el perfil TOML del juego seleccionado")
    parser.add_argument("--dry-run", action="store_true",
                        help="no lanza nada; muestra el plan de ejecución")
    parser.add_argument("--enable-remote", action="store_true",
                        help="habilita el lanzamiento remoto opt-in para --game")
    args = parser.parse_args()

    if args.exe and args.command != "run":
        print("--exe solo es válido con run", file=sys.stderr)
        return 2
    if args.exe and args.game:
        print("--exe y --game son excluyentes", file=sys.stderr)
        return 2
    if args.exe and args.write_profile:
        print("--write-profile requiere un juego Steam descubierto, no --exe", file=sys.stderr)
        return 2

    direct_report: dict[str, Any] | None = None
    try:
        if args.command == "games":
            games = discover_games()
            report = {"games": [asdict(game) for game in games]}
        elif args.command == "remote-selftest":
            report = remote_mvp_report()
        elif args.command == "run" and args.exe:
            executable = Path(args.exe).expanduser().resolve()
            if not executable.is_file():
                print(f"No existe el ejecutable: {executable}", file=sys.stderr)
                return 2
            gpus = discover_gpus()
            p2p = p2p_report()
            interop = interop_report()
            prefix = (Path(args.prefix).expanduser().resolve()
                      if args.prefix else None)
            if args.enable_remote:
                runner_path = Path(args.runner).expanduser().resolve()
                install_root = infer_direct_install_root(executable)
                runtime = runtime_status(
                    Game("direct", executable.stem, str(install_root),
                         str(prefix) if prefix else "/__missing_prefix__",
                         [str(executable)]),
                    proton_override=str(runner_path),
                    vkd3d_override=os.environ.get("VKD3D_DLL_DIR"),
                )
            else:
                runtime = {
                    "available": False,
                    "reason": "prueba directa sin bridge NGX propietario",
                    "transport_available": False,
                }
            plan = select_plan(gpus, p2p, interop, runtime)
            render = next((gpu for gpu in gpus if gpu.index == plan.get("render_gpu")), None)
            direct_report = {
                "project": "DLSS5-MGPU-Ampere",
                "target": str(executable),
                "runtime": runtime,
                "gpus": [asdict(gpu) | {"memory_free_mib": gpu.memory_free_mib}
                         for gpu in gpus],
                "p2p": p2p,
                "interop": interop,
                "plan": plan,
            }
            if args.enable_remote:
                direct_report["launch_policy"] = direct_remote_launch_policy(
                    executable, args.runner, args.exe_arg, prefix, plan, runtime)
            else:
                direct_report["launch_policy"] = direct_launch_policy(
                    executable, args.runner, args.exe_arg, prefix, plan, render)
            report = direct_report
        else:
            report = doctor(args.game)
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"mgpu-auto: {error}", file=sys.stderr)
        return 1

    if args.command == "selftest":
        report = {
            "p2p": report["p2p"],
            "interop": report["interop"],
            "cuda_native_sync": report["cuda_native_sync"],
            "vulkan_cuda_external_semaphore": report["vulkan_cuda_external_semaphore"],
            "image_cuda_p2p": report["image_cuda_p2p"],
            "passed": report["p2p"].get("available", False)
            and report["interop"].get("available", False)
            and report["cuda_native_sync"].get("available", False)
            and report["vulkan_cuda_external_semaphore"].get("available", False)
            and report["image_cuda_p2p"].get("available", False),
        }
    elif args.command in ("plan", "run") and direct_report is None:
        report = report["plan"]

    if args.write_profile:
        if args.command not in ("plan", "run"):
            print("--write-profile solo es válido con plan o run", file=sys.stderr)
            return 2
        full_report = doctor(args.game)
        if not full_report.get("game"):
            print("No se encontró un único juego para --game", file=sys.stderr)
            return 2
        profile_path = write_profile(
            Game(**full_report["game"]), full_report["plan"], full_report["runtime"])
        report["profile"] = str(profile_path)

    if args.command == "run" and direct_report is not None:
        report["launch_policy"] = direct_report["launch_policy"]
        if args.dry_run:
            report["launch"] = "prepared_only"
            report["launch_reason"] = "dry-run: no se inició el ejecutable"
        elif args.enable_remote and not report["launch_policy"].get("ready", False):
            report["launch"] = "blocked"
            report["launch_reason"] = report["launch_policy"].get(
                "reason", "política remota directa incompleta")
        else:
            try:
                execution = execute_direct(
                    direct_report["launch_policy"], args.timeout_seconds)
                report["execution"] = execution
                if execution["timed_out"]:
                    report["launch"] = "timed_out"
                elif execution["return_code"] == 0:
                    report["launch"] = "finished"
                else:
                    report["launch"] = "failed"
                    report["launch_reason"] = (
                        f"el proceso terminó con código {execution['return_code']}"
                    )
            except (OSError, subprocess.SubprocessError) as error:
                report["launch"] = "failed"
                report["launch_reason"] = str(error)
    elif args.command == "run":
        if not args.game:
            print("run requiere --game APPID_O_NOMBRE", file=sys.stderr)
            return 2
        full_report = doctor(args.game)
        if not full_report.get("game"):
            report["launch"] = "blocked"
            report["launch_reason"] = (
                "no se encontró un único juego Steam para --game; no se prepara "
                "ningún lanzamiento"
            )
            if args.json:
                print(json.dumps(report, indent=2, ensure_ascii=False))
            else:
                print(json.dumps(report, indent=2, ensure_ascii=False))
            return 2
        report["launch_policy"] = full_report["launch"]
        if args.dry_run or report.get("status") != "READY_REMOTE":
            report["launch"] = "prepared_only"
            report["launch_reason"] = (
                "modo seguro: no se inicia Proton; se conserva fallback local y el "
                "bridge remoto queda pendiente de integración"
            )
        elif not (args.enable_remote or
                   os.environ.get("MGPU_AUTO_LAUNCH_REMOTE") == "1"):
            report["launch"] = "prepared_only"
            report["launch_reason"] = (
                "política remota preparada; usar --enable-remote o "
                "MGPU_AUTO_LAUNCH_REMOTE=1 para iniciar el juego"
            )
        else:
            try:
                execution = execute_direct(
                    report["launch"], args.timeout_seconds)
                report["execution"] = execution
                if execution["timed_out"]:
                    report["launch"] = "timed_out"
                elif execution["return_code"] == 0:
                    report["launch"] = "finished"
                else:
                    report["launch"] = "failed"
                    report["launch_reason"] = (
                        f"el proceso terminó con código {execution['return_code']}"
                    )
            except (OSError, subprocess.SubprocessError) as error:
                report["launch"] = "failed"
                report["launch_reason"] = str(error)

    if args.json:
        print(json.dumps(report, indent=2, ensure_ascii=False))
    else:
        if args.command == "doctor":
            for gpu in report["gpus"]:
                print(f"GPU{gpu['index']}: {gpu['name']} {gpu['pci']} "
                      f"display={gpu['display_active']} "
                      f"free={gpu['memory_free_mib']} MiB")
            print(f"P2P: {'OK' if report['p2p'].get('available') else 'FAIL'}")
            print(f"Vulkan/CUDA: {'OK' if report['interop'].get('available') else 'FAIL'}")
            print("Vulkan/CUDA semáforos externos: "
                  f"{'OK' if report['vulkan_cuda_external_semaphore'].get('available') else 'FAIL'}")
            print(f"Games found: {report['games_found']}")
            print(f"NGX: {report['runtime']['reason']}")
        elif args.command == "games":
            for game in report["games"]:
                print(f"{game['appid']}  {game['name']}  {game['install_dir']}")
        else:
            print(json.dumps(report, indent=2, ensure_ascii=False))

    if args.command == "selftest":
        return 0 if report.get("passed", False) else 1
    if args.command == "remote-selftest":
        return 0 if report.get("available", False) else 1
    if args.command == "plan":
        return 0 if report.get("status") in {"READY_REMOTE", "READY_LOCAL_ONLY"} else 1
    if args.command == "run":
        if report.get("launch") == "prepared_only":
            return 0
        execution = report.get("execution", {})
        return 0 if report.get("launch") == "finished" and \
            execution.get("return_code") == 0 and not execution.get("timed_out") else 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
