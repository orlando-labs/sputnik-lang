# Notebook project v1 — shared document, not a runtime snapshot

Compatible extensions: [boards and project inputs (v2)](notebook-boards-v1.md),
[external directory dependencies (v3)](notebook-dependencies-v1.md).
Bundled modules are project-owned code; external packages are linked separately.

Implemented: a portable project document, persistent sheet/module tabs,
interactive structure commands, a full-profile bundled module editor, and
explicit application of bundled exports to notebook cells. The format is
independent of curses, VM heap objects and a future native UI. Definitions
remain outside notebook code cells. In-place migration of live module instances
is not implemented.

The first [native macOS host](notebook-macos-v1.md) opens this same format with
SwiftUI/AppKit. `make notebook-macos` produces an actual `.app` declaring the
package document type; the earlier iamber plist remains a declaration fragment.

## Opening and saving

From the repository root:

```sh
./build/iamber --new-project scratch.amberbook
./build/iamber --project scratch.amberbook
./build/iamber --project scratch.amberbook --sheet main
./build/iamber --project-info scratch.amberbook
./build/iamber --project scratch.amberbook --module models
./build/iamber --project scratch.amberbook --check-module models
./build/iamber --project scratch.amberbook --run-module models
./build/iamber --project scratch.amberbook --run-sheet main
```

`--new-project` creates a new directory and exits; it never overwrites an
existing directory. `--project` (or a positional directory argument) opens the
project's sheets and declared modules as tabs. **F7/F8** selects the previous/
next tab, wrapping in manifest order: sheets first, then modules. Switching
preserves source, cursor, edit mode, results and the existing backend; it never
saves or evaluates code. `--sheet`/`--module` selects the initial tab without
changing the document's default `active_sheet`.

**Ctrl-S/F4 saves only the active tab.** A sheet save merges that sheet into the
last saved document; other tabs' unsaved buffers are not persisted or replaced.
The tab strip marks modified buffers with `*`. Quit warns about unsaved changes
in **any** tab, including inactive ones; uppercase Q at that prompt explicitly
discards all of them. A save failure keeps the editor open. No implicit autosave
or on-open evaluation occurs.

**F3 Project** opens a command prompt in either navigation or edit mode:

```text
new-sheet analysis My analysis
new-module models
auto-import models on
auto-import models off
```

Enter explicitly saves the requested **structure change**, without saving any
existing tab's unsaved source. Esc cancels; Backspace edits the input and Ctrl-U
clears it. New IDs must match `[A-Za-z_][A-Za-z0-9_]*`; sheet titles may contain
spaces and Unicode. `new-sheet` creates and selects a sheet with one empty Watch
cell, after other sheets and before modules. Its title defaults to the ID.
`new-module` exclusively creates `modules/<id>.am`, initially containing only
`package <id>`, and selects its full-profile editor at the end of that line.
Existing files and IDs are never replaced. Creating a module does not enable
auto-import; `auto-import <id> on` appends it to the ordered ambient roots, and
`off` removes it. The selected module's tab strip shows the saved setting.

These commands preserve other Sessions, buffers, results and runtime grants,
and do not change the default `active_sheet` or the applied environment
generation. New sheets start from the last applied source snapshot. Changed
auto-import settings take effect only on **F6 Apply**; an already running sheet
keeps its current environment until then. Quitting with Q discards unsaved
buffer edits, not structure commands that Enter already saved. Renaming,
removing and reordering sheets/modules remain manifest operations for now.

The --module command initially selects a declared .am source as a full-profile
editor buffer: class, def, imports and exports are accepted there. Ctrl-S/F4
atomically saves source without compiling or executing it. Enter/Ctrl-X
explicitly runs the current buffer in a fresh isolated module runtime.
--check-module only compiles and verifies bytecode; --run-module performs the
same explicit isolated run without curses. Opening and checking never execute
module source. An isolated run is useful today for defining and exercising a
class inside its module:

    class A:
      def init(@a)
    export A
    A(7)

The module editor's isolated Run is deliberately reported as “not installed
in sheets”. To make `A` available in a sheet, save the module, use F3
`auto-import models on`, switch to the sheet with F7/F8 and use
**F6 Apply**. Save and Apply are separate actions; Apply refuses while any
module tab has unsaved edits, including a module that is not auto-imported.

## Package layout

