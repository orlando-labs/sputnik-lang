#!/usr/bin/env python3
"""Parser, diagnostics, and VM/native parity for scoped break and sequencing."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

cli = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parent.parent

def run(*args, ok=True):
    result = subprocess.run([str(cli), *map(str, args)], capture_output=True, text=True, timeout=300)
    assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
    return result

with tempfile.TemporaryDirectory(prefix="sputnik-language-updates-") as tmp:
    tmp = Path(tmp)
    source = tmp / "probe.s"
    def probe(text, command="parse", ok=True):
        source.write_text(text + "\n")
        return run(command, source, ok=ok)
    for text in ["a() then b()", "x = 1 then y = 2", "(if c then a else b) then finish()",
                 "if c then (a() then b()) else fallback()", "a() then break if invalid?",
                 "obj.then()", "then = 3", "next = 1", "next(2)", "print(next)", "print(then)", "a() then f(xs .map if ready |x|: x)"]:
        probe(text)
    for text in ["if c then a else b then finish()", "if c then a() then b() else fallback()",
                 "a() then if c then b else d", "x = if c then a else b then finish()"]:
        result = probe(text, ok=False)
        assert "ParenthesesError" in result.stderr, result.stderr
    for text in ["a() then b()", "a() then break if invalid?", '"then then then"']:
        assert json.loads(probe(text, "lint").stdout)["diagnostics"] == []
    for text, code in [("(if c then a else b) then finish()", "L1001"),
                       ("if c then (a() then b()) else fallback()", "L1001"),
                       ("a() then b() then c()", "L1002"),
                       ("a() then (b() then c())", "L1002")]:
        warnings = json.loads(probe(text, "lint").stdout)["diagnostics"]
        assert any(d["code"] == code and d["severity"] == "warning" for d in warnings), warnings
    for text in ["[1].each: break", "[1].map: break 2", "[1].map.with_index: break",
                 "[1].for_each: break 2", "for x in [1]: break 2"]:
        result = probe(text, "bc", ok=False)
        assert "break" in result.stderr, result.stderr
    assert "T0005" in probe('export wrong\ndef wrong() -> Int: 1 then "text"', "typed", ok=False).stderr
    ast = json.loads(probe("x = 1 then y = 2").stdout)["items"][0]["expr"]
    assert ast["kind"] == "AstThen" and [item["kind"] for item in ast["items"]] == ["AstAssign", "AstAssign"]
    # Dynamic receiver: error is rescuable, even if the block never executes break.
    source.write_text("xs = [1]\ntry:\n  xs.map: break 2\nrescue LocalJumpError:\n  print 42\n")
    assert run(source).stdout.strip() == "42"
    fixture = root / "tests/fixtures/scoped_break_then/main.s"
    vm = run(fixture)
    assert vm.stdout.strip() == "PASS scoped break and then", vm.stdout
    control = run(root / "tests/fixtures/scoped_break_then/vm_control.s")
    assert control.stdout.strip() == "PASS scoped break replacement", control.stdout
    output = tmp / "native"
    built = run("build", fixture, "--entry", "init", "--require-full-native", "-o", output)
    assert '"native_full_coverage": true' in built.stdout, built.stdout
    native = subprocess.run([str(output)], capture_output=True, text=True, timeout=300)
    assert native.returncode == 0 and native.stdout == vm.stdout, (native.stdout, native.stderr)
print("language_updates_test: ok")
