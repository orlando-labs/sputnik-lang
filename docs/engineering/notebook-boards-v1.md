# Notebook boards: first vertical slice

Boards are a third project entity beside sheets and bundled modules. They are
inert UI descriptions, not a second VM or a source generator. The native host
renders a responsive ordered grid, with a simple component/property editor.
iamber preserves boards and can drive the same inputs without rendering them.

## Try it

```sh
make -j4 notebook-macos
python3 tools/notebook-macos/make-plot-sample.py build/notebook-macos/Interactive.amberbook --board
open -a "build/Amber Notebook.app" build/notebook-macos/Interactive.amberbook
```

The generator requires the local amber-plot checkout and refuses replacement.
An input/value/button example without amber-plot is included at
`tools/notebook-macos/Samples/Inputs.amberbook`.
Choose **Boards → Interactive Curve → Run Sheet** once. Changing Gain and
pressing Apply updates the computed value and figure. Caption edits update the
same figure. The Manual snapshot stays stale until its explicit button runs.
Images retain the sheet gallery's lightbox, PNG save and share actions.

For a new board, use **+** beside Boards. Choose its controller sheet, columns,
then add Input, Text / Value, Figure, or Run Button components. Reorder with
arrows; edit their titles and bindings. **Save Board** persists this structure
immediately, independently of unsaved sheet/module sources. Unknown metadata
on surviving board/component IDs is retained by the backend without numeric
rounding. Cancel discards the board draft. This is a property-based grid
editor, not yet a drag-and-drop/free-position canvas.

**New Project Input** defines a named Number, Integer, Boolean or String value.
This is a separate explicit structure save, so cancelling the enclosing board
editor does not undo input creation. Current input values are session state:
Save does not turn them into defaults, and reopening restores defaults.

## Execution contract

```amber
gain = notebook.input("gain")
value = gain * 42
p = figure().line([[0, 0], [1, gain], [2, 4 * gain]])
notebook.show(p, caption: "Response")
value
```

- Inputs are shared at project scope. Every sheet has its own runtime; only
  immutable portable scalars cross this boundary, never heap Values.
- `notebook.input(key)` records actual synchronous reads in a run-local closed
  capture. Conditional reads replace the previous dependency set only after a
  successful output transaction. Failed runs retain their last successful
  dependencies and retry conservatively on the next changed input.
- Changing an input invalidates readers and their static downstreams. Watch
  consumers run in graph order; Manual cells and dependents become stale.
  A successful empty capture removes the input edge. Equal publication is quiet.
- Opening, defining inputs or saving boards never starts execution. Changing
  an input does not initialize an unstarted sheet. Edited source and stale
  module environments require explicit evaluation/Apply; input events do not
  silently execute uncompiled drafts.
- A root invocation sees one immutable input snapshot. Detached/resumable or
  foreign-thread execution cannot read that invocation's inputs. Read the
  needed value synchronously before passing it to asynchronous work.
- Values are validated before publication: finite numbers, exact signed Int64,
  Boolean, or UTF-8 strings up to 64 KiB without NUL. Optional numeric bounds
  are enforced. Input errors retain the old value. Runtime failures after an
  accepted edit are shown on the cells; they do not roll the input back.
- Figures bind to the **published display batch** of a cell, not to a borrowed
  plot object. Text binds to that cell's expression result or a named assignment.
  A missing/deleted cell is a visible broken binding, never an implicit rebind.
- Buttons explicitly run a controller cell using normal notebook evaluation.
  They are not yet module action handlers. Runtime work uses the host's existing
  serial queue, off the main AppKit thread. Cancellation is not added here.

iamber's F3 accepts `set-input gain 2.5`, `set-input enabled false`, or
`set-input label "Caption"`. `--project-info` lists boards and input definitions;
`--run-sheet` uses project defaults. This first terminal UI does not render or
edit the board layout.

## Project format

Version 1 projects stay version 1 until an input or board is saved. Version 2
requires `inputs` and `boards` arrays. Existing cells, modules and auto-imports
keep their format and behavior. Sources remain in `project.json` and `.am`
files; runtime values, images and dependency captures are not persisted.

```json
{
  "inputs": [{"id":"gain","title":"Gain","type":"number","default":1.0,"minimum":0,"maximum":10}],
  "boards": [{"id":"dashboard","title":"Dashboard","sheet":"main","columns":2,
    "components":[
      {"id":"gain_control","kind":"input","title":"Gain","input":"gain"},
      {"id":"result","kind":"text","title":"Result","cell":"1","binding":"value"},
      {"id":"figure","kind":"plot","title":"Curve","cell":"1"},
      {"id":"refresh","kind":"run","title":"Refresh","cell":"2"}
    ]}]
}
```

This is a fragment of a version 2 manifest, not a complete project. Cell IDs
are decimal strings. Board/component IDs are ASCII identifiers. Columns are
an upper bound (1–6); narrow windows wrap to fewer columns. Limits are 1024
inputs, 128 boards and 256 components per board. The bridge sends input values
as JSON **strings containing scalar JSON**, avoiding Int64-to-Double conversion.

## Next increments

1. Named action handlers exported by bundled modules, with explicit invocation
   context and parameter changes; cancellation and status before timers.
2. Lifecycle-owned periodic procedures: serial/coalesced ticks, pause on hidden
   or closed projects, errors surfaced in the UI; no implicit startup on open.
3. Tabs, table views, nested layouts, sizing/spans and drag-and-drop placement.
4. Parameter/default editing/removal, binding pickers, undo for the designer,
   and presentation mode. No arbitrary expressions in the binding format yet.

## Verification

`make test-notebook-macos` includes project/input tests, native model tests and
an isolated board smoke scenario when amber-plot is available. The smoke test
changes Gain, checks the numeric result and a newly rendered PNG, and verifies
that the manifest was not rewritten. Existing VM/kernel tests remain separate.
