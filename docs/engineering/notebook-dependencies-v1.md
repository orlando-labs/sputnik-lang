# Notebook directory dependencies

`modules` contains the project's own code. `dependencies` links existing local
package directories; their source is **not copied into the notebook**. Both
isputnik and the native macOS host use the same resolver and project format.

Explicit directory links take priority over automatically discovered packages.
Imports can also use `<project.sputnikbook>/packages/` and `SPUTNIK_PKG_HOME`
without adding a link. See [shared package installation](package-store-v1.md).
The limits below describe explicit directory links.

## Link and import

In the macOS sidebar choose **Dependencies → +**, then select the package
directory. In isputnik use **F3**:

```text
add-dependency /path/to/plot
```

Paths containing spaces are accepted as the remainder of the command; do not
add shell quotes. Relative paths are resolved from the `.sputnikbook` directory,
not the process working directory. New links are saved as relative paths where
possible. **Apply Modules / ⇧⌘A** (isputnik: **F6 Apply**) loads the new environment.
Opening a document or linking a directory only reads files and never executes
user code. Missing links do not prevent opening the notebook to repair them.

In a code cell:

```sputnik
import plot
p = plot.figure(width: 800, height: 480).line([[0, 0], [1, 1], [2, 4]])
notebook.show(p, caption: "Linked plot")
```

`import plot as p` and `from plot import figure as make_figure` work too. The
same explicit imports can select bundled modules, without enabling auto-import.
Imports remain **cell-local**. To use an import in another cell, import it
there too, or publish a normal variable such as `make_figure = plot.figure`.
Such variables follow normal notebook slot dependencies. A module namespace
itself is not a first-class runtime value; only its public exports are usable.

Only modules reachable from explicit cell imports or auto-import roots are
compiled/initialized. Module imports are environment declarations: their
initializers run during explicit Apply/evaluation, before cell execution, even
if the declaring cell is Manual. Import aliases do not become notebook locals.

Package source edits are refreshed by **Apply**. Ordinary Run uses the loaded
source snapshot; there is no filesystem watcher. Failed loading, compilation,
linking or initialization retains the previous environment and results. A
successful Apply reruns Watch cells and leaves Manual cells stale. As with
bundled modules, external effects of a failed initializer cannot be rolled back.

Prepared native packages may instead list source snapshots from a versioned
artifact generation. Their package preparation step updates the manifest,
library and project hash together. `isputnik --update-native-dependency PROJECT
PACKAGE MANIFEST LIBRARY` updates an existing native link atomically without
evaluating code, converting source-only links or changing other metadata.
Reopen an externally updated project to adopt its new save baseline and worker
dependency snapshot. This command never grants native trust or calls dlopen.

To unlink: use the dependency's context menu in macOS, or
`remove-dependency <exact stored path>` in F3. This only changes the manifest;
the package directory and all its files remain untouched. Apply adopts the
changed environment. Dependency paths also appear in `isputnik --project-info`.

## Document format

External links opt the project into **schema version 3**. Older v1/v2 notebooks
continue to load unchanged. A v3 document additionally requires `inputs`,
`boards` (as in v2) and `dependencies` arrays:

```json
"dependencies": [
  {"path": "../../plot", "auto_import": false}
]
```

`path` points to a directory containing one `sputnik.build.yaml`,
`sputnik.build.yml`, or `sputnik.build.json`. Its `files` and `source_root` define
modules, with optional `module_names` overrides; no recursive source scan runs.
The explicit module map remains available for bootstrap/custom build mappings.
Module names must be unique across links and cannot shadow native stdlib.
Transitive source imports use that same combined map.

`auto_import` defaults to `false`. Setting it to `true` explicitly opts the
package's **root module exports** into the sheet's ambient names, using the
same conflict rules as bundled auto-imports. The current UI creates explicit
links; this optional flag can be edited in `project.json` while the project is
closed. It is distinct from the `auto_imports` list of bundled module IDs.
Unknown dependency metadata is preserved on save.

The resolver accepts absolute package directory references, but module member
paths must stay inside that directory, including after symlink resolution.
Limits: 128 links, 1024 modules and 64 MiB per package, 16 MiB per file;
4096 external modules and 128 MiB of external source per project. Relative links
are portable only when the corresponding directory layout travels with them.

## Current scope

- Local source directories only: no registry download, installation, lockfile,
  version resolution or automatic native build. build manifests with
  versioned package dependencies are rejected explicitly, not silently resolved
  against arbitrary directories. Independent directory links can supply
  transitive source-module imports.
- Build-only numeric overrides and forbidden profiles are rejected; supported
  numeric source preambles retain their normal meaning. Cross-module macro
  provider loading is not added by this feature. Normal compiler/linker limits,
  including unsupported export forms, still apply.
- Linking grants no new runtime capabilities. Native extension metadata is not
  executed; foreign calls require a renderer/thunk already registered by the
  host. The native notebook build prelinks plot's PNG renderer when its
  checkout is available. Changing native C implementation requires rebuilding
  the app; Apply only reloads Sputnik sources. The ordinary isputnik binary does not
  acquire a PNG renderer simply by linking the source package.
- Linked packages have no editable module tabs in the notebook. Edit their
  original files in an external editor and Apply. Project-owned definitions
  still belong in bundled module tabs.

## Plot examples

```sh
python3 ../sputnik-notebook/tools/notebook-macos/make-plot-sample.py build/notebook-macos/LinkedPlots.sputnikbook
python3 ../sputnik-notebook/tools/notebook-macos/make-plot-sample.py build/notebook-macos/LinkedBoard.sputnikbook --board
```

These create links to the real `../plot` checkout and use `import plot`.
To explicitly copy the Sputnik source for a portable example, add `--bundle`.
That switch is specific to this sample generator, not a general dependency
vendoring/export command, and does not bundle the native renderer in the project.
Existing example directories are never overwritten.