`project.json` is the single atomic document: it contains the manifest and
ordered sheets. Bundled module sources are ordinary project-relative `.am`
files; resources may live beside them. There is no required `Contents/`
hierarchy and no embedded VM snapshot. Keeping all sheets in one JSON file
avoids partially committed multi-file notebook edits in v1. Module source
saving and environment application are separate transactions, not part of
Save Sheet.

Minimal schema (all shown top-level fields are required):

```json
{
  "format": "amber-notebook",
  "version": 1,
  "title": "Scratch",
  "active_sheet": "main",
  "sheets": [{
    "id": "main",
    "title": "Main",
    "cells": [{
      "id": "1",
      "kind": "code",
      "source": "x = 21\nx * 2\n",
      "mode": "watch"
    }]
  }],
  "modules": [],
  "auto_imports": []
}
```

- Cell IDs are canonical positive decimal **strings**, globally unique across
  the project, including opaque cells. `UINT64_MAX` is reserved. IDs are never
  derived from cell position; loading reserves every ID before new allocation.
- A code cell requires `source` and `mode` (`manual` or `watch`). A `text` cell
  requires plain-text `source` and accepts optional versioned `formatting`.
  Other kinds interpret only `id` and `kind`; their other fields are opaque JSON.
- Unknown members at document, sheet, module and cell level retain their JSON
  values, including large numeric literals. Whitespace/member ordering can
  normalize; this is a semantic round trip, not a byte-for-byte formatter.
  Unknown fields in schema v1 must not redefine execution semantics; such a
  change requires a version/capability contract or a new cell kind.
- Unsupported schema versions, duplicate keys/IDs, malformed Unicode/JSON,
  excessive nesting and oversized documents fail explicitly. The document
  limit is 16 MiB; large resources belong in separate files.
- Runtime results, exceptions, locals, watch cursors, VM generations and
  evaluation dirtiness are not written. Existing opaque cache/UI fields are
  preserved but not trusted or restored as runtime state.

iamber displays `text` cells as read-only plain text, preserving formatting on
save. They never participate in compilation, dependency graphs or execution;
inserting, editing or moving text cannot make code stale or restart modules.
The native macOS host creates and edits these blocks. iamber displays unknown
cell kinds as read-only placeholders. It cannot edit, delete or reinterpret
non-code cells. Since an unknown kind may supply executable
inputs, **the entire sheet's evaluation is blocked** until the host supports
its semantics. Preserving an unsupported feature must not silently execute a
different program.

### Portable rich text

```json
{
  "id": "2",
  "kind": "text",
  "source": "A notebook\nHello, Amber!",
  "formatting": {
    "version": 1,
    "runs": [
      {"start": 0, "length": 10, "style": "heading1"},
      {"start": 11, "length": 5, "bold": true}
    ]
  }
}
```

Offsets and lengths count UTF-16 code units, matching native editor selections,
but cannot split a Unicode surrogate pair. Runs are sorted, non-overlapping and
nonempty, within the source. Uncovered text uses body style; missing `style`
defaults to `body`, and missing `bold`, `italic` and `underline` default to false.
Supported styles are `body`, `heading1`, `heading2`, `heading3` and `code`.
Omitting `formatting` means plain text. A formatting object requires `version`
and `runs`; unknown formatting versions or attributes are rejected to prevent
lossy editing. At most 20,000 runs fit within the overall 16 MiB document limit.
Text and formatting are validated and published together. There are no embedded
HTML/RTF blobs, attachments or persisted runtime values.
Unknown **outer** cell fields still round-trip unchanged.

Formatting version 2 retains `runs` and adds optional `paragraphs` and `tables`:

```json
{"version":2,"runs":[],
 "paragraphs":[{"start":0,"length":8,"style":"heading1"}],
 "tables":[{"start":8,"length":4,"columns":2,
            "cells":[{"start":8,"length":2},{"start":10,"length":2}]}]}
```

For example the source above can be `Heading\nA\nB\n`. Paragraph ranges cover
whole LF-delimited paragraphs and optionally carry `style` (`body`, `heading1`
through `heading3`) and `list` (`bullet` or `numbered`). List markers remain in
the portable source as `•\t` or `1.\t` etc.; the editor normalizes numbering and
renders hanging indents. Table ranges cannot overlap paragraph records. Cells
partition their table consecutively in row-major order, end with LF, and form
a rectangle: 1–12 columns, at most 240 cells/table and 100 tables/document.
In-cell soft line breaks use U+2028. Runs plus paragraph records are limited to
20,000. Source and structural metadata are validated atomically. Version 1
remains readable; plain inline-only content still uses version 1.

