# VM microbenchmarks

Small interpreted-VM benchmark programs for focused hot paths. Run them with:

```sh
build/isputnik --eval-file bench/vm/<file>.s
```

The JSONL streaming microbenches read `bench/polyglot/build/json/events.jsonl`.
Prepare that fixture with:

```sh
python3 bench/polyglot/run_benchmark.py --workload json --repeats 1
```

`task_local_get.s` exercises the hot lookup path after one slot allocation and
one binding write. It deliberately excludes spawn/snapshot cost.

`forwarded_calls.s` exercises two `*args`/`**options` forwarding methods;
`send_arguments.s` exercises ordinary calls with both positional and keyword
arguments. Each runs 100000 calls and returns a checksum (100001 and 200009,
respectively). They can also run with `build/sputnik run <file>`. Their timings
measure call overhead and do not predict application speedups; compare complete
workloads separately.

`return_values.s` passes one heap object through two nontrivial methods 300000
times and returns 18. It exercises ownership transfers across RETURN and caller
register writes. Compare the same optimization level and PGO configuration:
an improvement in O2 may already be removed by PGO/ThinLTO.
