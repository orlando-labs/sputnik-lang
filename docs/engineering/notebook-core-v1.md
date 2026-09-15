# Amber notebook core v1

## Scope

The notebook core is a UI-independent layer shared by `iamber` and future
native notebook clients. The current slice owns stable cell identity, static
dependency analysis, transition invalidation, transactional binding slots,
cell scheduling, notebook-fragment compilation, and the VM execution boundary.

The terminal UI remains an adapter. It is intentionally not a dependency of
the notebook model, compiler, kernel, or runtime integration.

The common on-disk project format, bundled module environments, persistent
sheet/module tabs and iamber Save/Apply workflow are specified in
[Notebook project v1](notebook-project-v1.md). Regular code cells deliberately
retain their restricted compilation profile; definitions belong in modules.

## Dependency model

- `CellId` is a stable document identity and is independent of cell position.
- A published top-level output is identified by `BindingKey { cell_id, name }`.
- An external read resolves to the nearest preceding cell that writes the same
  name.
- Reads after a write in the same cell are local and do not create a graph
  edge.
- `x = x + 1` reads the preceding version of `x` and publishes a new version;
  it is not a self-cycle.
- Invalidation contains the changed roots and their transitive consumers in a
  stable topological order.
- Transition invalidation unions the old and new graphs, so removing or
  renaming an output still marks its former consumers stale.
- A changed manual cell may run explicitly. A downstream manual cell stays
  stale during automatic propagation and blocks watch consumers that depend on
  it.

Dependency analysis uses the Amber lexer/parser and retains frontend
diagnostics per cell. Exact external-read source spans are preserved for
compiler lowering; a name-only set is insufficient because the ordinary binder
predeclares module assignments.

## Persistent slots and kernel

`NotebookSlotTable` stores the last successful value for every `BindingKey`,
together with initialization/stale state, revision, provider metadata, and a
publication epoch. A copy-on-commit transaction publishes a complete output
batch atomically. Republishing an equal value clears staleness without advancing
the binding revision or emitting a change event.

`NotebookKernel` owns the dependency graph, slot table, dynamic-dependency
snapshots, and serialized execution/update boundary. It validates all inputs
before invoking a backend, validates the backend's exact declared output set,
and commits only a successful complete batch. A failed run leaves previous
published values intact. Short state locks are released while the backend runs,
so a VM safepoint can re-enter `kernel.gc_roots()` without deadlocking.

Graph replacement also has a move-only two-phase form. `prepare_update_cells()`
builds the next graph, dependency indexes, stale-output set, and evaluation plan
without changing published state while retaining the execution lock. A host can
then install its immutable VM image and either cancel safely or call
`commit_update_cells()`, whose checked `noexcept` publication path takes one
slot-table lock, marks existing slots, and swaps complete candidate generations.
iamber uses this boundary so its live VM image, descriptor metadata, graph, and
reverse indexes cannot expose mixed generations if candidate preparation or
image installation fails. If the
post-install kernel commit cannot be acquired, iamber rolls the image back; a
failed rollback discards the live backend so the next evaluation rebuilds it.

## Compiler and bytecode boundary

Notebook cells compile to `CodeKind::NotebookCell`, not to ordinary module
initializers. Two notebook-only instructions form the storage ABI:

- `LoadNotebookSlot(dst, descriptor)` reads an explicit input descriptor;
- `StoreNotebookSlot(descriptor, src)` stages an explicit output descriptor.

The verifier rejects these instructions outside notebook-cell code and rejects
using a notebook cell as a method, closure, handler, export, init, or other
ordinary executable target. Ordinary `RuntimeWorld::execute()` likewise rejects
notebook cells.

`NotebookImage` owns one shared in-memory bytecode image and its constant,
string, symbol, line, and code tables. Descriptors are interned by the full
`BindingKey`; input/output direction belongs to a particular cell use rather
than to the global descriptor. Precise source spans make `x = x + 1` lower its
right-hand `x` to a slot load while later reads in the same cell remain local.

