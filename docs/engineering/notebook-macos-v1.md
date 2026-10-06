# Amber Notebook for macOS — first native host

The first GUI is a **native SwiftUI/AppKit application**, not a browser or an
Electron wrapper. `NSTextView` supplies syntax-colored source editing, rich-text
blocks, selection, clipboard, native undo and two-space Tab insertion in code.
The compiled C++ notebook
kernel, compiler and runtime run in the same process through a small C ABI by
default; the isolated-worker preview below moves sheet execution into a child process.
This is an opt-in development app, not a signed/notarized public release.

## Build and launch

Requires macOS 14+, Xcode Command Line Tools with Swift, and OpenSSL **3.x**
development libraries. The native package currently assembles the build
machine's architecture; it is not a universal binary.

```sh
make -j4 notebook-macos
open "build/Amber Notebook.app"
open -a "build/Amber Notebook.app" tools/notebook-macos/Samples/Welcome.amberbook
open -a "build/Amber Notebook.app" "tools/notebook-macos/Samples/Rich Text.amberbook"
make test-notebook-macos
```

### Isolated execution preview

```sh
open -n "build/Amber Notebook.app" --args --isolated-worker
make test-notebook-worker
make test-notebook-worker-ui
```

The preview banner is visible only in this mode. Run / Run All / Apply on a sheet
use a lazy, persistent worker process per sheet. **Stop** (also ⌘.) requests
cooperative cancellation, including child tasks and ensure cleanup. **Force Stop**
requires confirmation and terminates that worker; completion waits for confirmed
process exit. **Restart Worker** resets runtime state without evaluating anything;
the next explicit Run creates a fresh worker. No automatic replay occurs.

Source/rich-text editing stays enabled during execution. Save, structure changes
and tab switching wait until it finishes. New drafts—including reverting to the
pre-run text—survive late snapshots and Force Stop. Retained results are marked
stale after forced termination. Only the host saves documents; the worker does
not write source or project metadata. The helper executable and its libraries
are included in the development `.app` bundle.

This preview is **not the default backend**. Module-editor Run, projects with
inputs, and board input execution are explicitly rejected; there is no silent
in-process fallback. Ordinary static cell dependencies still work during Run,
but automatic idle runtime-watch pumping has not moved to the worker yet.
Apply uses saved module sources and rejects unsaved module tabs. Cross-sheet
invalidation after Apply is conservative. Outputs are terminal snapshots, not
live streaming/progress bars; source synchronization is currently limited by the
8 MiB command-frame budget. iamber remains on its existing backend.

When `../amber-plot/native/plot_png.c` exists, the build also links amber-plot's
2D/3D PNG renderer into the application. Override the checkout location with
`AMBER_PLOT_DIR=/path/to/amber-plot`. This is a bundled renderer, not a runtime
download, plugin installation, or blanket FFI grant. The Amber module source
is linked from its package directory, not copied into `modules`. Generate an example with:

```sh
python3 tools/notebook-macos/make-plot-sample.py build/notebook-macos/Plots.amberbook
open -a "build/Amber Notebook.app" build/notebook-macos/Plots.amberbook
```

Run All in that project renders two panels. The generator refuses to overwrite
an existing project. Without the renderer checkout, the rest of the native app
still builds; PNG rendering reports amber-plot's unsupported-backend error.
Use `--bundle` only when an explicit copy of the Amber sources is wanted.
See [directory dependencies](notebook-dependencies-v1.md) for the shared
iamber/macOS import workflow and native-extension limitations.

The ordinary CLI/Linux build does not depend on Swift, AppKit or this target.
The `.app` contains the executable, its non-system dylibs, and a copy of the
build machine's system CA bundle. Dylib install names are relative to the app,
not Homebrew. `SSL_CERT_FILE`, if already supplied by the user, takes precedence.
Certificates are a build-time snapshot, not a live macOS Keychain integration.
Packaging applies an **ad-hoc development signature**. Distribution still needs
release signing, notarization, licensing review and testing on the target OS.
Packaging raises the bundle's minimum OS to the highest deployment target of
its executable and bundled libraries. The current machine's Homebrew OpenSSL
was built for **macOS 26**, so this particular `.app` requires macOS 26 despite
the UI's 14.0 deployment target. Building for older Macs requires compatible
OpenSSL binaries too. Only the current build host has been verified.

The app declares `org.amberlang.notebook-project` and the `.amberbook` package
document type. Finder can treat the project directory as one document after
registering the app. No document migration is required: iamber and the GUI
open the same [`project.json` format](notebook-project-v1.md).

## Working in the app

The first [board implementation](notebook-boards-v1.md) adds a **Boards** section,
a grid/property designer, typed project inputs, value/figure bindings and
explicit cell-run buttons. Saving a board/input opts the project into schema
version 2 (or retains version 3); existing sheet/module-only documents remain version 1.
Linking an external package opts into version 3.

- **New / Open** use native file panels. Opening reads source; it never runs
  cells or initializes module code. The sidebar selects stable sheet/module
  sessions without saving or executing them.
