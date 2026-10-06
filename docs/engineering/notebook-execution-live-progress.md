# Notebook execution, cancellation, live output and progress

Status: agreed direction; implementation proceeds in independently tested stages.
This document is a design contract, not a claim that all APIs below exist.

## Implemented slices (2026-09-27–2026-10-02)

Isolated execution is opt-in for ordinary projects and automatic for macOS
projects declaring prepared native dependencies. iamber has not switched:

- `tools/notebook-worker/`: reusable POSIX process supervisor and a single-sheet
  `amber-notebook-worker` executable. `posix_spawn` starts a fresh runtime; it
  does not fork a live VM. Load is inert; Source, Run and Apply operate on a
  private Session. The worker has no project-save command.
- Versioned, length-prefixed, binary-safe data frames (8 MiB maximum), a separate
  nonblocking datagram Stop channel, and generation-qualified request handles.
  Force Stop is qualified by generation too; stale callbacks cannot signal a
  restarted worker. Exit is confirmed/reaped with `waitpid`, not inferred from
  successful `kill`. Parent data-channel loss terminates the worker.
- A shared run cancellation scope propagates through task bodies, resumable
  VMs, detached block invokers and blocking FFI dispatch. VM polling precedes
  quickened instruction dispatch. Host cancellation bypasses rescue and runs
  ensure; cleanup checkpoints are masked. Kernel publication rejects cancelled
  output/dependency transactions, and the Session does not start later cells.
- The compiler now emits a direct ensure entry alongside rescue for combined
  `try/rescue/ensure`. Ordinary exceptions still prefer rescue. Recompile old
  bytecode to obtain this cleanup path for non-rescuable unwinds.
- Run-scoped task membership now tracks runtime scheduler tasks independently
  of task handles, including queued/parked tasks, nested tasks and tasks in
  different schedulers. Membership ends after executable captures retire, not
  merely when a terminal task status is visible. Small `std::function` moves
  are explicitly emptied so retained handles cannot keep completed work alive.
- Stop wakes timer/IO parks, including the race before `park_current` publishes
  its park. Reactor callbacks retain cancellation-flag owners through completion.
  Cleanup may suspend without immediately receiving the same cancellation again.
  A pending blocking FFI call remains pending until it actually returns; it is
  not repeatedly rescheduled or incorrectly reported as completed.
- Subprocess callbacks carry the run context and cancellation-flag owners across
  host threads. Detached callback invocations participate in draining; `ensure`
  may perform a short subprocess operation, including its own callbacks, after
  Stop. Native process-task drivers wait for their host stack to return instead
  of spinning on a cancelled park.
- Protocol v2 adds `Draining`: root execution returned, but registered child
  work remains. Result/Error is delayed until this work retires; Stop/Force Stop
  remain available throughout drain. Ordinary runs join their descendants;
  independent background-job lifetime is not silently inferred from `task.spawn`.
  A drained run is sealed against late detached VM-block invocations.
- Protocol v3 replaces diagnostic-only results with typed presentation snapshots:
  locals/interpolation text, watch metadata, diagnostic ranges, separate stdout/
  stderr events with source/order metadata, environment output and ordered PNG
  panels/captions. Optional built-in plot renderers are shared by the native host
  and worker; a linked amber-plot package is rendered inside the worker process.
- Snapshots use 1 MiB chunks, a 128 MiB aggregate limit and a versioned binary
  schema; the existing 8 MiB frame limit remains. The host assembler publishes
  only after complete decoding and a matching Result; malformed/stale/out-of-order
  transfers poison the assembler. Image header/dimension/size and collection
  limits are validated without decompressing images in the transport.
- Session exposes an owner-thread output-sink hook. Worker runs retain those
  sinks until drain, merge late child/cleanup/module output exactly once, then
  close them. Resumable/detached VM invocations now restore source-location
  providers on their current thread. This is final output capture, not live
  streaming. No worker callback retains a mutable host Session or UI pointer.
- Run-local failure receipts are recorded at scheduler terminal publication,
  before executable captures retire. Dropped handles and failures on different
  schedulers are retained. A structured failure propagated to its parent shares
  the original receipt rather than creating a duplicate diagnostic.
- `wait`/`result`/explicit C++ `failure()` retrieval of a terminal failure marks
  it observed. Status snapshots, predicates, timeouts and not-ready polls do not.
  This is delivery/ownership, not an assertion that a caller rescued it: an
  unrescued wait still fails its caller/root normally. Existing structured
  supervision/cancellation semantics are unchanged.
