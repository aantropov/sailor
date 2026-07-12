#!/usr/bin/env python3
import os
import platform
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
VCPKG_ROOT = os.path.join(SCRIPT_DIR, 'External', 'vcpkg')
VCPKG_TOOL_METADATA = os.path.join(VCPKG_ROOT, 'scripts', 'vcpkg-tool-metadata.txt')


def required_vcpkg_tool_release() -> str:
    with open(VCPKG_TOOL_METADATA, encoding='utf-8-sig') as metadata:
        for line in metadata:
            key, separator, value = line.strip().partition('=')
            if separator and key == 'VCPKG_TOOL_RELEASE_TAG':
                return value

    raise RuntimeError(f'Unable to read VCPKG_TOOL_RELEASE_TAG from {VCPKG_TOOL_METADATA}')


def has_current_vcpkg_tool(vcpkg_exe: str, required_release: str) -> bool:
    if not os.path.exists(vcpkg_exe):
        return False

    try:
        result = subprocess.run(
            [vcpkg_exe, 'version'],
            cwd=VCPKG_ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
    except OSError:
        return False

    first_line = result.stdout.partition('\n')[0]
    return result.returncode == 0 and required_release in first_line


def main() -> int:
    system = platform.system()
    if system == 'Windows':
        vcpkg_exe = os.path.join(VCPKG_ROOT, 'vcpkg.exe')
        bootstrap_cmd = ['cmd', '/c', 'bootstrap-vcpkg.bat']
    else:
        vcpkg_exe = os.path.join(VCPKG_ROOT, 'vcpkg')
        bootstrap_cmd = ['./bootstrap-vcpkg.sh']

    required_release = required_vcpkg_tool_release()
    if not has_current_vcpkg_tool(vcpkg_exe, required_release):
        print('Bootstrap vcpkg...', flush=True)
        subprocess.check_call(bootstrap_cmd, cwd=VCPKG_ROOT)

    def run_vcpkg(*args: str) -> None:
        subprocess.check_call([vcpkg_exe] + list(args), cwd=VCPKG_ROOT)

    run_vcpkg('update')
    run_vcpkg('upgrade', '--no-dry-run')

    if system == 'Windows':
        triplet = 'x64-windows'
    elif system == 'Darwin':
        # Prefer native Apple Silicon; fallback to Intel triplet on x86_64 Macs.
        triplet = 'arm64-osx' if platform.machine().lower() in ('arm64', 'aarch64') else 'x64-osx'
    else:
        triplet = 'x64-linux'
    packages = [
        'glm',
        'imgui[win32-binding]' if system == 'Windows' else 'imgui',
        'magic-enum',
        'nlohmann-json',
        'refl-cpp',
        'spirv-reflect',
        'stb',
        'tbb',
        'tinygltf',
        'tracy',
        'yaml-cpp',
    ]

    for pkg in packages:
        run_vcpkg('install', f'{pkg}:{triplet}')

    print('Dependencies updated and installed.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
