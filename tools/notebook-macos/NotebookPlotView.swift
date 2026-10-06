import AppKit
import SwiftUI

struct NotebookPlotView: View {
    let scene: NotebookPlotScene
    let width: Int, height: Int
    @Binding var viewport: PlotViewport?
    var onOpen: (() -> Void)? = nil
    var onSave: (() -> Void)? = nil
    var onShare: (() -> Void)? = nil
    @State private var showAxes = false
    @State private var xMin = ""
    @State private var xMax = ""
    @State private var yMin = ""
    @State private var yMax = ""
    @State private var error: String?
    @State private var fieldsEdited = false
    private var current: PlotViewport { viewport ?? scene.initialViewport }
    private var hasView: Bool { viewport != nil && current != scene.initialViewport }

    var body: some View {
        VStack(spacing: 6) {
            PlotCanvas(scene: scene, imageSize: CGSize(width: width, height: height), viewport: current,
                       onZoom: { viewport = $0; loadFields() }, onOpen: onOpen,
                       onReset: { viewport = nil; loadFields() })
                .aspectRatio(CGFloat(width) / CGFloat(height), contentMode: .fit)
                .accessibilityLabel("Interactive plot: drag a rectangle to zoom; double-click to reset")
                .help("Drag to zoom · double-click to reset · click to open")
                .overlay(alignment: .topTrailing) {
                    NotebookObjectMenu(title: "Plot actions") {
                        Button(showAxes ? "Hide axes settings" : "Axes…") { showAxes.toggle(); loadFields() }
                        if hasView { Button("Reset axes") { viewport = nil; loadFields() } }
                        if let onOpen { Button("Open…", action: onOpen) }
                        if let onSave { Divider(); Button("Save PNG…", action: onSave) }
                        if let onShare { Button("Share…", action: onShare) }
                    }.padding(6)
                }
            if hasView {
                HStack {
                    Spacer()
                    Button("Reset axes") { viewport = nil; loadFields() }
                        .buttonStyle(.plain).font(.caption).foregroundStyle(.secondary)
                }
            }
            if showAxes {
                HStack {
                    Text("Axis limits").font(.caption).foregroundStyle(.secondary)
                    Spacer()
                    Button { showAxes = false } label: { Image(systemName: "xmark") }
                        .buttonStyle(.plain).help("Close axes settings").accessibilityLabel("Close axes settings")
                }
                ViewThatFits(in: .horizontal) {
                    HStack { axis("X", low: $xMin, high: $xMax); axis("Y", low: $yMin, high: $yMax); applyButton }
                    VStack { axis("X", low: $xMin, high: $xMax); axis("Y", low: $yMin, high: $yMax); applyButton }
                }
                .controlSize(.small)
                if let error { Text(error).font(.caption).foregroundStyle(.red) }
            }
        }
        .onAppear(perform: loadFields)
        .onChange(of: current) { if !fieldsEdited { loadFields() } }
    }
    private func axis(_ title: String, low: Binding<String>, high: Binding<String>) -> some View {
        HStack(spacing: 4) {
            Text(title).font(.caption.bold())
            TextField("min", text: editing(low)).accessibilityLabel("\(title) minimum").frame(width: 90)
            Text("…")
            TextField("max", text: editing(high)).accessibilityLabel("\(title) maximum").frame(width: 90)
        }.textFieldStyle(.roundedBorder).onSubmit(apply)
    }
    private var applyButton: some View { Button("Apply axes", action: apply) }
    private func editing(_ value: Binding<String>) -> Binding<String> {
        Binding(get: { value.wrappedValue }, set: { value.wrappedValue = $0; fieldsEdited = true })
    }
    private func loadFields() {
        xMin = String(current.xMin); xMax = String(current.xMax)
        yMin = String(current.yMin); yMax = String(current.yMax); error = nil; fieldsEdited = false
    }
    private func apply() {
        let values = [xMin, yMin, xMax, yMax].compactMap { Double($0.trimmingCharacters(in: .whitespacesAndNewlines).replacingOccurrences(of: ",", with: ".")) }
        guard let value = PlotViewport(values) else {
            error = "Enter finite bounds with minimum < maximum (and a resolvable interval)."; return
        }
        viewport = value; loadFields()
    }
}

struct PlotCanvas: NSViewRepresentable {
    let scene: NotebookPlotScene, imageSize: CGSize, viewport: PlotViewport
    let onZoom: (PlotViewport) -> Void
    let onOpen: (() -> Void)?
    let onReset: () -> Void

