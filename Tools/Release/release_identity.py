#!/usr/bin/env python3
"""Read the checked-out release version without changing source files."""
import argparse
from pathlib import Path
from package_release import read_version

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--github-output', type=Path)
args = parser.parse_args()
version = read_version()
if args.github_output:
    with args.github_output.open('a', encoding='utf-8') as output:
        output.write(f'version={version}\n')
print(version)
