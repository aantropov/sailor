"""Exercise release identities, archives, checksums and parsed workflow relationships."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import subprocess
import tarfile
import tempfile
import unittest
import zipfile
from unittest.mock import patch

import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'Tools' / 'Release'))
from package_release import RELEASE_TARGETS, archive_filename, read_version, write_archive
from publish_release import verify_assets
from release_identity import RELEASE_ACTIONS
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

    @unittest.skipIf(os.name == 'nt', 'Linux executable permissions and symlinks require a POSIX host.')
    def test_linux_archive_preserves_executables_symlinks_and_existing_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage = root / 'Sailor-0.9.0-linux-x64'
            (stage / 'Editor').mkdir(parents=True)
            executable = stage / 'SailorEditor.sh'
            executable.write_text('#!/bin/sh\nexit 0\n', encoding='utf-8')
            executable.chmod(0o755)
            (stage / 'Editor' / 'link').symlink_to('../SailorEditor.sh')
            archive = root / archive_filename('0.9.0', 'linux-x64')
            write_archive(stage, archive, False, True)
            with tarfile.open(archive) as contents:
                self.assertEqual(contents.getmember(stage.name + '/SailorEditor.sh').mode & 0o777, 0o755)
                link = contents.getmember(stage.name + '/Editor/link')
                self.assertTrue(link.issym())
                self.assertEqual(link.linkname, '../SailorEditor.sh')
            original = archive.read_bytes()
            with self.assertRaises(FileExistsError):
                write_archive(stage, archive, False, True)
            self.assertEqual(original, archive.read_bytes())

    def test_publisher_requires_all_matching_platform_archives(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for target in RELEASE_TARGETS:
                stage = root / f'Sailor-0.1.0-{target}'
                stage.mkdir()
                (stage / 'release.json').write_text(json.dumps({
                    'schemaVersion': 1, 'engineVersion': '0.1.0', 'editorVersion': '0.1.0',
                    'platform': target, 'commit': 'tested-commit'}), encoding='utf-8')
                archive = root / archive_filename('0.1.0', target)
                write_archive(stage, archive, False, target == 'linux-x64')
                (root / (stage.name + '.sha256')).write_text(
                    hashlib.sha256(archive.read_bytes()).hexdigest() + '  ' + archive.name + '\n')
            self.assertEqual(len(verify_assets(root, '0.1.0')), 6)
            self.assertEqual(len(verify_assets(root, '0.1.0', 'tested-commit')), 6)
            linux = root / archive_filename('0.1.0', 'linux-x64')
            saved_linux = linux.read_bytes()
            linux.unlink()
            with self.assertRaises(FileNotFoundError):
                verify_assets(root, '0.1.0')
            linux.write_bytes(saved_linux)
            with self.assertRaises(ValueError):
                verify_assets(root, '0.1.0', 'different-commit')
            (root / 'Sailor-0.1.0-macos-arm64.zip').write_bytes(b'corrupt')
            with self.assertRaises(ValueError):
                verify_assets(root, '0.1.0')
            with self.assertRaises(FileNotFoundError):
                verify_assets(root, '0.2.0')

    def test_parsed_workflow_has_manual_owner_only_publication_after_all_builds(self):
        workflow = yaml.safe_load((ROOT / '.github/workflows/release-desktop.yml').read_text())
        self.assertEqual(set(workflow['on']), {'workflow_dispatch', 'pull_request'})
        inputs = workflow['on']['workflow_dispatch']['inputs']
        self.assertEqual(set(inputs), {'action'})
        action = inputs['action']
        self.assertEqual(action['type'], 'choice')
        self.assertIs(action['required'], True)
        self.assertEqual(action['default'], 'Build artifacts')
        self.assertEqual(set(action['options']), set(RELEASE_ACTIONS))
        self.assertEqual(workflow['permissions'], {'contents': 'read'})
        jobs = workflow['jobs']
        for output in ('publish', 'prerelease'):
            self.assertEqual(jobs['validate']['outputs'][output], '${{ steps.version.outputs.' + output + ' }}')
        permission_check = next(step for step in jobs['validate']['steps'] if step.get('if'))
        self.assertEqual(permission_check['if'], "steps.version.outputs.publish == 'true'")
        self.assertEqual(permission_check['env'], {'ACTOR': '${{ github.actor }}',
                                                 'OWNER': '${{ github.repository_owner }}', 'REF': '${{ github.ref }}'})
        self.assertEqual(jobs['build']['needs'], 'validate')
        targets = jobs['build']['strategy']['matrix']['include']
        self.assertEqual({entry['target'] for entry in targets}, set(RELEASE_TARGETS))
        linux = next(entry for entry in targets if entry['target'] == 'linux-x64')
        self.assertEqual((linux['os'], linux['rid'], linux['framework'], linux['triplet']),
                         ('ubuntu-24.04', 'linux-x64', 'net10.0', 'x64-linux'))
        for entry in targets:
            self.assertRegex(entry['sdk'], r'^10\.0\.[0-9]{3}$')
            if 'workload' in entry:
                self.assertEqual(entry['workload'].split('.')[:3], entry['sdk'].split('.'))
        mac = next(entry for entry in targets if entry['target'] == 'macos-arm64')
        self.assertEqual((mac['sdk'], mac['workload'], mac['xcode']), ('10.0.301', '10.0.301.1', '27.2'))
        self.assertEqual(jobs['publish']['needs'], ['validate', 'build'])
        self.assertEqual(jobs['publish']['permissions'], {'contents': 'write'})
        conditions = {condition.strip() for condition in jobs['publish']['if'].split('&&')}
        self.assertEqual(conditions, {"github.event_name == 'workflow_dispatch'", "needs.validate.outputs.publish == 'true'",
                                      'github.actor == github.repository_owner', "github.ref == 'refs/heads/main'"})
        publisher = jobs['publish']['steps'][-1]
        self.assertEqual(publisher['env']['PRERELEASE'], '${{ needs.validate.outputs.prerelease }}')

    def test_release_action_cli_emits_safe_publication_outputs(self):
        script = str(ROOT / 'Tools/Release/release_identity.py')
        expected = {
            'Build artifacts': ('false', 'true'),
            'Publish prerelease': ('true', 'true'),
            'Publish release': ('true', 'false'),
        }
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'outputs.txt'
            for action, (publish, prerelease) in expected.items():
                with self.subTest(action=action):
                    output.write_text('', encoding='utf-8')
                    subprocess.run([sys.executable, script, '--action', action, '--github-output', str(output)],
                                   check=True, capture_output=True)
                    actual = dict(line.split('=', 1) for line in output.read_text().splitlines())
                    self.assertEqual(actual, {'version': read_version(), 'publish': publish, 'prerelease': prerelease})
            for environment in ({}, {'RELEASE_ACTION': 'Publish prerelease'}, {'RELEASE_ACTION': 'invalid action'}):
                output.write_text('', encoding='utf-8')
                process_environment = os.environ.copy()
                process_environment.pop('RELEASE_ACTION', None)
                process_environment.update(environment)
                result = subprocess.run([sys.executable, script, '--github-output', str(output)],
                                        env=process_environment, capture_output=True, text=True)
                if environment.get('RELEASE_ACTION') == 'invalid action':
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(output.read_text(), '')
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    actual = dict(line.split('=', 1) for line in output.read_text().splitlines())
                    self.assertEqual(actual['publish'], 'true' if environment else 'false')
                    self.assertEqual(actual['prerelease'], 'true')

    def test_publication_permission_step_rejects_other_actors_and_refs(self):
        shell = shutil.which('bash')
        if shell is None:
            self.skipTest('The workflow permission step requires bash.')
        workflow = yaml.safe_load((ROOT / '.github/workflows/release-desktop.yml').read_text())
        step = next(step for step in workflow['jobs']['validate']['steps'] if step.get('if'))
        for actor, ref, allowed in (('owner', 'refs/heads/main', True),
                                    ('contributor', 'refs/heads/main', False),
                                    ('owner', 'refs/heads/feature', False),
                                    ('owner', 'refs/pull/1/merge', False)):
            with self.subTest(actor=actor, ref=ref):
                environment = os.environ.copy()
                environment.update(ACTOR=actor, OWNER='owner', REF=ref)
                result = subprocess.run([shell, '--noprofile', '--norc', '-e', '-o', 'pipefail', '-c', step['run']],
                                        env=environment, capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, allowed)


if __name__ == '__main__':
    unittest.main()
