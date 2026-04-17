#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent
EDITOR_PROJECT = REPO_ROOT / "Editor" / "SailorEditor.csproj"
LOCAL_DOTNET = Path.home() / ".dotnet" / ("dotnet.exe" if platform.system().lower() == "windows" else "dotnet")


def run(cmd: list[str], cwd: Path | None = None, env: dict[str, str] | None = None) -> None:
    print("+", " ".join(str(x) for x in cmd))
    subprocess.run(cmd, cwd=cwd or REPO_ROOT, env=env, check=True)


def detect_os() -> str:
    system = platform.system().lower()
    if system == "darwin":
        return "mac"
    if system == "windows":
        return "windows"
    if system == "linux":
        return "linux"
    raise RuntimeError(f"Unsupported OS: {platform.system()}")


def detect_dotnet() -> str:
    if LOCAL_DOTNET.exists():
        return str(LOCAL_DOTNET)
    dotnet = shutil.which("dotnet")
    if dotnet:
        return dotnet
    raise RuntimeError("dotnet was not found. Install .NET SDK or place it in ~/.dotnet")


def default_target_framework(host_os: str) -> str:
    if host_os == "mac":
        return "net10.0-maccatalyst"
    if host_os == "windows":
        return "net9.0-windows10.0.19041.0"
    if host_os == "linux":
        return "net9.0-android"
    raise RuntimeError("Unsupported host OS")


def prepare_env(dotnet_path: str) -> dict[str, str]:
    env = os.environ.copy()
    dotnet_dir = str(Path(dotnet_path).resolve().parent)
    env["PATH"] = dotnet_dir + os.pathsep + env.get("PATH", "")
    env.setdefault("DOTNET_ROOT", dotnet_dir)
    return env


def maybe_build_engine(engine: bool, config: str) -> None:
    if not engine:
        return
    cmd = [sys.executable, str(REPO_ROOT / "build_engine.py"), "--config", config]
    run(cmd)


def remove_generated_imgui_ini(config: str, output: Path | None) -> None:
    search_roots = [
        REPO_ROOT / "Binaries" / "Editor" / config,
        REPO_ROOT / "Binaries" / "Editor" / config.lower(),
    ]
    if output:
        search_roots.append(output.resolve())

    seen: set[Path] = set()
    for root in search_roots:
        if not root.exists():
            continue
        for imgui_ini in root.rglob("SailorEditor.app/imgui.ini"):
            imgui_ini = imgui_ini.resolve()
            if imgui_ini in seen:
                continue
            seen.add(imgui_ini)
            imgui_ini.unlink()
            print(f"Removed generated file before build: {imgui_ini}")


def sync_engine_library(config: str, output: Path | None) -> None:
    library_name = "Sailor-Debug.dylib" if config == "Debug" else "Sailor-Release.dylib"
    source = REPO_ROOT / "build-mac-vcpkg" / "Lib" / config / library_name
    if not source.exists():
        print(f"Engine library was not found, skipping app sync: {source}")
        return

    search_roots = [
        REPO_ROOT / "Binaries" / "Editor" / config,
        REPO_ROOT / "Binaries" / "Editor" / config.lower(),
    ]
    if output:
        search_roots.append(output.resolve())

    seen: set[Path] = set()
    for root in search_roots:
        if not root.exists():
            continue

        for app in root.rglob("SailorEditor.app"):
            app = app.resolve()
            if app in seen:
                continue
            seen.add(app)

            for relative_dir in ("Contents/Resources", "Contents/MonoBundle"):
                destination_dir = app / relative_dir
                if destination_dir.exists():
                    destination = destination_dir / library_name
                    shutil.copy2(source, destination)
                    print(f"Synced engine library: {destination}")


def main() -> int:
    host_os = detect_os()
    parser = argparse.ArgumentParser(description="Build SailorEditor cross-platform.")
    parser.add_argument("--framework", default=default_target_framework(host_os), help="Target framework to build")
    parser.add_argument("--config", default="Debug", choices=["Debug", "Release"], help="Build configuration")
    parser.add_argument("--publish", action="store_true", help="Run 'dotnet publish' instead of 'dotnet build'")
    parser.add_argument("--build-engine", action="store_true", help="Build SailorLib first using build_engine.py")
    parser.add_argument("--output", type=Path, default=None, help="Optional output directory")
    parser.add_argument("--self-contained", action="store_true", help="Publish self-contained app when supported")
    args = parser.parse_args()

    if not EDITOR_PROJECT.exists():
        raise RuntimeError(f"Editor project not found: {EDITOR_PROJECT}")

    maybe_build_engine(args.build_engine, args.config)
    remove_generated_imgui_ini(args.config, args.output)

    dotnet = detect_dotnet()
    env = prepare_env(dotnet)

    command = [dotnet, "publish" if args.publish else "build", str(EDITOR_PROJECT), "-f", args.framework, "-c", args.config]
    if args.output:
        command += ["-o", str(args.output.resolve())]
    if args.self_contained and args.publish:
        command += ["--self-contained", "true"]

    run(command, env=env)
    sync_engine_library(args.config, args.output)

    print("\nDone.")
    print(f"Framework: {args.framework}")
    print(f"Configuration: {args.config}")
    print(f"Mode: {'publish' if args.publish else 'build'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
