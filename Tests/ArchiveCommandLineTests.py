"""Exercise the shipped engine's headless workspace/archive commands."""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile


ENGINE = Path(sys.argv.pop(1)).resolve()


class ArchiveCommandLineTests(unittest.TestCase):
    def run_engine(self, *arguments: object) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(ENGINE), *(str(argument) for argument in arguments)],
            capture_output=True, text=True, encoding="utf-8", timeout=20,
        )

    def test_archive_options_and_utf8_paths(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sailor-cli-") as temporary:
            root = Path(temporary)
            source = root / "source Я 船"
            source.mkdir()
            (source / "empty directory").mkdir()
            payload = bytes(range(256)) * 1024
            name = "payload Я 船.bin"
            (source / name).write_bytes(payload)
            timestamp = (source / name).stat().st_mtime_ns
            for mode in ("stored", "fastest", "balanced", "smallest"):
                with self.subTest(mode=mode):
                    output = root / f"game {mode} Я.zip"
                    result = self.run_engine("--pack-assets", source, output, mode)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    with zipfile.ZipFile(output) as archive:
                        self.assertIsNone(archive.testzip())
                        self.assertEqual(archive.read(name), payload)
                        self.assertTrue(archive.getinfo("empty directory/").is_dir())
                        expected = zipfile.ZIP_STORED if mode == "stored" else zipfile.ZIP_DEFLATED
                        self.assertEqual(archive.getinfo(name).compress_type, expected)
                        index = json.loads(archive.read(".sailor-files.json"))
                        self.assertEqual(index["schemaVersion"], 1)
                        self.assertEqual(index["files"][name], {"size": len(payload), "modifiedUnixNs": timestamp})
                    original = output.read_bytes()
                    repeated = self.run_engine("--pack-assets", source, output, mode)
                    self.assertNotEqual(repeated.returncode, 0)
                    self.assertEqual(output.read_bytes(), original)
            invalid = root / "invalid.zip"
            self.assertNotEqual(self.run_engine("--pack-assets", source, invalid, "unknown").returncode, 0)
            self.assertFalse(invalid.exists())
            self.assertNotEqual(self.run_engine("--pack-assets", source).returncode, 0)

    def test_describe_workspace_without_starting_renderer(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sailor-describe-") as temporary:
            root = Path(temporary) / "game Я"
            (root / "Content").mkdir(parents=True)
            result = self.run_engine("--describe-workspace", root)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            description = json.loads(result.stdout)
            self.assertEqual(description["schemaVersion"], 1)
            self.assertTrue(description["legacy"])
            self.assertEqual(Path(description["content"]).resolve(), (root / "Content").resolve())
            self.assertEqual(Path(description["engineContent"]).resolve(), (root / "Content").resolve())
            self.assertNotEqual(self.run_engine("--describe-workspace").returncode, 0)

    def test_initial_cache_is_an_explicit_subset(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sailor-cache-cli-") as temporary:
            root = Path(temporary)
            workspace = root / "project Я"
            (workspace / "Content").mkdir(parents=True)
            cache = workspace / "Cache"
            (cache / "Lods").mkdir(parents=True)
            (cache / "Lods" / "mesh.bin").write_bytes(b"lod payload")
            (cache / "AssetCache.yaml").write_text("private absolute source paths", encoding="utf-8")
            (cache / "EditorSettings.yaml").write_text("private editor state", encoding="utf-8")
            output = root / "cache Я.zip"
            result = self.run_engine("--pack-cache", workspace, output, "balanced")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            with zipfile.ZipFile(output) as archive:
                self.assertEqual(archive.read("Lods/mesh.bin"), b"lod payload")
                self.assertNotIn("AssetCache.yaml", archive.namelist())
                self.assertNotIn("EditorSettings.yaml", archive.namelist())
                self.assertIsNone(archive.testzip())
            original = output.read_bytes()
            self.assertNotEqual(self.run_engine("--pack-cache", workspace, output, "balanced").returncode, 0)
            self.assertEqual(output.read_bytes(), original)
            (cache / "ShaderCache.yaml").write_text("invalid envelope", encoding="utf-8")
            invalid = root / "invalid.zip"
            self.assertNotEqual(self.run_engine("--pack-cache", workspace, invalid, "stored").returncode, 0)
            self.assertFalse(invalid.exists())


if __name__ == "__main__":
    unittest.main()
