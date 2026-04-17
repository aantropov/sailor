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
BINARIES_DIR = REPO_ROOT / "Binaries" / "Editor"
LOCAL_DOTNET = Path.home() / ".dotnet" / ("dotnet.exe" if platform.system().lower() == "windows" else "dotnet")


def run(cmd: list[str], cwd: Path | None = None, env: dict[str, str] | None = None, check: bool = True) -> subprocess.CompletedProcess:
    print("+", " ".join(str(x) for x in cmd))
    return subprocess.run(cmd, cwd=cwd or REPO_ROOT, env=env, check=check)


def detect_os() -> str:
    system = platform.system().lower()
    if system == "darwin":
        return "mac"
    if system == "windows":
        return "windows"
    if system == "linux":
        return "linux"
    raise RuntimeError(f"Unsupported OS: {platform.system()}")


def default_framework(host_os: str) -> str:
    if host_os == "mac":
        return "net10.0-maccatalyst"
    if host_os == "windows":
        return "net9.0-windows10.0.19041.0"
    return "net9.0-android"


def detect_dotnet() -> str | None:
    if LOCAL_DOTNET.exists():
        return str(LOCAL_DOTNET)
    return shutil.which("dotnet")


def prepare_env() -> dict[str, str]:
    env = os.environ.copy()
    dotnet = detect_dotnet()
    if dotnet:
        dotnet_dir = str(Path(dotnet).resolve().parent)
        env["PATH"] = dotnet_dir + os.pathsep + env.get("PATH", "")
        env.setdefault("DOTNET_ROOT", dotnet_dir)
    return env


def build_engine(config: str) -> None:
    run([sys.executable, str(REPO_ROOT / "build_engine.py"), "--config", config])


def build_editor(framework: str, config: str, publish: bool) -> None:
    cmd = [sys.executable, str(REPO_ROOT / "build_editor.py"), "--framework", framework, "--config", config]
    if publish:
        cmd.append("--publish")
    run(cmd, env=prepare_env())


def find_editor_artifact(host_os: str, framework: str, config: str, published: bool) -> Path:
    config_dir = BINARIES_DIR / config / framework
    if host_os == "mac":
        candidates = sorted(config_dir.rglob("SailorEditor.app"))
        if candidates:
            if published:
                pub = [c for c in candidates if "publish" in c.parts]
                return pub[0] if pub else candidates[0]
            return candidates[0]
    elif host_os == "windows":
        exes = sorted(config_dir.rglob("SailorEditor.exe"))
        if exes:
            return exes[0]
    elif host_os == "linux":
        # Placeholder if a runnable desktop artifact ever appears.
        exes = sorted(config_dir.rglob("SailorEditor"))
        if exes:
            return exes[0]
    raise FileNotFoundError(f"Could not find editor artifact under {config_dir}")


def launch_artifact(host_os: str, artifact: Path) -> None:
    if host_os == "mac":
        run(["open", str(artifact)])
    elif host_os == "windows":
        os.startfile(str(artifact))  # type: ignore[attr-defined]
    else:
        run([str(artifact)])


def main() -> int:
    host_os = detect_os()
    parser = argparse.ArgumentParser(description="Build and launch SailorEditor.")
    parser.add_argument("--framework", default=default_framework(host_os), help="Target framework to build/run")
    parser.add_argument("--config", default="Debug", choices=["Debug", "Release"], help="Build configuration")
    parser.add_argument("--skip-engine", action="store_true", help="Skip build_engine.py")
    parser.add_argument("--publish", action="store_true", help="Use dotnet publish before launch")
    parser.add_argument("--no-launch", action="store_true", help="Build only, do not launch")
    args = parser.parse_args()

    if not args.skip_engine:
        build_engine(args.config)

    build_editor(args.framework, args.config, args.publish)

    artifact = find_editor_artifact(host_os, args.framework, args.config, args.publish)
    print(f"Found editor artifact: {artifact}")

    if not args.no_launch:
        launch_artifact(host_os, artifact)
        print("Launch requested.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
