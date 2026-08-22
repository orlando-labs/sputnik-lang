# Amber performance optimization baseline — updated 2026-08-22

This document is the handoff point for a new optimization session. It records
the exact benchmark protocol, current results, known defects, and the next
profiling questions. Do not compare a future result with this baseline unless
the HTTP contract, client SHA, storage model, client count, and measurement
mode remain identical.

## Repository state

- Current optimization endpoint: `124cefc Skip watch probes in plain VM
  frames` (clean for the latest complete 2026-08-22 statistical reports).
- The endpoint retains the 16-byte tagged `Value` selected by `58b3600` and the
  final-release fast path from `2301e12`, then adds a conservative per-frame
  watch-cell flag so ordinary VM register traffic does not inspect the `Value`
  tail kind. `VALUE_REPR=variant` remains available as the exact legacy A/B
  control.
- Follow-up commits covered by this handoff:
  `dca2d4a Fix native UUID inspect dispatch`,
  `efded10 Avoid linear scans for indexed map misses`, and `7d0d0c9`.
- Benchmark/provenance harness: `534e17d Make polyglot benchmarks reproducible`.
- Partial-report support used for the broken UUID native lane:
  `0d0d6fa Allow partial polyglot benchmark reports`.
- The original baseline series below came from `534e17d`/`0d0d6fa`. The latest
  complete microbenchmark and HTTP matrices were measured from clean
  `124cefc`; the historical commits only changed reporting and remain listed
  so the older rows can be reproduced.
- Ember: clean `9e603ddf933999c70e4b40ae9daa6d879fb8bc3a`.
- amber-orm: clean `438be2194399fcb84680d0fdb6d9d9e927f9a981`.
- sqlite3-amber: clean `bb62e533bb6096702fbbe36dd0a9a15813ed28eb`.
- Ruby `4.0.5`, Rails `8.1.3`, Puma `8.0.2`.
- Rails uses real Strong Parameters: `expect` for create/PUT, `permit` for
  PATCH and query parameters, plus strict unknown-field checks matching Ember.
- HTTP client SHA-256:
  `f1a35a838dbbd16bebb510c95988fd72063ff3c379e8d754c10b9f3216584b27`.
  It has 38/38 native bodies and the explicitly reported
  `vm-stdlib-send-v1` HTTP bridge. The same executable drove every HTTP row.

## Statistical protocol

The latest microbenchmarks use five measured fresh-process runs, two unmeasured
warmups, and deterministic cyclic order rotation (`--order-seed 20260822`).
Results contain all raw samples, mean, median, sample standard deviation, CV,
Student-t 95%
confidence interval for the mean, exact commands, executable hashes, source
tree hashes, tool versions, and Git tracked-dirty state. The latest fresh build
root was `/private/tmp/amber-polyglot-124cefc`; the original baseline used the
older protocol recorded in its own result files.

HTTP benchmarks use five paired repeats, 30 seconds per server, four concurrent
clients, a fresh server process plus the complete 76-request contract smoke
before every timed sample, and cyclic server-order rotation. Profiling is
forbidden in these throughput series. RPS is total requests divided by the
maximum elapsed time reported by the clients. Competitor ratios are paired by
repeat; the displayed RPS is the median and the displayed CI is for the mean.

## HTTP baseline

| Lane | Amber median RPS | CV | Mean 95% CI | Competitors and paired ratio |
|---|---:|---:|---:|---|
| raw / VM | 20,262 | 0.68% | 20,103–20,444 | Go 37,042 / 1.823x; Rust 37,662 / 1.845x; Python 12,513 / 0.616x |
| raw / native | 32,426 | 1.10% | 31,848–32,734 | Go 37,005 / 1.155x; Rust 37,256 / 1.150x; Python 12,461 / 0.388x |
| Ember / VM / pool 1 | 1,788 | 0.41% | 1,779–1,797 | Rails 1,989 / 1.110x |
| Ember / native / pool 1 | 6,542 | 0.81% | 6,499–6,630 | Rails 1,990 / 0.304x (Amber is 3.287x faster) |

Derived top-level ratios (ratios across separate series are not paired):

- raw native / raw VM: `1.600x`.
- Ember native / Ember VM: `3.659x`.
- Ember VM retains `8.824%` of raw VM throughput (`91.176%` loss).
- Ember native retains `20.175%` of raw native throughput (`79.825%` loss).
- The raw/framework comparison intentionally changes storage from an in-memory
  map to ORM + SQLite. Its loss is the combined framework, model, SQL, row
  mapping, SQLite, and controller/schema cost; it is not all avoidable overhead.

Machine-readable and rendered reports:

- `results/raw-http-rps-vm-r5-2026-08-19-215535-+0300.{json,md}`
- `results/raw-http-rps-native-r5-2026-08-19-220613-+0300.{json,md}`
- `results/ember-http-rps-vm-pool1-r5-2026-08-19-221637-+0300.{json,md}`
- `results/ember-http-rps-native-pool1-r5-2026-08-19-222316-+0300.{json,md}`

The raw native server is a VM-independent 61/61-code-object binary, SHA-256
`f2bbb6f1ccb9661eaa15762d7d7dcfdddea5b87908c0d55fcf0e5863edda52d2`.
The Ember native server is a VM-independent 2048/2048-code-object, ten-module
binary with the SQLite extension embedded, SHA-256
`9c43d159b7bf6a56d87bdab24ede6d634fc6b0c63572277b88f1e9cf94db5bc8`,
size 11,930,856 bytes.

