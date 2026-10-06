import AppKit
import Combine
import Foundation

struct NotebookOutput: Decodable { let stream: String; let text: String }
struct NotebookLocal: Decodable {
    let name: String, value: String, role: String, binding_kind: String
    let initialized: Bool, watched: Bool
    // Older bridge snapshots do not have a text representation yet. Keep
    // decoding those snapshots and use the debug value only as a fallback.
    let text_value: String?

    init(name: String, value: String, role: String, binding_kind: String,
         initialized: Bool, watched: Bool, text_value: String? = nil) {
        self.name = name
        self.value = value
        self.role = role
        self.binding_kind = binding_kind
        self.initialized = initialized
        self.watched = watched
        self.text_value = text_value
    }

    private enum CodingKeys: String, CodingKey {
        case name, value, role, binding_kind, initialized, watched, text_value
    }

    init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        name = try values.decode(String.self, forKey: .name)
        value = try values.decode(String.self, forKey: .value)
        role = try values.decode(String.self, forKey: .role)
        binding_kind = try values.decode(String.self, forKey: .binding_kind)
        initialized = try values.decode(Bool.self, forKey: .initialized)
        watched = try values.decode(Bool.self, forKey: .watched)
        text_value = try values.decodeIfPresent(String.self, forKey: .text_value)
    }
}
struct NotebookCell: Decodable, Identifiable {
    var result_format: NotebookResultFormat? = nil
    var displays: [NotebookFigurePanel]? = nil
    var progress: [NotebookProgress]? = nil
    let id: String, kind: String, source: String, formatting: String
    let watch: Bool, dirty: Bool, ok: Bool
    let result: String, error: String
    let output: [NotebookOutput], locals: [NotebookLocal]
}
struct NotebookResultFormat: Decodable {
    let pretty: String
    let is_container: Bool, truncated: Bool, pretty_truncated: Bool
}
struct NotebookProgress: Decodable, Identifiable {
    let id: String, description: String
    let current: Double, total: Double
    let elapsed_ms: UInt64
    let done: Bool
}
struct NotebookLiveCell: Decodable, Identifiable {
    let id: String
    let displays: [NotebookFigurePanel]
    let progress: [NotebookProgress]
}
private struct NotebookLiveSnapshot: Decodable { let cells: [NotebookLiveCell] }
struct NotebookTab: Decodable, Identifiable {
    let id: String, kind: String, title: String
    let modified: Bool, environment_stale: Bool, auto_import: Bool
    let status: String, environment_error: String
    let environment_output: [NotebookOutput]
    let selected_cell: String, cells: [NotebookCell]
}
struct NotebookDependency: Decodable, Identifiable {
    let path: String
    let auto_import: Bool
    var native: Bool? = nil
    var id: String { path }
}
struct NotebookSnapshot: Decodable {
    var dependencies: [NotebookDependency]? = nil
    var inputs: [NotebookProjectInput]? = nil
    var boards: [NotebookBoard]? = nil
    let title: String, path: String, selected_tab: String
    let modified_count: Int, environment_generation: Int
    let tabs: [NotebookTab]
}

private struct SourceKey: Hashable { let tab: String; let cell: String }
private struct SourceDraft {
    let source: String
    // nil identifies executable source; empty string is an unformatted text block.
    let formatting: String?
}
private struct SourceEdit { let key: SourceKey; let draft: SourceDraft }
private struct NotebookFailure: LocalizedError {
    let message: String
    var errorDescription: String? { message }
}
private final class NotebookControl {
    let pointer: OpaquePointer
    init(_ pointer: OpaquePointer) { self.pointer = pointer }
    deinit { amber_notebook_control_destroy(pointer) }
}

