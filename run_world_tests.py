#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import shutil
import platform
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent
CONTENT_ROOT = REPO_ROOT / "Content"
DEFAULT_TEST_ROOT = REPO_ROOT / "Content" / "Tests"
DEFAULT_MAC_APP_ROOT = Path("/tmp") / "sailor_world_tests"


def detect_os() -> str:
    system = platform.system().lower()
    if system == "darwin":
        return "mac"
    if system == "windows":
        return "windows"
    if system == "linux":
        return "linux"
    raise RuntimeError(f"Unsupported OS: {platform.system()}")


def executable_suffix(host_os: str) -> str:
    return ".exe" if host_os == "windows" else ""


def default_engine_path(config: str) -> Path:
    host_os = detect_os()
    exe_name = f"SailorEngine-{config}{executable_suffix(host_os)}"
    config_dir = REPO_ROOT / "Binaries" / config
    expected = config_dir / exe_name
    if expected.exists():
        return expected

    if config_dir.exists():
        engine_candidates = sorted(
            (path for path in config_dir.glob(f"SailorEngine-*{executable_suffix(host_os)}") if path.is_file()),
            key=lambda path: path.stat().st_mtime,
            reverse=True,
        )
        if engine_candidates:
            return engine_candidates[0]

    return expected


def warn_if_engine_name_mismatch(engine: Path, config: str) -> None:
    expected_name = f"SailorEngine-{config}{executable_suffix(detect_os())}"
    if engine.name != expected_name:
        print(
            f"Warning: selected engine name is '{engine.name}', expected '{expected_name}'. "
            "Rebuild SailorExec if this is not intentional.",
            file=sys.stderr,
        )


def relative_to_repo(path: Path) -> str:
    return path.resolve().relative_to(REPO_ROOT).as_posix()


def relative_to_content(path: Path) -> str:
    return path.resolve().relative_to(CONTENT_ROOT).as_posix()


def find_worlds(test_root: Path) -> list[Path]:
    if not test_root.exists():
        raise FileNotFoundError(f"World tests root does not exist: {test_root}")
    return sorted(path for path in test_root.rglob("*.world") if path.is_file())


def engine_library_path(config: str) -> Path:
    return REPO_ROOT / "build-mac-vcpkg" / "Lib" / config / f"Sailor-{config}.dylib"


def write_mac_info_plist(app: Path, executable_name: str, bundle_identifier: str) -> None:
    plist = app / "Contents" / "Info.plist"
    plist.write_text(
        "\n".join(
            [
                '<?xml version="1.0" encoding="UTF-8"?>',
                '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "https://www.apple.com/DTDs/PropertyList-1.0.dtd">',
                '<plist version="1.0">',
                '<dict>',
                '  <key>CFBundleExecutable</key>',
                f'  <string>{executable_name}</string>',
                '  <key>CFBundleIdentifier</key>',
                f'  <string>{bundle_identifier}</string>',
                '  <key>CFBundleName</key>',
                '  <string>SailorEngineWorldTests</string>',
                '  <key>CFBundlePackageType</key>',
                '  <string>APPL</string>',
                '  <key>NSHighResolutionCapable</key>',
                '  <true/>',
                '</dict>',
                '</plist>',
                '',
            ]
        ),
        encoding="utf-8",
    )


def prepare_mac_app_bundle(engine: Path, config: str) -> Path:
    bundle_suffix = f"{config.lower()}.{os.getpid()}.{time.monotonic_ns()}"
    app = DEFAULT_MAC_APP_ROOT / f"SailorEngineWorldTests-{config}-{os.getpid()}-{time.monotonic_ns()}.app"
    contents = app / "Contents"
    macos = contents / "MacOS"
    macos.mkdir(parents=True)
    (contents / "Resources").mkdir()

    executable_name = "SailorEngineWorldTests"
    bundled_engine = macos / executable_name
    shutil.copy2(engine, bundled_engine)

    library = engine_library_path(config)
    if library.exists():
        shutil.copy2(library, macos / library.name)

    write_mac_info_plist(app, executable_name, f"com.sailor.engine.worldtests.{bundle_suffix}")

    subprocess.run(["install_name_tool", "-add_rpath", "@executable_path", str(bundled_engine)], check=False)
    subprocess.run(["codesign", "--force", "--deep", "--sign", "-", str(app)], check=True)
    time.sleep(0.25)
    return app


def build_engine_args(world: Path, extra_args: list[str]) -> list[str]:
    world_arg = relative_to_content(world)
    return [
        "--workspace",
        str(REPO_ROOT),
        "--world",
        world_arg,
        "--noconsole",
        *extra_args,
    ]


