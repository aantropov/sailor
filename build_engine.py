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
DEFAULT_WINDOWS_BUILD_DIR = REPO_ROOT / "build"
DEFAULT_MAC_BUILD_DIR = REPO_ROOT / "build-mac-vcpkg"
DEFAULT_LINUX_BUILD_DIR = REPO_ROOT / "build-linux"
VCPKG_TOOLCHAIN = REPO_ROOT / "External" / "vcpkg" / "scripts" / "buildsystems" / "vcpkg.cmake"


def run(cmd: list[str], cwd: Path | None = None, env: dict[str, str] | None = None) -> None:
    print("+", " ".join(cmd))
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


def default_build_dir(host_os: str) -> Path:
    if host_os == "mac":
        return DEFAULT_MAC_BUILD_DIR
    if host_os == "windows":
        return DEFAULT_WINDOWS_BUILD_DIR
    return DEFAULT_LINUX_BUILD_DIR


def default_generator(host_os: str) -> str:
    if host_os == "mac":
        return "Xcode"
    if host_os == "windows":
        return "Visual Studio 17 2022"
    return "Ninja"


def target_exists(build_dir: Path, target: str, config: str | None) -> bool:
    cmd = ["cmake", "--build", str(build_dir), "--target", "help"]
    if config:
        cmd += ["--config", config]
    result = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, text=True)
    return result.returncode == 0 and target in result.stdout


def configure(build_dir: Path, host_os: str, generator: str | None, config: str, arch: str | None, use_vcpkg: bool) -> None:
    build_dir.mkdir(parents=True, exist_ok=True)
    cmd = ["cmake", "-S", str(REPO_ROOT), "-B", str(build_dir)]

    chosen_generator = generator or default_generator(host_os)
    if chosen_generator:
        cmd += ["-G", chosen_generator]

    if host_os == "windows" and arch and "Visual Studio" in chosen_generator:
        cmd += ["-A", arch]

    if host_os != "windows" and chosen_generator not in {"Xcode", "Visual Studio 17 2022"}:
        cmd += [f"-DCMAKE_BUILD_TYPE={config}"]

    if use_vcpkg and VCPKG_TOOLCHAIN.exists():
        cmd += [f"-DCMAKE_TOOLCHAIN_FILE={VCPKG_TOOLCHAIN}"]
        if host_os == "mac":
            cmd += ["-DVCPKG_TARGET_TRIPLET=arm64-osx"]

    run(cmd)


def main() -> int:
    host_os = detect_os()

    parser = argparse.ArgumentParser(description="Build Sailor engine/SailorLib cross-platform.")
    parser.add_argument("--build-dir", type=Path, default=default_build_dir(host_os), help="CMake build directory")
    parser.add_argument("--config", default="Debug", choices=["Debug", "Release", "RelWithDebInfo", "MinSizeRel"], help="Build configuration")
    parser.add_argument("--generator", default=None, help="CMake generator override")
    parser.add_argument("--target", default="SailorLib", help="CMake target to build (default: SailorLib)")
    parser.add_argument("--arch", default="arm64" if host_os == "windows" else None, help="Generator architecture (mainly Windows VS)")
    parser.add_argument("--clean-configure", action="store_true", help="Delete build dir before configuring")
    parser.add_argument("--no-vcpkg", action="store_true", help="Do not use repo-local vcpkg toolchain even if available")
    parser.add_argument("--parallel", default=str(os.cpu_count() or 8), help="Parallel build jobs")
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    if args.clean_configure and build_dir.exists():
        print(f"Removing build dir: {build_dir}")
        shutil.rmtree(build_dir)

    use_vcpkg = not args.no_vcpkg
    cache_file = build_dir / "CMakeCache.txt"
    if not cache_file.exists():
        configure(build_dir, host_os, args.generator, args.config, args.arch, use_vcpkg)

    if not target_exists(build_dir, args.target, args.config if host_os in {"mac", "windows"} else None):
        print(f"Target '{args.target}' not visible in {build_dir}; reconfiguring...")
        configure(build_dir, host_os, args.generator, args.config, args.arch, use_vcpkg)

    build_cmd = ["cmake", "--build", str(build_dir), "--target", args.target]
    if host_os in {"mac", "windows"}:
        build_cmd += ["--config", args.config]
    build_cmd += ["--parallel", str(args.parallel)]
    run(build_cmd)

    print(f"\nDone. Built target '{args.target}' in {build_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