// The C++ host and all its Sessions have exactly one serial owner. Only
// immutable, decoded snapshots cross to AppKit's main thread; no runtime
// values, callbacks or borrowed Session pointers do.
private final class NotebookWorker {
    let executable: String?
    // Main-thread access only. Independent C++ lifetime, not a borrowed host.
    private var control: NotebookControl?
    var isolated: Bool { control != nil }
    init(executable: String?) { self.executable = executable }
    var phase: Int { Int(amber_notebook_control_phase(control?.pointer)) }
    func stop(force: Bool) -> Bool { amber_notebook_control_stop(control?.pointer, force ? 1 : 0) != 0 }
    func takeLive() -> [NotebookLiveCell]? {
        guard let json = amber_notebook_control_live(control?.pointer) else { return nil }
        defer { amber_notebook_string_free(json) }
        return (try? JSONDecoder().decode(NotebookLiveSnapshot.self, from: Data(String(cString: json).utf8)))?.cells
    }
    let queue = DispatchQueue(label: "org.amberlang.notebook.runtime", qos: .userInitiated)
    private var handle: OpaquePointer?
    private var activity: DispatchSourceRead?
    private var activitySuspended = false
    private var paused = false
    private var retry: DispatchWorkItem?
    private var generation: UInt64 = 0
    var onBackground: ((NotebookSnapshot?, String?) -> Void)?

    private func failure(_ error: UnsafeMutablePointer<CChar>?) -> NotebookFailure {
        defer { amber_notebook_string_free(error) }
        return NotebookFailure(message: error.map { String(cString: $0) } ?? "Notebook operation failed.")
    }

    private func snapshot() throws -> NotebookSnapshot {
        var error: UnsafeMutablePointer<CChar>?
        guard let json = amber_notebook_snapshot(handle, &error) else { throw failure(error) }
        defer { amber_notebook_string_free(json) }
        return try JSONDecoder().decode(NotebookSnapshot.self, from: Data(String(cString: json).utf8))
    }

    private func command(_ action: String, tab: String = "", cell: String = "", text: String = "") throws {
        guard ![action, tab, cell, text].contains(where: { $0.utf8.contains(0) }) else {
            throw NotebookFailure(message: "This editor cannot submit embedded NUL characters. The original file has not been overwritten.")
        }
        var error: UnsafeMutablePointer<CChar>?
        let result = cell.withCString { cellPointer in
            amber_notebook_command(handle, action, tab, cell.isEmpty ? nil : cellPointer, text, &error)
        }
        guard result != 0 else { throw failure(error) }
    }

    // Dispatch must finish unregistering the borrowed descriptor before its
    // owning host can be freed. A suspended source must be resumed to cancel.
    private func retireHost(completion: (() -> Void)? = nil) {
        retry?.cancel()
        retry = nil
        if let source = activity {
            let retired = handle
            source.setEventHandler {}
            source.setCancelHandler {
                amber_notebook_destroy(retired)
                completion?()
            }
            if activitySuspended { source.resume() }
            source.cancel()
        } else {
            amber_notebook_destroy(handle)
            completion?()
        }
        activity = nil
        activitySuspended = false
        handle = nil
    }

