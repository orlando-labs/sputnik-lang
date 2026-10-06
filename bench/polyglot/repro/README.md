# Polyglot VM repros

`direct_method_capture_failure.s` is a small reproducer for direct execution
of a compiled top-level method whose body captures a sibling top-level helper.

Expected good path:

```sh
build/isputnik --eval-file bench/polyglot/repro/direct_method_capture_failure.s
```

Expected output:

```text
=> 42
```

Compile it and run module init:

```sh
python3 bench/polyglot/run_benchmark.py \
  --workload arithmetic \
  --repeats 1 \
  --build-dir /private/tmp/direct_method_capture_repro_runner
build/sputnik build bench/polyglot/repro/sputnik.build.json \
  --out-dir /private/tmp/direct_method_capture_repro/out \
  --cache-dir /private/tmp/direct_method_capture_repro/cache
/private/tmp/direct_method_capture_repro_runner/sputnikbc_run \
  /private/tmp/direct_method_capture_repro/out/bench.polyglot.repro.direct_method_capture_failure.sputnikbc \
  __init__
```

Expected output:

```text
42
```

Direct method entry path (now fixed):

```sh
/private/tmp/direct_method_capture_repro_runner/sputnikbc_run \
  /private/tmp/direct_method_capture_repro/out/bench.polyglot.repro.direct_method_capture_failure.sputnikbc \
  main
```

Expected output:

```text
42
```

Historically this faulted with `VMError: capture slot out of range` because the
direct method entry has capture metadata for the sibling helper, but
`execute_code(... method.entry_code_id ...)` started the code object without the
closure captures materialized by module initialization. `Vm::execute` now calls
`prepare_direct_entry_captures`, which runs module init on demand and binds the
captured siblings before the entry runs (`runtime/vm.cpp`).

This case is locked in as a C++ regression test:
`test_direct_entry_materializes_sibling_captures` in `tests/vm_tests.cpp`.