## Pre-optimization microbenchmark baseline

Median wall time in milliseconds; lower is better.

| Workload | Amber VM | Amber native | Python | Ruby | C++ | Go | Rust | VM/native |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| arithmetic | 192.524 | 6.244 | 192.566 | 74.025 | 4.252 | 6.152 | 4.239 | 30.83x |
| calls-collections | 15.739 | 8.757 | 21.830 | 10.870 | 2.104 | 2.743 | 2.357 | 1.80x |
| codecs | 36.145 | 9.170 | 24.865 | 16.377 | 5.358 | 3.747 | 4.533 | 3.94x |
| json | 33.691 | 14.940 | 42.432 | 18.869 | 3.886 | 14.345 | 5.461 | 2.25x |
| map-words | 146.629 | 8.730 | 20.531 | 14.548 | 3.402 | 4.590 | 4.901 | 16.80x |
| secure-random | 23.636 | 9.434 | 52.203 | 24.541 | 6.675 | 6.311 | 17.066 | 2.51x |
| sha-digest | 42.746 | 14.415 | 22.080 | 41.977 | 11.994 | 4.567 | 11.475 | 2.97x |
| string-ops | 23.721 | 7.398 | 15.507 | 13.804 | 3.695 | 3.855 | 4.674 | 3.21x |
| time-flow | 99.532 | 11.177 | 246.249 | 62.799 | 2.972 | 5.185 | 3.162 | 8.91x |
| uuid | 32.977 | broken | 87.839 | 48.189 | 7.036 | 10.947 | 17.397 | unavailable |

VM CV is below 3% except `calls-collections` (7.85% on a very short sample).
Every per-workload JSON/Markdown report is named
`results/micro-<workload>-r5-2026-08-19-*` and contains the complete CI and
provenance envelope.

## UUID and map-words follow-up

| Workload | Amber VM, ms | Amber native, ms | VM/native | State |
|---|---:|---:|---:|---|
| uuid | 33.844 | 11.535 | 2.918x | native bailout fixed |
| map-words | 25.715 | 9.110 | 2.823x | indexed miss scan fixed |

Reports with clean provenance:

- `results/micro-uuid-r5-2026-08-20-112147-+0300.{json,md}` at `dca2d4a`.
- `results/micro-map-words-r5-2026-08-20-120042-+0300.{json,md}` at
  `7d0d0c9`.

For `map-words`, a saved-executable, 15-pair alternating A/B measured
`0.147163 s` before and `0.025683 s` after, a median `5.746x` speedup with the
same checksum (`235174`). The clean five-run result is `25.715 ms` (CV 1.20%)
versus the original `146.629 ms`: `82.5%` less wall time. The VM is now only
`1.19x` slower than Python and `1.68x` slower than Ruby on this workload,
instead of `7.14x` and `10.08x`.

## Resolved defects and profiling findings

1. UUID native was a code-generation dispatch bug, not UUID algorithm cost.
   The overlapping nullary selector `inspect` was routed to
   `native_regexp_send` before receiver type dispatch. `dca2d4a` dispatches a
   Uuid receiver to `native_uuid_nullary`, makes the fixture require full
   native coverage, and restores the complete matrix. The native workload is
   now approximately equal to Go (`11.535` vs `11.643 ms`) and faster than
   Rust (`18.210 ms`).
2. The `map-words` anomaly was an O(n) negative lookup in an otherwise indexed
   ordinary Map. Dynamic missing Str probes correctly had no canonical symbol
   id, and a complete name index therefore proved absence, but
   `map_value_find_entry_index` still scanned every entry using exact equality.
   In the pre-fix 10-second VM profile, `map_value_find_entry` owned
   `8,206/8,555` samples (`95.9%`), including `7,713` in the linear comparison
   loop. `efded10` returns immediately when the canonical index is complete;
   `7d0d0c9` ensures successful indexed hits still perform only one key-shape
   check. A checked-in 2500x diagnostic twin is
   `bench/polyglot/amber/profile/map_words_vm_profile.am`.
3. The post-fix map profile disproves continued linear-scan dominance:
   `map_value_find_entry` fell to `191/7,748` samples (`2.47%`). Canonical-name
   hash lookup is now visible (`607/7,748` samples in the quick map lookup
   branch), but it is a much smaller, separate optimization candidate.
4. The suspected link to low Ember VM RPS was tested and rejected. In the
   fresh 10-second Ember VM profile, `map_value_find_entry` did not reach the
   flat-profile reporting threshold of five top-of-stack samples. The
   unsampled five-pair follow-up at `7d0d0c9` measured Amber `1,700.51 RPS`,
   Rails `1,888.29 RPS`, and paired Rails/Amber `1.107x` (mean-ratio 95% CI
   `1.094...1.119x`). The original paired median was `1.110x`. Thus the large
   microbenchmark win is real, but it does not explain or materially improve
   Ember request throughput. See
   `results/ember-http-rps-vm-pool1-r5-2026-08-20-115325-+0300.{json,md}`.
5. The first map patch performed the key-shape check twice on successful hits.
   A clean intermediate run at `efded10` measured paired Rails/Amber `1.125x`.
   After folding hit and miss handling under one check (`7d0d0c9`), it returned
   to `1.107x`. This is why the final form, rather than `efded10` alone, is the
   optimization endpoint.
6. A prior native profile attributed 1,318 samples (3.83% of active
   Ember/native time) to `NativeClosure` construction/destruction in
   `string.chars.each`. Commit `f940d2d` borrowed the already rooted closure;
   an exact-binary A/B measured `+0.646%` Ember/native. It was real but small.