    func open(path: String, create: Bool, title: String,
              completion: @escaping (NotebookSnapshot?, String?) -> Void) {
        queue.async {
            var candidate: OpaquePointer?
            do {
                guard !path.utf8.contains(0), !title.utf8.contains(0) else {
                    throw NotebookFailure(message: "Project paths and titles cannot contain NUL characters.")
                }
                var error: UnsafeMutablePointer<CChar>?
                candidate = amber_notebook_open(path, create ? 1 : 0, title, &error)
                guard candidate != nil else { throw self.failure(error) }
                let descriptor = amber_notebook_activity_descriptor(candidate, &error)
                guard descriptor >= 0 else { throw self.failure(error) }
                // Decode before replacing the current project, including on
                // malformed/unsupported source bytes or protocol failures.
                let previous = self.handle
                self.handle = candidate
                let initial: NotebookSnapshot
                do { initial = try self.snapshot() }
                catch { self.handle = previous; throw error }
                self.handle = previous
                // Native projects always use a killable helper, including
                // Finder opens. Enabling transport is inert; only Run loads code.
                var candidateControl: NotebookControl?
                let native = initial.dependencies?.contains { $0.native == true } == true
                let executable = self.executable ?? (native ? Bundle.main.executableURL?
                    .deletingLastPathComponent().appendingPathComponent("amber-notebook-worker").path : nil)
                if let executable {
                    guard let pointer = amber_notebook_enable_worker(candidate, executable, &error) else {
                        throw self.failure(error)
                    }
                    candidateControl = NotebookControl(pointer)
                }
                self.retireHost()
                self.handle = candidate
                candidate = nil
                self.generation &+= 1
                let token = self.generation
                self.paused = false
                if candidateControl == nil {
                    let source = DispatchSource.makeReadSource(fileDescriptor: descriptor, queue: self.queue)
                    source.setEventHandler { [weak self] in
                        guard let self, self.generation == token, !self.paused else { return }
                        self.pump()
                    }
                    self.activity = source
                    source.resume()
                }
                let nextControl = candidateControl
                DispatchQueue.main.async { self.control = nextControl; completion(initial, nil) }
            } catch {
                amber_notebook_destroy(candidate)
                let message = error.localizedDescription
                DispatchQueue.main.async { completion(nil, message) }
            }
        }
    }

    func execute(edits: [SourceEdit], action: String, tab: String, cell: String, text: String,
                 trustNative: Bool = false,
                 completion: @escaping (NotebookSnapshot?, String?) -> Void) {
        queue.async {
            var operationError: String?
            do {
                if trustNative { try self.command("trust_native", tab: tab) }
                for edit in edits {
                    if let formatting = edit.draft.formatting {
                        var content: [String: Any] = ["source": edit.draft.source]
                        if !formatting.isEmpty {
                            content["formatting"] = try JSONSerialization.jsonObject(with: Data(formatting.utf8))
                        }
                        let payload = try JSONSerialization.data(withJSONObject: content, options: [.sortedKeys])
                        try self.command("rich_source", tab: edit.key.tab, cell: edit.key.cell,
                                         text: String(decoding: payload, as: UTF8.self))
                    } else {
                        try self.command("source", tab: edit.key.tab, cell: edit.key.cell, text: edit.draft.source)
                    }
                }
                try self.command(action, tab: tab, cell: cell, text: text)
            } catch { operationError = error.localizedDescription }
            var state: NotebookSnapshot?
            do { state = try self.snapshot() }
            catch { if operationError == nil { operationError = error.localizedDescription } }
            let result = state, message = operationError
            DispatchQueue.main.async { completion(result, message) }
        }
    }

    func setPaused(_ value: Bool) {
        queue.async {
            guard self.paused != value else { return }
            self.paused = value
            if value {
                self.retry?.cancel()
                self.retry = nil
                if let source = self.activity, !self.activitySuspended {
                    source.suspend()
                    self.activitySuspended = true
                }
            } else {
                if let source = self.activity, self.activitySuspended {
                    self.activitySuspended = false
                    source.resume()
                }
                // Also resume a pending cooldown whose mailbox was already
                // consumed before editing started. This is not an idle timer.
                if self.handle != nil { self.pump() }
            }
        }
    }

    private func pump() {
        guard executable == nil, !paused, handle != nil else { return }
        retry?.cancel()
        retry = nil
        var error: UnsafeMutablePointer<CChar>?
        var delay: Int32 = -1
        let ok = amber_notebook_pump(handle, &delay, &error) != 0
        let message = ok ? nil : failure(error).localizedDescription
        let state = try? snapshot()
        DispatchQueue.main.async { [weak self] in self?.onBackground?(state, message) }
        if ok, delay >= 0 {
            let token = generation
            let work = DispatchWorkItem { [weak self] in
                guard let self, self.generation == token, !self.paused else { return }
                self.pump()
            }
            retry = work
            queue.asyncAfter(deadline: .now() + .milliseconds(max(1, Int(delay))), execute: work)
        }
    }

