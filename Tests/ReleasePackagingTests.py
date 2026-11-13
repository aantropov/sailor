"""Exercise release identities, archives, checksums and parsed workflow relationships."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
import zipfile
from unittest.mock import patch

import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'Tools' / 'Release'))
from package_release import read_version, write_archive
from publish_release import verify_assets
import update_deps


class ReleasePackagingTests(unittest.TestCase):
    def test_all_desktop_hosts_use_the_pinned_dependency_overlays(self):
        for system in ('Darwin', 'Windows', 'Linux'):
            with self.subTest(system=system):
                with patch.object(update_deps.platform, 'system', return_value=system), \
                        patch.object(update_deps.platform, 'machine', return_value='arm64'), \
                        patch.object(update_deps, 'has_current_vcpkg_tool', return_value=True), \
                        patch.object(update_deps, 'required_vcpkg_tool_release', return_value='test'), \
                        patch.object(update_deps.subprocess, 'check_call') as execute:
                    self.assertEqual(update_deps.main(), 0)
                commands = [call.args[0] for call in execute.call_args_list]
                installations = [command for command in commands if 'install' in command]
                self.assertTrue(installations)
                self.assertTrue(all('--overlay-ports=' + update_deps.VCPKG_OVERLAY_PORTS in command
                                    for command in commands))
                self.assertEqual(any(any(value.startswith('--overlay-triplets=') for value in command)
                                     for command in commands), system == 'Windows')

    def test_runtime_utf8_output_does_not_depend_on_the_host_locale(self):
        environment = os.environ.copy()
        environment.update(PYTHONUTF8='0', PYTHONCOERCECLOCALE='0', LC_ALL='C')
        script = r'''
import sys
sys.path.insert(0, sys.argv[1])
from package_release import run
payload = bytes([208,159,209,128,208,190,208,181,208,186,209,130])
actual = run(sys.executable, '-c', 'import sys; sys.stdout.buffer.write(' + repr(payload) + ')')
assert actual == '\u041f\u0440\u043e\u0435\u043a\u0442'
'''
        subprocess.run([sys.executable, '-c', script, str(ROOT / 'Tools' / 'Release')],
                       env=environment, check=True, capture_output=True)

    def test_version_validation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for version in ('0.1.0', '1.20.300'):
                (root / 'VERSION').write_text(version + '\n', encoding='utf-8')
                self.assertEqual(read_version(root), version)
            for version in ('', '1.0', 'v1.0.0', '01.0.0', '1.0.0\nmalformed', '1.0.0;command'):
                (root / 'VERSION').write_text(version, encoding='utf-8')
                with self.assertRaises(ValueError):
                    read_version(root)

    def test_archive_retains_release_root_and_preserves_existing_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage = root / 'Sailor-0.1.0-windows-x64'
            (stage / 'Editor').mkdir(parents=True)
            (stage / 'Editor' / 'SailorEditor.exe').write_bytes(b'editor fixture')
            output = root / 'release.zip'
            write_archive(stage, output, False)
            with zipfile.ZipFile(output) as archive:
                self.assertEqual(archive.read(stage.name + '/Editor/SailorEditor.exe'), b'editor fixture')
                self.assertIsNone(archive.testzip())
            original = output.read_bytes()
            with self.assertRaises(FileExistsError):
                write_archive(stage, output, False)
            self.assertEqual(original, output.read_bytes())

    def test_publisher_requires_both_matching_platform_archives(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for target in ('windows-x64', 'macos-arm64'):
                archive = root / f'Sailor-0.1.0-{target}.zip'
                with zipfile.ZipFile(archive, 'w') as contents:
                    contents.writestr(archive.stem + '/release.json', json.dumps({
                        'schemaVersion': 1, 'engineVersion': '0.1.0', 'editorVersion': '0.1.0',
                        'platform': target, 'commit': 'tested-commit'}))
                archive.with_suffix('.sha256').write_text(
                    hashlib.sha256(archive.read_bytes()).hexdigest() + '  ' + archive.name + '\n')
            self.assertEqual(len(verify_assets(root, '0.1.0')), 4)
            self.assertEqual(len(verify_assets(root, '0.1.0', 'tested-commit')), 4)
            with self.assertRaises(ValueError):
                verify_assets(root, '0.1.0', 'different-commit')
            (root / 'Sailor-0.1.0-macos-arm64.zip').write_bytes(b'corrupt')
            with self.assertRaises(ValueError):
                verify_assets(root, '0.1.0')
            with self.assertRaises(FileNotFoundError):
                verify_assets(root, '0.2.0')

    def test_parsed_workflow_has_manual_owner_only_publication_after_all_builds(self):
        workflow = yaml.safe_load((ROOT / '.github/workflows/release.yml').read_text())
        self.assertEqual(set(workflow['on']), {'workflow_dispatch', 'pull_request'})
        self.assertIs(workflow['on']['workflow_dispatch']['inputs']['publish']['default'], False)
        self.assertEqual(workflow['permissions'], {'contents': 'read'})
        jobs = workflow['jobs']
        self.assertEqual(jobs['build']['needs'], 'validate')
        targets = jobs['build']['strategy']['matrix']['include']
        self.assertEqual({entry['target'] for entry in targets}, {'windows-x64', 'macos-arm64'})
        self.assertEqual(jobs['publish']['needs'], ['validate', 'build'])
        self.assertEqual(jobs['publish']['permissions'], {'contents': 'write'})
        conditions = {condition.strip() for condition in jobs['publish']['if'].split('&&')}
        self.assertEqual(conditions, {"github.event_name == 'workflow_dispatch'", 'inputs.publish',
                                      'github.actor == github.repository_owner', "github.ref == 'refs/heads/main'"})


if __name__ == '__main__':
    unittest.main()
