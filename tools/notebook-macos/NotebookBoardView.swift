import SwiftUI

struct NotebookBoardView: View {
    @ObservedObject var model: NotebookModel
    let board: NotebookBoard
    let onDesign: () -> Void

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                VStack(alignment: .leading) {
                    Text(board.title).font(.headline)
                    Text("Board · \(model.controller(for: board)?.title ?? board.sheet)").font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                if model.busy { ProgressView().controlSize(.small) }
                Button("Design…", systemImage: "slider.horizontal.3", action: onDesign)
                Button("Open Sheet", systemImage: "doc.text") {
                    if let tab = model.controller(for: board) { model.selectTab(tab) }
                }
                Button("Run Sheet", systemImage: "play.fill") {
                    if let tab = model.controller(for: board) { model.perform("run_all", tab: tab) }
                }.buttonStyle(.borderedProminent).tint(.orange)
            }.padding(18).disabled(model.busy)
            Divider()
            GeometryReader { geometry in
              ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    if let tab = model.controller(for: board) {
                        Text(tab.environment_stale ? "Modules changed. Open the sheet and Apply Modules before running."
                             : model.isolatedExecution
                                ? "Figures and progress update live. Run Sheet starts a new run; Stop preserves the last received frame."
                                : "Inputs update Watch cells after the sheet has run. Manual cells require an explicit run.")
                            .font(.callout).foregroundStyle(.secondary)
                        if !model.busy && !tab.status.isEmpty { Text(tab.status).font(.caption).foregroundStyle(.secondary) }
                        LazyVGrid(columns: Array(repeating: GridItem(.flexible(minimum: 180), alignment: .top),
                                                 count: max(1, min(board.columns, Int((geometry.size.width - 48) / 300)))), alignment: .leading, spacing: 16) {
                            ForEach(board.components) { component in componentCard(component, tab: tab) }
                        }
                    } else { unavailable("Controller sheet unavailable") }
                    if board.components.isEmpty {
                        ContentUnavailableView("An empty canvas", systemImage: "square.grid.2x2",
                                               description: Text("Choose Design to add inputs, text, figures and run buttons."))
                    }
                }.padding(24)
              }
            }
        }.accessibilityIdentifier("notebook-board")
    }

    private func componentCard(_ component: NotebookBoardComponent, tab: NotebookTab) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(component.title).font(.headline)
            if component.kind == "input" {
                if let input = model.snapshot?.inputs?.first(where: { $0.id == component.input }) {
                    BoardInputField(model: model, input: input).id(input.id)
                } else { unavailable("Input unavailable") }
            } else if let cell = tab.cells.first(where: { $0.id == component.cell && $0.kind == "code" }) {
                if component.kind == "plot" {
                    let live = model.liveCell(cell, in: tab)
                    let progress = live?.progress ?? cell.progress ?? []
                    if !progress.isEmpty { NotebookProgressView(items: progress, running: model.executingWorker && live != nil) }
                    let panels = (live?.displays.isEmpty == false ? live?.displays : cell.displays) ?? []
                    if !panels.isEmpty {
                        NotebookFigureView(panels: panels, stale: live == nil ? model.figureIsStale(for: cell, in: tab) : !model.executingWorker)
                    } else { unavailable("Run the sheet to publish a figure with notebook.show(…)") }
                } else if component.kind == "text" {
                    let binding = component.binding ?? ""
                    let local = cell.locals.first { $0.name == binding && $0.role == "module_cell" }
                    // A blocked cell's diagnostic result is the literal
                    // "error", not a published expression value.
                    let value = binding.isEmpty ? (cell.ok ? cell.result : nil) : (local?.initialized == true ? local?.text_value ?? local?.value : nil)
                    Text(value.flatMap { $0.isEmpty ? nil : $0 } ?? "Unavailable")
                        .font(.system(size: 26, weight: .medium, design: .rounded))
                        .textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
                    if model.figureIsStale(for: cell, in: tab) {
                        Label(value == nil ? "Requires evaluation" : "Previous value · stale", systemImage: "clock").font(.caption).foregroundStyle(.orange)
                    }
                } else if component.kind == "run" {
                    Button(component.title, systemImage: "play.fill") { model.perform("run", tab: tab, cell: cell) }
                        .buttonStyle(.borderedProminent).disabled(model.busy || tab.environment_stale)
                    Text("Runs cell \(cell.id) explicitly").font(.caption).foregroundStyle(.secondary)
                }
                if !cell.error.isEmpty {
                    Text(cell.error).font(.caption).foregroundStyle(cell.dirty ? Color.orange : Color.red).textSelection(.enabled)
                }
            } else { unavailable("Cell \(component.cell ?? "?") unavailable") }
        }
        .frame(maxWidth: .infinity, alignment: .leading).padding(18)
        .background(.white.opacity(0.9), in: RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(.gray.opacity(0.18)))
    }
    private func unavailable(_ message: String) -> some View {
        Label(message, systemImage: "questionmark.circle").font(.callout).foregroundStyle(.secondary)
    }
}

