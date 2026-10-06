import AppKit
import SwiftUI
import UniformTypeIdentifiers

private struct FigureSelection: Identifiable {
    let id = UUID()
    let index: Int
    var viewport: PlotViewport? = nil
}

struct NotebookFigureView: View {
    let panels: [NotebookFigurePanel]
    let stale: Bool
    @State private var selection: FigureSelection?
    @State private var viewports: [Int: PlotViewport] = [:]
    private var columns: [GridItem] {
        panels.count <= 2
            ? Array(repeating: GridItem(.flexible(minimum: 200), spacing: 16, alignment: .top), count: max(1, panels.count))
            : [GridItem(.adaptive(minimum: 280), spacing: 16, alignment: .top)]
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Label("FIGURE", systemImage: "chart.xyaxis.line").font(.system(size: 10, weight: .bold))
                Spacer()
                if stale { Text("Previous result · stale").font(.caption).foregroundStyle(.orange) }
            }.foregroundStyle(.secondary)
            LazyVGrid(columns: columns, spacing: 18) {
                ForEach(panels.indices, id: \.self) { index in
                    VStack(alignment: .leading, spacing: 7) {
                        if let scene = NotebookPlotScene.decode(panels[index].plot_scene, width: panels[index].width, height: panels[index].height) {
                            NotebookPlotView(scene: scene, width: panels[index].width, height: panels[index].height,
                                viewport: Binding(get: { viewports[index] }, set: { viewports[index] = $0 }),
                                onOpen: { selection = FigureSelection(index: index, viewport: viewports[index]) })
                        } else {
                          Button { selection = FigureSelection(index: index) } label: {
                            FigureImage(panel: panels[index])
                                .frame(maxWidth: .infinity)
                                .background(.white)
                                .overlay(RoundedRectangle(cornerRadius: 6).stroke(.gray.opacity(0.18)))
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel("Open figure panel \(NotebookFigurePanel.label(index)) \(panels[index].caption)")
                        .help("Open image · zoom, save or share")
                        .overlay(alignment: .topTrailing) {
                            NotebookObjectMenu(title: "Figure actions") {
                                Button("Open…") { selection = FigureSelection(index: index) }
                            }.padding(6)
                        }
                        }
                        if panels.count > 1 || !panels[index].caption.isEmpty {
                            Text((panels.count > 1 ? NotebookFigurePanel.label(index) + " " : "") + panels[index].caption)
                                .font(.system(size: 12)).textSelection(.enabled)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                }
            }
        }
        .padding(13)
        .background(Color.white.opacity(0.65), in: RoundedRectangle(cornerRadius: 9))
        .sheet(item: $selection) { selected in
            if !panels.isEmpty {
                FigureLightbox(panels: panels, initialIndex: min(selected.index, panels.count - 1), initialViewport: selected.viewport)
            }
        }
        .onChange(of: panels.count) {
            viewports = viewports.filter { panels.indices.contains($0.key) }
            if panels.isEmpty { selection = nil }
        }
    }
}

private struct FigureImage: View {
    let panel: NotebookFigurePanel
    var fullResolution = false
    @State private var image: NSImage?

    var body: some View {
        Group {
            if let image { Image(nsImage: image).resizable().aspectRatio(contentMode: .fit) }
            else { Label("Image unavailable", systemImage: "exclamationmark.triangle").frame(minHeight: 140) }
        }
        .onAppear { image = panel.image(maxPixelSize: fullResolution ? 8192 : 1200) }
        .onChange(of: panel.data) { image = panel.image(maxPixelSize: fullResolution ? 8192 : 1200) }
    }
}

private struct FigureLightbox: View {
    let panels: [NotebookFigurePanel]
    @State private var index: Int
    @State private var zoom: CGFloat = 1
    @State private var error: String?
    @State private var viewport: PlotViewport?
    @State private var shareRequest = 0
    @Environment(\.dismiss) private var dismiss

