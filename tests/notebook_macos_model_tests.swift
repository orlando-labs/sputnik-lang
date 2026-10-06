import AppKit
import Foundation

private struct ModelTestFailure: Error, CustomStringConvertible {
    let description: String
}

@main
private struct NotebookModelRegressionTests {
    @MainActor
    static func main() async {
        // Model.open records recent documents through AppKit. Keep this test
        // process hidden; it exercises the real async worker without a window.
        let application = NSApplication.shared
        application.setActivationPolicy(.prohibited)

        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("amber-notebook-model-\(UUID().uuidString)",
                                   isDirectory: true)
        let first = root.appendingPathComponent("first.amberbook", isDirectory: true)
        let second = root.appendingPathComponent("second.amberbook", isDirectory: true)
        let model = NotebookModel()
        defer { try? FileManager.default.removeItem(at: root) }

        do {
            try FileManager.default.createDirectory(at: root,
                                                   withIntermediateDirectories: false)
            try writeProject(first, title: "Model Test", cellIDs: ["101", "102"])
            try writeProject(second, title: "Second Project", cellIDs: ["901"])

            try testTextVariables()
            try testFigurePanels()
            try await testBoards(model, url: root.appendingPathComponent("board.amberbook"))
            try await testDraftsTabsAndStructures(model, first: first)
            try await testTextFormattingDrafts(model)
            try await testDeleteDoesNotLeaveAnOrphanDraft(model, first: first)
            try await testNulAndFailedOpenKeepTheCurrentDocument(model, first: first)
            try await testRepeatedReplacementAndExternalConflict(model,
                                                                 first: first,
                                                                 second: second)
            if CommandLine.arguments.count > 1 {
                try await testIsolatedWorker(path: CommandLine.arguments[1], url: root.appendingPathComponent("isolated.amberbook"))
            }

            await shutdown(model)
            // Completion must mean the old borrowed-fd source has been
            // cancelled and its host destroyed. Reopening proves the worker
            // remains usable after that cancellation barrier.
            let reopenedAfterShutdown = await open(model, url: second)
            try require(reopenedAfterShutdown,
                        "model should reopen after orderly shutdown")
            await shutdown(model)
            print("notebook macOS model tests passed")
        } catch {
            await shutdown(model)
            fputs("notebook macOS model tests failed: \(error)\n", stderr)
            exit(1)
        }

    }

    @MainActor
    private static func testIsolatedWorker(path: String, url: URL) async throws {
        let model = NotebookModel(workerExecutable: path)
        try writeProject(url, title: "Isolated", cellIDs: ["801"])
        do {
            let opened = await open(model, url: url)
            try require(opened, "open isolated host")
            guard let tab = model.activeTab, let cell = tab.cells.first else { throw ModelTestFailure(description: "missing isolated cell") }
            model.editSource("try:\n  while true:\n    x = 1\nensure:\n  print(\"STOPPED\")\n", cell: cell, tab: tab)
            var completion: Bool?
            model.perform("run_all", tab: tab) { completion = $0 }
            for _ in 0..<500 where !model.canStop && completion == nil { try await Task.sleep(nanoseconds: 10_000_000) }
            try require(model.canStop && model.canEdit, "independent controls and editor remain available")
            // Reverting to the PRE-run snapshot must not be mistaken for no edit.
            model.editSource(cell.source, cell: cell, tab: tab)
            model.stopExecution()
            for _ in 0..<500 where completion == nil { try await Task.sleep(nanoseconds: 10_000_000) }
            try require(completion == true && !model.busy, "Stop completes on main thread")
            guard let stoppedTab = model.activeTab, let stopped = stoppedTab.cells.first else { throw ModelTestFailure(description: "missing stopped cell") }
            try require(model.source(for: stopped, in: stoppedTab) == cell.source && stopped.source != cell.source && model.hasUnsavedChanges,
                        "late worker snapshot preserves a newer revert draft")
            let saved = await perform(model, "save_all")
            try require(saved && !model.hasUnsavedChanges, "host saves the newer draft, not executed source")
            guard let savedTab = model.activeTab, let savedCell = savedTab.cells.first else { throw ModelTestFailure(description: "missing saved cell") }
            model.editSource("while true:\n  x = 1\n", cell: savedCell, tab: savedTab)
            completion = nil
            model.perform("run_all") { completion = $0 }
            for _ in 0..<500 where !model.canForceStop && completion == nil { try await Task.sleep(nanoseconds: 10_000_000) }
            try require(model.canForceStop, "force control available")
            model.editSource("123\n", cell: savedCell, tab: savedTab)
            model.stopExecution(force: true)
            for _ in 0..<500 where completion == nil { try await Task.sleep(nanoseconds: 10_000_000) }
            try require(completion == false && !model.busy && model.error != nil, "forced worker is reaped, not reported successful")
            model.clearError()
            let restarted = await perform(model, "restart_worker")
            try require(restarted && model.activeTab?.cells.first?.source == "123\n", "restart keeps newer draft without replay")
            let reran = await perform(model, "run_all")
            try require(reran && model.activeTab?.cells.first?.result == "123", "explicit rerun creates fresh worker")
            await shutdown(model)
        } catch {
            if model.executingWorker { model.stopExecution(force: true) }
            await shutdown(model)
            throw error
        }
    }

