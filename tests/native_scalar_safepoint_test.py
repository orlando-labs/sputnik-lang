"""Scalar safepoint effect proof: admission, fallback cases and overflow trace."""
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
LOOP = "  i = 0\n  total = 0\n  while i < 1000:\n    total += i\n    i += 1\n  total\n"
CASES = {
    "scalar": ("def main():\n" + LOOP, True, "499500\n"),
    "unknown_argument": ("def nonleaf(limit):\n" + LOOP.replace("i < 1000", "i < limit") + "\ndef main():\n  nonleaf(1000)\n", False, "499500\n"),
    "heap": ("def main():\n" + LOOP.replace("    total += i", "    values = [i]\n    total += values[0]"), False, "499500\n"),
    "capture": ("limit = 1000\ndef main():\n" + LOOP.replace("i < 1000", "i < limit"), False, "499500\n"),
    "handler": ("def main():\n  try:\n" + "".join("  " + line for line in LOOP.splitlines(True)) + "  rescue ValueError:\n    -1\n", False, "499500\n"),
    "heap_argument": ("def leaf(ignored):\n" + LOOP + "\ndef main():\n  worker = task.spawn:\n    payload = [7]\n    leaf(payload) + payload[0]\n  worker.wait()\n", True, "499507\n"),
    "overflow": ("def main():\n  i = 0\n  value = 9223372036854775807\n  while i < 2:\n    value += 1\n    i += 1\n  value\n", True, None),
}

def main():
    compiler = Path(sys.argv[1]).resolve() if len(sys.argv)>1 else ROOT / "build/sputnik"
    out = Path(sys.argv[2]).resolve() if len(sys.argv)>2 else ROOT / "build/native-scalar-safepoint"
    out.mkdir(parents=True,exist_ok=True)
    for name,(body,selected,expected) in CASES.items():
        source=out / (name+".s");source.write_text("package test.scalar_"+name+"\nexport main\n\n"+body+"\nmain()\n")
        binary=out / name
        built=subprocess.run([str(compiler),"build",str(source),"--entry","init","--require-full-native","-o",str(binary)],cwd=ROOT,text=True,capture_output=True,timeout=240)
        if built.returncode: raise RuntimeError(built.stderr+built.stdout)
        result=json.loads(built.stdout)
        if result.get("native_full_coverage") is not True or result.get("bytecode_fallback") is not False: raise RuntimeError("incomplete native coverage")
        cpp=binary.with_suffix(".native.cpp").read_text()
        has_proof="// Proven scalar leaf:" in cpp
        if has_proof!=selected: raise RuntimeError(f"{name}: unexpected effect-proof selection {has_proof}")
        if selected and "if (scalar_leaf_cycle_polls) native_cycle_checkpoint_if_due();" not in cpp:
            raise RuntimeError("nonempty-argument invocation lost its back-edge checkpoint")
        ran=subprocess.run([str(binary)],cwd=ROOT,text=True,capture_output=True,timeout=30)
        if expected is None:
            if ran.returncode==0 or "OverflowError" not in ran.stderr or "at c" not in ran.stderr:
                raise RuntimeError("checked overflow must unwind with its source trace")
        elif ran.returncode or ran.stdout!=expected:
            raise RuntimeError(f"{name}: {ran.returncode} {ran.stdout!r} {ran.stderr}")
        print("native scalar safepoint: " + name + " ok",flush=True)

if __name__=="__main__": main()
