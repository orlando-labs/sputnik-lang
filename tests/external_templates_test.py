#!/usr/bin/env python3
"""External source formats: execution, immutable build inputs, diagnostics/cache."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = Path(sys.argv[1] if len(sys.argv) > 1 else root / "build/sputnik").resolve()
env = dict(os.environ)
env.setdefault("SPUTNIK_NATIVE_RT_CACHE", str(root / "build/interop-runtime-cache"))


def run(*args, ok=True):
    result = subprocess.run([str(compiler), *map(str, args)], cwd=root, env=env,
                            capture_output=True, text=True, timeout=300)
    assert (result.returncode == 0) == ok, (args, result.stdout[-6000:], result.stderr[-6000:])
    return result


with tempfile.TemporaryDirectory(prefix="sputnik-external-") as temporary:
    work = Path(temporary)
    shutil.copytree(root / "tests/fixtures/external_templates", work / "src")
    source = work / "src"
    page = source / "page.text.sv"
    page.write_text(page.read_text().replace("#{title}!\n", "#{title}!  \n"))
    main = source / "main.s"
    main.write_text(main.read_text().replace("Мир!\\n", "Мир!  \\n"))
    manifest = source / "sputnik.build.yaml"
    output = work / "out"
    cache = work / "cache"

    def build(target="bytecode", ok=True):
        args = ["build", manifest, "--target", target, "--out-dir", output, "--cache-dir", cache]
        if target == "native": args.append("--require-full-native")
        return json.loads(run(*args, ok=ok).stdout)

    def artifact(data, name="app.external"):
        return next(item for item in data["artifacts"] if item["name"] == name)

    assert run("run", manifest).stdout == "PASS external templates\ntrue\n"
    first = build()
    second = build()
    assert artifact(second)["cached"]
    assert len(artifact(first)["template_inputs"]) == 5
    body = source / "args.cmd.sv"
    original = body.read_text()
    body.write_text(original + "%# cache input changed\n")
    changed = build()
    assert not artifact(changed)["cached"]
    assert artifact(changed)["cache_key"] != artifact(first)["cache_key"]
    assert artifact(changed)["abi_hash"] != artifact(first)["abi_hash"]
    body.write_text(original)
    assert artifact(build())["cached"]
    hidden = body.with_suffix(".missing")
    body.rename(hidden)
    assert "TEMPLATE_INPUT" in json.dumps(build(ok=False))
    hidden.rename(body)
    provider = source / "text.s"
    provider.write_text(provider.read_text() + "\n# provider changed\n")
    assert not artifact(build())["cached"]

    native = build("native")
    assert native["native_graph_full_coverage"] and native["native_graph_vm_independent"]
    assert not native["native_bytecode_fallback"] and not native["native_graph_runtime_bridge"]
    assert native["native_graph_vm_fallback_code_count"] == native["native_graph_fallback_code_count"] == 0
    # The executable must not open template source files at runtime.
    for path in source.glob("*.sv"): path.rename(path.with_suffix(".hidden"))
    executed = subprocess.run([native["native_output"]], cwd=work, capture_output=True, text=True, timeout=30)
    assert executed.returncode == 0 and executed.stdout == "PASS external templates\ntrue\n", (executed.stdout, executed.stderr)
    for path in source.glob("*.hidden"): path.rename(path.with_suffix(".sv"))

    # Declaration and file errors are rejected before a cached module is used.
    declaration = source / "main.s"
    module_text = declaration.read_text()
    for old, new, code in [
        ('template "./args.cmd.sv"', 'template "./missing.cmd.sv"', "TEMPLATE_INPUT"),
        ('template "./args.cmd.sv"', 'template(path)', "TEMPLATE_PATH"),
        ('def command(values, prefix: "--item=")', 'def command(values, prefix: "--item="): null', "TEMPLATE_DECLARATION"),
        ('template "./args.cmd.sv"', 'template "./args.cmd.sv"\n', "TEMPLATE_DECLARATION"),
    ]:
        declaration.write_text(module_text.replace(old, new, 1))
        failure = build(ok=False)
        assert code in json.dumps(failure, ensure_ascii=False), failure
    declaration.write_text(module_text)

    # Line/column positions refer to the external file, including Unicode.
    for text, code, line in [
        ("echo\n%for value in values:\n  Ж#{missing_name}\n", "E2012", 3),
        ("echo\n#{1 + }\n", "AMB_STRING_INTERP_PARSE_ERROR", 2),
        ("echo\n\\q\n", "AMB_STRING_BAD_ESCAPE", 2),
        ("echo\n%for value in values\n  x\n", "STRING_TEMPLATE_CONTROL", 2),
        ("echo '\n%for value in values:\n  #{value}\n'\n", "CMD_TEMPLATE_CONTROL", None),
    ]:
        body.write_text(text)
        failed = run("run", manifest, ok=False)
        message = failed.stdout + failed.stderr
        assert code in message and str(body.resolve()) in message, message
        if line is not None: assert f'"line":{line}' in message or f':{line}:' in message, message
        if "missing_name" in text: assert '"col":6' in message, message
    body.write_text(original)
    # Manifest validation / selection is explicit and deterministic.
    text = manifest.read_text()
    manifest.write_text(text.replace("suffix: .cmd.sv", "suffix: .unregistered").replace("suffix: .sv", "suffix: .fallback-disabled"))
    assert "TEMPLATE_FORMAT" in json.dumps(build(ok=False))
    manifest.write_text(text.replace("macro: cmd", "macro: command"))
    assert "TEMPLATE_PROVIDER" in json.dumps(build(ok=False))
    manifest.write_text(text.replace("source_formats:", "source_formats:\n  - suffix: .cmd.sv\n    module: system\n    macro: cmd"))
    assert "unique" in json.dumps(build(ok=False))
    manifest.write_text(text)

print("external_templates_test: VM/full-native, source diagnostics and cache invalidation passed")