def run_world_direct(engine: Path, world: Path, timeout: float, extra_args: list[str]) -> tuple[int | None, float, str]:
    cmd = [
        str(engine),
        *build_engine_args(world, extra_args),
    ]

    start = time.monotonic()
    try:
        result = subprocess.run(
            cmd,
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
        return result.returncode, time.monotonic() - start, result.stdout or ""
    except subprocess.TimeoutExpired as ex:
        output = ex.stdout or ""
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        return None, time.monotonic() - start, output


def run_world_mac_app(app: Path, world: Path, timeout: float, extra_args: list[str]) -> tuple[int | None, float, str]:
    cmd = [
        "open",
        "-W",
        "-n",
        str(app),
        "--args",
        *build_engine_args(world, extra_args),
    ]

    start = time.monotonic()
    try:
        result = subprocess.run(
            cmd,
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
        if result.returncode != 0 and "kLSNoExecutableErr" in (result.stdout or ""):
            time.sleep(0.5)
            result = subprocess.run(
                cmd,
                cwd=REPO_ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=timeout,
                check=False,
            )
        return result.returncode, time.monotonic() - start, result.stdout or ""
    except subprocess.TimeoutExpired as ex:
        output = ex.stdout or ""
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        return None, time.monotonic() - start, output


def main() -> int:
    host_os = detect_os()
    parser = argparse.ArgumentParser(description="Run built SailorEngine Release against all Content/Tests/**/*.world files.")
    parser.add_argument("--config", default="Release", help="Built engine config to run, default: Release")
    parser.add_argument("--engine", type=Path, default=None, help="Path to built SailorEngine executable")
    parser.add_argument("--direct-exec", action="store_true", help="Run the executable directly instead of using a macOS .app bundle")
    parser.add_argument("--tests-root", type=Path, default=DEFAULT_TEST_ROOT, help="Root folder containing .world tests")
    parser.add_argument("--timeout", type=float, default=180.0, help="Per-world timeout in seconds")
    parser.add_argument("--stop-on-failure", action="store_true", help="Stop after the first failed world")
    parser.add_argument("--verbose", action="store_true", help="Print full engine output for every world")
    parser.add_argument("engine_args", nargs=argparse.REMAINDER, help="Extra args passed to SailorEngine after --")
    args = parser.parse_args()

    engine = (args.engine or default_engine_path(args.config)).resolve()
    if not engine.exists():
        print(f"Engine executable was not found: {engine}", file=sys.stderr)
        print(f"Build it first, for example: python3 build_engine.py --config {args.config} --target SailorExec", file=sys.stderr)
        return 2
    warn_if_engine_name_mismatch(engine, args.config)

    worlds = find_worlds(args.tests_root.resolve())
    if not worlds:
        print(f"No .world files found under {args.tests_root}", file=sys.stderr)
        return 2

    extra_args = args.engine_args
    if extra_args and extra_args[0] == "--":
        extra_args = extra_args[1:]

    print(f"Engine: {engine}")
    app = None
    if host_os == "mac" and not args.direct_exec:
        app = prepare_mac_app_bundle(engine, args.config)
        print(f"App: {app}")
    print(f"Worlds: {len(worlds)}")
    print(f"Timeout: {args.timeout:.1f}s per world")

    failures: list[tuple[Path, str]] = []
    completed = 0
    for index, world in enumerate(worlds, start=1):
        completed += 1
        label = relative_to_repo(world)
        print(f"\n[{index}/{len(worlds)}] {label}")
        if app:
            return_code, elapsed, output = run_world_mac_app(app, world, args.timeout, extra_args)
        else:
            return_code, elapsed, output = run_world_direct(engine, world, args.timeout, extra_args)

        if args.verbose and output:
            print(output.rstrip())

        if return_code is None:
            reason = f"timeout after {elapsed:.2f}s"
            print(f"FAIL: {reason}")
            failures.append((world, reason))
        elif return_code != 0:
            reason = f"exit code {return_code} after {elapsed:.2f}s"
            print(f"FAIL: {reason}")
            if not args.verbose and output:
                print(output.rstrip())
            failures.append((world, reason))
        else:
            print(f"OK: {elapsed:.2f}s")

        if failures and args.stop_on_failure:
            break

    print(f"\nResult: {completed - len(failures)}/{completed} passed")
    if failures:
        print("Failures:")
        for world, reason in failures:
            print(f"- {relative_to_repo(world)}: {reason}")
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