    func close(completion: @escaping () -> Void) {
        queue.async {
            self.generation &+= 1
            self.retireHost { DispatchQueue.main.async { self.control = nil; completion() } }
        }
    }
}

final class NotebookModel: ObservableObject {
    private let workerExecutable: String?
    init(workerExecutable: String? = nil) { self.workerExecutable = workerExecutable }
    var isolatedExecution: Bool { workerExecutable != nil || worker.isolated }
    @Published private(set) var executingWorker = false
    @Published private(set) var executionPhase = 0
    private var executionTimer: Timer?
    @Published private(set) var liveCells: [String: NotebookLiveCell] = [:]
    private var liveTab = ""
    private var nativeTrusted = false
    func liveCell(_ cell: NotebookCell, in tab: NotebookTab) -> NotebookLiveCell? {
        liveTab == tab.id ? liveCells[cell.id] : nil
    }
    private func pollLive() {
        if let cells = worker.takeLive() {
            liveCells = Dictionary(uniqueKeysWithValues: cells.map { ($0.id, $0) })
        }
    }
    var canEdit: Bool { !busy || executingWorker }
    var canStop: Bool { executingWorker && executionPhase > 0 && executionPhase < 5 }
    var canForceStop: Bool { executingWorker && executionPhase > 0 }
    var executionLabel: String {
        switch executionPhase {
        case 1: return "Preparing worker"
        case 2: return "Running"
        case 3: return "Waiting for child tasks"
        case 4: return "Stopping…"
        case 5: return "Terminating worker…"
        default: return "Ready"
        }
    }
    func stopExecution(force: Bool = false) {
        guard executingWorker else { return }
        if !worker.stop(force: force) { error = "Execution has already finished, or the stop request could not be delivered." }
        executionPhase = worker.phase
    }
    private func trackExecution(_ active: Bool) {
        executionTimer?.invalidate(); executionTimer = nil
        executingWorker = active; executionPhase = 0
        guard active else { return }
        liveCells = [:]
        let timer = Timer(timeInterval: 0.1, repeats: true) { [weak self] _ in
            guard let self else { return }
            self.executionPhase = self.worker.phase
            self.pollLive()
        }
        executionTimer = timer
        RunLoop.main.add(timer, forMode: .common)
    }
    @Published var snapshot: NotebookSnapshot?
    @Published var busy = false
    @Published var error: String?
    @Published var selectedCellID = ""
    @Published var selectedBoardID: String?
    var boardEditorOpen = false
    @Published private(set) var documentIdentity = UUID()
    @Published private var draftVersion = 0
    var requestNew: (() -> Void)?
    var requestOpen: (() -> Void)?
    var requestDependency: (() -> Void)?
    var onChange: (() -> Void)?
    private var drafts: [SourceKey: SourceDraft] = [:]
    // A native rich editor can contain a buffer that the portable format
    // cannot represent. Keep it protected even before it becomes a draft.
    private var editorFailures: [SourceKey: String] = [:]
    private var closing = false
    private var closeCompletions: [() -> Void] = []
    private lazy var worker: NotebookWorker = {
        let worker = NotebookWorker(executable: workerExecutable)
        worker.onBackground = { [weak self] state, error in
            guard let self else { return }
            if let state { self.accept(state) }
            if let error { self.error = error }
        }
        return worker
    }()