Self-describing snapshots use bytecode format 1.1 and carry the optional
`NBMD` section with its own 1.0 schema version. It serializes global
`descriptor_id -> BindingKey` records plus a per-cell manifest containing
`CellId`, `code_id`, and ordered input/output descriptor IDs. The bytecode
verifier requires an exact match between those direction sets and the actual
load/store operands, validates provider/output closure, and rejects orphan or
cross-cell descriptors. Raw 1.0 `NotebookCell` code without `NBMD` remains a
supported low-level boundary for hosts that supply an adapter table directly.

The compiler accepts call-site blocks, including nested/threaded callbacks.
External notebook inputs are loaded in the cell and lifted through lexical
captures before a worker closure is created. Helpers use reserved code IDs and
are included in the verified image, including on reactive rebuilds. Native
stdlib imports resolve directly to runtime namespaces.

The compiler profile still rejects definitions, classes, standalone lambda
literals, ivar/cvar access, pattern assignment, and member/index assignment. These need
stable code/image identity or a more complete mutation contract before they can
be persisted safely.

## Runtime transaction

`RuntimeWorld::execute_notebook_cell()` accepts run-local callbacks for slot
loads, successful-load observation, staged stores, and staged GC roots. The VM
never publishes a notebook binding directly. A host adapter maps opaque
descriptor IDs to `BindingKey`s, deduplicates repeated stores with
last-write-wins semantics, and returns the staged batch to `NotebookKernel`.
The observation callback runs only after a `LoadNotebookSlot` value has been
installed in its destination register. The adapter therefore captures only
the descriptors read by the executed control-flow path and deduplicates them
as notebook-native dynamic `BindingKey`s.

An executor may also request a rich runtime dependency capture with its
consumer `CellId`. Watched binding/ivar reads keep their runtime watch-cell or
object identity and revision; they are never converted to notebook
`BindingKey`s. The world installs a thread-safe per-run sink alongside the
legacy public capture API. It deduplicates repeated sources, retains the newest
observed revision, closes at root return (including exceptional paths), and
ignores records from child tasks that arrive after the run has finished. The
adapter exposes this set only after VM and adapter output validation succeed.
`NotebookKernel` stores it separately from notebook-slot observations and
replaces both snapshots only with the successful slot transaction. An absent
capture preserves the prior rich snapshot for generic executors, while a
successful explicitly empty capture clears it. Rich sources are normalized to
stable runtime identities (`watch cell`, `object + ivar`, or whole `object`),
deduplicated by identity, and indexed back to their notebook consumers.
Every capture, including an empty one, also carries its producing watch-stream
`world_id/generation`. The kernel validates that namespace and atomically binds
an unbound kernel with its first successful capture; a foreign capture cannot
publish slots or replace dependency metadata. Revisions remain snapshot
metadata and never participate in source identity.

`NotebookKernel::plan_runtime_events()` is the first event-side scheduling
boundary. It accepts an already-copied event or batch, ignores registration and
non-advancing revisions, finds affected consumers through the rich reverse
index, and expands them through the conservative static notebook graph. A
watch root runs; an automatically invalidated manual root remains stale and
blocks watch descendants. All returned outputs are marked stale before the
plan is handed to the host. Batched roots are coalesced before topological
ordering, so event order does not affect the plan. A runtime root whose static
provider is already stale or uninitialized is reported as blocked even when
that provider is outside the event closure. Runtime watch-cell IDs are
never interpreted as notebook `CellId`s, and observed runtime edges supplement
rather than replace static edges.

`NotebookKernel::plan_runtime_event_batch()` is the typed host boundary around
that lower-level planner. Every poll result carries the exact immutable cursor
passed to `RuntimeWorld`; the kernel validates the requested cursor, source,
contiguous event epochs, retention bounds, and successor as one unit before
taking its locks. It rejects mixed namespaces, gaps/replays, malformed tail
cursors, and batches from the wrong bound world. Only an `Accepted` result
returns an acknowledgement cursor for the host to publish; rejected results
cannot accidentally expose one. Gap recovery remains a separate operation and
cannot be mistaken for a partial event batch.

