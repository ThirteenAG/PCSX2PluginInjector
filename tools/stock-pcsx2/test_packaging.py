"""Fail-closed profile emission and idempotent source-bound release notes."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from discover import digest
from test_discovery import image

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("download_links", Path(__file__).with_name("update-download-links.py"))
links = importlib.util.module_from_spec(spec)
spec.loader.exec_module(links)


class Packaging(unittest.TestCase):
    def test_fork_download_update_preserves_upstream_readme(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / 'configs/stock-pcsx2/source-target.json'
            target.parent.mkdir(parents=True)
            target.write_text(json.dumps(dict(upstream_tag='v2.9.94', upstream_commit='81526d4dc7cc70e4ae75abb35a789417456c6d43')))
            (root / 'README.md').write_text('# Injector\n')
            fork = root / 'fork'
            fork.mkdir()
            readme = fork / 'README.md'
            original = b'# Upstream README\nHuman text remains unchanged.\n'
            readme.write_bytes(original)
            with patch.object(links, 'ROOT', root), patch.object(sys, 'argv', ['links', '--fork', str(fork)]):
                links.main()
            self.assertEqual(readme.read_bytes(), original)
            self.assertIn('PCSX2 v2.9.94', (fork / 'docs/plugin-upstream-download.md').read_text())

    def test_download_block_preserves_other_notes(self):
        block = links.BEGIN + "\nnew target\n" + links.END
        initial = "# Release\nmanual notes\n" + links.BEGIN + "\nold target\n" + links.END + "\nmore notes\n"
        updated = links.replace(initial, block)
        self.assertIn("manual notes", updated)
        self.assertIn("more notes", updated)
        self.assertNotIn("old target", updated)
        self.assertEqual(links.replace(updated, block), updated)
        with self.assertRaises(ValueError):
            links.replace(initial + links.BEGIN, block)

    def test_release_notes_preserve_text_and_are_idempotent(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            snippet = root / "snippet.md"
            snippet.write_text(links.BEGIN + "\nnew target\n" + links.END)
            output = root / "notes.md"
            body = "Manual notes with `code`, $(), and quotes: \"keep\".\n"
            command = [sys.executable, str(Path(__file__).with_name("release-notes.py")), str(snippet), str(output)]
            process = subprocess.run(command, env=dict(os.environ, EXISTING_RELEASE_BODY=body), capture_output=True, text=True)
            self.assertEqual(process.returncode, 0, process.stderr)
            first = output.read_text()
            self.assertIn(body.rstrip(), first)
            process = subprocess.run(command, env=dict(os.environ, EXISTING_RELEASE_BODY=first), capture_output=True, text=True)
            self.assertEqual(process.returncode, 0, process.stderr)
            self.assertEqual(output.read_text(), first)

    def test_mismatched_source_cannot_activate_or_replace_output(self):
        candidate = json.loads((ROOT / "configs/stock-pcsx2/candidates/v2.9.96.json").read_text())
        self.assertFalse(candidate["source_matches_target"])
        reference = json.loads((ROOT / "configs/stock-pcsx2/reference-abi.json").read_text())
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "fixture.exe"
            executable.write_bytes(image())
            candidate["identity"]["sha256"] = digest(executable.read_bytes())
            output = root / "profile.ini"
            output.write_text("previous profile")
            source = root / "candidate.json"
            source.write_text(json.dumps(candidate))
            ref = root / "reference.json"
            ref.write_text(json.dumps(reference))
            command = [sys.executable, str(Path(__file__).with_name("emit-runtime-config.py")), "--candidate", str(source),
                "--exe", str(executable), "--reference", str(ref), "--output", str(output), "--activate"]
            process = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(process.returncode, 0)
            self.assertIn("Only the exact source-matched ABI", process.stderr)
            self.assertEqual(output.read_text(), "previous profile")


if __name__ == "__main__":
    unittest.main()
