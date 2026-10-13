# Profile-guided VM and native builds

`tools/profile-build` builds an optional Clang PGO configuration without
changing the normal `make` defaults. It requires Clang, its matching
`llvm-profdata`, and a linker with ThinLTO support. Python is not required.
Verified toolchains: Apple clang 21.0.0 and Homebrew LLVM 22.1.8 on arm64 macOS.

Every invocation requires a **new output directory**. Make does not normally
recompile existing objects when `CXXFLAGS` changes. Separate directories avoid
mixing instrumented, ordinary and profile-optimized objects. Optimization copies
the input profile into the new directory and makes that copy read-only.
Do not edit generated profiles or compiler wrappers in place.

## VM

Run from the Sputnik repository:

```sh
tools/profile-build vm-instrument build/pgo-training
build/pgo-training/sputnik run bench/vm/task_local_get.s
build/pgo-training/sputnik run bench/polyglot/sputnik/src/calls_collections.s
# Also run representative applications through build/pgo-training/sputnik.
llvm-profdata merge -o build/vm.profdata build/pgo-training/raw/*.profraw
tools/profile-build vm-optimize build/pgo-release build/vm.profdata
```

On macOS, `xcrun llvm-profdata` selects the Command Line Tools version. `CXX`
can select another Clang executable and `JOBS` controls make parallelism
(default 3). VM builds prefer `llvm-ar` beside the selected compiler; `AR`
overrides this choice. This avoids mixing newer LLVM ThinLTO bitcode with an
older system archiver. VM build paths must use letters, digits, `_`, `.`, `/` or `-`
because make target syntax does not preserve arbitrary filenames.

Instrumentation uses O2; optimized builds use O3, ThinLTO, NDEBUG and the
supplied profile. The script builds and runs `vm_tests` and `sputnik_ext_tests`
before reporting an optimized executable. Compiler output, flags, version,
source root and test logs are retained in the output directory. A CFG/profile
mismatch is a compilation error. Unprofiled functions retain the compiler's
ordinary optimization path and may emit warnings.
The two test harness translation units use O2 without PGO and link against the
same optimized runtime archive as the CLI. Their `main` functions are unrelated
to the CLI entry point in the training profile; applying that profile to them
would produce a false CFG mismatch. Runtime/compiler units keep the strict
profile check.

Keep the source checkout and compiler fixed between generation and use.
Train on representative inputs; validate correctness and timings on different
inputs. Instrumented timings are not performance results. PGO can regress
workloads absent from the profile, and training weights can change results.

For sputniktorch, prepare its manifest with the normal `tools/build` workflow,
then run the instrumented VM on that manifest with its usual FFI/filesystem
grants. Use actual MPS with CPU fallback disabled when profiling MPS. Keep the
LibTorch version, thread counts and numerical validation identical across the
ordinary VM, optimized VM, full-native target and Python reference.

## Built targets

Native modes produce a compiler wrapper; invoke the ordinary Sputnik native
builder with it. Preserve the same source checkout, manifest and output path
between instrumented and optimized compilation, including the generated C++
filename (Clang uses source names to identify internal functions).

```sh
tools/profile-build native-instrument build/native-training
SPUTNIK_NATIVE_CXX="$PWD/build/native-training/cxx" \
  build/sputnik build path/to/app.build.json --target native \
  --out-dir build/app --require-full-native
# Run build/app/<target> on representative inputs.
llvm-profdata merge -o build/native.profdata build/native-training/raw/*.profraw
tools/profile-build native-optimize build/native-release build/native.profdata
SPUTNIK_NATIVE_CXX="$PWD/build/native-release/cxx" \
  build/sputnik build path/to/app.build.json --target native \
  --out-dir build/app --require-full-native
```

Add the application's required `--grant` arguments to both build commands.
The instrumenting wrapper preserves the builder's optimization flags;
the optimizing wrapper applies O3/ThinLTO/PGO to the generated program, runtime
archive and extension units. Each fresh wrapper has a distinct absolute path,
which is part of Sputnik's runtime-archive cache key. Replacing a profile inside
an existing wrapper directory would defeat that separation and is unsupported.
Check `native_graph_full_coverage`, `native_bytecode_fallback` and executable
hashes in the build result, then run the application's own checks and benchmark.

The MNIST investigation and measured limits of this configuration are recorded
in [sputniktorch stage 5](../../../sputniktorch/docs/mnist-runtime-stage5.md).
