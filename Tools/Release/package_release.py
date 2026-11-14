#!/usr/bin/env python3
"""Stage and archive a versioned desktop editor/runtime/SDK release."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import subprocess
import tarfile
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
RELEASE_TARGETS = ('windows-x64', 'macos-arm64', 'linux-x64')


def archive_filename(version: str, target: str) -> str:
    extension = '.tar.gz' if target == 'linux-x64' else '.zip'
    return f'Sailor-{version}-{target}{extension}'


def read_version(root: Path = ROOT) -> str:
    version = (root / 'VERSION').read_text(encoding='utf-8').strip()
    if not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', version):
        raise ValueError('VERSION must contain a major.minor.patch release version.')
    return version


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def run(*arguments: object) -> str:
    return subprocess.check_output([str(argument) for argument in arguments], text=True, encoding='utf-8').strip()


def copy_directory(source: Path, target: Path) -> None:
    if not source.is_dir():
        raise FileNotFoundError(source)
    shutil.copytree(source, target, symlinks=True)


def bundle_mac_dependencies(binary: Path, extra_paths: list[Path]) -> None:
    """Keep every non-system dylib beside its consumer, including transitive dependencies."""
    pending = [binary]
    visited: set[Path] = set()
    while pending:
        current = pending.pop()
        if current in visited:
            continue
        visited.add(current)
        lines = run('otool', '-L', current).splitlines()[1:]
        for line in lines:
            dependency = line.strip().split(' (', 1)[0]
            if dependency.startswith(('/System/', '/usr/lib/')) or not dependency.endswith('.dylib'):
                continue
            name = Path(dependency).name
            if name == current.name:
                continue
            target = current.parent / name
            if not target.exists():
                candidates = ([Path(dependency)] if dependency.startswith('/') else []) + [
                    directory / name for directory in [current.parent, *extra_paths]
                ]
                source = next((candidate for candidate in candidates if candidate.is_file()), None)
                if source is None:
                    raise FileNotFoundError(f'Cannot bundle {dependency} required by {current}')
                shutil.copy2(source, target)
                target.chmod(target.stat().st_mode | 0o200)
            subprocess.run(['install_name_tool', '-change', dependency, '@loader_path/' + name, str(current)], check=True)
            pending.append(target)
    for current in visited:
        subprocess.run(['codesign', '--force', '--sign', '-', str(current)], check=True)


def write_archive(directory: Path, output: Path, mac: bool, linux: bool = False) -> None:
    if mac:
        # ditto preserves the app's symlinks, executable bits and resource metadata.
        subprocess.run(['ditto', '-c', '-k', '--sequesterRsrc', '--keepParent', str(directory), str(output)], check=True)
    elif linux:
        with tarfile.open(output, 'x:gz') as archive:
            archive.add(directory, arcname=directory.name)
    else:
        with zipfile.ZipFile(output, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            for path in sorted(directory.rglob('*')):
                if path.is_file():
                    archive.write(path, path.relative_to(directory.parent))


def editor_version(editor: Path, system: str) -> str:
    if system == 'Darwin':
        with (editor / 'Contents' / 'Info.plist').open('rb') as info:
            return plistlib.load(info)['CFBundleShortVersionString']
    if system == 'Linux':
        version = run(editor / 'SailorEditor', '--version')
        prefix = 'Sailor Editor '
        if not version.startswith(prefix):
            raise ValueError('The Linux editor did not report its compiled version.')
        return version[len(prefix):]
    executable = (editor / 'SailorEditor.exe').resolve()
    environment = os.environ.copy()
    environment['SAILOR_EDITOR_IMAGE'] = str(executable)
    return subprocess.check_output([
        'powershell', '-NoProfile', '-NonInteractive', '-Command',
        '$ErrorActionPreference = "Stop"; (Get-Item -LiteralPath $env:SAILOR_EDITOR_IMAGE).VersionInfo.ProductVersion'
    ], text=True, env=environment).strip().split('+', 1)[0]


def package(build: Path, editor: Path, dependencies: Path, output: Path,
            bridge: Path | None = None, cli: Path | None = None) -> Path:
    system = platform.system()
    if system not in ('Darwin', 'Windows', 'Linux'):
        raise ValueError('Desktop releases must be staged on their native Windows/macOS/Linux host.')
    mac = system == 'Darwin'
    linux = system == 'Linux'
    target = 'macos-arm64' if mac else 'linux-x64' if linux else 'windows-x64'
    if mac and platform.machine() != 'arm64':
        raise ValueError('The macOS release currently targets Apple Silicon (arm64).')
    if linux and platform.machine() not in ('x86_64', 'amd64'):
        raise ValueError('The Linux release currently targets x86-64.')
    version = read_version()
    if editor_version(editor.resolve(), system) != version:
        raise ValueError('The compiled editor does not match VERSION.')
    name = f'Sailor-{version}-{target}'
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    archive = output / archive_filename(version, target)
    checksum = output / (name + '.sha256')
    if archive.exists() or checksum.exists():
        raise FileExistsError('A release archive/checksum already exists; choose a new output directory.')
    with tempfile.TemporaryDirectory(prefix='sailor-release-') as temporary:
        stage = Path(temporary) / name
        subprocess.run(['cmake', '--install', str(build.resolve()), '--config', 'Release', '--prefix', str(stage)], check=True)
        executable = stage / 'bin' / ('SailorEngine-Release' + ('.exe' if system == 'Windows' else ''))
        if run(executable, '--version') != f'Sailor Engine {version}':
            raise ValueError('The installed native runtime does not match VERSION.')
        copy_directory(ROOT / 'Content', stage / 'Content')
        shutil.copy2(ROOT / 'ProjectSettings.yaml', stage / 'ProjectSettings.yaml')
        shutil.copy2(ROOT / 'VERSION', stage / 'VERSION')
        copy_directory(dependencies.resolve(), stage / 'Dependencies')
        editor_target = stage / 'Editor'
        if mac:
            editor_target.mkdir()
            copy_directory(editor.resolve(), editor_target / 'SailorEditor.app')
            # The app contains Mono-rewritten assemblies. C# evaluation uses
            # the original reference image beside the bundle, as in source builds.
            shutil.copy2(editor.resolve().parent / 'SailorEditor.dll', editor_target / 'SailorEditor.dll')
            native = editor_target / 'SailorEditor.app' / 'Contents' / 'Resources' / 'Sailor-Release.dylib'
            # Always use the native image from this SDK generation.
            shutil.copy2(stage / 'lib' / 'Sailor-Release.dylib', native)
            search = [stage / 'lib', dependencies.resolve() / 'lib']
            bundle_mac_dependencies(stage / 'lib' / 'Sailor-Release.dylib', search)
            bundle_mac_dependencies(native, search)
            subprocess.run(['codesign', '--force', '--deep', '--sign', '-', str(editor_target / 'SailorEditor.app')], check=True)
            subprocess.run(['codesign', '--verify', '--deep', '--strict', str(editor_target / 'SailorEditor.app')], check=True)
            launcher = stage / 'SailorEditor.command'
            launcher.write_text('#!/bin/sh\nset -eu\npackage_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\nexec open "$package_dir/Editor/SailorEditor.app" --args "$@"\n', encoding='utf-8')
            launcher.chmod(0o755)
        elif linux:
            copy_directory(editor.resolve(), editor_target)
            shutil.copy2(stage / 'lib' / 'Sailor-Release.so', editor_target / 'Sailor-Release.so')
            launcher = stage / 'SailorEditor.sh'
            launcher.write_text(
                '#!/bin/sh\nset -eu\n'
                'package_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
                'export LD_LIBRARY_PATH="$package_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n'
                'exec "$package_dir/Editor/SailorEditor" "$@"\n', encoding='utf-8')
            launcher.chmod(0o755)
        else:
            copy_directory(editor.resolve(), editor_target)
            if not (editor_target / 'SailorEditor.exe').is_file():
                raise FileNotFoundError('The self-contained editor publish output is missing SailorEditor.exe.')
            for library in (stage / 'bin').glob('*.dll'):
                shutil.copy2(library, editor_target / library.name)
            (stage / 'SailorEditor.cmd').write_text('@echo off\r\npushd "%~dp0"\r\n"%~dp0Editor\\SailorEditor.exe" %*\r\npopd\r\n', encoding='utf-8', newline='')
        for label, source in [('McpBridge', bridge), ('SailorBuild', cli)]:
            if source is not None:
                copy_directory(source.resolve(), stage / 'Tools' / label)
        metadata = {'schemaVersion': 1, 'engineVersion': version, 'editorVersion': version,
                    'platform': target, 'commit': run('git', '-C', ROOT, 'rev-parse', 'HEAD')}
        (stage / 'release.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
        (stage / 'README.txt').write_text(
            f'Sailor Engine and Editor {version} ({target})\n\n'
            'Launch SailorEditor.command on macOS, SailorEditor.cmd on Windows, or ./SailorEditor.sh on Linux.\n'
            'The Editor directory also contains the application for direct launch.\n'
            'Windows requires a Vulkan-capable graphics driver. macOS targets Apple Silicon.\n'
            'This macOS package is ad-hoc signed, not Apple-notarized; allow it through macOS Privacy & Security.\n'
            'To compile C++ workspace logic, install CMake and a C++20 toolchain (Xcode command-line tools or Visual Studio 2022).\n'
            'C++ SDK consumers also need Vulkan SDK 1.3+ on Windows, or brew install vulkan-headers on macOS.\n'
            'Linux targets Ubuntu 24.04 x86-64 (glibc 2.39+) with GTK4, Vulkan loader/driver and .NET native runtime dependencies.\n'
            'Ubuntu runtime packages: libgtk-4-1 libvulkan1 libuuid1 libasound2t64 libicu74 libssl3t64 libgssapi-krb5-2 zlib1g.\n'
            'Linux C++ SDK consumers also need libvulkan-dev. No system .NET installation is required.\n'
            'The matching SDK and dependency headers/libraries are included.\n'
            'GitHub releases: https://github.com/aantropov/sailor/releases\n', encoding='utf-8')
        # Validate the relocated runtime too, before emitting a publishable archive.
        if run(executable, '--version') != f'Sailor Engine {version}':
            raise ValueError('The relocated native runtime does not match VERSION.')
        run(executable, '--describe-workspace', stage)
        partial = output / (name + '.partial' + ('.tar.gz' if linux else '.zip'))
        if partial.exists():
            raise FileExistsError(partial)
        try:
            write_archive(stage, partial, mac, linux)
            digest = sha256_file(partial)
            partial.rename(archive)
            checksum.write_text(f'{digest}  {archive.name}\n', encoding='utf-8')
        finally:
            partial.unlink(missing_ok=True)
    return archive


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--editor', type=Path, required=True)
    parser.add_argument('--dependencies', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--bridge', type=Path)
    parser.add_argument('--cli', type=Path)
    args = parser.parse_args()
    print(package(args.build_dir, args.editor, args.dependencies, args.output, args.bridge, args.cli))


if __name__ == '__main__':
    main()
