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
MCP_BRIDGE_PROJECT = REPO_ROOT / "Editor" / "McpBridge" / "SailorEditor.McpBridge.csproj"
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


def default_runtime(host_os: str) -> str | None:
    if host_os == "mac" and platform.machine() == "arm64":
        return "maccatalyst-arm64"
    return None


def prepare_env(dotnet_path: str) -> dict[str, str]:
    env = os.environ.copy()
    dotnet_dir = str(Path(dotnet_path).resolve().parent)
    env["PATH"] = dotnet_dir + os.pathsep + env.get("PATH", "")
    env.setdefault("DOTNET_ROOT", dotnet_dir)
    return env


def maybe_build_engine(engine: bool, config: str) -> None:
    if not engine:
        return
    cmd = [
        sys.executable,
        str(REPO_ROOT / "build_engine.py"),
        "--config",
        config,
        "--target",
        "SailorExec",
    ]
    run(cmd)


def build_mcp_bridge(dotnet: str, config: str, env: dict[str, str]) -> None:
    run([
        dotnet,
        "build",
        str(MCP_BRIDGE_PROJECT),
        "-c",
        config,
    ], env=env)


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


def find_engine_library(config: str) -> Path | None:
    library_name = "Sailor-Debug.dylib" if config == "Debug" else "Sailor-Release.dylib"
    library_root = REPO_ROOT / "build-mac-vcpkg" / "Lib"
    candidates = [
        library_root / library_name,
        library_root / config / library_name,
    ]
    existing_candidates = [candidate for candidate in candidates if candidate.exists()]
    if not existing_candidates:
        print(f"Engine library was not found, skipping staging: {candidates}")
        return None
    return max(existing_candidates, key=lambda candidate: candidate.stat().st_mtime_ns)


def stage_engine_library(config: str) -> None:
    source = find_engine_library(config)
    if source is None:
        return

    destination = REPO_ROOT / "build-mac-vcpkg" / "Lib" / config / source.name
    if source.resolve() == destination.resolve():
        return

    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    print(f"Staged engine library for editor build: {destination}")


def export_mac_editor(config: str, framework: str, runtime: str | None, output: Path, publish: bool) -> None:
    build_dir = REPO_ROOT / "Binaries" / "Editor" / config / framework
    if runtime:
        build_dir /= runtime
    app = build_dir / "SailorEditor.app"
    output = output.resolve()
    if output.is_relative_to(app.resolve()):
        raise ValueError("The output directory must not be inside SailorEditor.app")

    output.mkdir(parents=True, exist_ok=True)
    artifacts = [app]
    symbols = build_dir / "SailorEditor.app.dSYM"
    if symbols.exists():
        artifacts.append(symbols)
    if publish:
        artifacts.extend((build_dir / "publish").glob("*.pkg"))

    for source in artifacts:
        destination = output / source.name
        if source.resolve() == destination.resolve():
            continue
        if source.is_dir():
            if destination.exists():
                shutil.rmtree(destination)
            shutil.copytree(source, destination, symlinks=True)
        else:
            shutil.copy2(source, destination)
        print(f"Exported editor artifact: {destination}")


def main() -> int:
    host_os = detect_os()
    parser = argparse.ArgumentParser(description="Build SailorEditor cross-platform.")
    parser.add_argument("--framework", default=default_target_framework(host_os), help="Target framework to build")
    parser.add_argument("--config", default="Debug", choices=["Debug", "Release"], help="Build configuration")
    parser.add_argument("--publish", action="store_true", help="Run 'dotnet publish' instead of 'dotnet build'")
    parser.add_argument("--runtime", default=default_runtime(host_os), help="Optional .NET runtime identifier, for example maccatalyst-arm64")
    parser.add_argument("--build-engine", action="store_true", help="Build SailorLib first using build_engine.py")
    parser.add_argument("--output", type=Path, default=None, help="Optional output directory; Mac Catalyst exports the signed app and package here")
    parser.add_argument("--self-contained", action="store_true", help="Publish self-contained app when supported")
    args = parser.parse_args()

    if not EDITOR_PROJECT.exists():
        raise RuntimeError(f"Editor project not found: {EDITOR_PROJECT}")

    maybe_build_engine(args.build_engine, args.config)
    remove_generated_imgui_ini(args.config, args.output)
    # MSBuild packages and signs the staged library as part of the app.
    stage_engine_library(args.config)

    dotnet = detect_dotnet()
    env = prepare_env(dotnet)
    export_bundle = host_os == "mac" and "-maccatalyst" in args.framework

    command = [dotnet, "publish" if args.publish else "build", str(EDITOR_PROJECT), "-f", args.framework, "-c", args.config]
    if args.runtime:
        command += ["-r", args.runtime]
    if args.output and not export_bundle:
        command += ["-o", str(args.output.resolve())]
    if args.self_contained and args.publish:
        command += ["--self-contained", "true"]

    run(command, env=env)
    build_mcp_bridge(dotnet, args.config, env)
    if args.output and export_bundle:
        # Keep SDK bundle paths unchanged, then export the complete signed app.
        export_mac_editor(args.config, args.framework, args.runtime, args.output, args.publish)

    print("\nDone.")
    print(f"Framework: {args.framework}")
    print(f"Configuration: {args.config}")
    print(f"Mode: {'publish' if args.publish else 'build'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