This API deliberately does not poll `RuntimeWorld`; polling belongs to the
host. Runtime watch events now live in a shared bounded stream whose identity,
epoch, watch-cell IDs, and handle IDs survive `RuntimeState` copies. A cursor
names the first unacknowledged epoch. Polling is read-only and returns a
candidate successor, which the host publishes only after downstream planning
and execution have handled the batch. Overflow, source mismatch, and a cursor
from the future are distinct statuses; no partial suffix is presented as a
complete batch. Notebook image replacement and package reload keep one stream
for the same world, while independent worlds have different identities.
Runtime-backed watch cells carry a weak event sink, so a host write through a
returned watch handle enters the same ordered stream rather than bypassing it.
Each binding/object serializes its mutation through the beginning of that sink
invocation, so parallel writers cannot publish revision 2 before revision 1.
The value-state mutex is released before host code runs; read/snapshot and
same-thread reentrant writes from a sink therefore remain supported. The
RuntimeWorld sink appends immediately, making stream epoch order agree with
per-source revisions while unrelated sources may still interleave.
`RuntimeWorld::wait_watch_events()` adds a read-only blocking view of the same
cursor protocol. It snapshots the shared stream under the world execution lock
and releases that lock before sleeping, so VM execution and host writes remain
producers while a desktop event thread waits. A wake returns the same bounded,
replayable batch as `poll_watch_events()`; a timeout is `Ok` with an empty batch
and an explicit `timed_out` flag. Neither result acknowledges its successor.
Invalid, foreign, future, or overflowed cursors return immediately rather than
waiting for another event. Cancellation is intentionally outside this low-level
primitive: hosts use finite waits and own any shutdown/activity multiplexing.
`RuntimeWorldOptions::watch_activity_notifier` also provides a direct advisory
wakeup after each event publication (and retention-policy change), without a
polling helper thread. The stream snapshots this callback under its mutex and
invokes it after unlocking; other producer/world locks may still be held. The
callback must be short and thread-safe, must not enter the world, mutate watched
sources, or execute UI code, and may overlap callbacks from other producers.
Exceptions from it are isolated from the already-published event. It is not an
event delivery or acknowledgement mechanism: cursor polling remains authoritative.
The persistent kernel binds its opaque dependency IDs to that stream identity
and ignores events from another world even if their numeric cell/object IDs
coincide. Rebinding requires a kernel reset.

`vm_notebook_image_manifest()` reconstructs those adapter tables from an
immutable decoded image. It does not retain pointers into compiler-temporary
`CellCompileResult` objects. An executor captures the world's image epoch and
passes it into the execution boundary; the epoch is checked under the same
execution lock as image replacement, so an adapter from an older image cannot
run a new body that reused its `code_id`.

A cell run is therefore:

1. Resolve and snapshot initialized, non-stale input slots.
2. Execute one immutable `NotebookCell` fragment.
3. Stage every store in run-local state. Heap values are roots at VM safepoints
   and are temporarily pinned until the kernel finishes publication.
4. On success, validate the exact declared output set and atomically commit it.
5. Replace the prior notebook-slot observation and rich runtime-dependency
   snapshots only after successful output publication, then release temporary
   pins.
6. On compile/runtime/validation failure, discard all staged writes and retain
   the last successful published values and dependency snapshot.

`RuntimeWorldOptions::external_gc_root_provider` keeps already-published kernel
slots visible to explicit and safepoint garbage collection. Its callback is
sampled without holding the provider mutex, avoiding runtime/host lock
inversion. The iamber backend captures shared ownership of the kernel in this
callback, so a retained `RuntimeWorld`/cell executor cannot call through a
dangling root-provider pointer after the UI backend is replaced.

An installed notebook image is immutable, but `RuntimeWorld` can atomically
replace it under the world execution lock. The replacement boundary accepts
only verified notebook-only images, bounds sparse code IDs before allocating
runtime caches, prepares module-dependent cache state before publication, and
leaves the old image active on rejection. Runtime name tables, validators,
code indices, state caches, and replay events are all prepared first; the
publication phase contains only non-throwing swaps, clears, and scalar stores.
The existing `RuntimeState`, heap,
published kernel roots, and host options survive the swap. A copyable heap
handle owns temporary pin release, so an execution result may safely outlive
the `RuntimeWorld` object that produced it.