- Protocol v4 / presentation schema v2 carries `unobserved_task_failures` with
  run-local IDs, error names/messages and spawn source locations. The immutable
  snapshot is taken after drain. Cancellation remains separate and does not
  erase a real cleanup/native failure. IDs are not scheduler task IDs; source
  locations identify spawning code, not a new exception backtrace.
- Protocol v5 adds inert `Sync(projectJSON, environmentStale: 0|1)` for a host's
  unsaved sheet source/order/structure, preserving runtime state. It never saves
  the payload or executes code. Cell revisions become the Sync request ID.
- Native `NotebookExecution` owns one lazy worker per sheet and merges only a
  fully validated presentation (IDs/order/source revisions must match). Source,
  formatting, Watch mode, selection and document dirtiness remain host-owned.
  Main-thread drafts are overlaid independently and stay editable during a run.
  Status-only polling exists only while an explicit worker command is active.
- `--isolated-worker` enables Stop / Force Stop / explicit Restart Worker in the
  packaged macOS app. The control handle has independent shared ownership and
  never touches the host Session from the main thread. Final publication is
  serialized against Force Stop; interrupted worlds require explicit restart.
  Last results/drafts survive; no process restart or cell replay happens implicitly.

Build and run the focused checks:

```sh
make test-notebook-worker
# macOS desktop session, including packaged helper and rendered view/plots:
make test-notebook-worker-ui
```

The supervisor API is internal C++; this executable is not a new interactive
CLI. Initial command schemas (all carry generation and monotonically increasing
request ID): `Load(path, sheetID)`, `Source(cellID, source, watch: 0|1)`,
`Sync(projectJSON, environmentStale: 0|1)`, `Run(cellID, forceAll: 0|1)`,
`Apply(forceAll: 0|1)`, `Shutdown()`.
Execution emits Started, optionally Draining (one pending-registration count),
then SnapshotBegin/zero-based-offset SnapshotChunk frames/Result, or Error.
Load, Source and Sync also return a presentation; Shutdown has an empty Result only.
Each transfer is bound to its generation/request. Cell source revisions are the
last acknowledged Source/Sync request ID (0 for loaded source). The native host
validates this revision and preserves any newer editor draft separately. The
presentation intentionally excludes document source, rich-text formatting,
selection and input values: receiving one cannot overwrite unsaved edits.
Result means root command and registered lifetimes returned, **not** that all
children succeeded. On Run/Apply, check root diagnostics, cancellation and
`unobserved_task_failures`; never infer global success from one cell's `ok`.
Load/Source/Sync acknowledgements are not new execution outcomes and must not clear
the host's last-run outcome. No heap values cross this API. Discard incomplete snapshots
on EOF, Force Stop, or abandoned receive; never publish a partial figure.

Current limits before switching real hosts:

- Ordinary non-native UI projects and the TUI retain the in-process runtime. The
  preview supports sheet Run/Run All/Apply only, explicitly rejects module Run
  and projects with inputs, and does not service idle runtime watch events.
  These paths must move before changing the default backend. The preview does
  not fall back to a local VM. Cross-sheet Apply invalidation is conservative;
  source/structure Sync currently uses a single bounded command frame.
- Completion covers registered runtime tasks/VM invocations, not arbitrary OS
  threads secretly created by native extensions. Such extensions need an explicit
  lifetime/cancellation contract. This is not yet a production job supervisor.
  External helper processes and device operations require their own tracking.
- This first drain implementation joins successful descendants naturally.
  A child that never finishes keeps the run draining until Stop/Force Stop.
  Root cell values already committed before a later Stop are not rolled back.
  Late child text and unobserved scheduler failures are included at drain.
  Failure reporting does not retroactively roll back a successful root or module
  Apply, or rewrite its committed slots. No new fail-fast sibling policy is
  imposed by the host. Failure receipts/text currently have no run-wide quota.
- Force Stop tests use an uncooperative native fixture and real MPS training;
  they are not GPU-driver recovery tests. Helper process-tree tracking remains
  pending. Process isolation is a failure boundary, not a security sandbox.
- IPC is bounded; buffered runtime text, VM memory and user/native allocations
  are not yet governed by a run-wide quota. Live telemetry has bounded,
  coalesced replacement storage. Timed-out/invalid data channels are poisoned, never
  retried as if a partial message had not been written.
- The independently importable progressbar package, stderr/native receivers,
  throttled live plots and plot-bound board updates are implemented for VM
  execution. Protocol v6 / presentation v3 adds separate live transfer frames;
  they do not commit slots or overwrite editor drafts. See the runnable
  [MNIST showcase](notebook-mnist-showcase.md) for APIs and verification.