7. SQLite blocking calls already run through the Blocking FFI executor. Waiting
   inside the C extension while holding the bridge lock was rejected because a
   C library callback into Amber may need to park a strand. Pool size 1 is the
   fairness baseline for serialized in-memory SQLite; increasing the pool is a
   separate scalability experiment, not the explanation for the Rails gap.

## Interpretation and next profiling gates

The current data rejects three overly broad explanations:

- The VM is not universally slower than dynamic competitors: it beats Python
  on calls/collections, JSON, secure-random, time-flow, and UUID, and is close
  to Ruby on secure-random and SHA digest.
- Fully native is not a weak optimization: it is `+60%` on raw HTTP and
  `+266%` on Ember. The much larger Ember uplift means VM execution of
  framework-heavy semantics is a first-class problem.
- Large ordinary-map misses are not the Ember bottleneck. They were nearly the
  entire `map-words` VM profile, are now fixed, and were absent from the Ember
  flat profile. Do not use the `5.746x` micro result to predict framework RPS.

The current Ember VM flat top-of-stack profile (diagnostic, sampled while under
load) is broad rather than dominated by one map operation. Excluding parked
thread primitives, leading entries include `Vm::step` (991), allocator malloc
(335), free (252), `memcmp` (226), `try_apply_scalar_send` (200), frame recycle
(177), `Value` destruction (169), watch-value unwrap/write-register (128 each),
and generic `step_send` (125). These counts are the starting hypotheses, not
exclusive percentages: `/usr/bin/sample` includes many parked scheduler,
reactor, SQLite-executor, and listener threads.

Raw local samples used for the summary are
`/private/tmp/amber-map-words-vm.sample.txt`,
`/private/tmp/amber-map-words-vm-post.sample.txt`, and
`bench/polyglot/build/http-rps/runs/ember-vm-r1-2026-08-20-114155-+0300/repeat-01-position-01-amber/server.sample.txt`.
They are diagnostic build artifacts and are not required to trust or reproduce
the statistical reports.

The next optimization must therefore pass these gates:

1. Re-profile the unchanged Ember VM workload with active samples separated
   from accept/reactor/scheduler/Blocking-FFI waits. Attribute active time to
   dispatch, method argument shaping, Value copy/destruction, frame lifecycle,
   allocation, JSON, ORM row materialization, and SQLite bridge categories.
2. Phase-decompose one request into routing, parameter/schema handling,
   controller, validation, ORM query construction, row mapping, executor/SQL,
   and JSON response generation. A top-level flat profile alone cannot assign
   framework ownership to the generic VM functions above.
3. Prefer a candidate that removes repeated work per dynamic send or per Value
   lifetime across several phases. `try_apply_scalar_send`, `step_send`, frame
   recycling, and Value destruction/copying are currently stronger shared
   hypotheses than map lookup.
4. Keep the negative map lookup fast path, but only pursue its remaining
   canonical-name hash cost if a future real workload profile supports it.
5. Validate changes with saved exact binaries, paired order-rotated repeats,
   and both raw and Ember lanes. A faster microbenchmark alone is insufficient.

## 2026-08-21 optimization: tagged Value as the default

Commit `58b3600` completed the migration that the earlier representation A/B
had already recommended. Besides selecting `VALUE_REPR=tagged` by default, it
adds representation-independent direct `ObjHeader` access. The legacy runtime
therefore no longer copies an `IntrusivePtr` just to discover a heap header,
and the tagged runtime performs the same operation as a tag-range test plus a
pointer load. The separate `VALUE_REPR=variant` build remains supported and
was compiled and smoke-tested after the change.

The saved-binary Ember/VM gate used identical 4-client, pool-1, 15-second,
three-repeat commands on the same machine:

| Exact binary | Median RPS | Delta from legacy |
|---|---:|---:|
| legacy 24-byte variant | 1,828.76 | — |
| tagged before direct `ObjHeader` accessor | 1,911.94 | +4.548% |
| final tagged + direct accessor | 1,962.05 | **+7.288%** |

This is a sequential same-day exact-binary gate, not a single alternating
paired series. The final result is also 2.621% above the first tagged build.
The old executables were saved as
`/private/tmp/amberc-variant-before-tagged-default` and
`/private/tmp/iamber-variant-before-tagged-default` for this gate.

Compiler binary size improved as a second, independent consequence:

| Binary | Legacy variant | Tagged default | Change |
|---|---:|---:|---:|
| `amberc` | 9,880,872 B | 9,010,296 B | -8.811% |
| `iamber` | 8,415,032 B | 7,544,504 B | -10.345% |

### Current statistical HTTP matrix

Every row below is an unprofiled five-repeat series with 30-second samples,
four clients, a contract warmup before each timed sample, and balanced rotation
seed `20260821`. All Amber rows use clean `58b3600`; Ember is clean `9e603ddf`,
amber-orm is clean `438be219`, sqlite3-amber is clean `bb62e533`. The same
pinned client SHA `f1a35a83...` drives every server.

| Lane | Amber median RPS | Competitor medians | Paired interpretation |
|---|---:|---|---|
| raw / VM | 21,584.94 | Go 38,795.11; Rust 38,933.78; Python 12,634.20 | Go 1.800x; Rust 1.823x; Python 0.585x |
| raw / native | 33,730.67 | Go 38,886.48; Rust 39,147.09; Python 12,654.26 | Go 1.152x; Rust 1.161x; Python 0.376x |
| Ember / VM / pool 1 | 1,967.90 | Rails 1,991.74 | Rails 1.011x (+1.08% paired); median-RPS gap 1.21% |
| Ember / native / pool 1 | 6,595.67 | Rails 1,992.70 | Rails 0.303x; Amber 3.310x by medians |

