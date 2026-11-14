"""Exercise component routing, Git change detection and parsed CI dependencies."""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'Tools/CI'))
from select_checks import changed_paths, checks_for_event, select_checks

RULES = json.loads((ROOT / '.github/ci-paths.json').read_text(encoding='utf-8'))


class CiWorkflowTests(unittest.TestCase):
    def test_native_and_shader_changes_do_not_build_the_editor(self):
        for path in ('Runtime/FrameGraph/DepthHighZNode.cpp', 'Lib/Sailor.cpp',
                     'Content/Shaders/ComputeDepthHighZ.shader', 'CMake/SailorConfig.cmake.in',
                     'Tests/ShaderCacheArtifactTests.cpp', '.github/vcpkg-ports/tinygltf/portfile.cmake'):
            with self.subTest(path=path):
                self.assertEqual(select_checks([path], RULES), {'engine': True, 'editor': False})

    def test_editor_and_tools_changes_do_not_build_native_runtime(self):
        for path in ('Editor/ViewModels/SceneViewModel.cs', 'Editor/Views/SceneView.xaml',
                     'Editor.Linux/LinuxMainThread.cs', 'Editor/McpBridge/Program.cs',
                     'Editor.Tests/SceneSelectionRoutingTests.cs', 'Tools/SailorBuild/Program.cs'):
            with self.subTest(path=path):
                self.assertEqual(select_checks([path], RULES), {'engine': False, 'editor': True})

    def test_shared_contracts_and_sdk_inputs_require_both_components(self):
        for path in ('Protocol/editor_engine.proto', 'Editor/Protocol/EngineProtocolNative.cs',
                     'Runtime/Editor/EditorInterop.h', 'Lib/EditorProtocolExports.cpp', 'VERSION',
                     'Directory.Build.props', 'Editor/Workspace/GameBuildService.cs',
                     'Editor/Utility/EngineTypes.cs',
                     'Editor.Tests/WorkspaceProjectGeneratorTests.cs', '.github/ci-paths.json',
                     '.github/workflows/ci-windows.yml', 'Tools/CI/select_checks.py'):
            with self.subTest(path=path):
                self.assertEqual(select_checks([path], RULES), {'engine': True, 'editor': True})

    def test_documentation_only_changes_skip_component_builds(self):
        self.assertEqual(select_checks(['README.md', 'Docs/Plans/CI.md', '.github/media/editor.png'], RULES),
                         {'engine': False, 'editor': False})
        self.assertEqual(select_checks([], RULES), {'engine': False, 'editor': False})

    def test_mixed_and_unknown_inputs_preserve_coverage(self):
        self.assertEqual(select_checks(['Runtime/Sailor.cpp', 'Editor/Views/SceneView.xaml'], RULES),
                         {'engine': True, 'editor': True})
        self.assertEqual(select_checks(['NewBuildSystem/tool.config'], RULES), {'engine': True, 'editor': True})

    def test_manual_runs_select_all_components(self):
        self.assertEqual(checks_for_event('workflow_dispatch', {}, ROOT, RULES), {'engine': True, 'editor': True})
        with self.assertRaises(ValueError):
            checks_for_event('unexpected_event', {}, ROOT, RULES)

    def test_diff_uses_merge_base_and_retains_both_sides_of_renames(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)

            def git(*args):
                return subprocess.check_output(['git', '-c', 'user.name=CI test', '-c',
                                                'user.email=ci@example.invalid', *args], cwd=root,
                                               text=True).strip()

            git('init', '-q')
            (root / 'Runtime').mkdir()
            (root / 'Runtime/engine.cpp').write_text('original\n')
            (root / 'README.md').write_text('original\n')
            git('add', '.')
            git('commit', '-qm', 'Initial fixture')
            initial = git('rev-parse', 'HEAD')
            git('checkout', '-qb', 'topic')
            (root / 'README.md').write_text('topic docs\n')
            git('commit', '-qam', 'Topic docs')
            docs = git('rev-parse', 'HEAD')
            git('checkout', '-qb', 'base', initial)
            (root / 'Runtime/engine.cpp').write_text('unrelated base change\n')
            git('commit', '-qam', 'Advance base')
            base = git('rev-parse', 'HEAD')
            self.assertEqual(changed_paths(root, base, docs), ['README.md'])
            event = {'pull_request': {'base': {'sha': base}, 'head': {'sha': docs}}}
            self.assertEqual(checks_for_event('pull_request', event, root, RULES), {'engine': False, 'editor': False})
            git('checkout', '-q', 'topic')
            (root / 'Editor/Views').mkdir(parents=True)
            git('mv', 'Runtime/engine.cpp', 'Editor/Views/renamed Проект 船.cpp')
            git('commit', '-qm', 'Move between components')
            paths = changed_paths(root, docs, git('rev-parse', 'HEAD'))
            self.assertCountEqual(paths, ['Runtime/engine.cpp', 'Editor/Views/renamed Проект 船.cpp'])
            self.assertEqual(select_checks(paths, RULES), {'engine': True, 'editor': True})

    def test_ci_jobs_are_independent_and_use_component_outputs(self):
        for platform in ('windows', 'linux'):
            workflow = yaml.safe_load((ROOT / f'.github/workflows/ci-{platform}.yml').read_text())
            with self.subTest(platform=platform):
                self.assertEqual(set(workflow['on']), {'pull_request', 'workflow_dispatch'})
                self.assertEqual(workflow['permissions'], {'contents': 'read'})
                jobs = workflow['jobs']
                for component in ('engine', 'editor'):
                    self.assertEqual(jobs[component]['needs'], 'changes')
                    self.assertEqual(jobs[component]['if'], f"needs.changes.outputs.{component} == 'true'")
                    self.assertEqual(jobs['changes']['outputs'][component],
                                     '${{ steps.select.outputs.' + component + ' }}')
                checkout = next(step for step in jobs['changes']['steps'] if step.get('uses', '').startswith('actions/checkout@'))
                self.assertEqual(checkout['with']['fetch-depth'], 0)
        windows = yaml.safe_load((ROOT / '.github/workflows/ci-windows.yml').read_text())
        self.assertEqual({item['compiler'] for item in windows['jobs']['engine']['strategy']['matrix']['include']},
                         {'MSVC', 'clang-cl'})


if __name__ == '__main__':
    unittest.main()