    var activeTab: NotebookTab? { snapshot?.tabs.first { $0.id == snapshot?.selected_tab } }
    var activeBoard: NotebookBoard? { snapshot?.boards?.first { $0.id == selectedBoardID } }
    func controller(for board: NotebookBoard) -> NotebookTab? {
        snapshot?.tabs.first { $0.id == "sheet:" + board.sheet }
    }
    func selectBoard(_ board: NotebookBoard) {
        guard !busy else { return }
        selectedBoardID = board.id
    }
    func setProjectInput(_ input: NotebookProjectInput, text: String) {
        do {
            perform("set_input", text: try NotebookProjectInput.valueJSON(text, type: input.type),
                    commandCellID: input.id)
        } catch { self.error = error.localizedDescription }
    }
    func saveBoard(_ board: NotebookBoard, completion: @escaping (Bool) -> Void) {
        do {
            let json = String(decoding: try JSONEncoder().encode(board), as: UTF8.self)
            perform("save_board", text: json) { [weak self] success in
                if success { self?.selectedBoardID = board.id }
                completion(success)
            }
        } catch { self.error = error.localizedDescription; completion(false) }
    }
    var selectedCell: NotebookCell? {
        activeTab?.cells.first { $0.id == selectedCellID } ?? activeTab?.cells.first
    }
    var hasUnsavedChanges: Bool { !drafts.isEmpty || !editorFailures.isEmpty || (snapshot?.modified_count ?? 0) != 0 }
    func isModified(_ tab: NotebookTab) -> Bool {
        tab.modified || drafts.keys.contains { $0.tab == tab.id } || editorFailures.keys.contains { $0.tab == tab.id }
    }
    func source(for cell: NotebookCell, in tab: NotebookTab) -> String {
        drafts[SourceKey(tab: tab.id, cell: cell.id)]?.source ?? cell.source
    }

    // Return the values that a text block may interpolate. Values are
    // collected in source order: a later successful module cell shadows an
    // earlier one, and a later stale/uninitialized provider must shadow it as
    // well so that an old value is never exposed as current by accident.
    func textVariables(before cell: NotebookCell,
                       in tab: NotebookTab) -> [NotebookTextVariable] {
        guard let targetIndex = tab.cells.firstIndex(where: { $0.id == cell.id }) else {
            return []
        }

        var hasStalePredecessor = tab.environment_stale
        var variables: [String: NotebookTextVariable] = [:]

        for index in tab.cells.indices where index < targetIndex {
            let predecessor = tab.cells[index]
            guard predecessor.kind == "code" else { continue }

            // Any code edit, failed evaluation, or dirty snapshot before the
            // text block can invalidate values from the runtime. Keep them
            // visible for the picker, but label every result stale.
            if predecessor.dirty || !predecessor.ok {
                hasStalePredecessor = true
            }
            let hasDraft = drafts.keys.contains {
                $0.tab == tab.id && $0.cell == predecessor.id
            }
            if hasDraft { hasStalePredecessor = true }

            // The bridge uses module_cell for notebook assignments. Imports,
            // reads, synthetic temporaries, and unknown roles are not user
            // bindings and must not leak into interpolation suggestions.
            for local in predecessor.locals where local.role == "module_cell" && !local.name.isEmpty {
                let value: String
                let localIsStale: Bool
                if local.initialized {
                    value = local.text_value ?? local.value
                    localIsStale = hasStalePredecessor
                } else {
                    value = local.text_value ?? "<unavailable>"
                    localIsStale = true
                }
                variables[local.name] = NotebookTextVariable(
                    name: local.name,
                    value: value,
                    cellID: predecessor.id,
                    stale: localIsStale
                )
            }
        }

        return variables.values.sorted { $0.name < $1.name }
            .map { variable in
                guard hasStalePredecessor, !variable.stale else { return variable }
                return NotebookTextVariable(name: variable.name,
                                            value: variable.value,
                                            cellID: variable.cellID,
                                            stale: true)
            }
    }

    func figureIsStale(for cell: NotebookCell, in tab: NotebookTab) -> Bool {
        guard !tab.environment_stale,
              let index = tab.cells.firstIndex(where: { $0.id == cell.id }) else { return true }
        // Until the edited source is compiled, the UI has no new dependency
        // graph. Conservatively stale retained figures after any code edit or
        // failure at/before their cell, just like text interpolation values.
        return tab.cells.prefix(index + 1).contains { predecessor in
            predecessor.kind == "code" &&
                (predecessor.dirty || !predecessor.ok ||
                 drafts[SourceKey(tab: tab.id, cell: predecessor.id)] != nil)
        }
    }

