# Native blocking declarations

`blocking` is a contextual modifier after a native binding:

```sputnik
native def receive(timeout_ns as Int) from "transport.receive" blocking

native class PoolCore from "pool.Core" borrowed:
  def acquire_wait(timeout_ns as Int) from "pool.acquire_wait" blocking
```

Free bindings require `native def`; methods inside `native class` use `def`.
The modifier precedes an optional `:` fallback body. It requires a `from`
binding and remains an ordinary identifier elsewhere. It is independent of
class ownership (`borrowed`, `owned`, `collected`).

The parser carries `native_blocking` through HIR to the bytecode attribute
`sputnik.native.blocking:<entry_code_id>`, whose value is the logical binding.
Graph linking relocates that code id. Both dynamic extension loading and
static native registration read this metadata. Old manifests using
`blocking_symbols` remain accepted for compatibility; new source declarations
need no duplicate list. Policy applies to the logical binding: if aliases use
the same logical name, a blocking declaration makes that binding blocking.
Distinct logical names can point to the same C symbol with different policies.
A synchronous typed leaf cannot also be blocking; registration rejects this.

## Meaning of the declaration

`blocking` declares that the complete synchronous foreign invocation may wait,
including Sputnik code called through `sputnik_call_block`. It is not a CPU
classification and does not detect an OS wait after it starts. Arbitrary native
code has no suspension handshake with the scheduler; its declaration must be
known before entering that code.

The intended contract is:

1. A native socket/database wait must leave scheduler workers available to other
   async tasks. The blocked foreign OS thread need not become runnable itself.
2. A native invocation may call a Sputnik callback synchronously. If the callback
   waits for a channel, timer, task, I/O or another blocking binding, other async
   tasks must still be able to run and complete that wait.
3. Callback return resumes the same foreign activation, followed by the same
   Sputnik caller. Neither invocation nor preceding side effects may be replayed.
4. Task identity, task-local context, captures, receiver/arguments, output context,
   exception/rescue/ensure and foreign cleanup remain valid through the entire
   wait. Cancellation cannot retire resources while a foreign stack owns them.

The modifier is needed even when C++ itself only invokes a callback, if that
callback is permitted to wait. An unmarked synchronous foreign entry promises a
nonblocking dynamic extent, including its callbacks. Marking a nested binding
cannot by itself detach an already-running unmarked C/C++ caller's stack from
its scheduler worker. Ordinary Sputnik wrappers need no matching modifier:
their calls reach the declared foreign boundary.

The C ABI callback is synchronous: the C stack must remain alive until callback
completion. Returning a pretend success or unwinding arbitrary native frames to
park the Sputnik callback would break the ABI and its resource lifetimes.
Supporting callback suspension requires preserving that stack outside the
scheduler or adding an explicit asynchronous callback ABI. An asynchronous ABI
would be a separate, versioned contract, not an implicit change to v1.

## Current execution paths

In resumable VM tasks, marked CALL, CALL_SPREAD, constructor CALL and foreign
SEND preserve the call instruction, park the strand, then consume completion
before advancing. An already executing FFI worker reuses its current thread.
Callback-local handlers may run there, but exception/throw/non-local return
crossing the suspended caller is transferred back before that caller unwinds.
A direct VM invocation without a resumable task runs the thunk synchronously.

`sputnik_call_block` uses a nested VM on that FFI thread. Its timer/channel waits
are synchronous host waits; the scheduler is already free because the outer
task parked. A nested marked native call on the FFI thread runs inline, avoiding
recursive offload and a second completion wait. The callback remains part of
the original logical task; it is not a newly spawned task. This satisfies
scheduler progress, but does not release the occupied FFI executor slot.

Native programs containing source- or manifest-declared blocking bindings use
the existing stack-preserving resumable task driver for `async`/`spawn`, also
used by system/threaded programs. Before starting the complete C++ activation,
the driver parks its strand and retains the activation on a dedicated host
thread. The foreign call can still wait with `future.get()`, but that wait now
occupies the host stack thread, not a scheduler worker. Completion wakes the
same strand; its driver consumes the result/failure without replaying the body.
Programs with no such effects keep their previous direct task path.

The current compiler emits ordinary C++ stacks rather than native coroutine
frames. Conservatively selecting this driver for the whole linked program
costs a host thread per active async task, including tasks that do not actually
reach FFI. This is a correctness implementation with a scaling cost; it is not
a lightweight native continuation/fiber implementation or a CPU speedup.
Direct unmarked FFI and synchronous typed leaves keep their ordinary call path.

Foreign jobs retain cancellation owners, run cancellation/masking, task/strand,
sync-owner identity, task-local context, annotation and output writers. Native
callbacks enter a cycle-mutator scope; the host caller leaves its mutator slot
while waiting for the foreign completion. A cancelled native task publishes
cancelled status only after its foreign stack has unwound. A foreign library
that does not cooperate with cancellation must finish before task retirement.

Both the task scheduler and the FFI executor create OS workers on demand.
Constructing an empty pool creates no workers. Task workers grow with runnable
demand up to their configured limit, reuse existing threads, and queue excess
tasks. One worker also observes deadlines when the first task is sleeping.
Synchronous task joins still occupy an OS worker and count toward demand.
Started task workers remain available until scheduler shutdown. Scheduler
statistics distinguish the configured `worker_count` limit from the actual
`live_worker_count`.

Configure limits in the service environment before launching the process:

```sh
export SPUTNIK_TASK_MAX_THREADS=4
export SPUTNIK_FFI_MAX_THREADS=8
```

