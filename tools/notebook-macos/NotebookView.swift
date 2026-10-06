import SwiftUI

struct NotebookView: View {
    @State private var confirmForceStop = false
    @ObservedObject var model: NotebookModel

    @State private var collapsedCells = Set<String>()
    @State private var structurePrompt: StructurePrompt?
    @State private var pendingDelete: DeleteTarget?
    @State private var boardPrompt: NotebookBoard?

    private var activeTab: NotebookTab? {
        guard let snapshot = model.snapshot else { return nil }
        return snapshot.tabs.first { $0.id == snapshot.selected_tab }
    }

    private var selectedCell: NotebookCell? {
        guard let tab = activeTab else { return nil }
        return tab.cells.first { $0.id == model.selectedCellID }
            ?? tab.cells.first { $0.id == tab.selected_cell }
            ?? tab.cells.first
    }

    private var isModule: Bool { activeTab?.kind == "module" }

    var body: some View {
        Group {
            if let snapshot = model.snapshot {
                notebookWorkspace(snapshot)
                    .id(model.documentIdentity)
            } else {
                welcomeView
            }
        }
        .background(NotebookPalette.canvas)
        .confirmationDialog("Terminate this sheet's worker?", isPresented: $confirmForceStop, titleVisibility: .visible) {
            Button("Force Stop", role: .destructive) { model.stopExecution(force: true) }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Runtime state will be lost. Drafts and the last displayed results stay here. Restart Worker will be required; cells will not be replayed automatically.")
        }
        .frame(minWidth: 930, minHeight: 640)
        .alert("Notebook", isPresented: Binding(
            get: { model.error != nil },
            set: { if !$0 { model.clearError() } }
        )) {
            Button("OK", role: .cancel) { model.clearError() }
        } message: {
            Text(model.error ?? "")
        }
        .confirmationDialog(
            "Delete this cell?",
            isPresented: Binding(
                get: { pendingDelete != nil },
                set: { if !$0 { pendingDelete = nil } }
            ),
            titleVisibility: .visible
        ) {
            Button("Delete Cell", role: .destructive) {
                guard let target = pendingDelete else { return }
                model.perform("delete_cell", tab: target.tab, cell: target.cell)
                pendingDelete = nil
            }
            Button("Cancel", role: .cancel) { pendingDelete = nil }
        } message: {
            Text("The cell and its source will be removed from this sheet.")
        }
        .sheet(item: $structurePrompt) { prompt in
            StructurePromptSheet(prompt: prompt) { identifier in
                let action = prompt.kind == .sheet ? "new_sheet" : "new_module"
                model.perform(action, tab: activeTab, text: identifier)
                structurePrompt = nil
            } onCancel: {
                structurePrompt = nil
            }
        }
    }

    private func notebookWorkspace(_ snapshot: NotebookSnapshot) -> some View {
        NavigationSplitView {
            sidebar(snapshot)
                .navigationSplitViewColumnWidth(min: 220, ideal: 255, max: 310)
        } detail: {
            VStack(spacing: 0) {
                if model.isolatedExecution {
                    HStack(spacing: 12) {
                        Text("Isolated · \(model.executionLabel)").font(.system(size: 12, weight: .medium))
                        Spacer()
                        if model.executingWorker {
                            Button("Stop") { model.stopExecution() }.disabled(!model.canStop)
                            Button("Force Stop…", role: .destructive) { confirmForceStop = true }
                                .disabled(!model.canForceStop)
                        } else {
                            Button("Restart Worker") {
                                let tab = model.activeBoard.flatMap { model.controller(for: $0) } ?? activeTab
                                if let tab { model.perform("restart_worker", tab: tab) }
                            }.disabled(model.busy || (model.activeBoard == nil && isModule))
                        }
                    }
                    .padding(.horizontal, 19).padding(.vertical, 8)
                    .background(NotebookPalette.amber.opacity(0.08))
                    .help("Isolated sheet execution preview. Project inputs, module Run and background runtime-event pumping are not supported yet.")
                }
                if let board = model.activeBoard {
                    NotebookBoardView(model: model, board: board) { boardPrompt = board }
                } else if let tab = activeTab {
                    actionBar(snapshot)
                    Rectangle().fill(NotebookPalette.rule).frame(height: 1)
                    notebookContent(tab, snapshot: snapshot)
                } else {
                    missingTabView
                }
            }
            .background(NotebookPalette.canvas)
        }
        .navigationSplitViewStyle(.balanced)
        .navigationTitle(snapshot.title)
        .sheet(item: $boardPrompt) { board in
            NotebookBoardEditor(model: model, board: board, document: model.documentIdentity)
        }
    }

    private func sidebar(_ snapshot: NotebookSnapshot) -> some View {
        let modifiedTabCount = snapshot.tabs.filter { model.isModified($0) }.count
        return VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 11) {
                ZStack {
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .fill(NotebookPalette.amber.opacity(0.14))
                        .frame(width: 36, height: 36)
                    Image(systemName: "book.closed.fill")
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(NotebookPalette.amber)
                }
                VStack(alignment: .leading, spacing: 2) {
                    Text(snapshot.title)
                        .font(.system(size: 14, weight: .semibold))
                        .lineLimit(1)
                    Text(modifiedTabCount == 0 ? "All changes saved" : "\(modifiedTabCount) modified")
                        .font(.system(size: 11, weight: .medium))
                        .foregroundStyle(modifiedTabCount == 0 ? NotebookPalette.muted : NotebookPalette.amber)
                }
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 18)
            .padding(.top, 22)
            .padding(.bottom, 23)