Derived ratios across separate series (not paired): raw native/VM is 1.563x;
Ember native/VM is 3.352x. Against the original 2026-08-19 Ember/VM median of
1,788 RPS, the current 1,967.90 RPS is +10.06%; the exact saved-binary gate
attributes +7.288% to this representation change alone. The original Rails
median (1,989) and current Rails median (1,991.74) are stable, whereas the
intermediate 2026-08-20 Rails series was lower, so the paired ratios and exact
binary gate are safer than comparing that intermediate absolute RPS.

Reports:

- `results/raw-http-rps-vm-tagged-default-r5-2026-08-21.md`
  (`raw-http-rps-vm-r5-2026-08-21-005921-+0300.json`).
- `results/raw-http-rps-native-tagged-default-r5-2026-08-21.md`
  (`raw-http-rps-native-r5-2026-08-21-011016-+0300.json`).
- `results/ember-http-rps-vm-pool1-tagged-default-r5-2026-08-21.md`
  (`ember-http-rps-vm-pool1-r5-2026-08-21-012036-+0300.json`).
- `results/ember-http-rps-native-pool1-tagged-default-r5-2026-08-21.md`
  (`ember-http-rps-native-pool1-r5-2026-08-21-012710-+0300.json`).

### Current complete microbenchmark matrix

Median wall time in milliseconds; lower is better. These are fresh clean
`58b3600` five-repeat runs with two warmups and seed `20260821`. Every checksum
matched.

| Workload | Amber VM | Amber native | Python | Ruby | C++ | Go | Rust |
|---|---:|---:|---:|---:|---:|---:|---:|
| arithmetic | 185.043 | 6.234 | 192.859 | 73.866 | 4.188 | 6.071 | 4.070 |
| calls-collections | 15.305 | 8.802 | 21.889 | 10.350 | 2.145 | 2.685 | 2.275 |
| codecs | 36.973 | 9.062 | 24.372 | 16.179 | 5.308 | 3.685 | 4.503 |
| json | 34.793 | 14.583 | 41.293 | 18.650 | 4.014 | 14.241 | 5.379 |
| map-words | 27.310 | 8.661 | 19.977 | 14.336 | 3.381 | 4.483 | 4.815 |
| secure-random | 22.651 | 9.018 | 48.487 | 22.703 | 5.774 | 5.840 | 16.609 |
| sha-digest | 42.891 | 14.241 | 21.527 | 42.020 | 11.983 | 4.386 | 11.396 |
| string-ops | 24.894 | 7.591 | 15.323 | 13.802 | 3.688 | 3.784 | 4.473 |
| time-flow | 105.242 | 10.606 | 244.249 | 62.611 | 2.929 | 5.067 | 3.100 |
| uuid | 33.789 | 11.034 | 85.043 | 46.418 | 6.840 | 10.691 | 17.314 |

The rendered and machine-readable reports are
`results/micro-<workload>-tagged-default-r5-2026-08-21.{md,json}`.

### Pre/post profile and next largest area

The diagnostic Ember/VM samples used the same workload, but profiler overhead
makes their sampled RPS unsuitable for comparison with the table above. Their
flat totals are similar enough for hotspot-shape comparison (245,537 pre and
248,954 post entries as emitted by `/usr/bin/sample`). Before migration,
`std::__variant` leaves account for 1,951 top counts and the profile also shows
`Value::~Value` 164, heap add/release 422, `Vm::step` 877, malloc 314, and free
232. After migration, variant leaves are exactly zero and heap-header helpers
fall from 91 to 15, confirming that both intended dispatch layers disappeared.

The post profile exposes the next shared architectural cost instead of hiding
it behind variant visitation: `Value::release_payload` 1,064,
`Value` copy construction 402, assignments 256, destruction 236,
`is_watch_cell` 221, `Vm::step` 924, malloc 378, and free 319. Thus the next
large optimization area is tagged-Value ownership traffic across VM register,
argument, return, frame, and watch-cell paths. It should be attacked by tracing
which transfers can borrow or move rather than by merely forcing these methods
inline; correctness depends on roots surviving calls, suspension, rescue, and
frame recycling.

Raw samples and rendered one-run diagnostic reports:

- pre: `build/http-rps/runs/ember-vm-r1-2026-08-21-002018-+0300/repeat-01-position-01-amber/server.sample.txt`;
- post: `build/http-rps/runs/ember-vm-r1-2026-08-21-013345-+0300/repeat-01-position-01-amber/server.sample.txt`;
- post diagnostic: `results/ember-vm-tagged-post-profile-r1-2026-08-21.md`
  and `ember-http-rps-vm-pool1-r1-2026-08-21-013345-+0300.json`.

### Correctness gates

- `vm_tests`, `stdlib_collections_tests`, and `stdlib_task_tests`: pass.
- Full Amber corpus: 214 passed, 0 failed.
- Legacy `VALUE_REPR=variant`: separate compiler build and smoke test pass.
- Targeted full-native UUID: 3/3 direct-native code objects, full coverage,
  no fallback, expected output 42.
- Raw native HTTP: 61/61 direct-native, no bridge/fallback.
- Ember native HTTP: 2048/2048 direct-native, no bridge/fallback; cached SQLite
  extension reused.
