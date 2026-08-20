# Amber performance optimization baseline — 2026-08-20

This document is the handoff point for a new optimization session. It records
the exact benchmark protocol, current results, known defects, and the next
profiling questions. Do not compare a future result with this baseline unless
the HTTP contract, client SHA, storage model, client count, and measurement
mode remain identical.

## Repository state

- Benchmark/provenance harness: `534e17d Make polyglot benchmarks reproducible`.
- Partial-report support used for the broken UUID native lane:
  `0d0d6fa Allow partial polyglot benchmark reports`.
- The first nine microbenchmarks were measured from clean `534e17d`; UUID and
  all HTTP series were measured from clean `0d0d6fa`. The second commit only
  changes failure reporting, not workload semantics or execution.
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

Microbenchmarks use five measured fresh-process runs, one unmeasured warmup,
and deterministic cyclic order rotation (`--order-seed 0`). Results contain
all raw samples, mean, median, sample standard deviation, CV, Student-t 95%
confidence interval for the mean, exact commands, executable hashes, source
tree hashes, tool versions, and Git tracked-dirty state. The fresh build root
was `/private/tmp/amber-polyglot-fresh-534e17d`.

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

## Microbenchmark baseline

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

## Known defects and prior findings

1. The UUID native workload currently terminates with
   `NativeCodeError: c2: native bailout`. The failing executable SHA-256 is
   `7f8aeaed609c84ee9b6c34c290960c64a232143485ca87a16f0068541786dba5`.
   Generated code routes the overlapping nullary selector `Uuid#inspect` to
   `native_regexp_send` instead of `native_uuid_nullary`. The VM checksum is
   correct (`1040000`). See
   `results/micro-uuid-r5-2026-08-19-215421-+0300.{json,md}`.
2. `map-words` is the strongest VM-specific anomaly: VM/native is `16.80x`,
   VM/Ruby is about `10.08x`, and VM/Python is about `7.14x`. This points to a
   repeated map/string/iteration mechanism rather than universal VM slowness.
3. Framework-heavy code amplifies the backend gap: Ember native/VM is `3.659x`
   while raw native/VM is `1.600x`. This is consistent with, but does not yet
   prove, a shared map/string/dynamic-send bottleneck between `map-words` and
   request parameters, controller data, ORM attributes, and row mapping.
4. A prior native profile attributed 1,318 samples (3.83% of active
   Ember/native time) to `NativeClosure` construction/destruction in
   `string.chars.each`. Commit `f940d2d` borrowed the already rooted closure;
   an exact-binary A/B measured `+0.646%` Ember/native. It was real but small.
5. SQLite blocking calls already run through the Blocking FFI executor. Waiting
   inside the C extension while holding the bridge lock was rejected because a
   C library callback into Amber may need to park a strand. Pool size 1 is the
   fairness baseline for serialized in-memory SQLite; increasing the pool is a
   separate scalability experiment, not the explanation for the Rails gap.

## Interpretation and next profiling gates

The current data rejects two overly broad explanations:

- The VM is not universally slower than dynamic competitors: it beats Python
  on calls/collections, JSON, secure-random, time-flow, and UUID, and is close
  to Ruby on secure-random and SHA digest.
- Fully native is not a weak optimization: it is `+60%` on raw HTTP and
  `+266%` on Ember. The much larger Ember uplift means VM execution of
  framework-heavy semantics is a first-class problem.

The next optimization must pass these gates:

1. Fix UUID dispatch and restore a complete native microbenchmark matrix.
2. Sample `map-words` VM with symbols and attribute time to opcode/selector,
   hash lookup, map allocation/copying, string tokenization, closure call, and
   refcount/destruction categories.
3. Sample the unchanged Ember VM workload and test whether the leading
   `map-words` category also dominates request parameters, controller hashes,
   ORM attribute maps, SQL bind maps, or row materialization.
4. Only optimize a shared mechanism if both profiles support it. Otherwise
   treat `map-words` as a microbenchmark-specific VM issue and continue the
   Ember phase decomposition: routing, parameter/schema handling, controller,
   model validation, ORM query construction, row mapping, SQLite executor, and
   JSON response generation.
5. Validate changes with saved exact binaries, paired order-rotated repeats,
   and both raw and Ember lanes. A faster microbenchmark alone is insufficient.

## Reproduction commands

```sh
python3 bench/polyglot/run_benchmark.py --workload <name> \
  --repeats 5 --warmups 1 --order-seed 0 \
  --build-dir /private/tmp/amber-polyglot-fresh-<commit>

python3 bench/polyglot/run_http_rps.py --stack raw \
  --amber-execution vm --duration 30 --clients 4 --repeats 5 \
  --order-seed 0 --languages amber,go,rust,python

python3 bench/polyglot/run_http_rps.py --stack ember \
  --amber-execution vm --duration 30 --clients 4 --repeats 5 \
  --order-seed 0 --amber-pool-size 1 --languages amber,rails
```