String and symbol indices are also persistent value ABI. Before compiling a
replacement, the host snapshots the world's complete runtime name tables and
seeds `NotebookImage` with them. Compilation may append names, while image
installation verifies that every previously active name remains an exact
prefix. This preserves String/Symbol values already stored in notebook slots,
including names interned dynamically rather than present in the prior module.

## Current `iamber` adapter

The reusable session/evaluation layer now lives in `tools/iamber/session.*`;
the curses translation unit only owns terminal interaction. Non-interactive
`iamber_tests` no longer include the TUI translation unit or link ncurses.

`iamber` now keeps one `NotebookKernel` and one `RuntimeWorld` for the lifetime
of a session. `CellId -> code_id` assignments remain stable across edits. A
source transition is compiled into a candidate immutable image, installed into
the existing world, and only then published as the session's descriptor/code
metadata decoded from `NBMD`. The kernel applies old/new graph invalidation and executes the watch
plan against its existing slots; unrelated successful values and heap objects
are not reconstructed. Compile or install failure retains the previous runtime
image and establishes a failure barrier for affected dependents.

The persistent backend also retains a runtime watch cursor. The POSIX curses
loop now waits on stdin and a lazy activity pipe, with no periodic timer at
genuine idle. Nonblocking `getch()` remains the only input parser. Activity is
serviced between keys as well as during keyboard pauses; continuous input no
longer postpones runtime pumping. The owner submits each successful runtime
poll through `plan_runtime_event_batch()` and acknowledges the successor only
after the accepted automatic plan has been processed. Reentrant pumps are
coalesced and draining is round-bounded. `RuntimePumpDispatch` yields for 16 ms
after a fairness-capped/busy drain and backs off retained-cursor recovery from
100 ms to 1 s. Further runtime hints cannot bypass these deadlines; stdin and
resize remain responsive while the already-known runtime work is pending.
The session adapter exposes a typed pump disposition in addition to its legacy
boolean wrapper: native hosts can distinguish idle, acknowledged, coalesced
busy, retry-required, invalid-cursor, unavailable, and failed outcomes without
reading status text or touching the private cursor. It also reports observed
events, acknowledgement/resynchronization counts, execution completeness,
retained-cursor recovery, and fairness-cap exhaustion, including the case where
an earlier round committed before a later round requested recovery. The kernel
rejects a `timed_out` wait result as an event batch; a waiter is only an
availability hint and the session owner remains responsible for
`poll -> plan -> execute -> ack`.
If visible source/order/watch policy differs from the installed image, the
pump returns `DeferredByEdits` before polling and leaves the editor and cursor
untouched. It must not execute an old body and clear the new text's dirty flag.
The terminal waits for explicit evaluation instead of retrying that state on a
timer. Evaluation re-arms a runtime hint even when the new source emits no watch
events, so a retained old cursor is checked against the synchronized backend.
Registration or already-observed events advance the cursor without rerunning
cells. A future/invalid cursor is surfaced as a host error rather than silently
acknowledged. Overflow uses `plan_runtime_resync()`; source change first clears
rich runtime snapshots/reverse edges and all published slot values in the
obsolete namespace, then resyncs from the retained sources/static graph. This
prevents old-world watch wrappers and heap objects from becoming inputs or GC
roots of the replacement world. All cells become
automatic roots, so watch cells rerun conservatively while manual cells remain
stale and block their watch descendants. A failed automatic resync does not
publish the recovery cursor and is retried from the same gap. A normal accepted
event is acknowledged after its plan publishes an error/stale barrier, rather
than replaying permanently broken user code. The pump and its reentrancy guard
are owned by the single curses UI thread.