private struct BoardInputField: View {
    @ObservedObject var model: NotebookModel
    let input: NotebookProjectInput
    @State private var draft = ""
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            if input.type == "boolean" {
                Toggle(input.title, isOn: Binding(get: { input.value_json == "true" }, set: {
                    model.setProjectInput(input, text: $0 ? "true" : "false")
                }))
            } else {
                HStack {
                    TextField(input.title, text: $draft).textFieldStyle(.roundedBorder)
                        .onSubmit { model.setProjectInput(input, text: draft) }.accessibilityLabel(input.title)
                    Button("Apply") { model.setProjectInput(input, text: draft) }.disabled(draft == input.editingText)
                }
                Text("Current: \(input.editingText)").font(.caption).foregroundStyle(.secondary)
            }
            Text("notebook.input(\"\(input.id)\")").font(.system(.caption, design: .monospaced))
                .foregroundStyle(.secondary).textSelection(.enabled)
        }
        .onAppear { draft = input.editingText }
        .onChange(of: input.value_json) { draft = input.editingText }.disabled(model.busy)
    }
}

// The first designer is an ordered grid and property inspector, not a code
// generator. Stable component IDs preserve opaque metadata on the backend.
struct NotebookBoardEditor: View {
    @ObservedObject var model: NotebookModel
    @State var board: NotebookBoard
    let document: UUID
    @Environment(\.dismiss) private var dismiss
    @State private var newInput = false
    @State private var saveError: String?
    private var sheets: [NotebookTab] { model.snapshot?.tabs.filter { $0.kind == "sheet" } ?? [] }
    private var cells: [NotebookCell] { model.controller(for: board)?.cells.filter { $0.kind == "code" } ?? [] }
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                Text("Board Designer").font(.title2.bold()); Spacer()
                Button("New Project Input…") { newInput = true }
            }
            HStack {
                TextField("Board title", text: $board.title).textFieldStyle(.roundedBorder)
                Picker("Sheet", selection: $board.sheet) {
                    ForEach(sheets) { Text($0.title).tag(String($0.id.dropFirst("sheet:".count))) }
                }.frame(width: 220)
                Stepper("\(board.columns) columns", value: $board.columns, in: 1...6).frame(width: 150)
            }
            Text("Arrange components in reading order. Outputs bind to stable cell IDs in the controller sheet.")
                .font(.callout).foregroundStyle(.secondary)
            ScrollView {
                VStack(spacing: 12) {
                    ForEach(board.components) { component in componentEditor(component) }
                }
            }.frame(minHeight: 250)
            HStack {
                Menu("Add Component") {
                    Button("Input") { add("input") }; Button("Text / Value") { add("text") }
                    Button("Figure") { add("plot") }; Button("Run Button") { add("run") }
                }.disabled(board.components.count >= 256)
                Spacer()
                Button("Cancel", role: .cancel) { dismiss() }.keyboardShortcut(.cancelAction)
                Button("Save Board") {
                    guard model.documentIdentity == document else { saveError = "Project changed. Close this editor."; return }
                    model.saveBoard(board) { success in
                        if success { dismiss() } else { saveError = model.error; model.clearError() }
                    }
                }.keyboardShortcut(.defaultAction).disabled(board.title.isEmpty || board.sheet.isEmpty)
            }
        }
        .padding(24).frame(width: 780, height: 570).disabled(model.busy).interactiveDismissDisabled()
        .onAppear { model.boardEditorOpen = true }
        .onDisappear { model.boardEditorOpen = false }
        .sheet(isPresented: $newInput) { NotebookInputEditor(model: model, document: document) }
        .alert("Could not save board", isPresented: Binding(get: { saveError != nil }, set: { if !$0 { saveError = nil } })) {
            Button("OK") { saveError = nil }
        } message: { Text(saveError ?? "") }
    }
    private func componentEditor(_ component: NotebookBoardComponent) -> some View {
        let index = board.components.firstIndex { $0.id == component.id } ?? 0
        // Bind by stable identity: a focused row may send its final edit
        // after a neighboring row was removed/reordered.
        let item = Binding<NotebookBoardComponent>(get: {
            board.components.first { $0.id == component.id } ?? component
        }, set: { value in
            if let index = board.components.firstIndex(where: { $0.id == component.id }) {
                board.components[index] = value
            }
        })
        return VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("\(index + 1). \(component.kind.capitalized)").font(.headline).frame(width: 95, alignment: .leading)
                TextField("Title", text: item.title).textFieldStyle(.roundedBorder)
                Button { move(component.id, by: -1) } label: { Image(systemName: "arrow.up") }.disabled(index == 0)
                Button { move(component.id, by: 1) } label: { Image(systemName: "arrow.down") }.disabled(index + 1 == board.components.count)
                Button(role: .destructive) { board.components.removeAll { $0.id == component.id } } label: { Image(systemName: "trash") }
            }
            if component.kind == "input" {
                Picker("Project input", selection: Binding(get: { item.wrappedValue.input ?? "" }, set: { item.wrappedValue.input = $0 })) {
                    Text("Choose input…").tag("")
                    ForEach(model.snapshot?.inputs ?? []) { Text("\($0.title) · \($0.id)").tag($0.id) }
                }
            } else {
                HStack {
                    Picker("Cell", selection: Binding(get: { item.wrappedValue.cell ?? "" }, set: { item.wrappedValue.cell = $0 })) {
                        Text("Choose cell…").tag("")
                        if let selected = component.cell, !selected.isEmpty, !cells.contains(where: { $0.id == selected }) {
                            Text("\(selected) · unavailable").tag(selected)
                        }
                        ForEach(cells) { cell in Text("\(cell.id) · \(cell.source.components(separatedBy: .newlines).first ?? "")").tag(cell.id) }
                    }
                    if component.kind == "text" {
                        TextField("Variable (empty = cell result)", text: Binding(
                            get: { item.wrappedValue.binding ?? "" }, set: { item.wrappedValue.binding = $0 }))
                            .textFieldStyle(.roundedBorder).frame(width: 250)
                    }
                }
            }
        }.padding(12).background(.gray.opacity(0.07), in: RoundedRectangle(cornerRadius: 9))
    }
    private func move(_ id: String, by offset: Int) {
        guard let index = board.components.firstIndex(where: { $0.id == id }),
              board.components.indices.contains(index + offset) else { return }
        board.components.swapAt(index, index + offset)
    }
    private func add(_ kind: String) {
        board.components.append(NotebookBoardComponent(id: "component_" + UUID().uuidString.lowercased().replacingOccurrences(of: "-", with: "_"),
            kind: kind, title: kind == "plot" ? "Figure" : kind.capitalized,
            input: kind == "input" ? model.snapshot?.inputs?.first?.id ?? "" : nil,
            cell: kind == "input" ? nil : cells.first?.id ?? "", binding: nil))
    }
}

