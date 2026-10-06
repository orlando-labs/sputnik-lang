# LOOKUP_CONST benchmark

The provider's `LookupProbe.qualified` method reads its module's exported
function on each iteration. Its bytecode contains
`LOOKUP_CONST bench.vm.lookup_constant.provider.answer`; the function retains
its real captured module state. The other methods read a local function,
an Amber class, a builtin type, an error class, and the task module.

Each method uses the same while loop. The block and method calls occur once
per timed batch, outside the inner loop. Results and captured state are checked
outside the timer. There is one warmup and five timed batches per case.

The optional Python driver alternates before/after process order, prepares
bytecode before measuring, checks instruction presence in the disassembly,
and records commands, source and executable hashes, individual batches and
process medians. Run it without simultaneous builds, tests or other benchmarks:

```sh
python3 bench/vm/lookup_constant/run.py \
  --before build/lookup-const-before/amberc \
  --after build/amberc \
  --processes 5 --output build/lookup-const-atoms
```

The full row includes the loop and register writes. Qualified minus local is
a paired comparison of two bytecode paths, not a pure instruction timer.
The before binary must be saved before rebuilding the runtime.

The driver is a standalone diagnostic; Python is not required to build or
execute the Amber benchmark. To run just the Amber program:

```sh
build/amberc run bench/vm/lookup_constant/manifest.json -- \
  --iterations 300000 --repeats 5
```
