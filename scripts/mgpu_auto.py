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
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
P2P_PROBE = BUILD / "mgpu-p2p-probe"
VULKAN_PROBE = BUILD / "mgpu-vulkan-cuda-probe"


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


def runtime_status(game: Game | None) -> dict[str, Any]:
    if game is None:
        return {"game_selected": False, "available": False, "reason": "no se seleccionó juego"}
    roots = [Path(game.install_dir), Path(game.prefix) / "drive_c/windows/system32"]
    names = ("nvngx_dlssnr.dll", "nvngx_dlss.dll", "_nvngx.dll")
    found: dict[str, list[str]] = {name: [] for name in names}
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
                found[candidate.name.lower()].append(str(candidate))
    bridge_candidates = [
        ROOT / "build/proton/bridge-nvngx.dll",
        ROOT / "build/proton/_nvngx.dll",
        Path.home() / ".local/lib/dlss5-mgpu/bridge-nvngx.dll",
    ]
    bridge = [str(path) for path in bridge_candidates if path.exists()]
    complete = bool(found["nvngx_dlssnr.dll"] and found["nvngx_dlss.dll"] and bridge)
    return {
        "game_selected": True,
        "available": complete,
        # The current bridge has no in-process Vulkan/CUDA resource transport
        # yet, and VKD3D's D3D12 cross-adapter handle export is unavailable.
        # Keep this gate explicit so runtime presence cannot imply remote mode.
        "transport_available": False,
        "transport_reason": "transporte cross-adapter aún no implementado",
        "bridge": bridge,
        "runtimes": found,
        "reason": "bridge y runtimes encontrados" if complete
        else "faltan bridge-nvngx.dll o runtimes NGX locales",
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
        return {
            "ready": True,
            "mode": "remote-neural",
            "fallback_local": True,
            "render_gpu": plan.get("render_gpu"),
            "neural_gpu": plan.get("neural_gpu"),
            "bridge": runtime.get("bridge", []),
            "reason": "bridge y runtimes verificados; falta integrar el launcher NGX",
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


def execute_direct(policy: dict[str, Any], timeout_seconds: int) -> dict[str, Any]:
    command = [str(item) for item in policy["command"]]
    env = os.environ.copy()
    env.update({str(key): str(value) for key, value in policy["env"].items()})
    process = subprocess.Popen(command, cwd=policy["cwd"], env=env,
                               start_new_session=True)
    try:
        return_code = process.wait(timeout=timeout_seconds or None)
        return {"started": True, "return_code": return_code, "timed_out": False}
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            return_code = process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            return_code = process.wait(timeout=5)
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


def select_plan(gpus: list[Gpu], p2p: dict[str, Any], interop: dict[str, Any],
                runtime: dict[str, Any] | None = None) -> dict[str, Any]:
    plan: dict[str, Any] = {
        "status": "READY_LOCAL_ONLY",
        "render_gpu": None,
        "neural_gpu": None,
        "reason": "",
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
    games = discover_games()
    game = find_game(games, game_query)
    runtime = runtime_status(game)
    plan = select_plan(gpus, p2p, interop, runtime)
    return {
        "project": "DLSS5-MGPU-Ampere",
        "root": str(ROOT),
        "gpus": [asdict(gpu) | {"memory_free_mib": gpu.memory_free_mib} for gpu in gpus],
        "p2p": p2p,
        "interop": interop,
        "games_found": len(games),
        "game": asdict(game) if game else None,
        "runtime": runtime,
        "plan": plan,
        "launch": launch_preparation(game, plan, runtime),
    }


def main() -> int:
    parser = argparse.ArgumentParser(prog="mgpu-auto")
    parser.add_argument("command", choices=("doctor", "selftest", "plan", "games", "run"))
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
        elif args.command == "run" and args.exe:
            executable = Path(args.exe).expanduser().resolve()
            if not executable.is_file():
                print(f"No existe el ejecutable: {executable}", file=sys.stderr)
                return 2
            gpus = discover_gpus()
            p2p = p2p_report()
            interop = interop_report()
            runtime = {
                "available": False,
                "reason": "prueba directa sin bridge NGX propietario",
            }
            plan = select_plan(gpus, p2p, interop, runtime)
            render = next((gpu for gpu in gpus if gpu.index == plan.get("render_gpu")), None)
            direct_report = {
                "project": "DLSS5-MGPU-Ampere",
                "target": str(executable),
                "gpus": [asdict(gpu) | {"memory_free_mib": gpu.memory_free_mib}
                         for gpu in gpus],
                "p2p": p2p,
                "interop": interop,
                "plan": plan,
                "launch_policy": direct_launch_policy(
                    executable, args.runner, args.exe_arg,
                    Path(args.prefix).expanduser().resolve() if args.prefix else None,
                    plan, render),
            }
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
            "passed": report["p2p"].get("available", False)
            and report["interop"].get("available", False),
        }
    elif args.command in ("plan", "run"):
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
        else:
            try:
                report["execution"] = execute_direct(
                    direct_report["launch_policy"], args.timeout_seconds)
                report["launch"] = "finished"
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
        else:
            report["launch"] = "not_implemented_until_ngx_bridge_is_integrated"
            report["launch_reason"] = (
                "los runtimes están presentes, pero todavía falta el launcher/proxy "
                "NGX que conecte el juego real con el transporte"
            )

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
            print(f"Games found: {report['games_found']}")
            print(f"NGX: {report['runtime']['reason']}")
        elif args.command == "games":
            for game in report["games"]:
                print(f"{game['appid']}  {game['name']}  {game['install_dir']}")
        else:
            print(json.dumps(report, indent=2, ensure_ascii=False))

    if args.command == "selftest":
        return 0 if report.get("passed", False) else 1
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
