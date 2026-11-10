#!/usr/bin/env python3
"""Package an installed Linux runtime and a workspace for execution outside the checkout."""
from __future__ import annotations

import argparse
import json
import platform
import shutil
import subprocess
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=REPO_ROOT / "build-linux")
    parser.add_argument("--workspace", type=Path, default=REPO_ROOT)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--config", choices=["Debug", "Release", "RelWithDebInfo", "MinSizeRel"], default="Release")
    args = parser.parse_args()
    if platform.system() != "Linux":
        parser.error("Package the runtime on Linux using its native build.")
    workspace = args.workspace.resolve()
    output = args.output.resolve()
    if output.exists():
        parser.error("The output must be a new directory to preserve existing packages.")
    if output.is_relative_to(workspace):
        parser.error("The output must be outside the workspace being packaged.")
    built_executable = REPO_ROOT / "Binaries" / f"SailorEngine-{args.config}"
    description = json.loads(subprocess.check_output(
        [str(built_executable), "--describe-workspace", str(workspace)], text=True))
    if description["schemaVersion"] != 1:
        raise ValueError("Unsupported workspace description schema.")
    content = Path(description["content"])
    module = None
    if not description["legacy"]:
        module = Path(description["logicOutput"]) / args.config / f"lib{description['logicModuleName']}.so"
        if not module.is_file():
            parser.error(f"Build the workspace module in {args.config} before packaging: {module}")

    subprocess.run([
        "cmake", "--install", str(args.build_dir.resolve()), "--config", args.config,
        "--prefix", str(output), "--component", "Runtime",
    ], check=True)
    executable = output / "bin" / f"SailorEngine-{args.config}"
    if not executable.is_file():
        raise FileNotFoundError(f"Runtime installation did not produce {executable}")
    shutil.copytree(content, output / "Content")
    if module is not None:
        shutil.copytree(description["engineContent"], output / "Engine" / "Content")
        module_directory = output / "Binaries" / args.config
        module_directory.mkdir(parents=True)
        shutil.copy2(module, module_directory / module.name)
        manifest = {
            "manifestVersion": 1, "workspaceId": description["workspaceId"],
            "name": description["name"], "enginePath": "Engine", "engineReferenceKind": "installed",
            "contentPath": "Content", "sourcePath": "Source", "generatedProjectPath": "Generated",
            "cachePath": "Cache", "buildPath": "Cache/Build", "logicOutputPath": "Binaries",
            "logicModuleName": description["logicModuleName"],
        }
        # JSON is a YAML subset; the engine's existing manifest reader validates it.
        (output / "workspace.sailor").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    settings = workspace / "ProjectSettings.yaml"
    if settings.exists():
        shutil.copy2(settings, output / settings.name)
    shutil.copy2(REPO_ROOT / "ThirdPartyLicenses.txt", output / "ThirdPartyLicenses.txt")
    launcher = output / "Sailor.sh"
    launcher.write_text(
        '#!/bin/sh\nset -eu\n'
        'package_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
        f'exec "$package_dir/bin/{executable.name}" --workspace "$package_dir" "$@"\n',
        encoding="utf-8",
    )
    launcher.chmod(0o755)
    print(f"Linux runtime package: {output}")
    print(f"Launch: {launcher}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