    func makeNSView(context: Context) -> PlotCanvasView { PlotCanvasView() }
    func updateNSView(_ view: PlotCanvasView, context: Context) {
        if view.viewport != viewport { view.dragStart = nil; view.dragEnd = nil }
        view.scene = scene; view.imageSize = imageSize; view.viewport = viewport
        view.onZoom = onZoom; view.onOpen = onOpen; view.onReset = onReset
        view.needsDisplay = true
    }
}

final class PlotCanvasView: NSView {
    var scene: NotebookPlotScene?
    var imageSize = CGSize(width: 800, height: 540)
    var viewport: PlotViewport?
    var onZoom: ((PlotViewport) -> Void)?, onOpen: (() -> Void)?, onReset: (() -> Void)?
    var dragStart: CGPoint?, dragEnd: CGPoint?
    private var pendingOpen: DispatchWorkItem?
    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    override func resetCursorRects() { addCursorRect(bounds, cursor: .crosshair) }

    override func draw(_ dirtyRect: NSRect) {
        guard let scene, let viewport, bounds.width > 0, bounds.height > 0 else { return }
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current?.cgContext.scaleBy(x: bounds.width / imageSize.width, y: bounds.height / imageSize.height)
        scene.draw(view: viewport, size: imageSize)
        NSGraphicsContext.restoreGraphicsState()
        if let start = dragStart, let end = dragEnd {
            let rectangle = CGRect(x: min(start.x, end.x), y: min(start.y, end.y), width: abs(end.x - start.x), height: abs(end.y - start.y))
            NSColor.controlAccentColor.withAlphaComponent(0.15).setFill(); NSBezierPath(rect: rectangle).fill()
            NSColor.controlAccentColor.setStroke(); let path = NSBezierPath(rect: rectangle); path.lineWidth = 1.5; path.stroke()
        }
    }
    private func imagePoint(_ p: CGPoint) -> CGPoint {
        CGPoint(x: p.x * imageSize.width / max(1, bounds.width), y: p.y * imageSize.height / max(1, bounds.height))
    }
    override func mouseDown(with event: NSEvent) {
        pendingOpen?.cancel(); pendingOpen = nil
        if event.clickCount == 2 { dragStart = nil; dragEnd = nil; onReset?(); return }
        window?.makeFirstResponder(self)
        let p = convert(event.locationInWindow, from: nil)
        guard let scene, let viewport, scene.plotFrame(view: viewport, size: imageSize).contains(imagePoint(p)) else { return }
        dragStart = p; dragEnd = p
    }
    override func mouseDragged(with event: NSEvent) {
        guard dragStart != nil, let scene, let viewport else { return }
        let f = scene.plotFrame(view: viewport, size: imageSize)
        let p = imagePoint(convert(event.locationInWindow, from: nil))
        dragEnd = CGPoint(x: min(max(p.x, f.minX), f.maxX) * bounds.width / imageSize.width,
                          y: min(max(p.y, f.minY), f.maxY) * bounds.height / imageSize.height)
        needsDisplay = true
    }
    override func mouseUp(with event: NSEvent) {
        defer { dragStart = nil; dragEnd = nil; needsDisplay = true }
        guard let start = dragStart, let end = dragEnd, let scene, let viewport else { return }
        if abs(end.x - start.x) < 5 || abs(end.y - start.y) < 5 {
            if abs(end.x - start.x) < 5 && abs(end.y - start.y) < 5 && onOpen != nil {
                // A first click must not open a sheet before the second click
                // can perform reset on this canvas.
                let work = DispatchWorkItem { [weak self] in
                    guard let self, self.window != nil else { return }
                    self.onOpen?(); self.pendingOpen = nil
                }
                pendingOpen = work
                DispatchQueue.main.asyncAfter(deadline: .now() + NSEvent.doubleClickInterval, execute: work)
            }
            return
        }
        let a = imagePoint(start), b = imagePoint(end)
        let rect = CGRect(x: min(a.x, b.x), y: min(a.y, b.y), width: abs(a.x - b.x), height: abs(a.y - b.y))
        if let view = scene.selectedViewport(rect, view: viewport, size: imageSize) { onZoom?(view) }
    }
    override func keyDown(with event: NSEvent) {
        if event.keyCode == 53 { pendingOpen?.cancel(); pendingOpen = nil; dragStart = nil; dragEnd = nil; needsDisplay = true }
        else { super.keyDown(with: event) }
    }
}