- The current full backend-equivalence script contains 146 cold full-runtime
  native links. It was stopped rather than letting hours of compilation skew
  the statistical runs; do not claim that complete gate for this commit.

## 2026-08-22 optimization: dispatch only on final heap release

The tagged representation initially recovered `Closure`/`Instance`/`List`/
`Tuple`/`Set`/`Map` from `ObjHeader::kind` before every reference decrement.
Most decrements are non-final, so this paid a six-way switch only to perform a
single atomic `fetch_sub`. Commit `2301e12` adds
`runtime_heap_release_header`: it decrements first and returns immediately when
the reference remains live. Concrete-type dispatch and the deleter selection
now run only for `refcount == 1`. Unmanaged `make_intrusive` objects and
RuntimeHeap-owned objects retain their original final-release behavior.

### Exact-binary A/B

The pre-change compiler and REPL were saved before editing. Three unprofiled
4-client, pool-1, 15-second Ember/VM series were run old -> new -> old to check
both directions of machine drift:

| Position | Exact binary | Median RPS | CV |
|---|---|---:|---:|
| A1 | pre-change `amberc` | 1,764.77 | 0.59% |
| B | optimized `amberc` | 1,841.86 | 0.64% |
| A2 | pre-change `amberc` | 1,765.27 | 0.40% |

The optimized binary is **+4.368%** against A1 and **+4.338%** against A2.
The saved control is `/private/tmp/amberc-before-header-release`, SHA-256
`2b7bd0d1...`; the optimized binary is SHA-256 `af29d072...`. The runner marks
the A/B repository dirty because the binaries deliberately bracketed the
uncommitted patch; binary hashes and the reversed old/new/old order are the
provenance for this gate.

### Post-change profile

The comparable 10-second sampled Ember/VM profile changes the targeted flat
top as follows:

| Symbol/category | Before | After |
|---|---:|---:|
| `Value::release_payload` | 1,064 | 0 |
| `runtime_heap_release_header` | 0 | 174 |
| typed `runtime_heap_release<T>` | 237 | 69 |
| `Value::~Value` | 236 | 677 |
| `Value` copy constructor | 402 | 390 |
| `Value` assignment operators | 256 | 445 |
| `Value::is_watch_cell` | 221 | 217 |
| `Vm::step` | 924 | 997 |

The destructor now contains the inlined tag classification that used to be
charged to `release_payload`, so its larger isolated count is expected. The
important structural result is that the 1,064-count common helper disappears
and the replacement header-release helper has only 174 top counts; source
inspection confirms that its type switch executes only on final release.
Profiler throughput
(`1,775.76 RPS`) is diagnostic only and must not be compared with unprofiled
RPS. Raw sample:
`build/http-rps/runs/ember-vm-r1-2026-08-22-123219-+0300/repeat-01-position-01-amber/server.sample.txt`.
Rendered report:
`results/ember-vm-header-release-post-profile-r1-2026-08-22.md`.

The next broad target is no longer concrete heap-kind dispatch. It is the
remaining `Value` copy/move/destructor traffic plus unconditional watch-cell
probing on ordinary VM registers. Any borrow/move optimization must preserve
roots across calls, suspension, rescue/ensure, direct block reuse, and frame
recycling; a frame-level no-watch fast path is a safer first experiment than
borrowing arbitrary register values through a potentially suspending send.

### Complete current microbenchmark matrix

Median wall time in milliseconds; lower is better. All rows are clean
`2301e12`, five measured runs, two warmups, balanced seed `20260822`, and
matching checksums. `VM delta` and `native delta` compare with the clean
`58b3600` series from 2026-08-21; negative is faster.

| Workload | Amber VM | VM delta | Amber native | Native delta | Python | Ruby | C++ | Go | Rust |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| arithmetic | 168.820 | -8.77% | 5.994 | -3.85% | 187.051 | 69.961 | 4.036 | 5.845 | 4.005 |
| calls-collections | 13.690 | -10.55% | 8.843 | +0.47% | 21.923 | 10.527 | 2.107 | 2.759 | 2.259 |
| codecs | 35.647 | -3.59% | 8.970 | -1.02% | 24.418 | 16.135 | 5.310 | 3.818 | 4.518 |
| json | 31.942 | -8.19% | 14.473 | -0.75% | 40.962 | 18.549 | 3.916 | 13.945 | 5.125 |
| map-words | 25.263 | -7.50% | 8.262 | -4.61% | 20.090 | 14.020 | 3.186 | 4.387 | 4.587 |
| secure-random | 21.886 | -3.38% | 8.948 | -0.78% | 48.130 | 22.581 | 5.985 | 5.835 | 15.769 |
| sha-digest | 40.640 | -5.25% | 14.232 | -0.06% | 21.613 | 41.276 | 11.738 | 4.417 | 11.048 |
| string-ops | 23.918 | -3.92% | 7.381 | -2.77% | 15.033 | 13.378 | 3.701 | 3.741 | 4.584 |
| time-flow | 96.768 | -8.05% | 10.695 | +0.84% | 239.943 | 61.117 | 3.059 | 5.149 | 2.944 |
| uuid | 31.785 | -5.93% | 10.749 | -2.58% | 84.171 | 46.899 | 6.701 | 10.632 | 16.516 |

Every VM workload improved; the largest reductions are calls/collections
10.55%, arithmetic 8.77%, JSON 8.19%, and time-flow 8.05%. Native movements
are smaller and include both signs. These inter-day deltas are descriptive and
also contain host-speed drift; the old/new/old gate above is the causal
measurement for this patch. Reports:
`results/micro-<workload>-header-release-r5-2026-08-22.{md,json}`.