Focused tests cover CPU loops, module initialization, suspending ensure, timer
and loopback-socket IO cancellation, subprocess callbacks and cleanup,
Stop-before-park, cross-scheduler descendants,
dropped handles, concurrent spawn/Stop, early/stale controls, natural drain and
Force Stop of an uncooperative native child after root return. The production
native task driver is also tested for run-context/flag ownership and for waiting
on an active host stack without a cancellation wake loop. Loopback tests
require a test environment that permits local listening sockets.

Presentation tests cover >8 MiB binary transfer, full metadata round-trips,
truncated/duplicate/stale frames, invalid counts/booleans/PNG dimensions, late
child cleanup and environment text, source revisions and compile diagnostics.
When amber-plot is available on macOS, the same target also checks real worker
rendering, ordered captions and preservation of the previous figure after failure.

Failure tests cover late cell/module errors, dropped handles, cross-scheduler
IDs, structured propagation without duplication, status inspection vs explicit
retrieval, not-ready polls/timeouts, Amber rescue and unrescued waits, and native
stack failures during Stop. Subsequent runs cannot inherit old failure lists.

Native host/model tests also cover Stop/Force Stop, explicit inert restart,
source-only opening, unsaved structure synchronization, preserved last results,
control lifetime after host destruction and newer/reverted drafts during runs.
Apply tests cover unsaved-module rejection, independent sheet workers, stale
cross-sheet environments and cancellation during module initialization. GUI
smoke tests render rich text, interpolation and real amber-plot panels through
the packaged helper as well as the unchanged default backend.

Next: worker runtime-event pumping, board input bridge and isolated module Run,
then general default-backend rollout and long-lived background jobs. Do not move
document Save into the worker.

## Execution and isolation

Every ordinary cell may run for an arbitrarily long time. No annotation, special
cell kind, `live: true`, or manual `task.spawn` is required to make it stoppable.
A background job has a different lifetime, not a monopoly on cancellation.

The document, editor drafts, UI and execution supervisor stay in the host.
Each active sheet runtime owns a separate, lazily started worker process. Its
VM, heap, native libraries, model/device state and plot rendering live inside
that worker. Cells in a sheet share a runtime; do not spawn one process per cell.
Jobs sharing its live objects share that worker's failure boundary. Independently
isolated jobs can later get their own workers with explicit input/output transfer.

Module functions contain reusable training/action implementations. A cell or
board button explicitly invokes one. Module opening never runs initialization;
initialization during Apply must itself be stoppable. Changing an input must not
silently restart training. Initial parameters are snapshots; live parameter
changes need an explicit command contract.

Each worker generation and run has an identity. A run owns its cancellation
scope and child tasks; logical Amber tasks may migrate between native threads.
No borrowed heap Values, UI pointers or Session references cross IPC.

### Stop and Force Stop

- Stop requests cooperative cancellation through a separate control channel,
  never queued behind execution or blocked on the world execution mutex.
- VM checkpoints include tight loops and quickened instruction paths; blocking
  operations/native extensions observe a cancellation token where supported.
  Normal cancellation unwinds Amber `ensure` and task-local scopes.
- UI states distinguish running, stopping, cancelled, failed and completed.
  The control acknowledgement is not evidence that execution has stopped.
- Force Stop kills the exact owned worker independently of VM/native progress.
  The host confirms process exit; until then it remains stopping. A configurable
  grace period can offer escalation. Forced termination cannot execute `ensure`.
- Terminating the worker is not a guarantee of resetting a wedged GPU/driver.
  Device recovery is backend-specific, can affect other applications, and must
  be reported separately. An unkillable OS process must not be labelled stopped.
- A full notebook host must track helper child processes and shutdown policy;
  killing a worker does not automatically prove all external work has ceased.

Uncommitted cell outputs are discarded on cancellation/failure. The last
successful outputs remain snapshots, explicitly stale after losing their world.
This is not rollback of shared object mutation, file writes or external effects.
After Force Stop the model is gone; resuming training requires an explicit
checkpoint. Restart creates a clean worker, never implicitly replays user code.
Old-generation replies and live updates cannot overwrite the current run.

## Shared live-output transport

Output types initially are progress, figure and metrics. They share identity,
lifetime, bounded transport and subscriptions, not a single generic payload.
Envelope: protocol version, worker generation, RunId, source cell/action,
output instance ID, optional stable binding key, type, sequence number.
Never bind a board to a description or a panel index.

Live output is telemetry, independent of the cell's atomic final slot commit.
It must not rerun the notebook dependency graph on every update. UI consumers
receive immutable snapshots. No concurrent UI reads of a mutable plot/model.
Limits apply to event size, open outputs and retained data. Non-critical updates
coalesce; lifecycle events cannot silently disappear. Cancellation/Force Stop
must remain usable even if the data channel is saturated.