- **Save / ⌘S** saves the active tab; **Save All / ⇧⌘S** saves every modified
  buffer. Unsaved changes, including drafts in inactive tabs, are checked on
  Open, New, Close and Quit. A conflict with an external editor is reported;
  it never silently overwrites the external edit.
- **Run Selected / ⌘Return** evaluates the selected cell and its invalidation
  plan. **Run All / ⇧⌘Return** explicitly includes Manual cells. Watch/Manual
  policies and stale dependencies are the same as in iamber. Editing does not
  execute source automatically in this first UI.
- Cells show results, stdout/stderr, diagnostic text and local variables.
  Add, move and delete operate on stable CellIds. Delete asks for confirmation;
  structural cell edits are only persisted by Save. Code collapse is currently
  view-only, and is not stored in the project.
- **Add text block / ⌥⌘T** inserts an inert, editable text block. Its toolbar
  supports body text, three paragraph heading sizes, inline code, bold, italic,
  underline, bullet and numbered lists, and native editable tables. Headings
  and lists operate on whole selected paragraphs, including at an empty caret.
  Enter continues lists; Enter on an empty item exits. Tables support row/column
  insertion and removal; Tab/Shift-Tab navigate cells, Enter adds an in-cell line
  break, and **Continue after table** leaves the table. Paste inserts plain text; web content,
  attachments and arbitrary RTF are not imported. Source and formatting save
  together, including style-only changes, and formatting supports Undo/Redo.
- Text templates accept `{{name}}` (optional surrounding whitespace), or use
  **Insert variable / {}** for a searchable list of published variables.
  **Preview** substitutes values; **Edit** retains placeholders and its Undo
  stack. Only assignments in preceding code cells of this sheet are visible,
  with the nearest definition winning. Strings display without debug quotes.
  Missing values show `unavailable`; edits, failed or dirty preceding cells and
  stale environments mark cached values `stale`. Run code to refresh them.
  Values update with runtime snapshots, including Watch updates. No expressions,
  recursive substitution or code execution occur in text; resolved values are
  never saved into the template. Each displayed value is capped at 4096 characters.
- Code and module editors color Amber keywords, numbers, strings, comments and
  identifiers as you type, including incomplete source. Highlighting is lexical;
  it does not compile or run code and does not modify the source document.
- The sidebar's **+** creates a sheet or bundled module using an identifier
  `[A-Za-z_][A-Za-z0-9_]*`. These are explicit, immediately saved **project
  structure** operations, independent of other unsaved source buffers.
- Modules have a full-profile editor for `class`, `def`, `export`, etc. Their
  **Run Isolated** does not install definitions into sheets. To use definitions
  in a sheet: save the module, enable **Auto-import**, select the sheet and
  **Apply Modules / ⇧⌘A**. Apply uses saved module files, refuses unsaved module
  buffers, and follows the existing transactional environment restart rules.
- **Dependencies → +** links a local package directory without copying it.
  Apply, then use `import plot` / `from plot import figure` in cells. The same
  imports can select bundled modules instead of ambient Auto-import. Dependencies
  can be unlinked through their context menu; external package files are never
  deleted. Apply refreshes external source edits. See the dependency guide above.

Text blocks have portable plain-text source plus versioned UTF-16 style ranges;
they are not HTML, RTF, or executable cells. iamber displays their plain text
read-only while running the surrounding code normally. Editing or moving text
does not invalidate code or reinitialize modules. iamber shows interpolation
templates literally; native Preview resolves them. The shared format preserves unknown cell/UI metadata. Unsupported cell kinds
are read-only and block execution of their sheet; they are not reinterpreted as
code or silently discarded. Native source editors require valid UTF-8 without
embedded NUL; incompatible source files fail opening instead of being rewritten
lossily. Runtime output can contain arbitrary bytes and is made display-safe.

## Ownership and scheduling

`NotebookBridge.cpp` adapts `ProjectTabs`, `Session`, and the existing checked
project persistence APIs. It does not maintain a second dependency engine.
Only UTF-8 JSON value snapshots cross the C/Swift boundary; heap Values and
borrowed Session pointers never cross it. Every handle operation, destruction
included, belongs to one serial worker queue. Compiling/running does not block
the AppKit main thread. UI mutation controls are disabled during explicit work,
except draft editing in isolated-worker preview. Its independent control handle
contains no Session/UI pointers and can outlive the host safely.

Editor drafts are keyed by `(tab ID, CellId)` and flushed in order before an
explicit command. A failed save retains them as modified Session buffers; a
failed source submission retains the unsubmitted Swift draft. Result snapshots
cannot overwrite newer drafts. Editor views are keyed by CellId, not position,
so reordering does not attach another cell's undo history to the source.

In the default backend, a `DispatchSourceRead` observes the project activity mailbox. Only the C++
mailbox consumer drains its borrowed descriptor. The bridge services at most
one bounded round-robin runtime pump per turn; `RuntimePumpDispatch` provides
coalescing and recovery deadlines. No timer runs at genuine idle. While Swift
code drafts differ from the installed sources, automatic pumping is suspended; after
submission, the existing Session source/environment barriers take over.
Inert text drafts do not pause runtime watchers. An unrepresentable native editor
buffer is marked unsaved and blocks Save/tab replacement until corrected or
undone; it is never silently replaced by the last serializable snapshot.
Replacement cancels/unregisters the old dispatch source before destroying its
host, with balanced suspend/resume. Shutdown completion follows destruction.