    init(panels: [NotebookFigurePanel], initialIndex: Int, initialViewport: PlotViewport? = nil) {
        self.panels = panels
        _index = State(initialValue: initialIndex)
        _viewport = State(initialValue: initialViewport)
    }
    private var panel: NotebookFigurePanel { panels[min(index, panels.count - 1)] }
    private var scene: NotebookPlotScene? { NotebookPlotScene.decode(panel.plot_scene, width: panel.width, height: panel.height) }
    private func exportPanel() -> NotebookFigurePanel? {
        guard let scene else { return panel }
        guard let data = scene.png(view: viewport ?? scene.initialViewport, width: panel.width, height: panel.height) else { return nil }
        return NotebookFigurePanel(mime: "image/png", caption: panel.caption, width: panel.width, height: panel.height, data: data)
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 14) {
                Button { index = max(0, index - 1); zoom = 1; viewport = nil } label: { Image(systemName: "chevron.left") }
                    .disabled(index == 0).keyboardShortcut(.leftArrow, modifiers: [])
                Text("\(NotebookFigurePanel.label(index))  \(index + 1) / \(panels.count)")
                Button { index = min(panels.count - 1, index + 1); zoom = 1; viewport = nil } label: { Image(systemName: "chevron.right") }
                    .disabled(index == panels.count - 1).keyboardShortcut(.rightArrow, modifiers: [])
                Spacer()
                Button("Done") { dismiss() }.keyboardShortcut(.cancelAction)
            }
            .padding(14).background(.regularMaterial)
            GeometryReader { geometry in
                let aspect = CGFloat(max(1, panel.width)) / CGFloat(max(1, panel.height))
                let fitWidth = min(geometry.size.width - 32, (geometry.size.height - 32) * aspect)
                if let scene {
                    NotebookPlotView(scene: scene, width: panel.width, height: panel.height,
                                     viewport: $viewport, onSave: save, onShare: { shareRequest += 1 })
                        .padding(16).frame(maxWidth: .infinity, maxHeight: .infinity)
                        .background(.white)
                } else { ScrollView([.horizontal, .vertical]) {
                    FigureImage(panel: panel, fullResolution: true)
                        .frame(width: max(1, fitWidth) * zoom, height: max(1, fitWidth / aspect) * zoom)
                        .padding(16)
                        .frame(minWidth: geometry.size.width, minHeight: geometry.size.height)
                }
                .overlay(alignment: .topTrailing) {
                    NotebookObjectMenu(title: "Figure actions") {
                        Button("Zoom in") { zoom = min(8, zoom * 1.25) }
                        Button("Zoom out") { zoom = max(0.25, zoom / 1.25) }
                        Button("Fit image") { zoom = 1 }
                        Divider()
                        Button("Save PNG…", action: save)
                        Button("Share…") { shareRequest += 1 }
                    }.padding(6)
                }
                }
            }.background(Color(white: 0.12))
            VStack(alignment: .leading, spacing: 4) {
                if !panel.caption.isEmpty { Text(panel.caption).textSelection(.enabled) }
                Text("\(panel.width) × \(panel.height) px · \(scene == nil ? "Raster image" : "2D plot · export uses current axes")")
                    .font(.caption).foregroundStyle(.secondary)
                if let error { Text(error).foregroundStyle(.red).font(.caption) }
            }.frame(maxWidth: .infinity, alignment: .leading).padding(14)
        }
        .frame(minWidth: 760, idealWidth: 1000, maxWidth: 1200, minHeight: 540, idealHeight: 720, maxHeight: 900)
        .background(alignment: .topTrailing) {
            FigureShareAnchor(request: shareRequest, makePanel: exportPanel).frame(width: 1, height: 1)
        }
        .onChange(of: panels.count) { index = max(0, min(index, panels.count - 1)) }
    }

    private func save() {
        let dialog = NSSavePanel()
        dialog.allowedContentTypes = [.png]
        dialog.nameFieldStringValue = "figure-\(NotebookFigurePanel.label(index).dropLast()).png"
        guard let rendered = exportPanel() else { error = "Unable to render current plot view"; return }
        let bytes = rendered.data
        dialog.begin { response in
            guard response == .OK, let url = dialog.url else { return }
            do { try bytes.write(to: url, options: .atomic) }
            catch { self.error = error.localizedDescription }
        }
    }
}

private struct FigureShareAnchor: NSViewRepresentable {
    let request: Int
    let makePanel: () -> NotebookFigurePanel?
    func makeCoordinator() -> Coordinator { Coordinator() }
    func makeNSView(context: Context) -> NSView { NSView() }
    func updateNSView(_ view: NSView, context: Context) {
        guard request != context.coordinator.lastRequest else { return }
        context.coordinator.lastRequest = request
        // Wait for the action menu to close before presenting the system picker.
        DispatchQueue.main.async { [weak view, coordinator = context.coordinator] in
            guard let view, view.window != nil, let panel = makePanel(), let image = panel.image(maxPixelSize: 8192) else { return }
            let items: [Any] = panel.caption.isEmpty ? [image] : [image, panel.caption]
            let picker = NSSharingServicePicker(items: items)
            coordinator.picker = picker
            picker.show(relativeTo: view.bounds, of: view, preferredEdge: .minY)
        }
    }
    final class Coordinator {
        var lastRequest = 0
        var picker: NSSharingServicePicker?
    }
}
