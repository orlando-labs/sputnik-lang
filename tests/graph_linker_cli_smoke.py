#!/usr/bin/env python3
"""Exercise the shared linker through sputnik, including its generated cache."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else "build/sputnik").resolve())
    with tempfile.TemporaryDirectory(prefix="sputnik-graph-cli-") as temporary:
        project = Path(temporary)
        # Flush each asynchronous logger before observing module-init order.
        sources = {
            "b": ('logger = io.Logger.new\nlogger.info("order-b")\nlogger.flush\n'
                  'def g(0): 0\ndef g(v) if v > 0: v\nexport g\n'),
            "a": ('logger = io.Logger.new\nlogger.info("order-a")\nlogger.flush\n'
                  'def f(0): 1\ndef f(v) if v > 0: v + 1\nexport f\n'),
            "unused": 'raise "unreachable module initialized"\n',
            "root": (
                "from b import g\nfrom a import f\n"
                'logger = io.Logger.new\nlogger.info("order-root")\nlogger.flush\n'
                "part = 0\ncase [0]:\n"
                "  when [1, v]: part = 99\n"
                "  else: part = 20\n"
                "rest = 0\ncase {other: 3}:\n"
                "  when {wanted:, **null}: rest = wanted\n"
                "  else: rest = 21\n"
                "def main(): f(part) + g(rest)\nexport main\n"
            ),
        }
        for name, source in sources.items():
            (project / f"{name}.s").write_text(f"package {name}\n{source}")
        manifest = project / "sputnik.build.json"
        manifest.write_text(json.dumps({
            "schema": "sputnik.build.v1", "name": "graph-cli", "root": "root",
            "profiles": {"required": ["core.v1"], "optional": [], "forbidden": []},
            "modules": [{"name": name, "path": f"{name}.s"} for name in sources],
        }))
        cache = project / "cache"
        environment = dict(os.environ, SPUTNIK_VM_CACHE=str(cache))

        def run():
            result = subprocess.run([binary, "run", str(manifest)], env=environment,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, timeout=60)
            assert result.returncode == 0, result.stdout
            assert result.stdout.rstrip().endswith("42"), result.stdout
            assert (result.stdout.index("order-b") < result.stdout.index("order-a") <
                    result.stdout.index("order-root")), result.stdout
            assert "unreachable module initialized" not in result.stdout

        run()
        sidecars = list((cache / "graph").glob("*.meta"))
        assert len(sidecars) == 1, sidecars
        sidecar = sidecars[0]
        fields = sidecar.read_text().split()
        assert len(fields) == 4 and fields[3] == "3", fields
        timestamp = sidecar.stat().st_mtime_ns
        run()
        assert sidecar.stat().st_mtime_ns == timestamp, "valid cache should be reused"
        sidecar.write_text("truncated cache")
        run()
        assert sidecar.read_text().split() == fields, "invalid cache should be rebuilt"
    print("graph_linker_cli_smoke ok")


if __name__ == "__main__":
    main()
