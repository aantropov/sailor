#!/usr/bin/env python3
"""Verify both desktop archives and publish one new immutable GitHub Release."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import zipfile
from package_release import read_version, sha256_file


def verify_assets(assets: Path, version: str, commit: str | None = None) -> list[Path]:
    result = []
    for target in ('windows-x64', 'macos-arm64'):
        archive = assets / f'Sailor-{version}-{target}.zip'
        checksum = archive.with_suffix('.sha256')
        expected, filename = checksum.read_text(encoding='utf-8').strip().split('  ', 1)
        if filename != archive.name or expected != sha256_file(archive):
            raise ValueError(f'Invalid release checksum for {archive.name}')
        with zipfile.ZipFile(archive) as contents:
            metadata = json.loads(contents.read(archive.stem + '/release.json'))
        if (metadata['schemaVersion'] != 1 or metadata['engineVersion'] != version or
                metadata['editorVersion'] != version or metadata['platform'] != target or
                (commit is not None and metadata['commit'] != commit)):
            raise ValueError(f'Release metadata does not match the requested version/commit: {archive.name}')
        result.extend([archive, checksum])
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--commit', required=True)
    args = parser.parse_args()
    if args.version != read_version():
        raise ValueError('The release version must match the checked-out VERSION.')
    assets = verify_assets(args.assets, args.version, args.commit)
    tag = 'v' + args.version
    # A successful ls-remote with an empty result proves absence; network/auth
    # errors remain failures rather than being mistaken for a missing tag.
    existing = subprocess.check_output(['git', 'ls-remote', '--tags', 'origin', 'refs/tags/' + tag], text=True)
    if existing.strip():
        raise FileExistsError(f'Tag {tag} already exists; releases are immutable.')
    command = ['gh', 'release', 'create', tag, '--target', args.commit,
               '--title', 'Sailor ' + args.version, '--generate-notes']
    if os.environ.get('PRERELEASE', 'true').lower() == 'true':
        command.append('--prerelease')
    command.extend(str(asset) for asset in assets)
    subprocess.run(command, check=True)


if __name__ == '__main__':
    main()