`{{name}}` interpolation templates are ordinary source text, not saved values or
executable expressions. Native Preview resolves published variables from earlier
code cells; iamber preserves and displays the templates literally. Opening a
project never evaluates code to resolve a template.

## Bundled environment: Save, Apply, Run

A module entry has an `id` (identifier) and a `path` (for example
`modules/models.am`). `auto_imports` is an ordered list of unique module IDs
that opt into the interactive environment. Neither directory discovery nor
loading the manifest executes these files. Listed files must exist as regular
project members; Save Sheet only touches `project.json`, not their contents.

For example, F3 `new-module models` followed by `auto-import models on` creates
the source file and saves these entries in the manifest:

```json
"modules": [{"id": "models", "path": "modules/models.am"}],
"auto_imports": ["models"]
```

`modules/models.am` can contain:

```amber
class A:
  def init(@a)
  def value(): @a
export A
```

The sheet can then evaluate `a = A(7)` and, in another cell, `a.value()` without
an explicit import. The manifest module ID supplies the compilation identity;
an optional `package` declaration must match it. Only selected public exports
are ambient names. A preceding notebook binding shadows an ambient name, with
the usual stale/manual dependency barrier. Duplicate ambient export names are
errors, not order-dependent overrides.

F6 Apply rereads bundled source files, compiles selected roots and their
transitive imports, links a candidate image, compiles the current sheet against
its export map, and initializes the modules in dependency order. Unrelated
dependency-only files are not compiled. Missing dependencies, cycles, unsupported
link forms and export conflicts fail before initialization. Modules imported as
dependencies are not implicitly auto-imported into the sheet. The shared linker
supports named class/function imports; bundled namespace aliases and reexports
are currently rejected explicitly.
The existing bytecode compiler exports classes and functions, not arbitrary
top-level values. Such values can remain private captured state of an exported
function; auto-import does not extend the language's export kinds.

The active backend and successful cell results survive a compilation, linking,
or module-initialization failure. Successful initialization publishes a **fresh
world generation**: all old slots/instances are discarded, Watch cells run, and
Manual cells remain stale. Watch cells behind a stale Manual provider remain
blocked until that provider is explicitly run. Even body-only module changes
take effect. This is a restart policy, not hot class-layout migration.
The commit boundary is successful module initialization, before automatic cell
execution. A subsequent cell fault or host cancellation leaves the new
environment applied, with failed/unrun cells available for explicit retry.
It does not restore the old environment after some new cells have already run.

Enter/Ctrl-X runs the selected cell; R/Ctrl-R runs all cells. The first explicit
evaluation prepares the opened environment. Editing a composite sheet also
requires a fresh world at the next explicit evaluation, using the currently
loaded bundled sources. Use F6 to reread saved module changes. Navigating away
from an edited cell in a bundled project does not implicitly initialize modules.
Neither opening nor saving executes source.

Each sheet owns its own lazily initialized runtime and heap; a module tab's
isolated Run does not replace any sheet runtime. The project host tracks a
generation of **saved bundled sources**, initially shared as source snapshots.
A successful Apply of changed sources advances that generation and updates
only the active sheet. Other sheets retain their results but display `!env`
and `environment changed; F6 Apply this sheet`. Both ordinary Run and runtime
Watch processing are deferred there until explicit Apply succeeds. This also
applies to sheets not yet evaluated, so visiting an unopened sheet cannot
silently initialize an older environment. Applying the same saved snapshot in
another sheet catches it up without invalidating already-current sheets.
Saving a module alone does not advance the applied generation. Failed Apply
does not advance it or invalidate other sheets.

`--run-sheet [id]` explicitly initializes the environment and runs **all** cells,
including Manual cells, prints module/cell output, and exits nonzero on failure.
It does not save runtime results. Omitting the ID selects `active_sheet`.

Initialization executes ordinary Amber code. The transactional guarantee covers
the notebook's world and published results, **not external side effects** such
as files or network writes; failed initialization can already have performed
them. Apply and composite source edits may repeat initialization. Keep module
top-level code declarative, and run side-effecting operations explicitly.