`SPUTNIK_TASK_MAX_THREADS` defaults to hardware concurrency (2 if unknown).
`SPUTNIK_FFI_MAX_THREADS` defaults to twice hardware concurrency, clamped to
4..32. Each value must be a positive decimal integer; empty, zero, negative,
malformed and overflowing values raise `ArgumentError` naming the variable.
Limits are read when the corresponding pool is constructed; explicit nonzero
C++ worker counts override ENV. FFI carriers all retire after 30 seconds idle,
are joined on a subsequent submission, and start again when needed.

The FFI limit is hard. When every carrier is occupied or already reserved by
an accepted job, a new submission raises `ResourceBusyError` with
`blocking FFI thread pool is full`, the limit and the ENV setting to adjust.
Rejected jobs are never queued or executed later. In a resumable VM the fault
wakes the parked strand and runs normal `rescue`/`ensure`; freeing a carrier
allows subsequent calls to proceed. A synchronous nested foreign call already
on an FFI carrier runs inline and does not acquire a second carrier.
Completion notifications run after the foreign call releases its capacity,
so sequential calls at a one-thread limit do not spuriously report overload.

This explicit overload behavior replaces unbounded surplus threads: a queued
foreign producer could otherwise deadlock when all bounded carriers are waiting
for its result. Applications should leave capacity for independent producers,
handle the error, retry after capacity is released, or raise the configured
limit. These are pool limits, not a cap on every OS thread in the process:
native-stack task drivers and service/reactor threads have separate lifetimes.
Scheduler parking and FFI producer progress remain separate guarantees.

Use `blocking` for waits and I/O that must yield a resumable VM worker. A CPU
kernel also prevents cooperative switching until it returns, but relocating
every small kernel to a separate executor is an expensive scheduling policy.
Prefer inline compute or an explicit coarse compute boundary when fairness is
required; neither changes the work performed by the kernel.

Validation: `make test-native-blocking` exercises imported bindings, function
values/spreads, construction, foreign methods, inline calls, worker reuse,
rescue/ensure, callback faults and non-local return in VM and
`--require-full-native`. A separate VM fixture checks throw/catch, whose native
lowering is not yet supported. Editor tests check contextual highlighting while
preserving ordinary identifiers, comments and string literals.

`blocking_ffi_tests` is part of that target. It supplies exactly one scheduler
worker, a native `poll`/socketpair reader, an async native writer, and a nested
callback that sleeps and receives from a zero-capacity Channel while another
async task sends. It checks task identity, actual sleep, native continuation
counts and scheduler park/resume stats. Both cases pass in the VM.

The same unit executable compiles the exact generated native stack driver
against a one-worker scheduler. It checks a nested blocking callback, task-local
context, 64 immediate-completion races, cancellation/cleanup and park/resume
statistics. It also checks ENV validation and precedence, empty pool startup,
bounded FFI growth, explicit overload rejection, idle retirement and restart.
Task tests cover sequential reuse, excess queued work, initial sleeping tasks,
nested synchronous joins and shutdown.

`tests/fixtures/native_blocking/progress.build.json` has `--case native`,
`--case callback`, `--case ffi-producer` and `--case ffi-overload`. The first two
fill the scheduler's configured worker budget, then start an async producer.
The third leaves one FFI carrier available for its blocking producer. The
fourth fills the hard FFI limit, catches the extra producer's overload fault,
checks `ensure`, releases the waits through an inline binding and successfully
retries the blocking call. Timeouts bound failures. All four run with task/FFI
limits of 2/4 in `make test-native-blocking`, in VM and full native.
The additional `--case ffi-sequential` checks 256 successive calls with an
FFI limit of one in both backends.

Before the 2026-10-09 fix, the full-native pressure build had full coverage and
no bytecode fallback, but **both modes failed** on the ten-worker host. The direct
native wait raised `ValueError: scheduler worker remained blocked during FFI wait`;
the callback mode raised `TimeoutError: channel recv timed out`. The async producer
was queued while each scheduler worker waited for its FFI result. This exposed a
native scheduler correctness gap. The original value/error/worker-selection
fixture passed in both backends; it did not prove progress without spare workers.

After the fix, all three pressure modes pass in both backends: ten native waits,
ten channel callbacks and twenty occupied FFI carriers with an FFI producer on
the tested host. Native builds have full coverage and no bytecode fallback.
The source fixture additionally verifies inherited task/strand identity,
task-local scope restoration, cancellation status after foreign cleanup,
rescue/ensure and nonlocal callback return. Full VM/task suites and adjacent
native ABI/language fixtures pass. Current results are recorded in
`build/blocking-fix-{native,vm,task,adjacent}-tests.log` and
`build/native_blocking/progress/build.json`.

Torch task-local autograd/`execution_batch` idioms pass in VM and full native.
Actual MPS checks pass in both backends with `PYTORCH_ENABLE_MPS_FALLBACK=0`,
including device materialization and checkpoint placement. Their logs are
`sputniktorch/build/blocking-fix-idioms-{vm,native}.log` and
`sputniktorch/build/blocking-fix-mps-{vm,native}.log`. These are correctness
checks; call timings and MNIST throughput were not remeasured in this fix.

`make check-native-blocking-progress` remains available to run the progress
fixture separately. The original failure evidence is in `build/blocking-contract-unit.log`;
the two VM and two native pressure logs are
`build/blocking-contract-progress-{vm,native}-{native,callback}.log`, with native
coverage in `build/blocking-contract-progress-build.json`.

The next architectural improvement is a native continuation/fiber design that
can retain language stacks without a dedicated host thread for every task.
That must preserve the synchronous C ABI's foreign stacks, TLS-sensitive native
code, captured lifetimes and dynamic exception behavior. Effect-aware selection
of task drivers can narrow the current conservative whole-program rule. Neither
change is required to alter the public `blocking` declaration or typed leaf ABI.