### Complete current HTTP matrix

These are clean `2301e12`, unprofiled five-repeat, 30-second, four-client
series with contract smoke before every sample and balanced seed `20260822`.

| Lane | Amber median RPS | CV | Competitor medians | Paired interpretation |
|---|---:|---:|---|---|
| raw / VM | 22,310.20 | 1.00% | Go 39,458.78; Rust 39,596.97; Python 11,892.47 | Go 1.788x; Rust 1.776x; Python 0.534x |
| raw / native | 33,775.35 | 0.52% | Go 39,492.94; Rust 39,543.64; Python 11,927.97 | Go 1.166x; Rust 1.171x; Python 0.355x |
| Ember / VM / pool 1 | 1,958.40 | 0.36% | Rails 1,833.15 | Rails 0.935x; Amber is 1.068x by medians |
| Ember / native / pool 1 | 6,171.63 | 0.26% | Rails 1,837.19 | Rails 0.298x; Amber is 3.359x by medians |

Raw/VM is +3.360% versus the preceding clean median, while raw/native is
+0.132% (neutral). Current cross-series native/VM ratios are 1.514x raw and
3.151x Ember. Do not interpret the inter-day framework deltas as patch effect:
the current Ember/VM median is -0.483% versus 2026-08-21 and Ember/native is
-6.429%, but Rails simultaneously moved from approximately 1,992 to 1,833--
1,837 RPS. The same-day old/new/old gate above isolates the +4.34% VM effect;
the current Rails comparison states today's competitive position only.

Reports:

- `results/raw-http-rps-vm-header-release-r5-2026-08-22.md`
  (`raw-http-rps-vm-r5-2026-08-22-124413-+0300.json`).
- `results/raw-http-rps-native-header-release-r5-2026-08-22.md`
  (`raw-http-rps-native-r5-2026-08-22-125512-+0300.json`).
- `results/ember-http-rps-vm-pool1-header-release-r5-2026-08-22.md`
  (`ember-http-rps-vm-pool1-r5-2026-08-22-130531-+0300.json`).
- `results/ember-http-rps-native-pool1-header-release-r5-2026-08-22.md`
  (`ember-http-rps-native-pool1-r5-2026-08-22-131206-+0300.json`).

### Correctness and size

- `vm_tests`, `stdlib_collections_tests`, and `stdlib_task_tests`: pass.
- Full corpus: 214 passed, 0 failed.
- Separate legacy `VALUE_REPR=variant` compiler build and calls/collections
  smoke checksum `2047795430`: pass.
- Raw native: 61/61 direct-native; Ember native: 2048/2048 direct-native;
  both have full body coverage and zero VM fallback/runtime bridge.
- `amberc` is 9,010,536 bytes and `iamber` is 7,544,744 bytes: +240 bytes each
  versus `58b3600`. Raw and Ember native servers are 1,580,488 and 11,881,464
  bytes respectively: 16 bytes smaller each than the preceding endpoint.

## 2026-08-22 optimization: skip watch probes in plain VM frames

Commit `124cefc Skip watch probes in plain VM frames` is the current endpoint.
`Kernel.watch` storage cells are rare, but prior to this change every ordinary
VM register read, write, and integer-sidecar materialization called
`Value::is_watch_cell()`. A `Frame::has_watch_registers` flag now proves the
common negative case once for the whole frame. Creating local watched storage
raises it explicitly; nested execution, register merge, and handler-frame
copies propagate it conservatively; all new, reset, and recycled frames clear
it. A stale `true` can only retain the old checked path, while `false` proves
that direct register access is safe.

### Exact-binary A/B

The gate used the same pinned client SHA, 4 clients, pool 1, 15-second samples,
three repeats per phase, and the saved old compiler on both sides of the new
compiler (`old / new / old`). The old compiler was
`/private/tmp/amberc-before-watch-reg-fast-path`, SHA-256
`af29d072531fd9aa0f50123dd2d21a7402ddf584a53157b28151a3ec5de200df`;
the accepted compiler is SHA-256
`4e71d30bc8e2c0fa6296eecbfce099b853c69004b5ba0f634364ef27b4a14d0e`.

| Phase | Ember/VM median RPS | Mean RPS | CV |
|---|---:|---:|---:|
| old flank 1 | 1,983.81 | 1,982.84 | 1.119% |
| new | 1,993.27 | 1,993.09 | 0.464% |
| old flank 2 | 1,960.28 | 1,958.21 | 0.739% |

The mean of the two old-flank medians is 1,972.05 RPS, so the accepted result
is **+1.076%**. Using phase means gives **+1.145%**. Raw result files are
`ember-http-rps-vm-pool1-r3-2026-08-22-192802-+0300.json`,
`...-192858-+0300.json`, and `...-192955-+0300.json`.

### Post-change profile

The diagnostic profile is
`bench/polyglot/build/http-rps/runs/ember-vm-r1-2026-08-22-193147-+0300/`
`repeat-01-position-01-amber/server.sample.txt`; its unprofiled RPS must not be
compared with statistical runs. Relative to the preceding profile at
`ember-vm-r1-2026-08-22-123219-+0300`, top-of-stack samples changed as follows:

| Symbol | Before | After | Delta |
|---|---:|---:|---:|
| `Vm::unwrap_watch_value_for_read` | 49 | 5 | -89.8% |
| `Value::is_watch_cell` | 217 | 109 | -49.8% |
| `Vm::step` | 997 | 986 | -1.1% |

