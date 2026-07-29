# Polyglot Ember soak-client HTTP RPS benchmark

Date: `2026-07-28T14:14:13.964959+03:00`
Host: `Darwin 25.5.0 / arm64 / arm`
Client: the unchanged full-native Amber client from `ember/examples/soak/client.am`
Load: `4` concurrent clients, `60` seconds per server, `mixed`, negative suite every 25 iterations.

| Server | RPS | Requests | Valid | Invalid | Peak server RSS | vs Amber |
|---|---:|---:|---:|---:|---:|---:|
| Amber + Ember (full native) | 1301.57 | 78,232 | 48,656 | 29,576 | 66.1 MiB | 1.00× |
| Go `net/http` | 12393.54 | 743,648 | 463,184 | 280,464 | 20.6 MiB | 9.52× |
| Python `ThreadingHTTPServer` | 7774.92 | 466,568 | 290,544 | 176,024 | 17.8 MiB | 5.97× |

## Reading

On this workload, Go is 9.52x and Python is 5.97x the Amber/Ember throughput. Because the timed Amber server is already in-memory, SQLite cannot account for this gap; the limiting work is in the Amber/Ember request path (HTTP I/O, routing, parameter handling, JSON, validation, telemetry, or native/runtime dispatch). The Amber memory result is bounded, but the throughput difference is large enough to justify request-path profiling and optimization.

## Method

This is a server-side polyglot comparison, not a replacement-client microbenchmark. Every row is driven by the same compiled Amber executable and therefore performs the same persistent-connection CRUD cycle, JSON checks, schema failures, model-validation failures, optimistic-lock conflicts, method/Host rejection, and oversized-body case.

All three benchmark servers use a process-local, mutex-protected in-memory store so the table compares HTTP routing, JSON/schema handling, validation, and concurrency rather than unrelated database drivers. The production Ember qualification soak remains the separate SQLite-backed workload.

Before each timed row, the runner executes one complete mixed Amber-client iteration (76 requests) and rejects the row on any contract mismatch. Timed RPS is total requests divided by the maximum elapsed time reported by the concurrent Amber clients. Server and clients share the same host, so client CPU is part of the available-machine budget; results are comparative for this machine, not universal language rankings.

## Runtime versions

- amber: `amberc 0.1.0-dev`
- go: `go version go1.26.4 darwin/arm64`
- python: `3.9.6`

## Reproduce

```sh
python3 bench/polyglot/run_http_rps.py --duration 60 --clients 4
```

Machine-readable result: `bench/polyglot/results/http-rps-2026-07-28-141413-+0300.json`.