            ScrollView {
                VStack(alignment: .leading, spacing: 22) {
                    tabSection(
                        "Sheets",
                        symbol: "plus",
                        actionLabel: "New sheet",
                        tabs: snapshot.tabs.filter { $0.kind == "sheet" }
                    ) {
                        structurePrompt = StructurePrompt(kind: .sheet)
                    }

                    tabSection(
                        "Modules",
                        symbol: "plus",
                        actionLabel: "New module",
                        tabs: snapshot.tabs.filter { $0.kind == "module" }
                    ) {
                        structurePrompt = StructurePrompt(kind: .module)
                    }
                    VStack(alignment: .leading, spacing: 8) {
                        HStack {
                            Text("BOARDS").font(.system(size: 10, weight: .bold)).foregroundStyle(NotebookPalette.muted)
                            Spacer()
                            Button {
                                let sheet = snapshot.tabs.first { $0.id == snapshot.selected_tab && $0.kind == "sheet" }
                                    ?? snapshot.tabs.first { $0.kind == "sheet" }
                                if let sheet { boardPrompt = .empty(sheet: String(sheet.id.dropFirst("sheet:".count))) }
                            } label: { Image(systemName: "plus") }
                                .help("New board").accessibilityLabel("New board").disabled(model.busy)
                        }.padding(.horizontal, 8)
                        ForEach(snapshot.boards ?? []) { board in
                            Button { model.selectBoard(board) } label: {
                                Label(board.title, systemImage: "square.grid.2x2")
                                    .frame(maxWidth: .infinity, alignment: .leading).padding(9)
                                    .background(model.selectedBoardID == board.id ? .white : .clear, in: RoundedRectangle(cornerRadius: 8))
                            }.buttonStyle(.plain).disabled(model.busy)
                        }
                    }
                }
                .padding(.horizontal, 12)
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        Text("DEPENDENCIES").font(.system(size: 10, weight: .bold)).foregroundStyle(NotebookPalette.muted)
                        Spacer()
                        Button { model.requestDependency?() } label: { Image(systemName: "plus") }
                            .help("Link package directory").accessibilityLabel("Link package directory").disabled(model.busy)
                    }
                    ForEach(snapshot.dependencies ?? []) { dependency in
                        Label(dependency.path, systemImage: "link")
                            .font(.system(size: 11)).lineLimit(3).help(dependency.path)
                            .contextMenu {
                                Button("Unlink (keep package files)") {
                                    model.perform("remove_dependency", text: dependency.path)
                                }.disabled(model.busy)
                            }
                    }
                    Text("External sources · explicit imports").font(.caption2).foregroundStyle(.secondary)
                }.padding(.horizontal, 20).padding(.top, 16)
                .padding(.bottom, 22)
            }

            Spacer(minLength: 0)

            HStack(spacing: 7) {
                Image(systemName: "circle.grid.2x2")
                    .font(.system(size: 11, weight: .medium))
                Text("Environment · v\(snapshot.environment_generation)")
                    .font(.system(size: 11, weight: .medium))
            }
            .foregroundStyle(NotebookPalette.muted)
            .padding(.horizontal, 19)
            .padding(.vertical, 13)
            .frame(maxWidth: .infinity, alignment: .leading)
            .overlay(alignment: .top) {
                Rectangle().fill(NotebookPalette.rule).frame(height: 1)
            }
        }
        .background(NotebookPalette.sidebar)
    }

    private func tabSection(
        _ title: String,
        symbol: String,
        actionLabel: String,
        tabs: [NotebookTab],
        onAdd: @escaping () -> Void
    ) -> some View {
        VStack(alignment: .leading, spacing: 7) {
            HStack {
                Text(title.uppercased())
                    .font(.system(size: 10, weight: .bold, design: .rounded))
                    .tracking(1.1)
                    .foregroundStyle(NotebookPalette.muted)
                Spacer()
                Button(action: onAdd) {
                    Image(systemName: symbol)
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundStyle(NotebookPalette.ink)
                        .frame(width: 25, height: 25)
                        .background(.white.opacity(0.75), in: RoundedRectangle(cornerRadius: 7, style: .continuous))
                }
                .buttonStyle(.plain)
                .help(actionLabel)
                .accessibilityLabel(actionLabel)
                .disabled(model.busy)
            }
            .padding(.horizontal, 8)

            if tabs.isEmpty {
                Text(title == "Sheets" ? "No sheets yet" : "No modules yet")
                    .font(.system(size: 12))
                    .foregroundStyle(NotebookPalette.muted)
                    .padding(.horizontal, 9)
                    .padding(.vertical, 6)
            } else {
                ForEach(tabs) { tab in
                    sidebarRow(tab)
                }
            }
        }
    }

    private func sidebarRow(_ tab: NotebookTab) -> some View {
        let selected = model.activeBoard == nil && activeTab?.id == tab.id
        let modified = model.isModified(tab)
        return Button {
            model.selectTab(tab)
        } label: {
            HStack(spacing: 10) {
                Image(systemName: tab.kind == "module" ? "shippingbox" : "doc.text")
                    .font(.system(size: 13, weight: selected ? .semibold : .regular))
                    .foregroundStyle(selected ? NotebookPalette.amber : NotebookPalette.muted)
                    .frame(width: 17)
                Text(tab.title)
                    .font(.system(size: 13, weight: selected ? .semibold : .regular))
                    .foregroundStyle(NotebookPalette.ink)
                    .lineLimit(1)
                Spacer(minLength: 2)

                if tab.environment_stale {
                    Text("ENV")
                        .font(.system(size: 8, weight: .bold, design: .rounded))
                        .tracking(0.4)
                        .foregroundStyle(NotebookPalette.amber)
                        .padding(.horizontal, 5)
                        .padding(.vertical, 3)
                        .background(NotebookPalette.amber.opacity(0.12), in: Capsule())
                        .help("This sheet needs the current module environment applied")
                }
                if modified {
                    Circle()
                        .fill(NotebookPalette.amber)
                        .frame(width: 6, height: 6)
                        .accessibilityLabel("Modified")
                }
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 8)
            .contentShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
            .background(selected ? .white : .clear, in: RoundedRectangle(cornerRadius: 8, style: .continuous))
            .overlay {
                if selected {
                    RoundedRectangle(cornerRadius: 8, style: .continuous)
                        .stroke(NotebookPalette.rule, lineWidth: 1)
                }
            }
        }
        .buttonStyle(.plain)
        .disabled(model.busy)
        .accessibilityAddTraits(selected ? .isSelected : [])
    }

    private func actionBar(_ snapshot: NotebookSnapshot) -> some View {
        HStack(spacing: 10) {
            if let tab = activeTab {
                VStack(alignment: .leading, spacing: 2) {
                    Text(tab.title)
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundStyle(NotebookPalette.ink)
                    Text(tab.kind == "module" ? "Full-profile module" : "Notebook sheet")
                        .font(.system(size: 10, weight: .medium))
                        .foregroundStyle(NotebookPalette.muted)
                }
                .lineLimit(1)
            }

            Spacer(minLength: 10)

            if model.busy {
                ProgressView()
                    .controlSize(.small)
                    .help("Notebook work is in progress")
                    .padding(.trailing, 3)
            }

            Button {
                guard let tab = activeTab else { return }
                model.perform("save", tab: tab)
            } label: {
                Label("Save", systemImage: "square.and.arrow.down")
            }
            .keyboardShortcut("s", modifiers: .command)
            .help("Save this tab · ⌘S")
            .disabled(model.busy || activeTab == nil || !(activeTab.map { model.isModified($0) } ?? false))
            .buttonStyle(.bordered)

            Button {
                runSelected()
            } label: {
                Label(isModule ? "Run Isolated" : "Run Selected", systemImage: "play.fill")
            }
            .keyboardShortcut(.return, modifiers: .command)
            .help(isModule ? "Run this module in an isolated runtime · ⌘Return" : "Run the selected cell · ⌘Return")
            .buttonStyle(.borderedProminent)
            .tint(NotebookPalette.amber)
            .disabled(!canRunSelected)

            if !isModule {
                Button {
                    guard let tab = activeTab else { return }
                    model.perform("run_all", tab: tab)
                } label: {
                    Label("Run All", systemImage: "forward.end.fill")
                }
                .help("Run every cell in this sheet")
                .buttonStyle(.bordered)
                .disabled(model.busy || activeTab == nil || hasUnsupportedCell)

                Button {
                    guard let tab = activeTab else { return }
                    model.perform("apply", tab: tab)
                } label: {
                    Label("Apply Modules", systemImage: "arrow.triangle.2.circlepath")
                }
                .help("Build and apply the saved module environment to this sheet")
                .buttonStyle(.bordered)
                .disabled(model.busy || activeTab == nil)
            }
        }
        .padding(.horizontal, 19)
        .padding(.vertical, 11)
        .background(NotebookPalette.topBar)
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Notebook actions")
    }

    private var canRunSelected: Bool {
        guard !model.busy, let tab = activeTab, let cell = selectedCell else { return false }
        if tab.kind == "module" { return cell.kind == "code" && !model.isolatedExecution }
        return cell.kind == "code" && !tab.environment_stale && !hasUnsupportedCell
    }

    private var hasUnsupportedCell: Bool {
        guard let tab = activeTab, tab.kind == "sheet" else { return false }
        return tab.cells.contains { $0.kind != "code" && $0.kind != "text" }
    }

    private func runSelected() {
        guard let tab = activeTab, let cell = selectedCell, canRunSelected else { return }
        model.perform("run", tab: tab, cell: cell)
    }

    private func notebookContent(_ tab: NotebookTab, snapshot: NotebookSnapshot) -> some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 17) {
                pageHeader(tab, snapshot: snapshot)

                if tab.environment_stale {
                    noticeCard(
                        symbol: "arrow.triangle.2.circlepath",
                        title: "Module environment changed",
                        message: "Apply the current modules to this sheet before running cells.",
                        tone: .amber
                    )
                }

                if hasUnsupportedCell {
                    let kinds = Array(Set(tab.cells.filter { $0.kind != "code" && $0.kind != "text" }.map(\.kind))).sorted().joined(separator: ", ")
                    noticeCard(
                        symbol: "lock.fill",
                        title: "Evaluation is paused",
                        message: "This sheet contains unsupported cell types (\(kinds)). The cells are preserved as read-only placeholders.",
                        tone: .slate
                    )
                }

                if !tab.environment_error.isEmpty {
                    noticeCard(
                        symbol: "exclamationmark.triangle.fill",
                        title: "Module environment error",
                        message: tab.environment_error,
                        tone: .red
                    )
                }

                if !tab.environment_output.isEmpty {
                    environmentOutputCard(tab.environment_output)
                }

                if tab.cells.isEmpty {
                    emptySheetCard(tab)
                } else {
                    ForEach(tab.cells.enumerated().map {
                        PositionedNotebookCell(position: $0.offset, cell: $0.element)
                    }) { item in
                        cellCard(item.cell, index: item.position, tab: tab)
                    }
                }

                if tab.kind == "sheet" {
                    HStack(spacing: 12) {
                    Button {
                        model.perform("add_cell", tab: tab, cell: tab.cells.last)
                    } label: {
                        Label("Add code cell", systemImage: "plus")
                            .font(.system(size: 13, weight: .semibold))
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 13)
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(NotebookPalette.amber)
                    .background(.white.opacity(0.58), in: RoundedRectangle(cornerRadius: 11, style: .continuous))
                    .overlay {
                        RoundedRectangle(cornerRadius: 11, style: .continuous)
                            .strokeBorder(NotebookPalette.rule, style: StrokeStyle(lineWidth: 1, dash: [4, 4]))
                    }
                    .disabled(model.busy)
                    .help("Insert a new code cell after the last cell")
                    Button {
                        model.perform("add_text", tab: tab, cell: tab.cells.last)
                    } label: {
                        Label("Add text block", systemImage: "textformat")
                            .font(.system(size: 13, weight: .semibold))
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 13)
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(NotebookPalette.amber)
                    .background(.white.opacity(0.58), in: RoundedRectangle(cornerRadius: 11))
                    .overlay(RoundedRectangle(cornerRadius: 11).strokeBorder(NotebookPalette.rule, style: StrokeStyle(lineWidth: 1, dash: [4, 4])))
                    .disabled(model.busy)
                    .help("Insert a formatted, non-executable text block")
                    }
                }

                Text(tab.status)
                    .font(.system(size: 11, weight: .medium))
                    .foregroundStyle(NotebookPalette.muted)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.top, 2)
            }
            .frame(maxWidth: 960, alignment: .leading)
            .frame(maxWidth: .infinity)
            .padding(.horizontal, 28)
            .padding(.top, 28)
            .padding(.bottom, 36)
        }
        .scrollIndicators(.hidden)
        .background(NotebookPalette.canvas)
        .id(tab.id)
    }

    private func pageHeader(_ tab: NotebookTab, snapshot: NotebookSnapshot) -> some View {
        HStack(alignment: .center, spacing: 18) {
            VStack(alignment: .leading, spacing: 6) {
                HStack(spacing: 9) {
                    Text(tab.title)
                        .font(.system(size: 27, weight: .semibold, design: .rounded))
                        .foregroundStyle(NotebookPalette.ink)
                    statusPill(tab)
                }
                Text(tab.kind == "module"
                     ? "Full-profile source · isolated execution keeps sheet runtimes separate"
                     : "A persistent Amber worksheet")
                    .font(.system(size: 12, weight: .regular))
                    .foregroundStyle(NotebookPalette.muted)
            }

            Spacer(minLength: 0)

            if tab.kind == "module" {
                Button {
                    model.perform("auto_import", tab: tab, text: tab.auto_import ? "off" : "on")
                } label: {
                    Label(
                        tab.auto_import ? "Auto-import on" : "Auto-import off",
                        systemImage: tab.auto_import ? "checkmark.circle.fill" : "circle"
                    )
                    .font(.system(size: 11, weight: .semibold))
                    .padding(.horizontal, 11)
                    .padding(.vertical, 8)
                    .foregroundStyle(tab.auto_import ? NotebookPalette.amber : NotebookPalette.ink)
                    .background(.white, in: Capsule())
                    .overlay(Capsule().stroke(NotebookPalette.rule, lineWidth: 1))
                }
                .buttonStyle(.plain)
                .disabled(model.busy)
                .help("Choose whether this saved module is applied as an ambient sheet import")
                .accessibilityLabel(tab.auto_import ? "Disable module auto-import" : "Enable module auto-import")
            } else {
                HStack(spacing: 7) {
                    Image(systemName: "circle.grid.2x2")
                    Text("ENV \(snapshot.environment_generation)")
                }
                .font(.system(size: 10, weight: .bold, design: .rounded))
                .tracking(0.4)
                .foregroundStyle(NotebookPalette.muted)
                .padding(.horizontal, 10)
                .padding(.vertical, 7)
                .background(NotebookPalette.card, in: Capsule())
                .overlay(Capsule().stroke(NotebookPalette.rule, lineWidth: 1))
                .accessibilityLabel("Environment generation \(snapshot.environment_generation)")
            }
        }
        .padding(.bottom, 3)
    }

    @ViewBuilder
    private func statusPill(_ tab: NotebookTab) -> some View {
        if model.isModified(tab) {
            Text("MODIFIED")
                .font(.system(size: 8, weight: .bold, design: .rounded))
                .tracking(0.6)
                .foregroundStyle(NotebookPalette.amber)
                .padding(.horizontal, 7)
                .padding(.vertical, 4)
                .background(NotebookPalette.amber.opacity(0.12), in: Capsule())
        } else if tab.environment_stale {
            Text("STALE ENV")
                .font(.system(size: 8, weight: .bold, design: .rounded))
                .tracking(0.6)
                .foregroundStyle(NotebookPalette.amber)
                .padding(.horizontal, 7)
                .padding(.vertical, 4)
                .background(NotebookPalette.amber.opacity(0.12), in: Capsule())
        }
    }

    @ViewBuilder
    private func cellCard(_ cell: NotebookCell, index: Int, tab: NotebookTab) -> some View {
        if cell.kind == "text" { textBlockCard(cell, index: index, tab: tab) }
        else { codeCellCard(cell, index: index, tab: tab) }
    }

    private func textBlockCard(_ cell: NotebookCell, index: Int, tab: NotebookTab) -> some View {
        let selected = model.selectedCellID == cell.id
        let document = model.documentIdentity
        return VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 10) {
                Text(String(format: "%02d", index + 1))
                    .font(.system(size: 10, weight: .bold, design: .monospaced))
                    .foregroundStyle(NotebookPalette.muted)
                Label("TEXT", systemImage: "textformat")
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundStyle(NotebookPalette.muted)
                Spacer()
                iconAction("arrow.up", label: "Move text block up", disabled: model.busy || index == 0) {
                    model.perform("move_up", tab: tab, cell: cell)
                }
                iconAction("arrow.down", label: "Move text block down", disabled: model.busy || index == tab.cells.count - 1) {
                    model.perform("move_down", tab: tab, cell: cell)
                }
                iconAction("trash", label: "Delete text block", disabled: model.busy, role: .destructive) {
                    pendingDelete = DeleteTarget(tab: tab, cell: cell)
                }
            }
            RichTextEditor(text: model.source(for: cell, in: tab),
                           formatting: model.formatting(for: cell, in: tab),
                           editable: model.canEdit,
                           variables: model.textVariables(before: cell, in: tab),
                           onChange: { text, formatting in
                               model.editRichText(text, formatting: formatting, cell: cell, tab: tab)
                           },
                           onFocus: { model.selectedCellID = cell.id },
                           onError: { model.reportEditorError($0, cell: cell, tab: tab, document: document) })
        }
        .padding(16)
        .background(NotebookPalette.card, in: RoundedRectangle(cornerRadius: 13))
        .overlay(RoundedRectangle(cornerRadius: 13).stroke(selected ? NotebookPalette.amber.opacity(0.43) : NotebookPalette.rule, lineWidth: 1))
        .simultaneousGesture(TapGesture().onEnded { model.selectedCellID = cell.id })
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Text block \(index + 1)")
    }

    private func codeCellCard(_ cell: NotebookCell, index: Int, tab: NotebookTab) -> some View {
        let supported = cell.kind == "code"
        let selected = model.selectedCellID == cell.id || (model.selectedCellID.isEmpty && tab.selected_cell == cell.id)
        let collapsed = collapsedCells.contains(cellKey(tab: tab, cell: cell))
        let modified = cell.dirty || model.source(for: cell, in: tab) != cell.source

        return VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 10) {
                Text(String(format: "%02d", index + 1))
                    .font(.system(size: 10, weight: .bold, design: .monospaced))
                    .foregroundStyle(NotebookPalette.muted)
                    .frame(width: 25, height: 25)
                    .background(NotebookPalette.canvas, in: Circle())

                Text(supported ? (tab.kind == "module" ? "MODULE SOURCE" : "CODE") : cell.kind.uppercased())
                    .font(.system(size: 9, weight: .bold, design: .rounded))
                    .tracking(0.9)
                    .foregroundStyle(supported ? NotebookPalette.muted : NotebookPalette.slate)

                if tab.kind == "sheet", supported {
                    stateBadge(modified ? "STALE" : (cell.ok ? "CURRENT" : (cell.error.isEmpty ? "NOT RUN" : "ERROR")), tone: modified ? .amber : (cell.ok ? .green : (cell.error.isEmpty ? .slate : .red)))
                }

                Spacer(minLength: 4)

                if supported && tab.kind == "sheet" {
                    Button {
                        model.perform("watch", tab: tab, cell: cell, text: cell.watch ? "off" : "on")
                    } label: {
                        Label(cell.watch ? "Watch" : "Manual", systemImage: cell.watch ? "eye" : "hand.raised")
                            .font(.system(size: 10, weight: .medium))
                    }
                    .buttonStyle(.borderless)
                    .disabled(model.busy)
                    .help(cell.watch ? "Turn off automatic watch evaluation" : "Turn on automatic watch evaluation")

                    iconAction("arrow.up", label: "Move cell up", disabled: model.busy || index == 0) {
                        model.perform("move_up", tab: tab, cell: cell)
                    }
                    iconAction("arrow.down", label: "Move cell down", disabled: model.busy || index == tab.cells.count - 1) {
                        model.perform("move_down", tab: tab, cell: cell)
                    }
                    iconAction("trash", label: "Delete cell", disabled: model.busy, role: .destructive) {
                        pendingDelete = DeleteTarget(tab: tab, cell: cell)
                    }
                }

                if supported {
                    iconAction(collapsed ? "chevron.down" : "chevron.up",
                               label: collapsed ? "Show source code" : "Collapse source code",
                               disabled: false) {
                        if collapsed { collapsedCells.remove(cellKey(tab: tab, cell: cell)) }
                        else { collapsedCells.insert(cellKey(tab: tab, cell: cell)) }
                    }
                }
            }

            if supported {
                if collapsed {
                    Button {
                        collapsedCells.remove(cellKey(tab: tab, cell: cell))
                    } label: {
                        HStack(spacing: 8) {
                            Image(systemName: "chevron.right")
                                .font(.system(size: 10, weight: .bold))
                            Text("\(model.source(for: cell, in: tab).components(separatedBy: .newlines).count) source lines hidden")
                                .font(.system(size: 11, weight: .medium))
                            Spacer()
                        }
                        .foregroundStyle(NotebookPalette.muted)
                        .padding(.horizontal, 15)
                        .padding(.vertical, 12)
                    }
                    .buttonStyle(.plain)
                    .help("Show source code")
                } else {
                    CodeEditor(
                        text: Binding(
                            get: { model.source(for: cell, in: tab) },
                            set: { model.editSource($0, cell: cell, tab: tab) }
                        ),
                        editable: model.canEdit,
                        onFocus: { model.selectedCellID = cell.id },
                        onRun: { model.selectedCellID = cell.id; runSelected() }
                    )
                    .frame(maxWidth: .infinity)
                    .background(NotebookPalette.editor, in: RoundedRectangle(cornerRadius: 9, style: .continuous))
                    .overlay(RoundedRectangle(cornerRadius: 9, style: .continuous).stroke(NotebookPalette.rule, lineWidth: 1))
                    .clipShape(RoundedRectangle(cornerRadius: 9, style: .continuous))
                }
            } else {
                VStack(alignment: .leading, spacing: 6) {
                    Label("Read-only placeholder", systemImage: "lock.fill")
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundStyle(NotebookPalette.slate)
                    Text("This cell kind is preserved, but its execution semantics are not supported by this editor.")
                        .font(.system(size: 11))
                        .foregroundStyle(NotebookPalette.muted)
                    if !cell.source.isEmpty {
                        Text(cell.source)
                            .font(.system(size: 12, design: .monospaced))
                            .textSelection(.enabled)
                            .foregroundStyle(NotebookPalette.ink)
                    }
                }
                .padding(14)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(NotebookPalette.canvas.opacity(0.72), in: RoundedRectangle(cornerRadius: 9, style: .continuous))
            }

            if supported {
                let live = model.liveCell(cell, in: tab)
                let progress = live?.progress ?? cell.progress ?? []
                if !progress.isEmpty {
                    NotebookProgressView(items: progress, running: model.executingWorker && live != nil)
                }
                evaluationOutput(cell, isStale: modified)
                let panels = (live?.displays.isEmpty == false ? live?.displays : cell.displays) ?? []
                if !panels.isEmpty {
                    NotebookFigureView(panels: panels, stale: live == nil ? model.figureIsStale(for: cell, in: tab) : !model.executingWorker)
                }
                if selected && !cell.locals.isEmpty {
                    localsPanel(cell.locals)
                }
            }
        }
        .padding(14)
        .background(selected ? NotebookPalette.cardSelected : NotebookPalette.card,
                    in: RoundedRectangle(cornerRadius: 13, style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: 13, style: .continuous)
                .stroke(selected ? NotebookPalette.amber.opacity(0.43) : NotebookPalette.rule, lineWidth: selected ? 1.2 : 1)
        }
        .shadow(color: .black.opacity(selected ? 0.035 : 0.018), radius: 8, x: 0, y: 3)
        .simultaneousGesture(TapGesture().onEnded { model.selectedCellID = cell.id })
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Cell \(index + 1)")
    }

    @ViewBuilder
    private func evaluationOutput(_ cell: NotebookCell, isStale: Bool) -> some View {
        if !cell.error.isEmpty {
            outputBlock(title: "Error", text: cell.error, symbol: "exclamationmark.triangle.fill", tone: .red)
        }

        ForEach(Array(cell.output.indices), id: \.self) { index in
            let event = cell.output[index]
            outputBlock(
                title: event.stream.uppercased(),
                text: event.text,
                symbol: event.stream.lowercased().contains("err") ? "exclamationmark.bubble" : "text.alignleft",
                tone: event.stream.lowercased().contains("err") ? .red : .slate
            )
        }

        if !cell.result.isEmpty && cell.result != "not evaluated" {
            NotebookResultView(
                title: isStale ? "LAST RESULT · STALE" : "RESULT",
                text: cell.result,
                pretty: cell.result_format?.is_container == true ? cell.result_format?.pretty : nil,
                truncated: cell.result_format?.truncated ?? false,
                prettyTruncated: cell.result_format?.pretty_truncated ?? false,
                tint: isStale ? BadgeTone.amber.color : BadgeTone.green.color
            )
        }
    }

    private func outputBlock(title: String, text: String, symbol: String, tone: BadgeTone) -> some View {
        VStack(alignment: .leading, spacing: 7) {
            Label(title, systemImage: symbol)
                .font(.system(size: 9, weight: .bold, design: .rounded))
                .tracking(0.7)
                .foregroundStyle(tone.color)
            Text(text)
                .font(.system(size: 12, weight: .regular, design: .monospaced))
                .foregroundStyle(NotebookPalette.ink)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 10)
        .background(tone.color.opacity(0.045), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
    }

    private func localsPanel(_ locals: [NotebookLocal]) -> some View {
        VStack(alignment: .leading, spacing: 9) {
            HStack {
                Label("Locals", systemImage: "point.3.connected.trianglepath.dotted")
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundStyle(NotebookPalette.ink)
                Spacer()
                Text("\(locals.count)")
                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                    .foregroundStyle(NotebookPalette.muted)
            }

            VStack(spacing: 0) {
                ForEach(Array(locals.indices), id: \.self) { index in
                    let local = locals[index]
                    HStack(alignment: .firstTextBaseline, spacing: 12) {
                        Text(local.name)
                            .font(.system(size: 11, weight: .semibold, design: .monospaced))
                            .foregroundStyle(NotebookPalette.ink)
                            .frame(minWidth: 92, alignment: .leading)
                        Text(local.value)
                            .font(.system(size: 11, design: .monospaced))
                            .foregroundStyle(NotebookPalette.slate)
                            .lineLimit(2)
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                        Text(local.role.isEmpty ? local.binding_kind : local.role)
                            .font(.system(size: 9, weight: .medium))
                            .foregroundStyle(NotebookPalette.muted)
                        if local.watched {
                            Image(systemName: "eye.fill")
                                .font(.system(size: 9))
                                .foregroundStyle(NotebookPalette.amber)
                                .help("Watched runtime value")
                        }
                    }
                    .padding(.vertical, 7)
                    if index < locals.count - 1 {
                        Rectangle().fill(NotebookPalette.rule.opacity(0.75)).frame(height: 1)
                    }
                }
            }
        }
        .padding(12)
        .background(NotebookPalette.canvas.opacity(0.75), in: RoundedRectangle(cornerRadius: 9, style: .continuous))
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Selected cell locals")
    }

    private func noticeCard(symbol: String, title: String, message: String, tone: BadgeTone) -> some View {
        HStack(alignment: .top, spacing: 11) {
            Image(systemName: symbol)
                .font(.system(size: 13, weight: .semibold))
                .foregroundStyle(tone.color)
                .padding(.top, 1)
            VStack(alignment: .leading, spacing: 4) {
                Text(title)
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(NotebookPalette.ink)
                Text(message)
                    .font(.system(size: 11))
                    .foregroundStyle(NotebookPalette.slate)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(tone.color.opacity(0.075), in: RoundedRectangle(cornerRadius: 10, style: .continuous))
        .overlay(alignment: .leading) {
            RoundedRectangle(cornerRadius: 2).fill(tone.color.opacity(0.62)).frame(width: 3).padding(.vertical, 9)
        }
    }

    private func environmentOutputCard(_ output: [NotebookOutput]) -> some View {
        VStack(alignment: .leading, spacing: 9) {
            Label("Module output", systemImage: "terminal")
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(NotebookPalette.ink)
            ForEach(Array(output.indices), id: \.self) { index in
                let event = output[index]
                Text(event.text)
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundStyle(event.stream.lowercased().contains("err") ? NotebookPalette.error : NotebookPalette.slate)
                    .textSelection(.enabled)
            }
        }
        .padding(13)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(NotebookPalette.card, in: RoundedRectangle(cornerRadius: 10, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous).stroke(NotebookPalette.rule, lineWidth: 1))
    }

    private func emptySheetCard(_ tab: NotebookTab) -> some View {
        VStack(alignment: .leading, spacing: 9) {
            Image(systemName: "text.page")
                .font(.system(size: 22, weight: .regular))
                .foregroundStyle(NotebookPalette.amber)
            Text("This sheet is empty")
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(NotebookPalette.ink)
            Text("Add a code cell to start working in Amber.")
                .font(.system(size: 12))
                .foregroundStyle(NotebookPalette.muted)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(23)
        .background(NotebookPalette.card, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).stroke(NotebookPalette.rule, lineWidth: 1))
    }

    private var missingTabView: some View {
        VStack(spacing: 11) {
            Image(systemName: "doc.questionmark")
                .font(.system(size: 28, weight: .light))
                .foregroundStyle(NotebookPalette.amber)
            Text("No tab selected")
                .font(.system(size: 17, weight: .semibold))
                .foregroundStyle(NotebookPalette.ink)
            Text("Choose a sheet or module from the sidebar.")
                .font(.system(size: 12))
                .foregroundStyle(NotebookPalette.muted)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var welcomeView: some View {
        VStack(spacing: 0) {
            Spacer()
            ZStack {
                RoundedRectangle(cornerRadius: 21, style: .continuous)
                    .fill(NotebookPalette.amber.opacity(0.12))
                    .frame(width: 72, height: 72)
                Image(systemName: "book.closed.fill")
                    .font(.system(size: 28, weight: .medium))
                    .foregroundStyle(NotebookPalette.amber)
            }
            .padding(.bottom, 22)

            Text("Amber Notebook")
                .font(.system(size: 30, weight: .semibold, design: .rounded))
                .foregroundStyle(NotebookPalette.ink)
            Text("A quiet workspace for interactive Amber programs.")
                .font(.system(size: 13))
                .foregroundStyle(NotebookPalette.muted)
                .padding(.top, 7)

            HStack(spacing: 10) {
                Button {
                    model.newProject()
                } label: {
                    Label("New Notebook", systemImage: "plus")
                        .frame(minWidth: 138)
                }
                .buttonStyle(.borderedProminent)
                .tint(NotebookPalette.amber)
                .help("Create a new Amber notebook project")

                Button {
                    model.openProject()
                } label: {
                    Label("Open Notebook", systemImage: "folder")
                        .frame(minWidth: 138)
                }
                .buttonStyle(.bordered)
                .help("Open an existing Amber notebook project")
            }
            .padding(.top, 24)

            Text("Projects keep sheets, bundled modules, and source files together.")
                .font(.system(size: 11))
                .foregroundStyle(NotebookPalette.muted)
                .padding(.top, 17)
            Spacer()
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(NotebookPalette.canvas.ignoresSafeArea())
    }

    private func stateBadge(_ title: String, tone: BadgeTone) -> some View {
        Text(title)
            .font(.system(size: 8, weight: .bold, design: .rounded))
            .tracking(0.45)
            .foregroundStyle(tone.color)
            .padding(.horizontal, 6)
            .padding(.vertical, 4)
            .background(tone.color.opacity(0.10), in: Capsule())
    }

    @ViewBuilder
    private func iconAction(
        _ symbol: String,
        label: String,
        disabled: Bool,
        role: ButtonRole? = nil,
        action: @escaping () -> Void
    ) -> some View {
        if let role {
            Button(role: role, action: action) {
                Image(systemName: symbol)
                    .font(.system(size: 10, weight: .medium))
                    .frame(width: 23, height: 23)
            }
            .buttonStyle(.borderless)
            .foregroundStyle(role == .destructive ? NotebookPalette.error : NotebookPalette.muted)
            .disabled(disabled)
            .help(label)
            .accessibilityLabel(label)
        } else {
            Button(action: action) {
                Image(systemName: symbol)
                    .font(.system(size: 10, weight: .medium))
                    .frame(width: 23, height: 23)
            }
            .buttonStyle(.borderless)
            .foregroundStyle(NotebookPalette.muted)
            .disabled(disabled)
            .help(label)
            .accessibilityLabel(label)
        }
    }

    private func cellKey(tab: NotebookTab, cell: NotebookCell) -> String {
        "\(tab.id):\(cell.id)"
    }
}

private struct PositionedNotebookCell: Identifiable {
    let position: Int
    let cell: NotebookCell
    var id: String { cell.id }
}

private enum NotebookPalette {
    static let canvas = Color(red: 0.963, green: 0.953, blue: 0.929)
    static let sidebar = Color(red: 0.937, green: 0.925, blue: 0.896)
    static let topBar = Color(red: 0.980, green: 0.972, blue: 0.952)
    static let card = Color(red: 0.995, green: 0.990, blue: 0.976)
    static let cardSelected = Color(red: 1.0, green: 0.996, blue: 0.983)
    static let editor = Color(red: 0.998, green: 0.994, blue: 0.984)
    static let rule = Color(red: 0.866, green: 0.850, blue: 0.811)
    static let ink = Color(red: 0.153, green: 0.161, blue: 0.166)
    static let slate = Color(red: 0.352, green: 0.376, blue: 0.385)
    static let muted = Color(red: 0.486, green: 0.493, blue: 0.478)
    static let amber = Color(red: 0.710, green: 0.392, blue: 0.129)
    static let error = Color(red: 0.704, green: 0.247, blue: 0.204)
    static let success = Color(red: 0.246, green: 0.478, blue: 0.337)
}

private enum BadgeTone {
    case amber, slate, red, green

    var color: Color {
        switch self {
        case .amber: NotebookPalette.amber
        case .slate: NotebookPalette.slate
        case .red: NotebookPalette.error
        case .green: NotebookPalette.success
        }
    }
}

private struct StructurePrompt: Identifiable {
    enum Kind { case sheet, module }
    let kind: Kind
    var id: String { kind == .sheet ? "sheet" : "module" }
}

private struct DeleteTarget: Identifiable {
    let tab: NotebookTab
    let cell: NotebookCell
    var id: String { "\(tab.id):\(cell.id)" }
}

private struct StructurePromptSheet: View {
    let prompt: StructurePrompt
    let onCreate: (String) -> Void
    let onCancel: () -> Void
    @State private var identifier = ""

    private var kindName: String { prompt.kind == .sheet ? "sheet" : "module" }
    private var validIdentifier: Bool {
        identifier.range(of: "^[A-Za-z_][A-Za-z0-9_]*$", options: .regularExpression) != nil
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("New \(kindName)")
                .font(.system(size: 18, weight: .semibold, design: .rounded))
                .foregroundStyle(NotebookPalette.ink)
            Text("Choose a short identifier. It becomes the \(kindName)'s stable project name.")
                .font(.system(size: 12))
                .foregroundStyle(NotebookPalette.muted)
                .fixedSize(horizontal: false, vertical: true)
            TextField("Identifier", text: $identifier)
                .textFieldStyle(.roundedBorder)
                .font(.system(size: 13, design: .monospaced))
                .onSubmit(create)
                .accessibilityLabel("New \(kindName) identifier")

            if prompt.kind == .module {
                Label("Modules start outside the sheet environment. Save and apply them when you are ready.", systemImage: "info.circle")
                    .font(.system(size: 10))
                    .foregroundStyle(NotebookPalette.muted)
                    .fixedSize(horizontal: false, vertical: true)
            }

            HStack {
                Spacer()
                Button("Cancel", action: onCancel)
                    .keyboardShortcut(.cancelAction)
                Button("Create", action: create)
                    .keyboardShortcut(.defaultAction)
                    .buttonStyle(.borderedProminent)
                    .tint(NotebookPalette.amber)
                    .disabled(!validIdentifier)
            }
            .padding(.top, 4)
        }
        .padding(23)
        .frame(width: 390)
        .background(NotebookPalette.canvas)
    }

    private func create() {
        guard validIdentifier else { return }
        onCreate(identifier)
    }
}