    func formatting(for cell: NotebookCell, in tab: NotebookTab) -> String {
        drafts[SourceKey(tab: tab.id, cell: cell.id)]?.formatting ?? cell.formatting
    }
    private func sameFormatting(_ left: String, _ right: String) -> Bool {
        func normalized(_ value: String) -> Data? {
            let plain = "{\"version\":1,\"runs\":[]}"
            guard let object = try? JSONSerialization.jsonObject(with: Data((value.isEmpty ? plain : value).utf8)) else { return nil }
            return try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
        }
        return left == right || (normalized(left) != nil && normalized(left) == normalized(right))
    }
    private func pauseForCodeDrafts() {
        worker.setPaused(drafts.values.contains { $0.formatting == nil })
    }
    func editSource(_ source: String, cell: NotebookCell, tab: NotebookTab) {
        guard canEdit else { return }
        let key = SourceKey(tab: tab.id, cell: cell.id)
        // During execution the next host snapshot contains the submitted
        // buffer, not necessarily the snapshot currently displayed. Reverting
        // to that older displayed text is still a NEW edit that must survive.
        if !executingWorker && source == cell.source { drafts.removeValue(forKey: key) }
        else { drafts[key] = SourceDraft(source: source, formatting: nil) }
        draftVersion &+= 1
        pauseForCodeDrafts()
        onChange?()
    }
    func editRichText(_ source: String, formatting: String, cell: NotebookCell, tab: NotebookTab) {
        guard canEdit, cell.kind == "text" else { return }
        let key = SourceKey(tab: tab.id, cell: cell.id)
        if !executingWorker && source == cell.source && sameFormatting(formatting, cell.formatting) { drafts.removeValue(forKey: key) }
        else { drafts[key] = SourceDraft(source: source, formatting: formatting) }
        draftVersion &+= 1
        // Inert text edits must not suspend live code watchers.
        pauseForCodeDrafts()
        onChange?()
    }
    func reportEditorError(_ message: String?, cell: NotebookCell, tab: NotebookTab, document: UUID) {
        guard document == documentIdentity,
              snapshot?.tabs.first(where: { $0.id == tab.id })?.cells.contains(where: { $0.id == cell.id }) == true else { return }
        let key = SourceKey(tab: tab.id, cell: cell.id)
        guard editorFailures[key] != message else { return }
        editorFailures[key] = message
        draftVersion &+= 1
        onChange?()
    }
    func selectTab(_ tab: NotebookTab) {
        perform("select", tab: tab) { [weak self] success in
            if success { self?.selectedBoardID = nil }
        }
    }
    func newProject() { requestNew?() }
    func openProject() { requestOpen?() }
    func clearError() { error = nil }

    private func accept(_ state: NotebookSnapshot) {
        let oldTab = snapshot?.selected_tab
        snapshot = state
        let liveKeys = Set(state.tabs.flatMap { tab in
            tab.cells.map { SourceKey(tab: tab.id, cell: $0.id) }
        })
        // A successful deletion has already consumed the submitted draft.
        // Keeping it would make every subsequent Save target a stale CellId.
        drafts = drafts.filter { liveKeys.contains($0.key) }
        editorFailures = editorFailures.filter { liveKeys.contains($0.key) }
        for tab in state.tabs {
            for cell in tab.cells {
                let key = SourceKey(tab: tab.id, cell: cell.id)
                if let draft = drafts[key], draft.source == cell.source,
                   draft.formatting.map({ sameFormatting($0, cell.formatting) }) ?? true {
                    drafts.removeValue(forKey: key)
                }
            }
        }
        if oldTab != state.selected_tab || !(activeTab?.cells.contains { $0.id == selectedCellID } ?? false) {
            selectedCellID = activeTab?.selected_cell ?? activeTab?.cells.first?.id ?? ""
        }
        onChange?()
    }