### Figures

Proposed extension (not implemented yet):

```amber
notebook.show(p, id: "loss", caption: "Training loss", live: true, throttle_ms: 200)
```

Same ID replaces one panel; different IDs are panels a), b), c). `order` is only
presentation order. Throttle before expensive PNG rendering, not merely before
UI repaint. Keep at most one pending latest snapshot and deliver a trailing/final
frame. The board can set its display interval; hidden views stop rendering, not
training. Multiple subscribers must not trigger duplicate renders unnecessarily.
Save/share exports the displayed immutable frame. Lightbox follow/freeze behavior
must be explicit. On cancellation retain the last live frame labelled cancelled;
on forced termination mark it interrupted, never successfully completed.

Loss metrics should be numeric samples rather than repeated full image messages.
Dropping render frames must not drop metric history silently. Bounded history,
sampling/aggregation and display downsampling are separate policies.

## Independent `progressbar` package

Desired public syntax:

```amber
from progressbar import ProgressBar
pb = ProgressBar(total: 100500, desc: "Training", throttle: 0.2)
pb.inc!() # optional argument: current Amber requires parentheses
pb.inc!(5)
pb.end

some_array.with_progress(desc: "Processing", throttle: 0.2).each |x|:
  process(x)
```

`throttle` is an explicitly documented interval in seconds (0.2 = 200 ms), not
a number of iterations. Internal counts update on every increment. Validate
nonnegative finite intervals/counts and overflow. `total: null` means unknown
size: count/rate/spinner, no fictional percentage/ETA. End is idempotent and
does not falsify the counter by assigning total. A scoped/block form provides
automatic normal/error/cancel completion. Forgotten bars are closed by their
owning run when possible, and by the host on process death.

`with_progress` is a lazy iteration adapter, not a copied collection. Known
sizes are cheap metadata; never exhaust a lazy/infinite source to count it.
Count successful block completions. Preserve iteration/block return behavior;
break, exception and cancellation do not become successful 100% completion.
Parallel variants count completed, not submitted work and need thread-safe
updates under Amber ownership rules. New traversals create new bar instances.
Collection convenience methods delegate to one adapter implementation.

The package emits structured absolute-state events (not lossy `+1` deltas).
Payload: current, total, description, unit, monotonic timing and status. Rate/ETA
are estimates; omit ETA when unknowable. Start/end/error/cancel bypass update
throttling, and final state is always flushed. Nested bars have parent IDs, but
unrelated progress units are not automatically added together.

An explicit receiver or a scoped task-local receiver can be installed. The
notebook installs its receiver automatically; ordinary modules do not import
the notebook to report progress. Child tasks inherit a safe run-scoped sink,
not OS-thread-local ownership or raw UI pointers. Optional stable `id` is used
for board binding; generated instance IDs distinguish runs/repeated bars.

Receivers:

- ordinary TTY: tqdm-like stderr rendering, coordinated with concurrent text;
- redirected output: sparse plain lines, no ANSI cursor controls;
- iamber: render within curses, never overwrite it with terminal escape codes;
- native notebook: inline bars beside the running cell;
- board: named progress components subscribing to the same events;
- custom receiver: same event contract without UI dependency.

The host synthesizes aborted states for open outputs after Force Stop. Progress
reporting is not a cancellation checkpoint requirement: Stop works even when user
code never calls `inc!`. Telemetry failure/backpressure must not deadlock Stop.

## Implementation sequence and acceptance checks

1. Host-neutral single-sheet worker, bounded versioned IPC and process supervisor;
   CPU cancellation, forced native hang termination, clean restart, no project
   writes from a worker. Tests use isolated fixtures and bounded timeouts.
2. Integrate both hosts without moving document/editor ownership into the worker;
   Stop/Force Stop, preserve drafts and last snapshots, explicit restart/re-run,
   environment Apply and module actions, structured task ownership.
3. Shared live event transport and progress lifecycle; independent progressbar
   package and console receiver; collection adapter; cancellation/exit events.
4. Inline/native/curses/board progress subscribers; live figure IDs and throttle;
   metrics retention/downsampling and hidden-view policy.
5. Background action jobs and Start/Stop/Progress bindings; checkpoint workflow;
   device/native backend cancellation hooks and backend-specific recovery.

Regression requirements include CPU infinite loops with no yields; ensure cleanup;
native calls ignoring cancellation; independent responsive host; stale controls
not cancelling the next run; unexpected EOF; oversized/truncated messages; no
zombies; final frame/progress delivery; bounded queues; concurrent/nested bars;
Manual cell barriers; failed runs never publishing new slots or dependencies.
