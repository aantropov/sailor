#!/usr/bin/env python3
"""Select component checks from the complete pull-request diff."""
from __future__ import annotations

import argparse
from fnmatch import fnmatchcase
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def select_checks(paths: list[str], rules: dict[str, list[str]]) -> dict[str, bool]:
    selected = {'engine': False, 'editor': False}
    for path in paths:
        if any(fnmatchcase(path, pattern) for pattern in rules['shared']):
            return dict.fromkeys(selected, True)
        matched = False
        for component in selected:
            if any(fnmatchcase(path, pattern) for pattern in rules[component]):
                selected[component] = True
                matched = True
        if not matched and not any(fnmatchcase(path, pattern) for pattern in rules['ignored']):
            # New build inputs must not silently lose coverage before routing is added.
            return dict.fromkeys(selected, True)
    return selected


def changed_paths(repository: Path, base: str, head: str) -> list[str]:
    # Report both the deleted and added paths of a rename, even across components.
    output = subprocess.check_output([
        'git', 'diff', '--name-only', '--no-renames', '-z', base + '...' + head, '--',
    ], cwd=repository)
    return [path.decode('utf-8') for path in output.split(b'\0') if path]


def checks_for_event(event_name: str, event: dict, repository: Path,
                     rules: dict[str, list[str]]) -> dict[str, bool]:
    if event_name == 'workflow_dispatch':
        return {'engine': True, 'editor': True}
    if event_name != 'pull_request':
        raise ValueError('Unsupported CI event: ' + event_name)
    request = event['pull_request']
    return select_checks(changed_paths(repository, request['base']['sha'], request['head']['sha']), rules)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--event-name', default=os.environ.get('GITHUB_EVENT_NAME'))
    parser.add_argument('--event-path', type=Path, default=os.environ.get('GITHUB_EVENT_PATH'))
    parser.add_argument('--github-output', type=Path, default=os.environ.get('GITHUB_OUTPUT'))
    args = parser.parse_args()
    rules = json.loads((ROOT / '.github/ci-paths.json').read_text(encoding='utf-8'))
    event = json.loads(args.event_path.read_text(encoding='utf-8')) if args.event_path else {}
    checks = checks_for_event(args.event_name, event, ROOT, rules)
    output = ''.join(f'{name}={str(enabled).lower()}\n' for name, enabled in checks.items())
    print(output, end='')
    if args.github_output:
        with args.github_output.open('a', encoding='utf-8') as stream:
            stream.write(output)


if __name__ == '__main__':
    main()
