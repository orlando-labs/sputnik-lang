# MNIST in the native macOS notebook

Implemented on 2026-10-02; final native UI/regression recheck on 2026-10-03.
This is real Amber/LibTorch training,
not simulated progress or prerecorded plots. No Python is involved in training,
native package preparation or the MNIST verifier.

## Open the prepared project

The local build produces `build/Amber Notebook.app` and
`build/MNIST-Showcase.amberbook`. Open the project in that app, choose **Run All**
on the sheet (or **Run Sheet** on *Training dashboard*), then **Trust and Run**.
Opening/previewing the document does not execute code or load native libraries.
Native projects automatically use the isolated helper even when opened in Finder.

The settings cell controls `device`, `epochs`, `batch_size`, dataset limits and
`plot_throttle_ms`. These are ordinary notebook locals. Changing them does not
silently restart training. The training module imports the original MNIST reader,
SmallResNet, optimizer and scheduler from the linked ambertorch directory.
Neither ambertorch nor amber-plot sources are copied into project modules.

The sheet contains editable rich-text headings and interpolated result fields.
The board shows the same cell's progress and two clickable figure panels:
training cross-entropy and held-out accuracy. The existing lightbox offers zoom,
save and share. Outputs are runtime state, not saved checkpoints: reopening the
project requires an explicit run. Training does not write a model checkpoint.

## Rebuild from sibling repositories

From the Amber workspace, with LibTorch and package build prerequisites installed:

```sh
cd ambertorch
./tools/fetch-mnist
./tools/build examples/showcase.am --prepare-notebook
cd ../amber-lang
make -j2 notebook-macos build/notebook-mnist
./build/notebook-mnist prepare ../ambertorch ../amber-plot build/MNIST-Showcase.amberbook
open -a "$PWD/build/Amber Notebook.app" "$PWD/build/MNIST-Showcase.amberbook"
```

Preparation refuses to overwrite an existing project. Use a new output path
when regenerating. Add `--smoke` for 1 epoch, 1,024 train / 256 test images.
The full showcase uses all 60,000 / 10,000 images, five epochs and MPS.
Dataset download is explicit, separate from build/run. No automatic CPU fallback:
select `device = "cpu"` yourself if MPS is unavailable.

The generated dependency declares a root-relative build manifest and prepared
native library, plus its SHA-256. The worker verifies root containment, artifact
size, digest and extension ABI before loading. A changed already-loaded library
requires a worker restart. Native trust grants FFI and file reads; native code
is **not sandboxed** and may access anything the user process can access.
This is local prepared-artifact loading, not an automatic arbitrary-package builder.
The hash detects changed build artifacts; it is not race-free code attestation:
do not replace a library concurrently with loading (hashing and `dlopen` are
separate operations). Only load native code from trusted directories.

## Live output contract

```amber
from progressbar import ProgressBar

pb = ProgressBar(total: steps, desc: "Training", throttle: 0.1, id: "training")
pb.inc!()
notebook.show(plot, id: "loss", live: true, caption: "Loss", order: 0,
              throttle_ms: 500)
# Always flush the final plot explicitly, bypassing its throttle.
notebook.show(final_plot, id: "loss", live: true, throttle_ms: 0)
pb.end
```

`throttle` is seconds; `throttle_ms` is milliseconds. Stable IDs replace panels.
Plot throttling happens **before PNG rendering**. Coalesced, bounded live messages
carry copied numbers/text/PNG bytes, never heap objects. The main thread renders
them while the execution queue is busy. Stop retains the last count/frame; a
partial count is not changed to 100%. Successful final frames also enter the
ordinary cell figure batch. Progress is deliberately nontransactional telemetry.

`array.with_progress(desc: ..., throttle: ...).each |item|: ...` is supported
after importing progressbar. The package also supplies `with_progress(source, ...)`
for custom/lazy sources and stderr rendering outside notebooks. Since `inc!`
has an optional argument, Amber's current syntax requires `inc!()` rather than
the bare optional-argument call. `end` can be called bare.

## Verification

```sh
make test-notebook-progress test-notebook-worker
PYTORCH_ENABLE_MPS_FALLBACK=0 ./build/notebook-mnist verify "$PWD/build/amber-notebook-worker" build/MNIST-Showcase.amberbook
PYTORCH_ENABLE_MPS_FALLBACK=0 ./build/notebook-mnist verify "$PWD/build/amber-notebook-worker" build/MNIST-Showcase.amberbook --stop
PYTORCH_ENABLE_MPS_FALLBACK=0 ./build/notebook-mnist verify "$PWD/build/amber-notebook-worker" build/MNIST-Showcase.amberbook --force-stop
```

Native AppKit/SwiftUI end-to-end harness (explicit test-only trust):

```sh
PYTORCH_ENABLE_MPS_FALLBACK=0 'build/Amber Notebook.app/Contents/MacOS/AmberNotebook' \
  --trust-native --showcase-test build/MNIST-Showcase.amberbook \
  --live-snapshot-png /tmp/mnist-live.png --snapshot-png /tmp/mnist-final.png
```

Verified on macOS arm64, Homebrew LibTorch 2.14.0, MPS with CPU fallback disabled:

- 19,706 parameters, 2,345 optimizer steps, 5 full epochs.
- Test accuracy **98.69%**, test cross-entropy **0.0445108**.
- Worker verification: about **47.65 s**, 407 live snapshots and 93 distinct
  figure images (loss plus accuracy); native GUI runs: about **49–52 s**,
  90 observed changing loss frames and a real intermediate progress count.
- Cooperative cancellation and confirmed SIGKILL exit during MPS training.
- Binder/emitter/VM regressions, actual progressbar package hook and CLI receiver,
  collection wrapper, bounded replacement/throttling, transport and host tests.

Timings depend on hardware/load and include presentation overhead. The local
app bundles Homebrew libraries requiring macOS 26; this is reflected in its
Info.plist, despite the source compilation target being macOS 14.
Local verification artifacts are in `outputs/notebook-mnist/`: `live.png`,
`final.png`, `full-training.log` and `gui-verification.log`.

## Boundaries

Force Stop kills the isolated process, not a macOS kernel/GPU driver. Arbitrary
native helper process trees/device-driver recovery remain separate work.
The worker still rejects project inputs and module-tab Run; use saved module
functions invoked by a sheet. Idle runtime watch-event pumping and persistent
background-job/periodic scheduling are not implemented by this showcase.
Ordinary non-native projects retain the in-process backend unless launched with
`--isolated-worker`. Progress/live convenience APIs currently target the VM,
not standalone native code generation. The app build is ad-hoc signed, not notarized.