    @MainActor
    private static func testBoards(_ model: NotebookModel, url: URL) async throws {
        let created = await withCheckedContinuation { continuation in
            model.open(url: url, create: true) { continuation.resume(returning: $0) }
        }
        try require(created, "create board project")
        let inputCreated = await perform(model, "new_input", text: #"{"id":"gain","title":"Gain","type":"number","default":2}"#)
        try require(inputCreated && model.snapshot?.inputs?.first?.editingText == "2.0", "typed input snapshot")
        var board = NotebookBoard.empty(sheet: "main")
        board.components = [NotebookBoardComponent(id: "gain", kind: "input", title: "Gain", input: "gain")]
        let saved = await withCheckedContinuation { continuation in
            model.saveBoard(board) { continuation.resume(returning: $0) }
        }
        try require(saved && model.activeBoard?.id == board.id, "designer board saves with valid generated ID")
        guard let tab = model.activeTab, let cell = tab.cells.first else { throw ModelTestFailure(description: "board controller missing") }
        model.editSource("notebook.input(\"gain\") * 21\n", cell: cell, tab: tab)
        let ran = await perform(model, "run_all", tab: tab)
        try require(ran && model.activeTab?.cells.first?.result == "42", "board controller reads default")
        let changed = await withCheckedContinuation { continuation in
            model.perform("set_input", text: "3", commandCellID: "gain") { continuation.resume(returning: $0) }
        }
        try require(changed && model.activeTab?.cells.first?.result == "63", "native model input event reruns controller")
        let rejected = await withCheckedContinuation { continuation in
            model.perform("set_input", text: "false", commandCellID: "gain") { continuation.resume(returning: $0) }
        }
        try require(!rejected && model.snapshot?.inputs?.first?.editingText == "3.0", "invalid input retains snapshot")
        let integerJSON = try NotebookProjectInput.valueJSON("9223372036854775807", type: "integer")
        let stringJSON = try NotebookProjectInput.valueJSON("a\"b", type: "string")
        try require(integerJSON == "9223372036854775807", "integer edits remain exact")
        try require(stringJSON == #""a\"b""#, "string editor escapes JSON")
    }

    private static func testFigurePanels() throws {
        let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 2, pixelsHigh: 2,
                                      bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
                                      isPlanar: false, colorSpaceName: .deviceRGB,
                                      bytesPerRow: 0, bitsPerPixel: 0)!
        for y in 0..<2 { for x in 0..<2 {
            bitmap.setColor(NSColor(deviceRed: 1, green: 0.5, blue: 0, alpha: 1), atX: x, y: y)
        } }
        let png = bitmap.representation(using: .png, properties: [:])!
        let json = try JSONSerialization.data(withJSONObject: [
            "mime": "image/png", "caption": "Тест 🦊", "width": 2, "height": 2,
            "data": png.base64EncodedString()
        ])
        let decoded = try JSONDecoder().decode(NotebookFigurePanel.self, from: json)
        try require(decoded.data == png && decoded.caption == "Тест 🦊" && decoded.image() != nil,
                    "figure JSON must retain original PNG bytes and Unicode captions")
        try require(decoded.image(maxPixelSize: 1)?.size == NSSize(width: 1, height: 1),
                    "gallery previews must respect their decode size budget")
        try require(NotebookFigurePanel.label(0) == "a)" && NotebookFigurePanel.label(25) == "z)" &&
                    NotebookFigurePanel.label(26) == "aa)" && NotebookFigurePanel.label(63) == "bl)",
                    "scientific panel labels must continue beyond z")
        for invalid in [
            NotebookFigurePanel(mime: "image/svg+xml", caption: "", width: 2, height: 2, data: png),
            NotebookFigurePanel(mime: "image/png", caption: "", width: 3, height: 2, data: png),
            NotebookFigurePanel(mime: "image/png", caption: "", width: 9000, height: 2, data: png),
            NotebookFigurePanel(mime: "image/png", caption: "", width: 2, height: 2, data: Data("not PNG".utf8))
        ] {
            try require(invalid.image() == nil, "invalid or mismatched figure images must not decode")
        }
    }

    private static func testTextVariables() throws {
        let first = NotebookCell(
            id: "code-1", kind: "code", source: "answer = 1\n", formatting: "",
            watch: false, dirty: false, ok: true, result: "", error: "",
            output: [], locals: [
                NotebookLocal(name: "answer", value: "debug-1", role: "module_cell",
                              binding_kind: "local", initialized: true, watched: false,
                              text_value: "one"),
                NotebookLocal(name: "from_import", value: "debug-import", role: "module_import",
                              binding_kind: "import", initialized: true, watched: false,
                              text_value: "should-not-appear")
            ])
        let shadow = NotebookCell(
            id: "code-2", kind: "code", source: "answer = 2\n", formatting: "",
            watch: false, dirty: false, ok: true, result: "", error: "",
            output: [], locals: [
                NotebookLocal(name: "answer", value: "debug-2", role: "module_cell",
                              binding_kind: "local", initialized: true, watched: false,
                              text_value: "two"),
                NotebookLocal(name: "pending", value: "", role: "module_cell",
                              binding_kind: "local", initialized: false, watched: false,
                              text_value: nil)
            ])
        let target = NotebookCell(
            id: "text-1", kind: "text", source: "{{answer}}", formatting: "",
            watch: false, dirty: false, ok: false, result: "", error: "",
            output: [], locals: [])
        let later = NotebookCell(
            id: "code-3", kind: "code", source: "answer = 3\n", formatting: "",
            watch: false, dirty: false, ok: true, result: "", error: "",
            output: [], locals: [
                NotebookLocal(name: "later", value: "debug-later", role: "module_cell",
                              binding_kind: "local", initialized: true, watched: false,
                              text_value: "later")
            ])
        let tab = makeTab(id: "sheet:variables", cells: [first, shadow, target, later])
        let model = NotebookModel()
        model.snapshot = makeSnapshot(tab: tab)

        let values = model.textVariables(before: target, in: tab)
        try require(values.map { $0.name } == ["answer", "pending"],
                    "variables should be deterministic and exclude later/import bindings")
        let answer = try requireVariable("answer", in: values)
        try require(answer.value == "two" && answer.cellID == shadow.id,
                    "nearest module-cell provider should shadow the older value and use text_value")
        try require(!answer.stale, "fresh providers should not be marked stale")
        try require(!model.figureIsStale(for: shadow, in: tab), "fresh figures should not be stale")
        let pending = try requireVariable("pending", in: values)
        try require(pending.value == "<unavailable>" && pending.stale,
                    "uninitialized providers should remain visible but unavailable and stale")

        var staleTab = makeTab(id: "sheet:stale", cells: [
            first,
            NotebookCell(
                id: "failed", kind: "code", source: "broken", formatting: "",
                watch: false, dirty: true, ok: false, result: "", error: "failed",
                output: [], locals: []),
            target
        ])
        staleTab = NotebookTab(id: staleTab.id, kind: staleTab.kind, title: staleTab.title,
                               modified: staleTab.modified, environment_stale: true,
                               auto_import: staleTab.auto_import, status: staleTab.status,
                               environment_error: staleTab.environment_error,
                               environment_output: staleTab.environment_output,
                               selected_cell: staleTab.selected_cell, cells: staleTab.cells)
        let staleTarget = staleTab.cells[2]
        let staleModel = NotebookModel()
        staleModel.snapshot = makeSnapshot(tab: staleTab)
        let staleAnswer = try requireVariable("answer",
                                              in: staleModel.textVariables(before: staleTarget,
                                                                           in: staleTab))
        try require(staleAnswer.stale,
                    "environment and dirty/failed predecessors should stale retained values")

        let draftModel = NotebookModel()
        draftModel.snapshot = makeSnapshot(tab: tab)
        draftModel.editSource("answer = draft\n", cell: first, tab: tab)
        let draftedAnswer = try requireVariable("answer",
                                                in: draftModel.textVariables(before: target,
                                                                             in: tab))
        try require(draftedAnswer.stale,
                    "a prior code draft should stale the nearest runtime provider")
        try require(draftModel.figureIsStale(for: shadow, in: tab),
                    "a prior code draft should also stale retained dependent figures")
    }

    private static func makeTab(id: String, cells: [NotebookCell]) -> NotebookTab {
        NotebookTab(id: id, kind: "sheet", title: id, modified: false,
                     environment_stale: false, auto_import: false, status: "ready",
                     environment_error: "", environment_output: [],
                     selected_cell: cells.first?.id ?? "", cells: cells)
    }

    private static func makeSnapshot(tab: NotebookTab) -> NotebookSnapshot {
        NotebookSnapshot(title: "Variables", path: "", selected_tab: tab.id,
                         modified_count: 0, environment_generation: 1, tabs: [tab])
    }

    private static func requireVariable(_ name: String,
                                        in values: [NotebookTextVariable]) throws -> NotebookTextVariable {
        guard let variable = values.first(where: { $0.name == name }) else {
            throw ModelTestFailure(description: "missing variable \(name)")
        }
        return variable
    }

    @MainActor
    private static func testDraftsTabsAndStructures(_ model: NotebookModel,
                                                    first: URL) async throws {
        let initiallyOpened = await open(model, url: first)
        try require(initiallyOpened, "initial project should open")
        let initial = try requireSnapshot(model)
        let main = try requireTab("sheet:main", in: initial)
        let firstCell = try requireCell("101", in: main)

        // Create a second sheet, then edit the first sheet while it is
        // inactive. Selecting a different tab must flush every cached draft
        // before changing the backend's active tab.
        let sheetCreated = await perform(model, "new_sheet", tab: main,
                                         text: "second")
        try require(sheetCreated, "new sheet should be created")
        let secondSheet = try requireTab("sheet:second", in: requireSnapshot(model))
        model.editSource("answer = 42\n", cell: firstCell, tab: main)
        let selectedMainTab = await perform(model, "select", tab: main)
        try require(selectedMainTab,
                    "cross-tab selection should not use the old tab's cell ID")
        try require(model.error == nil,
                    "cross-tab selection should not report a stale cell ID")
        let selectedMain = try requireTab("sheet:main", in: requireSnapshot(model))
        try require(selectedMain.cells.first(where: { $0.id == "101" })?.source ==
                        "answer = 42\n",
                    "selecting a tab should flush its pending source draft")

        // A module tab has a different cell ID. Editing its source and
        // selecting a sheet must still route the select with no stale cell.
        let selectedSecondSheet = await perform(model, "select", tab: secondSheet)
        try require(selectedSecondSheet, "second sheet should be selectable")
        let moduleCreated = await perform(model, "new_module", text: "scratch")
        try require(moduleCreated, "new module should be created")
        let module = try requireTab("module:scratch", in: requireSnapshot(model))
        let moduleCell = try requireCell(module.cells.first?.id ?? "", in: module)
        let moduleSource = "package scratch\nvalue = 7\n"
        model.editSource(moduleSource, cell: moduleCell, tab: module)
        let selectedFromModule = await perform(model, "select", tab: selectedMain)
        try require(selectedFromModule,
                    "module-to-sheet selection should succeed")
        try require(model.error == nil,
                    "module-to-sheet selection should not report a stale cell ID")
        let savedAll = await perform(model, "save_all")
        try require(savedAll,
                    "Save All should persist drafts across sheet and module tabs")
        try require(!model.hasUnsavedChanges,
                    "successful Save All should clear committed drafts")
        let saved = try requireSnapshot(model)
        let savedMain = try requireTab("sheet:main", in: saved)
        let savedModule = try requireTab("module:scratch", in: saved)
        try require(!savedMain.modified, "main sheet should be saved")
        try require(!savedModule.modified, "module source should be saved")
    }

    @MainActor
    private static func testTextFormattingDrafts(_ model: NotebookModel) async throws {
        let created = await perform(model, "add_text")
        try require(created, "text block creation should succeed")
        guard let tab = model.activeTab, let cell = model.selectedCell, cell.kind == "text" else {
            throw ModelTestFailure(description: "text block should be selected")
        }
        let source = "Заметка 🦊"
        let bold = "{\"version\":1,\"runs\":[{\"start\":0,\"length\":7,\"bold\":true}]}"
        model.editRichText(source, formatting: bold, cell: cell, tab: tab)
        try require(model.hasUnsavedChanges, "rich source draft is modified")
        let savedDraft = await perform(model, "save_all")
        try require(savedDraft, "rich draft should save")
        try require(!model.hasUnsavedChanges, "saved rich draft clears both source and formatting")
        guard let saved = model.selectedCell, let savedTab = model.activeTab else {
            throw ModelTestFailure(description: "saved text should exist")
        }
        let italic = "{\"version\":1,\"runs\":[{\"start\":0,\"length\":7,\"italic\":true}]}"
        model.editRichText(source, formatting: italic, cell: saved, tab: savedTab)
        try require(model.hasUnsavedChanges && model.source(for: saved, in: savedTab) == saved.source,
                    "style-only edits should count dirty with unchanged plain text")
        let savedStyle = await perform(model, "save_all")
        try require(savedStyle, "style-only draft should save")
        try require(!model.hasUnsavedChanges && model.selectedCell?.formatting.contains("italic") == true,
                    "style-only save must reach backend and clear draft")
        // A native buffer that cannot be encoded is still unsaved. Do not
        // silently save the previous source or unmount that editor on select.
        let document = model.documentIdentity
        model.reportEditorError("Unsupported attachment", cell: saved, tab: savedTab, document: document)
        try require(model.hasUnsavedChanges && model.isModified(savedTab), "unencodable buffer must protect close")
        let rejectedSave = await perform(model, "save_all")
        let rejectedSelect = await perform(model, "select", tab: savedTab)
        try require(!rejectedSave && !rejectedSelect, "unencodable editor must block save and tab replacement")
        model.reportEditorError(nil, cell: saved, tab: savedTab, document: UUID())
        try require(model.hasUnsavedChanges, "stale document callback must not clear current protection")
        model.reportEditorError(nil, cell: saved, tab: savedTab, document: document)
        let savedRecovery = await perform(model, "save_all")
        try require(savedRecovery && !model.hasUnsavedChanges, "undoing the unsupported edit recovers normal saving")
        if let latest = model.selectedCell, let latestTab = model.activeTab, let path = model.snapshot?.path {
            let structured = "Title\n1.\t{{answer}}\nA\nB\n"
            let version2 = #"{"version":2,"runs":[],"paragraphs":[{"start":0,"length":6,"style":"heading1"},{"start":6,"length":14,"list":"numbered"}],"tables":[{"start":20,"length":4,"columns":2,"cells":[{"start":20,"length":2},{"start":22,"length":2}]}]}"#
            model.editRichText(structured, formatting: version2, cell: latest, tab: latestTab)
            let savedStructure = await perform(model, "save_all")
            try require(savedStructure, "v2 heading/list/table template must pass the real worker and save")
            let reopened = NotebookModel()
            let opened = await open(reopened, url: URL(fileURLWithPath: path))
            let restored = reopened.snapshot?.tabs.first(where: { $0.id == latestTab.id })?.cells.first(where: { $0.id == latest.id })
            let preserved = restored?.source == structured && restored?.formatting.contains("\"tables\"") == true
            await shutdown(reopened)
            try require(opened && preserved, "v2 structure and interpolation templates must survive a fresh runtime reopen")
        }
        // Removal of a drafted text block must clear its whole structured draft.
        if let latest = model.selectedCell, let latestTab = model.activeTab {
            model.editRichText(source + "!", formatting: italic, cell: latest, tab: latestTab)
            let deleted = await perform(model, "delete_cell", tab: latestTab, cell: latest)
            try require(deleted, "drafted text deletes")
            let savedDelete = await perform(model, "save_all")
            try require(savedDelete, "deleting rich draft leaves project saveable")
        }
    }

    @MainActor
    private static func testDeleteDoesNotLeaveAnOrphanDraft(_ model: NotebookModel,
                                                             first: URL) async throws {
        _ = first
        let snapshot = try requireSnapshot(model)
        let main = try requireTab("sheet:main", in: snapshot)
        let cell = try requireCell("102", in: main)
        model.editSource("doomed = 9\n", cell: cell, tab: main)
        let deleted = await perform(model, "delete_cell", tab: main, cell: cell)
        try require(deleted, "deleting a drafted cell should succeed")
        let afterDelete = try requireSnapshot(model)
        let mainAfterDelete = try requireTab("sheet:main", in: afterDelete)
        try require(mainAfterDelete.cells.allSatisfy { $0.id != cell.id },
                    "deleted cell should disappear from the snapshot")
        let savedAfterDelete = await perform(model, "save_all")
        try require(savedAfterDelete,
                    "Save All must not retry a deleted cell's stale draft ID")
        try require(!model.hasUnsavedChanges,
                    "deleting then saving should leave no orphan draft")

        // The model keeps the selected cell stable while moving it in the
        // backend. Add one after the remaining cell, then move it both ways.
        let currentMain = try requireTab("sheet:main", in: requireSnapshot(model))
        guard let remaining = currentMain.cells.first else {
            throw ModelTestFailure(description: "sheet should retain one code cell")
        }
        let addedSuccessfully = await perform(model, "add_cell", tab: currentMain,
                                               cell: remaining)
        try require(addedSuccessfully, "adding a code cell should succeed")
        let added = model.activeTab?.selected_cell ?? ""
        try require(!added.isEmpty && model.selectedCellID == added,
                    "new cell should become the stable selected cell")
        let latestMain = try requireTab("sheet:main", in: requireSnapshot(model))
        let addedCell = try requireCell(added, in: latestMain)
        let movedUp = await perform(model, "move_up", tab: currentMain,
                                    cell: addedCell)
        try require(movedUp, "moving a cell up should succeed")
        try require(model.activeTab?.selected_cell == added,
                    "moving up should preserve the selected cell ID")
        let movedDown = await perform(model, "move_down", tab: model.activeTab,
                                      cell: addedCell)
        try require(movedDown, "moving a cell down should succeed")
        try require(model.activeTab?.selected_cell == added,
                    "moving down should preserve the selected cell ID")
        let savedStructure = await perform(model, "save_all")
        try require(savedStructure, "structural edits should remain saveable")
    }

    @MainActor
    private static func testNulAndFailedOpenKeepTheCurrentDocument(
        _ model: NotebookModel, first: URL) async throws {
        let snapshot = try requireSnapshot(model)
        let main = try requireTab("sheet:main", in: snapshot)
        guard let cell = main.cells.first else {
            throw ModelTestFailure(description: "main sheet should have a cell")
        }

        let nulSource = "prefix\0suffix\n"
        model.editSource(nulSource, cell: cell, tab: main)
        try require(!model.source(for: cell, in: main).isEmpty,
                    "NUL draft should remain visible before submission")
        let nulSaved = await perform(model, "save_all")
        try require(!nulSaved, "NUL source should be rejected instead of truncated")
        try require(model.error?.contains("NUL") == true,
                    "NUL rejection should explain why the source was not submitted")
        let afterNul = try requireSnapshot(model)
        let persistedCell = try requireCell(cell.id,
            in: try requireTab("sheet:main", in: afterNul))
        try require(persistedCell.source != "prefix",
                    "C string truncation must never persist a source prefix")
        try require(model.source(for: cell, in: main) == nulSource,
                    "rejected source must remain recoverable as a Swift draft")

        // A replacement project that cannot be created must leave the live
        // handle, pending editor buffer, and recent project untouched.
        let overwroteProject = await open(model, url: first, create: true)
        try require(!overwroteProject,
                    "creating over the existing project should fail")
        try require(model.snapshot?.path == snapshot.path &&
                    model.snapshot?.selected_tab == snapshot.selected_tab,
                    "failed open must keep the old host and snapshot: \(model.snapshot?.path ?? "nil") vs \(snapshot.path)")
        try require(model.source(for: cell, in: main) == nulSource,
                    "failed open must keep pending drafts")
        model.clearError()
        model.editSource(cell.source, cell: cell, tab: main)
        let savedAfterRevert = await perform(model, "save_all")
        try require(savedAfterRevert,
                    "reverting a rejected draft should restore a saveable project")
    }

    @MainActor
    private static func testRepeatedReplacementAndExternalConflict(
        _ model: NotebookModel, first: URL, second: URL) async throws {
        for project in [second, first, second, first] {
            let replacementOpened = await open(model, url: project)
            try require(replacementOpened, "replacing a project should succeed")
            let reported = model.snapshot.map {
                URL(fileURLWithPath: $0.path, isDirectory: true).resolvingSymlinksInPath().standardizedFileURL
            }
            try require(reported == project.resolvingSymlinksInPath().standardizedFileURL,
                        "replacement snapshot should refer to the latest project: \(reported?.path ?? "nil") vs \(project.path)")
        }

        let manifestURL = first.appendingPathComponent("project.json")
        let baseline = try String(contentsOf: manifestURL, encoding: .utf8)
        let changed = baseline.replacingOccurrences(of: "Model Test", with: "Outside Edit")
        try require(changed != baseline,
                    "fixture should contain the title field for conflict injection")
        try changed.write(to: manifestURL, atomically: true, encoding: .utf8)

        let main = try requireTab("sheet:main", in: requireSnapshot(model))
        guard let cell = main.cells.first else {
            throw ModelTestFailure(description: "main sheet should have a cell")
        }
        model.editSource("local = 5\n", cell: cell, tab: main)
        let conflictSaved = await perform(model, "save_all")
        try require(!conflictSaved,
                    "Save All should reject an externally changed manifest")
        try require(model.error?.localizedCaseInsensitiveContains("changed") == true,
                    "save conflict should explain that the project changed")
        try require(model.hasUnsavedChanges,
                    "conflicting Save All must retain dirty source state")
        try require(model.source(for: cell, in: main) == "local = 5\n" ||
                        model.snapshot?.tabs.first(where: { $0.id == main.id })?.cells
                            .first(where: { $0.id == cell.id })?.source == "local = 5\n",
                    "failed Save All must retain the source in the draft or backend")
    }

    @MainActor
    private static func open(_ model: NotebookModel, url: URL,
                             create: Bool = false) async -> Bool {
        await withCheckedContinuation { continuation in
            model.open(url: url, create: create) { opened in
                continuation.resume(returning: opened)
            }
        }
    }

    @MainActor
    private static func perform(_ model: NotebookModel, _ action: String,
                                tab: NotebookTab? = nil,
                                cell: NotebookCell? = nil,
                                text: String = "") async -> Bool {
        await withCheckedContinuation { continuation in
            model.perform(action, tab: tab, cell: cell, text: text) { succeeded in
                continuation.resume(returning: succeeded)
            }
        }
    }

    @MainActor
    private static func shutdown(_ model: NotebookModel) async {
        await withCheckedContinuation { continuation in
            model.shutdown { continuation.resume() }
        }
    }

    private static func writeProject(_ path: URL, title: String,
                                     cellIDs: [String]) throws {
        let fileManager = FileManager.default
        try fileManager.createDirectory(at: path, withIntermediateDirectories: false)
        let cells = cellIDs.enumerated().map { index, id in
            "{\"id\":\"\(id)\",\"kind\":\"code\",\"source\":\"value_\(index) = \(index)\\n\",\"mode\":\"watch\"}"
        }.joined(separator: ",")
        let json = "{\"format\":\"amber-notebook\",\"version\":1,\"title\":\"\(title)\",\"active_sheet\":\"main\",\"sheets\":[{\"id\":\"main\",\"title\":\"Main\",\"cells\":[\(cells)]}],\"modules\":[],\"auto_imports\":[]}"
        try json.write(to: path.appendingPathComponent("project.json"),
                       atomically: true, encoding: .utf8)
    }

    private static func requireSnapshot(_ model: NotebookModel) throws -> NotebookSnapshot {
        guard let snapshot = model.snapshot else {
            throw ModelTestFailure(description: "model should have an open snapshot")
        }
        return snapshot
    }

    private static func requireTab(_ id: String,
                                   in snapshot: NotebookSnapshot) throws -> NotebookTab {
        guard let tab = snapshot.tabs.first(where: { $0.id == id }) else {
            throw ModelTestFailure(description: "snapshot is missing tab \(id)")
        }
        return tab
    }

    private static func requireCell(_ id: String,
                                    in tab: NotebookTab) throws -> NotebookCell {
        guard let cell = tab.cells.first(where: { $0.id == id }) else {
            throw ModelTestFailure(description: "tab \(tab.id) is missing cell \(id)")
        }
        return cell
    }

    private static func require(_ condition: @autoclosure () -> Bool,
                                _ message: String) throws {
        guard condition() else { throw ModelTestFailure(description: message) }
    }
}