Regular notebook code cells keep the restricted profile. A future inline
module cell should reference the same module source, not introduce a second
authoritative copy or a blanket unrestricted-cell flag.

`bytecode/graph_linker` is the reusable bytecode graph linker. `NotebookImage`
can extend its verified ordinary image with notebook cell code and descriptors.
Ambient source occurrences lower to qualified constant lookups; notebook
bindings still use CellId-backed slot descriptors. Classes, functions and
captured module bindings live in the **same** image and heap as the cells.
No raw Value crosses an unrelated RuntimeWorld boundary. The bytecode verifier
accepts composite images, but the existing notebook hot-install API deliberately
rejects ordinary classes/methods/init: composite changes use a fresh world.

`ProjectTabs` owns stable Session objects and a notification-only host mailbox.
Each tab's weak notifier wakes that shared descriptor while keeping local work
hints separate. The terminal services at most one bounded runtime pump between
keyboard reads, round-robin across tabs. Inactive sheets can process genuine
Watch events without drawing over the active tab. Stale/edited sheets defer
their events. No periodic timer is used at genuine idle; retry cooldowns are
bounded. The host and local mailboxes close before any backend is destroyed.
Nothing in tab switching exchanges a live Session under a registered waiter.

## Filesystem and concurrency contract

The current persistence implementation targets macOS/Linux POSIX hosts.
Member paths cannot be absolute, traversing, backslash-separated or symlinked.
During each operation, every module path component and `project.json` is
opened relative to that operation's project-directory descriptor with symlink
rejection. The descriptor is not retained between operations; replacing the
project root path itself between calls is outside the current concurrency
contract. Opaque resource paths are never followed by this reader. Source
opening does not instantiate a RuntimeWorld.

Project and module saves take the same nonblocking cooperative
`.iamber-project.lock`. A module save compares both the exact project
manifest baseline (so its path cannot change underneath the editor) and exact
module source baseline, then stages beside that module and syncs its immediate
parent. It preserves the source file's permission bits. It never changes
`project.json`; adding/removing a module remains a manifest transaction.

New module creation holds that same lock while staging the source and updated
manifest. It publishes the source with an atomic no-replace operation **before**
replacing the manifest; a saved manifest never deliberately points at a missing
new file. This is not a filesystem-wide atomic transaction: a failure or crash
between publications can leave an unreferenced source file or newly created
parent directories. They are retained for recovery, and a reported partial
creation includes the source path; retry never overwrites them. The in-memory
project baseline and new tab are published at manifest replacement, even when
a subsequent directory-sync failure reports a durability warning.

Project Save compares exact on-disk contents against the load baseline, stages
an exclusive sibling file, syncs it, rechecks the baseline and atomically
renames it over `project.json`.
Readers see a whole old or new document. The baseline advances at replacement;
a subsequent directory-sync error explicitly reports that the project member
has already been saved. Existing source/resource files are not overwritten.

External changes cause a conflict, not last-writer-wins. Editors that ignore
the lock can still race the final comparison/rename: this is not a cross-tool
filesystem compare-and-swap. A failed new-project creation may leave a partial
new directory for inspection; it is never recursively cleaned up. The lock
file is persistent (removing it while another writer holds it breaks locking).
Crash-left `.iamber.tmp-*` files are not loaded as project content.

## macOS document package

`.amberbook` is the recommended directory extension. Apple identifies package
document types using an exported UTType conforming to `com.apple.package`,
declared by the owning application. See Apple's
[file type declarations](https://developer.apple.com/documentation/uniformtypeidentifiers/defining-file-and-data-types-for-your-app)
and [document package guide](https://developer.apple.com/library/archive/documentation/CoreFoundation/Conceptual/CFBundles/DocumentPackages/DocumentPackages.html).

`tools/iamber/macos/AmberbookDocumentTypes.plist` is a mergeable declaration for
the future native application's Info.plist. The CLI currently has no `.app`
bundle, document-open handler, Launch Services registration, icon or signing
pipeline. This fragment alone does **not** make Finder show a directory as a
file. No machine-wide associations are installed by the project commands.
Once the owning native app registers the type, the existing directory format
can be presented as one Finder document without a data-format conversion.

## Version 2 extension

The [board/input extension](notebook-boards-v1.md) adds project-scoped typed
parameters and inert board descriptions. It is opt-in on the first explicit
board/input structure save; ordinary version 1 documents remain unchanged.