The legacy `evaluate_prefix` helper remains for compatibility tests and
standalone prefix evaluation, but the interactive `evaluate_from` path uses the
persistent notebook backend.

## Verified invariants

- edits rerun only the transition-invalidated watch plan;
- removing an output invalidates and blocks former consumers;
- multiple names read from one provider create one cell-level topological edge;
- equal publication does not advance a slot revision;
- incomplete, duplicate, unexpected, failed, or throwing output batches publish
  nothing;
- dynamic dependencies replace only after success;
- conditional slot observation records only executed loads, deduplicates
  repeated loads, replaces the snapshot after a successful branch switch, and
  preserves the old snapshot after a later VM fault;
- notebook dependency analysis treats unshadowed `Kernel.watch` as the same
  intrinsic as HIR lowering: its target is analyzed, but `Kernel` does not
  become a spurious missing notebook input; same-cell shadowing remains an
  ordinary send;
- published heap values survive explicit and safepoint GC;
- a store followed by a later VM fault remains staging only;
- repeated stores produce one final `KernelWrite`;
- failed emission restores the complete image and every emitter intern/side
  table, so rejected source consumes no persistent IDs;
- an explicit notebook-cell `code_id` is reserved before lowering, so
  try/rescue/ensure helper code objects cannot reuse it;
- rich runtime dependency capture remains isolated from the legacy global
  capture, deduplicates watched reads, reports successful empty captures, and
  is not exposed by the adapter after a VM fault;
- rich dependency captures retain their producing world namespace through the
  VM adapter and kernel transaction; foreign captures are rejected before any
  slot or reverse-index publication;
- runtime dependency storage validates the notebook consumer identity and is
  replaced atomically with slots; an absent capture preserves it, an empty
  capture clears it, and failed/invalid runs retain the previous snapshot;
- bounded runtime watch cursors are replayable until acknowledged, report
  overflow/source/future-cursor errors explicitly, keep one ordered namespace
  across parallel writers, and route host writes through the world stream;
- blocking watch waits preserve the exact requested cursor, cannot miss a
  producer racing with wait startup, expose timeout without acknowledgement,
  and do not retain `RuntimeWorld`'s execution lock while sleeping; oversized
  watch/mailbox timeouts saturate the deadline instead of overflowing;
- binding and ivar mutation delivery is serialized per binding/object, so
  stream epochs cannot expose a newer source revision before an older one;
- a kernel binds its opaque runtime dependency IDs before first execution and
  rejects foreign-world events or late namespace attachment over old captures;
- iamber drains runtime events without treating them as explicit cell edits,
  validates each poll as one typed acknowledgement unit, preserves manual-cell
  blocking, acknowledges registration-only batches exactly once, and retains
  the old cursor after an incomplete conservative resync;
- typed iamber pump results distinguish idle/busy/retry/error outcomes, retain
  the boolean compatibility contract through acknowledged batch counts, and
  never allow a wait timeout to cross the kernel acknowledgement boundary;
- session activity handles coalesce runtime/input hints, preserve pre-wait
  notifications, wake all waiters on close, and remain safe after Session moves
  or destruction; runtime hints survive image replacement without advancing
  the watch cursor;
- the runtime activity callback observes a fully published event outside the
  stream mutex, and callback failure cannot undo that event;
- the POSIX activity descriptor is nonblocking/close-on-exec, latches pre-attach
  hints, drains atomically with hint consumption, and retains sticky close
  readiness while copied waiter handles keep it alive;
- owner dispatch has no idle timer; hot/retry hints cannot bypass its bounded
  fairness/recovery deadlines;
- valid image replacement preserves heap values and external roots, rejects
  stale/ordinary/oversized code targets, and removes old code IDs
  deterministically;
- edited images append runtime names without changing IDs held by old slots;
- source cells compile and execute end-to-end through
  `NotebookImage -> serialize/deserialize -> NBMD loader -> RuntimeWorld ->
  NotebookVmCellExecutor -> NotebookKernel`;
- a pre-swap executor is rejected atomically by the world-epoch check after an
  image replacement.

## Next runtime increment

