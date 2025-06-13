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

    triplet = 'x64-windows' if system == 'Windows' else 'x64-linux'
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