The remaining watch checks come from captures and generic unwrap paths rather
than ordinary frame reads. The new largest flat VM areas are `Vm::step` (986),
`Value::~Value` (734), `Value` copy construction (415), move assignment (355),
allocator/free paths, and scalar send dispatch (204). The next optimization
should therefore target register-value ownership traffic or a broader
instruction/send specialization; further watch-specific work is no longer a
large enough target.

### Complete current microbenchmark matrix

Five measured runs, two warmups, seed 20260822. Times are median milliseconds;
lower is better.

| Workload | Amber VM | Amber native | Python | Ruby | C++ | Go | Rust |
|---|---:|---:|---:|---:|---:|---:|---:|
| arithmetic | 172.183 | 5.788 | 185.743 | 69.138 | 4.002 | 5.880 | 3.978 |
| calls-collections | 13.470 | 8.718 | 21.685 | 10.437 | 2.153 | 2.868 | 2.227 |
| codecs | 35.307 | 9.057 | 24.470 | 16.194 | 5.356 | 3.641 | 4.488 |
| json | 32.035 | 14.821 | 41.580 | 18.513 | 4.000 | 13.905 | 5.266 |
| map-words | 25.527 | 8.642 | 19.964 | 13.903 | 3.332 | 4.490 | 4.655 |
| secure-random | 21.483 | 8.814 | 47.470 | 22.372 | 5.821 | 5.736 | 15.633 |
| sha-digest | 39.796 | 14.035 | 21.308 | 41.054 | 11.824 | 4.426 | 11.279 |
| string-ops | 23.539 | 7.140 | 15.172 | 13.359 | 3.703 | 3.866 | 4.611 |
| time-flow | 92.221 | 10.339 | 239.914 | 60.921 | 2.945 | 4.986 | 3.031 |
| uuid | 30.912 | 10.672 | 83.612 | 46.812 | 6.548 | 10.525 | 16.338 |

Against the immediately preceding `header-release` medians, Amber VM improved
on seven workloads: calls/collections +1.63%, codecs +0.96%, secure-random
+1.88%, SHA digest +2.12%, string ops +1.61%, time-flow +4.93%, and UUID +2.82%.
JSON was neutral (-0.29%); map-words (-1.04%, current CV 3.10%) and arithmetic
(-1.95%, current CV 2.53%) do not show a broad regression signal.

Reports are `results/micro-<workload>-watch-fast-r5-2026-08-22.{json,md}`.

### Complete current HTTP matrix

Five paired/rotated repeats, 30 seconds, 4 clients, pool 1, no profiling.

| Lane | Amber median RPS | CV | Competitor median RPS | Current relation |
|---|---:|---:|---:|---|
| raw / VM | 22,281.32 | 0.585% | Go 39,453.00; Rust 39,429.92; Python 11,913.19 | native/VM reported below |
| raw / native | 33,726.55 | 0.216% | Go 39,456.15; Rust 39,550.38; Python 11,940.35 | native/VM 1.514x |
| Ember / VM / pool 1 | 1,972.23 | 0.764% | Rails 1,834.15 | Amber 1.075x; +7.53% |
| Ember / native / pool 1 | 6,183.11 | 0.135% | Rails 1,837.84 | Amber 3.364x |

Compared with the preceding clean matrix, raw/VM is -0.129%, raw/native is
-0.144%, Ember/VM is **+0.706%**, and Ember/native is +0.186%. Rails stayed
effectively fixed in the VM comparison (1,833.15 to 1,834.15 RPS), so the
framework VM gain is not a competitor drift artifact. Current native/VM ratios
are 1.514x raw and 3.135x Ember.

Reports and machine-readable samples:

- `results/raw-http-rps-vm-watch-fast-r5-2026-08-22.md`
  (`raw-http-rps-vm-r5-2026-08-22-194442-+0300.json`).
- `results/raw-http-rps-native-watch-fast-r5-2026-08-22.md`
  (`raw-http-rps-native-r5-2026-08-22-195539-+0300.json`).
- `results/ember-http-rps-vm-pool1-watch-fast-r5-2026-08-22.md`
  (`ember-http-rps-vm-pool1-r5-2026-08-22-200559-+0300.json`).
- `results/ember-http-rps-native-pool1-watch-fast-r5-2026-08-22.md`
  (`ember-http-rps-native-pool1-r5-2026-08-22-201234-+0300.json`).

### Correctness and size

- `vm_tests`, `stdlib_collections_tests`, and `stdlib_task_tests`: pass.
- Full corpus: 214 passed, 0 failed.
- Separate legacy `VALUE_REPR=variant` compiler build and calls/collections
  checksum `2047795430`: pass.
- Raw native remains 61/61 direct-native; Ember native remains 2048/2048;
  both have full body coverage and zero VM fallback/runtime bridge.
- `amberc` is 9,010,536 bytes and `iamber` is 7,544,744 bytes; raw and Ember
  native servers are 1,580,488 and 11,881,464 bytes. All four sizes are exactly
  unchanged from `2301e12`.

## 2026-08-22 optimization: reset recycled VM values directly

Commit `d5ca4c7 Reset recycled VM values directly` is the current endpoint.
The post-watch profile attributed 355 flat samples to `Value` move assignment,
with frame recycling its largest caller. Both ordinary frame recycling and the
direct-block-reuse reset path previously cleared every initialized register via
`value = Value::null()`. In the tagged representation that expands into the
null factory, move assignment, payload release, and temporary destruction even
when the slot already contains an unboxed-integer null placeholder.