private struct NotebookInputEditor: View {
    @ObservedObject var model: NotebookModel
    let document: UUID
    @Environment(\.dismiss) private var dismiss
    @State private var id = ""
    @State private var title = ""
    @State private var type = "number"
    @State private var initial = "0"
    @State private var error: String?
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("New Project Input").font(.title2.bold())
            Text("Stored independently of the board. Creating an input saves its definition; Cancel in the board designer does not delete it.")
                .font(.callout).foregroundStyle(.secondary)
            TextField("Name used in notebook.input(…)", text: $id)
            TextField("Display title", text: $title)
            Picker("Type", selection: $type) {
                Text("Number").tag("number"); Text("Integer").tag("integer")
                Text("Boolean").tag("boolean"); Text("String").tag("string")
            }.onChange(of: type) { initial = type == "boolean" ? "false" : type == "string" ? "" : "0" }
            TextField("Default value", text: $initial)
            if let error { Text(error).foregroundStyle(.red).font(.caption) }
            HStack {
                Spacer()
                Button("Cancel", role: .cancel) { dismiss() }.keyboardShortcut(.cancelAction)
                Button("Create Input") { create() }.keyboardShortcut(.defaultAction).disabled(id.isEmpty)
            }
        }.textFieldStyle(.roundedBorder).padding(24).frame(width: 480).disabled(model.busy).interactiveDismissDisabled()
    }
    private func create() {
        guard model.documentIdentity == document else { error = "Project changed. Close this editor."; return }
        do {
            func quote(_ value: String) throws -> String { String(decoding: try JSONEncoder().encode(value), as: UTF8.self) }
            let json = try "{\"id\":\(quote(id)),\"title\":\(quote(title.isEmpty ? id : title)),\"type\":\(quote(type)),\"default\":\(NotebookProjectInput.valueJSON(initial, type: type))}"
            model.perform("new_input", text: json) { success in
                if success { dismiss() } else { error = model.error; model.clearError() }
            }
        } catch { self.error = error.localizedDescription }
    }
}
