#!/usr/bin/env python3
"""A hanging fixture must fail without hiding the remaining backend results."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "backend_equivalence", Path(__file__).resolve().parents[1] / "tools/backend_equivalence.py"
)
backend = importlib.util.module_from_spec(spec)
spec.loader.exec_module(backend)
compiler = Path(sys.argv[1] if len(sys.argv) > 1 else backend.SPUTNIK).resolve()


class TimeoutRecoveryTest(unittest.TestCase):
    def test_timeout_fails_and_next_fixture_runs(self):
        with tempfile.TemporaryDirectory(prefix="sputnik-backend-runner-") as temporary:
            root = Path(temporary)
            for name, source in (("a_timeout", "while true:\n  1\n"), ("b_value", "42\n")):
                fixture = root / name
                fixture.mkdir()
                (fixture / "source.s").write_text(source)
                (fixture / "meta.json").write_text(json.dumps(
                    {"phase": "run", "source": "source.s", "entry": "init"}))
            original_capture = backend.run_capture

            def capture(command, timeout=60):
                if "run" in command and any(str(arg).endswith("a_timeout.s") for arg in command):
                    timeout = 0.2
                return original_capture(command, timeout)

            output, errors = io.StringIO(), io.StringIO()
            with patch.object(backend, "CORPUS", root), \
                 patch.object(backend, "WORK_DIR", root / "output"), \
                 patch.object(backend, "SPUTNIK", compiler), \
                 patch.object(backend, "run_capture", capture), \
                 contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
                result = backend.main()
            self.assertEqual(result, 1)
            self.assertIn("a_timeout: vm timed out", errors.getvalue())
            self.assertIn("1 passed, 1 failed", output.getvalue())
            self.assertTrue((root / "output/b_value.native").is_file())


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
