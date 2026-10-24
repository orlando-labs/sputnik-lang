#!/usr/bin/env python3
"""Keep documentation checks independent of optional sibling checkouts."""

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "spec_sync", Path(__file__).resolve().parents[1] / "tools/spec_sync.py"
)
spec_sync = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = spec_sync
spec.loader.exec_module(spec_sync)


class LinkScopeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="sputnik-spec-sync-")
        self.addCleanup(temporary.cleanup)
        self.parent = Path(temporary.name).resolve()
        self.root = self.parent / "sputnik-lang"
        self.root.mkdir()
        root_patch = patch.object(spec_sync, "ROOT", self.root)
        root_patch.start()
        self.addCleanup(root_patch.stop)

    def readme(self, target):
        (self.root / "README.md").write_text(f"# Sputnik\n\n[link]({target})\n")

    def test_missing_sibling_is_optional(self):
        self.readme("../sputnik-notebook/README.md")
        self.assertEqual(spec_sync.check_links(), [])

    def test_present_sibling_does_not_change_the_check(self):
        sibling = self.parent / "sputnik-notebook"
        sibling.mkdir()
        (sibling / "README.md").write_text("# Notebook\n")
        self.readme("../sputnik-notebook/README.md#not-a-local-anchor")
        self.assertEqual(spec_sync.check_links(), [])

    def test_missing_internal_target_is_an_error(self):
        self.readme("docs/missing.md")
        self.assertEqual(
            spec_sync.check_links(),
            ["README.md: missing link target docs/missing.md"],
        )

    def test_path_that_reenters_the_repository_is_still_checked(self):
        self.readme("../sputnik-lang/docs/missing.md")
        self.assertEqual(
            spec_sync.check_links(),
            ["README.md: missing link target ../sputnik-lang/docs/missing.md"],
        )

    def test_internal_anchors_are_still_checked(self):
        guide = self.root / "docs/engineering/guide.md"
        guide.parent.mkdir(parents=True)
        guide.write_text("# Guide\n")
        self.readme("docs/engineering/guide.md#missing")
        self.assertEqual(
            spec_sync.check_links(),
            ["README.md: missing anchor #missing in docs/engineering/guide.md"],
        )
        self.readme("docs/engineering/guide.md#guide")
        self.assertEqual(spec_sync.check_links(), [])


if __name__ == "__main__":
    unittest.main()
