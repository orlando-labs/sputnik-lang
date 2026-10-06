# 2D plot views in Amber Notebook

Implemented 2026-10-03. This is data-coordinate zoom, not image magnification.

## User contract

```amber
import plot
p = plot.figure().line([[0, 0], [1, 1], [2, 4], [3, 9]])
detail = p.view(x: [1, 2.5], y: [0, 7])
notebook.show(detail, caption: "Selected interval")
```

`Figure.view(x: null, y: null)` returns a new figure. Omitted axes retain their
current domain (explicit, or computed from source data). Numeric pairs must
be finite and increasing. Dimensions, layers, styles, labels, aspect policy
and Y orientation are retained. SVG and PNG clip layers to the plot rectangle
and recalculate axes. Annotation labels are omitted if they do not fit whole;
SVG/portable PNG use a conservative text-width estimate, AppKit uses measured
native font bounds. Colorbars describe the original layer values, not a new
normalization of the selected region. `vline`/`hline` span the selected domain.

The macOS sheet and board figure viewer offer:

- Drag a rectangle inside a 2D plot to select a data interval (either direction).
- The corner **…** menu opens **Axes…** (explicit X/Y limits) or **Open…**.
  Axes settings are hidden initially; **Apply axes** or Return applies them.
- **Reset axes** / double-click restores the current frame's original domain.
  The inline Reset link appears only while a different view is selected.
- Click / **Open…** opens the lightbox. Its corner **…** menu contains axes
  settings, **Save PNG…**, and **Share…**; raster figures put image zoom there.
- Escape cancels an in-progress selection. Invalid or numerically
  indistinguishable intervals are rejected without changing the plot.

There is no persistent action toolbar or drag-instruction footer below an
unzoomed figure. The interaction hint is available as a tooltip and accessible
description; open settings expose only their active editing controls.

These controls do not invoke Amber, rerun a cell, or mutate its graph object.
Explicitly selected ranges survive live frame updates. Reset resumes the
latest frame's domain. An open lightbox receives new frames as well; its range
is independent of the inline viewer after opening. Panel identity currently
follows its position in the figure, so producers should keep live panel order
stable. UI ranges are session presentation state, not project content. To save
a reproducible range, express `view(...)` in a cell/module.

## Boundary and resource limits

`notebook.show` still publishes validated PNG bytes. A `plot.Figure` exposing
`notebook_scene()` additionally supplies a v1 JSON scene: original data domain,
pixel transform, layout settings, a closed vocabulary of geometry/text
commands, semantic guide/annotation anchors, and colorbar decorations. It
contains no live VM handles, executable code, paths, or callbacks.

The renderer maps source pixels back to data, then into the selected domain.
It regenerates ticks and guides, and clips geometry before handing paths to
Core Graphics. Screen-pixel stroke widths, marker sizes and label offsets are
retained. PNG export uses the same AppKit renderer as the interactive canvas;
font rendering may differ from the package's portable PNG backend.

Runtime preflight accepts only inert scalars/arrays/maps, checks depth/node/
estimated JSON budgets and finite values. Scene transport is capped at 4 MiB
per panel; oversized scenes are omitted and PNG remains usable. This bounds
publication, not the allocation of the scene while Amber constructs it. The
host separately validates version, command arity/types/colors, geometry,
layout, node count and numeric ranges. Malformed/unsupported scenes fall back
to the raster viewer. Image and scene bytes both count against retained output
budgets, including live replacement accounting.

Worker framing is version 8; presentation schema is
`amber.notebook.presentation.v5`. PNG and scene replace atomically in the same
immutable output snapshot. Older workers are rejected by the normal handshake.
Older plot packages without the method, `plot.Rendered`, and `Figure3D` remain
raster-only. 3D camera manipulation, log/category axes, persistent UI ranges,
pan/history and independently editable color normalization are not in this
increment.

## Verification

`make test-notebook-plots` exercises the real external plot package, captures
its scene through the bridge, decodes it with the Swift renderer, checks
coordinate inverses, reversed Y, equal aspect, bad inputs, clipping, mouse
events and PNG export, and renders a temporary sheet/board in the app.

To exercise the isolated-worker route as well:

```sh
python3 tests/notebook_macos_plot_ui_smoke.py \
  'build/Amber Notebook.app/Contents/MacOS/AmberNotebook' \
  ../amber-plot/src/plot.am --isolated-worker
```

The smoke checks require a decodable interactive scene, not merely a PNG.
`notebook_worker_tests` covers scene transport/limits; `notebook_live_tests`
covers immutable replacement and budgets. The plot package selftest now
returns **32** and includes view immutability, one-axis chaining, SVG/PNG,
annotation visibility, reversed Y scene metadata and very long dashed guides.
