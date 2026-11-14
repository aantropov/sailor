#!/usr/bin/env python3
"""Resolve the checked-out release version and publication options."""
import argparse
import os
from pathlib import Path
from package_release import read_version

RELEASE_ACTIONS = {
    'Build artifacts': (False, True),
    'Publish prerelease': (True, True),
    'Publish release': (True, False),
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--github-output', type=Path)
    parser.add_argument('--action', choices=RELEASE_ACTIONS,
                        default=os.environ.get('RELEASE_ACTION', 'Build artifacts'))
    args = parser.parse_args()
    # Environment defaults need the same validation as explicit CLI choices.
    if args.action not in RELEASE_ACTIONS:
        parser.error('Unknown release action: ' + args.action)
    version = read_version()
    publish, prerelease = RELEASE_ACTIONS[args.action]
    if args.github_output:
        with args.github_output.open('a', encoding='utf-8') as output:
            output.write(f'version={version}\npublish={str(publish).lower()}\n'
                         f'prerelease={str(prerelease).lower()}\n')
    print(version)


if __name__ == '__main__':
    main()
