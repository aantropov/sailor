#!/usr/bin/env python3
"""Extract a release archive and exercise its relocated runtime and installed SDK."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
from package_release import ROOT, RELEASE_TARGETS, archive_filename, read_version, run, sha256_file


def verify(assets: Path, target: str) -> None:
    version = read_version()
    name = f'Sailor-{version}-{target}'
    archive = assets.resolve() / archive_filename(version, target)
    expected, filename = (assets / (name + '.sha256')).read_text(encoding='utf-8').strip().split('  ', 1)
    if filename != archive.name or expected != sha256_file(archive):
        raise ValueError('Archive checksum does not match its release identity.')
    with tempfile.TemporaryDirectory(prefix='sailor-verify-release-') as temporary:
        root = Path(temporary)
        if platform.system() == 'Darwin':
            subprocess.run(['ditto', '-x', '-k', str(archive), str(root)], check=True)
        elif target == 'linux-x64':
            shutil.unpack_archive(archive, root, filter='data')
        else:
            shutil.unpack_archive(archive, root)
        package = root / name
        metadata = json.loads((package / 'release.json').read_text(encoding='utf-8'))
        if metadata['schemaVersion'] != 1 or metadata['engineVersion'] != version or metadata['editorVersion'] != version or metadata['platform'] != target:
            raise ValueError('Archive metadata does not match the release being verified.')
        executable = package / 'bin' / ('SailorEngine-Release.exe' if os.name == 'nt' else 'SailorEngine-Release')
        if run(executable, '--version') != f'Sailor Engine {version}':
            raise ValueError('Extracted runtime has an unexpected version.')
        description = json.loads(run(executable, '--describe-workspace', package))
        if Path(description['content']).resolve() != (package / 'Content').resolve():
            raise ValueError('The extracted runtime did not resolve its own content.')
        if target == 'linux-x64':
            if run(package / 'SailorEditor.sh', '--version') != f'Sailor Editor {version}':
                raise ValueError('The extracted Linux editor has an unexpected version.')
        build = root / 'sdk-probe'
        subprocess.run(['cmake', '-S', str(ROOT / 'Tests' / 'ReleaseSdkProbe'), '-B', str(build),
                        '-DCMAKE_BUILD_TYPE=Release', '-DSailor_DIR=' + str(package / 'lib' / 'cmake' / 'Sailor')], check=True)
        subprocess.run(['cmake', '--build', str(build), '--config', 'Release', '--parallel', '2'], check=True)
        probe_path = Path((build / 'probe-Release.txt').read_text(encoding='utf-8'))
        environment = os.environ.copy()
        environment['PATH'] = str(package / 'bin') + os.pathsep + environment.get('PATH', '')
        actual = subprocess.check_output([str(probe_path)], text=True, encoding='utf-8', env=environment).strip()
        if actual != version:
            raise ValueError('An external SDK consumer loaded an unexpected runtime version.')
        print(f'Extracted {target}: runtime, workspace content and compiled SDK consumer match {version}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--target', choices=RELEASE_TARGETS, required=True)
    args = parser.parse_args()
    verify(args.assets, args.target)
