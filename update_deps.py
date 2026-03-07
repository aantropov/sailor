#!/usr/bin/env python3
import os
import platform
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
VCPKG_ROOT = os.path.join(SCRIPT_DIR, 'External', 'vcpkg')


def main() -> int:
    system = platform.system()
    if system == 'Windows':
        vcpkg_exe = os.path.join(VCPKG_ROOT, 'vcpkg.exe')
        bootstrap_cmd = ['cmd', '/c', 'bootstrap-vcpkg.bat']
    else:
        vcpkg_exe = os.path.join(VCPKG_ROOT, 'vcpkg')
        bootstrap_cmd = ['./bootstrap-vcpkg.sh']

    if not os.path.exists(vcpkg_exe):
        print('Bootstrap vcpkg...')
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
