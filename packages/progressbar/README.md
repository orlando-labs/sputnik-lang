# progressbar

`progressbar` is a host-neutral Amber package for long-running loops. It
renders a tqdm-like line to logical stderr and publishes the same absolute
state to the optional notebook live receiver.

```amber
from progressbar import ProgressBar

bar = ProgressBar(total: 100500, desc: "Training", throttle: 0.2)
bar.inc!()
bar.inc!(5)
bar.end
```

`throttle` is seconds between ordinary updates. Start and end snapshots always
flush. `total: null` keeps the total unknown: the bar shows a count and never
invents a percentage. `end` is idempotent and never changes the count.
`inc!` has an optional argument, so Amber requires `inc!()` for its default
increment; `end` is syntactically nullary and can be called bare.

The lazy adapter counts successful block completions and closes the bar on
normal return, error, or cancellation:

```amber
from progressbar import ProgressBar

(1..100).with_progress(desc: "Processing", throttle: 0.2).each |item|:
  process(item)
```

`with_progress(source, ...)` is also available when a source does not expose
the convenience method. Import it with `from progressbar import with_progress`.
Array, Tuple, Set and Range sources advertise their
known size; other sources remain unknown without exhausting a lazy or infinite
iterator. The VM convenience method forwards to the imported Amber
`ProgressEnumerable`; iteration and counting stay in Amber. These hooks are
currently VM APIs, not standalone native-codegen APIs.

The notebook hook has the following intentionally small contract:

```amber
notebook.progress("progress-1", 12, 100, "Training", false)
```

The hook returns `false` outside a notebook so the package renders to stderr,
or `true` when accepted by the notebook receiver. Payload errors and closed
runs are errors, not silently discarded. At this low-level boundary, a total
of zero means indeterminate. Ending early never displays a false 100%.