    func open(url: URL, create: Bool = false, completion: ((Bool) -> Void)? = nil) {
        guard !busy else { completion?(false); return }
        busy = true
        error = nil
        worker.open(path: url.path, create: create, title: url.deletingPathExtension().lastPathComponent) { [weak self] state, error in
            guard let self else { return }
            self.busy = false
            if let state {
                self.nativeTrusted = false
                self.liveCells = [:]
                self.drafts.removeAll()
                self.editorFailures.removeAll()
                self.documentIdentity = UUID()
                self.selectedBoardID = nil
                self.accept(state)
                NSDocumentController.shared.noteNewRecentDocumentURL(url)
            }
            self.error = error
            self.onChange?()
            completion?(state != nil)
        }
    }

    func perform(_ action: String, tab: NotebookTab? = nil, cell: NotebookCell? = nil,
                 text: String = "", commandCellID: String? = nil, completion: ((Bool) -> Void)? = nil) {
        guard !busy, snapshot != nil else { completion?(false); return }
        guard editorFailures.isEmpty else {
            let message = editorFailures.sorted { ($0.key.tab, $0.key.cell) < ($1.key.tab, $1.key.cell) }.first!.value
            error = "A text block has an unsaved editor buffer: \(message) Undo or correct that edit before saving or changing tabs."
            completion?(false)
            return
        }
        let target = tab ?? activeTab
        let targetCell = cell ?? (action == "select" ? nil : selectedCell)
        let edits = drafts.map { SourceEdit(key: $0.key, draft: $0.value) }.sorted {
            ($0.key.tab, $0.key.cell) < ($1.key.tab, $1.key.cell)
        }
        let executes = ["run", "run_all", "apply"].contains(action)
        if executes && snapshot?.dependencies?.contains(where: { $0.native == true }) == true && !nativeTrusted {
            guard isolatedExecution else {
                error = "This project uses native packages. Open with isolated worker execution to enable Stop and Force Stop."
                completion?(false); return
            }
            if CommandLine.arguments.contains("--trust-native") {
                nativeTrusted = true // explicit automated-test/launcher authorization
            } else {
                let alert = NSAlert()
                alert.messageText = "Trust this notebook's native packages?"
                alert.informativeText = "Run will load the prepared native libraries listed by this project and allow file reads. Native code has the same access as the app. Only continue if you trust these packages. Opening a notebook never loads them."
                alert.addButton(withTitle: "Trust and Run")
                alert.addButton(withTitle: "Cancel")
                guard alert.runModal() == .alertFirstButtonReturn else { completion?(false); return }
                nativeTrusted = true
            }
        }
        busy = true
        error = nil
        liveTab = target?.id ?? ""
        trackExecution(isolatedExecution && ["run", "run_all", "apply"].contains(action))
        worker.execute(edits: edits, action: action, tab: target?.id ?? "",
                       cell: commandCellID ?? targetCell?.id ?? "", text: text,
                       trustNative: executes && nativeTrusted) { [weak self] state, error in
            guard let self else { return }
            self.busy = false
            self.pollLive()
            self.trackExecution(false)
            if let state { self.accept(state) }
            if error == nil { self.liveCells = [:] }
            if ["add_cell", "add_text", "delete_cell", "move_up", "move_down"].contains(action) {
                self.selectedCellID = self.activeTab?.selected_cell ?? ""
            }
            self.error = error
            self.pauseForCodeDrafts()
            self.onChange?()
            completion?(error == nil)
        }
    }

    func shutdown(completion: @escaping () -> Void) {
        closeCompletions.append(completion)
        guard !closing else { return }
        closing = true
        busy = true
        worker.close { [weak self] in
            guard let self else { return }
            self.closing = false
            self.busy = false
            let completions = self.closeCompletions
            self.closeCompletions.removeAll()
            completions.forEach { $0() }
        }
    }
}