`Value::reset()` now performs the same payload release and establishes the null
invariant directly. The legacy variant representation implements the identical
API with `variant::emplace<monostate>()`. Recycled register slots plus `self`,
`block`, and `last_result` use the direct reset; capture vectors and all object
graphs are still released before a frame enters its pool.

### Exact-binary gates

The pre-change interpreter is
`/private/tmp/iamber-before-borrowed-reg-fast-path`, SHA-256
`1d10facd6f20376eebce510e59b84e1b2913bae5981a677d79c32d024b00d221`.
The accepted interpreter is SHA-256
`d62a627645426f095791e86bd2d94a6903c4cc23239b707e2eb3a2b7077ed29d`.
All gates used balanced old/new order and matching checksums.

| Gate | Repeats per binary | Old median | New median | Time delta |
|---|---:|---:|---:|---:|
| calls/collections process workload | 240 | 12.615 ms | 12.565 ms | **-0.393%** |
| 500,000 eight-argument VM calls | 60 | 218.467 ms | 205.786 ms | **-5.805%** |

The synthetic gate deliberately isolates frame acquisition/recycling; both
series had CV below 0.8%. It confirms the mechanism and the upper-bound effect
when frame cleanup dominates. The ordinary polyglot workload establishes a
smaller positive end-to-end effect.

Before selecting this scope, a broader borrowed-register experiment was
rejected. Borrowing across many quick handlers regressed calls/collections by
3.573%; restoring the common `read_reg` path reduced that regression to
0.871%. A jump-only scope measured -0.330% time, while adding cached/ivar
borrows was neutral (-0.088%). Those edits are not present in `d5ca4c7`; the
result is a useful warning that removing retain/release pairs can still lose to
code layout, register pressure, and added branches.

### Complete current microbenchmark matrix

Five measured runs, two warmups, balanced seed 20260822. Times are median
milliseconds; lower is better. `VM delta` compares with the immediately
preceding `watch-fast` series. This is a same-day cross-series comparison, not
the causal patch gate: native moved by 4--8% in several rows even though the
patch only changes VM frames, demonstrating material host drift.

| Workload | Amber VM | VM delta | VM CV | Amber native |
|---|---:|---:|---:|---:|
| arithmetic | 162.432 | -5.66% | 3.12% | 5.550 |
| calls-collections | 12.968 | -3.73% | 1.40% | 8.305 |
| codecs | 34.635 | -1.90% | 0.97% | 8.448 |
| json | 31.118 | -2.86% | 4.11% | 14.971 |
| map-words | 24.102 | -5.58% | 3.18% | 7.957 |
| secure-random | 20.884 | -2.79% | 1.00% | 8.375 |
| sha-digest | 39.723 | -0.18% | 2.03% | 13.454 |
| string-ops | 23.181 | -1.52% | 2.27% | 7.481 |
| time-flow | 93.758 | +1.67% | 0.66% | 9.937 |
| uuid | 30.988 | +0.25% | 1.75% | 10.437 |

Reports are
`results/micro-<workload>-frame-reset-r5-2026-08-22.{json,md}`. All workload
checksums matched.

### Correctness, size, and unavailable measurements

- Tagged `vm_tests` and `stdlib_collections_tests`: pass.
- Corpus available inside the current sandbox: 212 passed. The two remaining
  cases, `net_socket_handoff_to_task` and `net_tcp_loopback`, fail only because
  local `listen` is denied. `stdlib_task_tests` reaches the same sandbox denial
  in its cooperative socket-read case.
- Separate legacy `VALUE_REPR=variant` interpreter build and
  calls/collections checksum `2047795430`: pass.
- `amberc` is 9,010,664 bytes and `iamber` is 7,544,872 bytes: +128 bytes each
  versus `124cefc`. Native artifacts are unaffected by this VM-only change.
- The current environment denied both elevated loopback servers and macOS
  process inspection. Therefore an Ember/VM old/new/old HTTP gate and a
  post-change `sample` profile could not be collected in this continuation.
  The preceding `watch-fast` HTTP matrix remains the latest authoritative
  framework comparison; do not replace it with process-microbenchmark deltas.

The first action when elevated local execution is available is an exact saved-
binary Ember/VM old/new/old gate followed by a post-change profile. Confirm that
`Value::operator=(Value&&)` loses its `recycle_frame` samples; then reassess the
remaining destructor/copy traffic before returning to borrowed register reads.

## Reproduction commands

```sh
python3 bench/polyglot/run_benchmark.py --workload <name> \
  --repeats 5 --warmups 2 --order-seed 20260822 \
  --build-dir /private/tmp/amber-polyglot-fresh-<commit>

python3 bench/polyglot/run_http_rps.py --stack raw \
  --amber-execution vm --duration 30 --clients 4 --repeats 5 \
  --order-seed 20260822 --languages amber,go,rust,python

python3 bench/polyglot/run_http_rps.py --stack ember \
  --amber-execution vm --duration 30 --clients 4 --repeats 5 \
  --order-seed 20260822 --amber-pool-size 1 --languages amber,rails

# Diagnostic only; profiling changes throughput and must not be compared to
# the unprofiled statistical series.
python3 bench/polyglot/run_http_rps.py --stack ember \
  --amber-execution vm --duration 15 --clients 4 --repeats 1 \
  --amber-pool-size 1 --languages amber --sample-seconds 10 --skip-build
```