Blocking native bridge sessions are now image-bound: reload/install clears the
idle pool and atomically rebuilds module-derived native registries. Because a
blocking VM executes outside the world lock and retains generation-owned state,
image replacement reports `NativeBridgeBusyError` while any blocking call is
active; this covers both worker execution and publication of its runtime-name
tables. A separate monotonic bridge generation prevents a late session
recycler from crossing even an A-B-A shared-image replacement sequence. The
next concurrency hardening step is an explicit reentrancy contract for world
mutation from VM/host callbacks. The compiler profile can then be widened in
explicit
steps (definitions and stable escaping closure identity, and richer
assignment targets) without weakening the notebook-only verifier boundary.
Observed notebook-slot dependencies are retained by the kernel but are not yet
fed back into scheduling. They are an executed-path subset of the static read
graph and cannot soundly replace it: a future branch may read a currently
unobserved provider. Rich runtime dependencies retain their separate identity
domain and now feed the conservative static graph through the bounded world
stream without replacing static edges. Stored captures are explicitly
namespaced by their world stream. Cursor batches now cross a typed kernel
boundary, source-change/gap recovery has an explicit acknowledgement policy,
and the runtime supplies a blocking wakeup primitive without holding its
execution lock. The session-owned activity bridge in `tools/iamber/activity.*`
now supplies a notification-only mailbox for runtime, input, and shutdown. The
runtime hook holds only a weak notifier; worker handles never reference Session,
the kernel, runtime Values, or cursor state. A waiter retains the mailbox state,
not the Session. The owner closes it before backend teardown; moves transfer the
live mailbox, while move assignment first closes the destination's old mailbox.
Close is sticky and discards pending hints. Notify/wait use one mutex predicate,
so notification before or during wait startup is not lost; repeated hints are
coalesced and atomically consumed by one dispatch consumer. Notifiers are not
async-signal-safe.

Acquire handles on the session owner thread before handing copies to workers.
Input producers must enqueue input before calling `notify_input()`. The native
host can then wait on the mailbox and dispatch queued input or call
`pump_runtime_events_detailed()` on the single Session owner thread. Consuming a
runtime hint never acknowledges an event; the typed pump alone publishes its
cursor. A fairness-capped pump re-arms a runtime hint for a later dispatch turn.
For `needs_recovery()` results, the host must apply its recovery/backoff policy
instead of immediately retrying merely because a hint is pending. Spurious
hints, including ones from an old backend during replacement, are harmless:
polling revalidates the current backend and cursor.

On POSIX, `SessionActivityWaiter::poll_descriptor()` lazily attaches a
nonblocking close-on-exec pipe, before workers are started. It is a borrowed
descriptor valid for the waiter's lifetime: hosts poll/register it but never
read or close it. `wait()` consumes both readiness and flags under one mutex;
shutdown leaves the descriptor readable until the owner unregisters it. Both
ends remain owned by the shared state, so late weak notifications cannot write
to a recycled descriptor or a closed reader. No helper thread is introduced.

The foreground CLI's `TerminalEventWaiter` blocks SIGWINCH before constructing
Session/runtime workers, which inherit that mask. `pselect()` atomically
unblocks it while waiting, preserving ncurses' own resize handler and closing
the empty-input-to-wait race. A brief unmask/reblock dispatch point before each
`getch()` also handles resize during continuous input; it does not assume that
a zero-time `pselect` will run a pending signal handler. The guard outlives curses and
Session/backend teardown and restores the original mask/disposition. This is
an initial, single-owner foreground-console contract, not a process-wide signal
policy for embedding into an already multithreaded GUI. Descriptor-range and
system-call failures surface explicitly; curses teardown is exception-safe.
The isolated PTY smoke can be run with `make test-iamber-tui`.

The next host-side increment is a native GUI input queue/dispatcher using these
handles (and platform-specific wakeup support beyond POSIX); cursor and
reentrancy-guard ownership must remain with Session. Exhaustion guards for the remaining
revision/publication/world counters should accompany that hardening so every
monotonic identity fails closed rather than wrapping.