Save All is **not one multi-file transaction**: successful earlier tab saves
remain committed if a later file conflicts. Module initialization can have
external side effects; the runtime transaction does not roll those back.
The host grants no extra runtime capabilities and does not claim App Sandbox
isolation. Documents should still come from trusted sources before execution.

## Figures with amber-plot

`notebook` is a builtin; it does not need an import. `figure` / `figure3d` come
from the `plot` module's exports, using the project's normal bundled-module
auto-import and Apply flow. For example, in a code cell:

```amber
p = figure(width: 800, height: 540).line([[0, 0], [1, 1], [2, 4]])
q = figure3d(width: 800, height: 540).surface([[0, 1], [1, 0]])
notebook.show(q, caption: "Surface", order: 2)
notebook.show(p, caption: "Curve", order: 1)
```

- `notebook.show(obj, caption: null, order: 0)` returns `null` and stages a
  PNG panel. It supports amber-plot `Figure`, `Figure3D`, and PNG `Rendered`
  objects. Figures render through their normal `bytes(format: :png)` method;
  explicitly rendered SVG objects are not embedded in this version.
- All calls in one successful cell form one figure, sorted by ascending order.
  Multiple panels get `a)`, `b)`, … labels and individual captions. Equal keys
  retain invocation order; omitted order is `0`, so omitting every key retains
  invocation order too. `caption` accepts a string or `null`.
- Keys support finite Int/Float, Bool, Str, Symbol and lexicographic List/Tuple
  values, including `desc(...)`. Keys in the same figure must be mutually
  comparable; mixed incomparable types are an error. Custom object comparators
  and arbitrary heap objects are not used as keys. Keys are copied at the call,
  so subsequent mutation of a list key cannot reorder earlier panels.
- Clicking a panel opens a lightbox with previous/next, zoom/Fit, **Save PNG…**,
  and the native **Share…** menu. Save writes the original PNG bytes. A lightbox
  retains its opened snapshot if a Watch reevaluation produces a new figure.
- The batch is run-local and is published only after the cell and its slot
  transaction succeed. A rerun replaces the complete batch; a successful run
  with no `show` clears it. Failure keeps the last successful figure marked
  stale, including across a source-triggered environment rebuild. Calls from
  detached/parallel work, module initialization, or ordinary non-notebook
  execution are rejected: `show` requires an active synchronous notebook cell.
- Only immutable PNG bytes, dimensions and caption text reach Swift, never
  Amber object pointers. ImageIO checks the image format and dimensions before
  display. Limits are 8192 pixels per side / 16 megapixels, 16 MiB per PNG,
  64 panels / 64 MiB per cell, and 64 KiB per caption. This is bounded display
  output, not a disk cache or a general object display protocol.

Figure output is transient and is not saved into `project.json`. Reopening a
notebook requires running its cells again. iamber shares the runtime API but
does not display images; this build integrates the PNG renderer specifically
into the macOS host, not the standalone CLI executable.

## Deliberate first-version limits

One project window, code/module editing, textual results and PNG figures. Rich text has a
small portable style vocabulary, not a full word processor: no nested lists,
merged cells or nested tables. Interpolation is name-only and sheet-local;
module imports and arbitrary expressions are not picker sources. A Delphi-like component
designer is not yet implemented. There is no interrupt/Stop for running Amber
code: a long/infinite computation leaves the UI responsive but its runtime
queue occupied, and closing/replacing waits for it to finish (or requires an
OS-level force quit). There is no autosave/recovery journal or external-change
merge UI yet. Source collapse and editor undo are not persisted across reopen.

## Verification

`make test-notebook-macos` builds the app, exercises the C++ bridge and the Swift
model and rich-text/syntax editors, then launches a real native window with a
temporary project containing formatted text and two code cells.
The GUI smoke test evaluates `x = 21` → `x * 2`, checks `42`, renders the hosted
view to PNG, and verifies that execution did not save the source document.
Regression tests cover Unicode style ranges, style-only saving, Undo/Redo,
malformed formatting, lexical edge cases and inert text in mixed sheets.
Rendering uses the view itself, not screen recording or accessibility permission.
With the amber-plot checkout present, the same target also checks real 2D/3D
rendering, stable panel ordering, fault rollback, empty-batch replacement,
dimension/type errors, UTF-8 captions and PNG decoding, then renders the plot
sample in a second native-window smoke test. `make test-notebook-plots` runs
just this integration slice. `build/binder_tests` covers `self`, `fs` and
`notebook` name resolution needed by the bundled library.
The AppKit/model tests require a logged-in macOS desktop session. A process
sandbox that denies application registration can abort inside macOS before the
test code runs; execute these tests from a normal local terminal in that case.
