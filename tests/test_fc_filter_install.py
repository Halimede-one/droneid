import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[1] / "fc_filter" / "install.py"
spec = importlib.util.spec_from_file_location("fc_filter_install", SOURCE)
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class FilterInstallerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.tree = root / "checkout"
        self.assets = root / "assets"
        self.tree.mkdir()
        (self.assets / "patches").mkdir(parents=True)
        self.git("init", "-q")
        (self.tree / "source.c").write_text("original\n")
        (self.tree / "board.h").write_text("physical UART4\n")
        self.git("add", ".")
        self.git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-qm", "fixture")
        self.revision = self.git("rev-parse", "HEAD").strip()
        (self.assets / "policy.h").write_text("validated policy\n")
        (self.assets / "patches/inav.patch").write_text(
            "--- a/source.c\n+++ b/source.c\n@@ -1 +1 @@\n-original\n+filtered\n"
        )
        manifest = {"inav": {
            "tag": "test-pin", "commit": self.revision,
            "before": {"source.c": hashlib.sha256(b"original\n").hexdigest()},
            "after": {"source.c": hashlib.sha256(b"filtered\n").hexdigest()},
            "board_files": {"board.h": installer.digest(self.tree / "board.h")},
            "copies": {"policy.h": "policy.h"},
        }}
        (self.assets / "manifest.json").write_text(json.dumps(manifest))
        self.replace = patch.object(installer, "HERE", self.assets)
        self.replace.start()
        self.addCleanup(self.replace.stop)

    def git(self, *args):
        return subprocess.check_output(["git", *args], cwd=self.tree, text=True, stderr=subprocess.DEVNULL)

    def run_install(self, mode):
        return installer.install("inav", self.tree, mode)

    def test_check_is_read_only_then_apply_is_verifiable(self):
        self.assertEqual(self.run_install("check")["status"], "ready")
        self.assertEqual(self.git("status", "--porcelain"), "")
        self.assertEqual(self.run_install("apply")["status"], "verified")
        self.assertEqual(self.run_install("verify")["status"], "verified")
        self.assertEqual(self.run_install("apply")["status"], "verified")

    def test_changed_source_refused_before_mutation(self):
        (self.tree / "source.c").write_text("local changes\n")
        with self.assertRaisesRegex(ValueError, "local changes"):
            self.run_install("apply")
        self.assertFalse((self.tree / "policy.h").exists())
        self.assertEqual((self.tree / "source.c").read_text(), "local changes\n")

    def test_wrong_revision_refused(self):
        self.git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "--allow-empty", "-qm", "other")
        with self.assertRaisesRegex(ValueError, "Expected"):
            self.run_install("apply")

    def test_board_pinout_change_refused(self):
        (self.tree / "board.h").write_text("different UART order\n")
        with self.assertRaisesRegex(ValueError, "Board definition"):
            self.run_install("apply")

    def test_installed_policy_tampering_and_missing_file_refused(self):
        self.run_install("apply")
        (self.tree / "policy.h").write_text("bypass policy\n")
        with self.assertRaisesRegex(ValueError, "differs or is missing"):
            self.run_install("verify")
        (self.tree / "policy.h").unlink()
        with self.assertRaisesRegex(ValueError, "differs or is missing"):
            self.run_install("verify")

    def test_no_overwrite_or_symlink_escape(self):
        target = self.tree / "policy.h"
        target.write_text("existing file\n")
        with self.assertRaisesRegex(ValueError, "overwrite"):
            self.run_install("apply")
        target.unlink()
        target.symlink_to(self.assets / "policy.h")
        with self.assertRaisesRegex(ValueError, "Symlink"):
            self.run_install("apply")

    def test_verify_requires_installed_integration(self):
        with self.assertRaisesRegex(ValueError, "not installed"):
            self.run_install("verify")


if __name__ == "__main__":
    unittest.main()
