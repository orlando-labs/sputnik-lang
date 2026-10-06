import AppKit
import SwiftUI

private final class PlotFixtureStore: ObservableObject {
    @Published var panels: [NotebookFigurePanel]
    init(_ panels: [NotebookFigurePanel]) { self.panels = panels }
}
private struct LivePlotFixture: View {
    @ObservedObject var store: PlotFixtureStore
    var body: some View { NotebookFigureView(panels: store.panels, stale: false) }
}

@main
private enum NotebookPlotTests {
    static func near(_ a: Double, _ b: Double) { precondition(abs(a - b) < 1e-8, "\(a) != \(b)") }
    static func main() throws {
        _ = NSApplication.shared
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
        let fixture = CommandLine.arguments.dropFirst().first(where: { !$0.hasPrefix("--") })
            .map { URL(fileURLWithPath: $0) } ?? Bundle.main.url(forResource: "notebook-plot-view-snapshot", withExtension: "json")
            ?? root.appendingPathComponent("build/notebook-plot-view-snapshot.json")
        let json = try JSONSerialization.jsonObject(with: Data(contentsOf: fixture)) as! [String: Any]
        let tabs = json["tabs"] as! [[String: Any]]
        let cells = tabs[0]["cells"] as! [[String: Any]]
        let displays = cells[0]["displays"] as! [[String: Any]]
        let panels = try JSONDecoder().decode([NotebookFigurePanel].self,
            from: JSONSerialization.data(withJSONObject: displays))
        precondition(panels.count == 2)
        let panel = panels[0]
        let scene = NotebookPlotScene.decode(panel.plot_scene, width: panel.width, height: panel.height)!
        let view = PlotViewport([1, 0, 3, 10])!
        precondition(scene.initialViewport.values == [0, 0, 4, 16])
        precondition(NotebookPlotScene.decode(panels[1].plot_scene, width: 800, height: 540)!.initialViewport == view)
        let size = CGSize(width: 800, height: 540)
        let frame = scene.plotFrame(view: scene.initialViewport, size: size)
        precondition(frame == scene.originalFrame)
        let middle = CGPoint(x: frame.midX, y: frame.midY)
        let data = scene.dataPoint(middle, view: scene.initialViewport, frame: frame)
        near(data.x, 2); near(data.y, 8)
        let screen = scene.screenPoint(data, view: scene.initialViewport, frame: frame)
        near(screen.x, middle.x); near(screen.y, middle.y)
        let selected = scene.selectedViewport(frame.insetBy(dx: frame.width / 4, dy: frame.height / 4),
            view: scene.initialViewport, size: size)!
        precondition(selected.values == [1, 4, 3, 12])
        precondition(scene.selectedViewport(CGRect(x: 0, y: 0, width: 10, height: 10), view: view, size: size) == nil)
        precondition(scene.selectedViewport(CGRect(x: frame.minX, y: frame.minY, width: 2, height: 10), view: view, size: size) == nil)
        precondition(PlotViewport([1, 0, 1, 10]) == nil && PlotViewport([3, 0, 1, 10]) == nil)
        precondition(PlotViewport([0, 0, .infinity, 1]) == nil && PlotViewport([0, 0, 1]) == nil)
        precondition(PlotViewport([1, 0, 1 + 1e-14, 10]) == nil)
        precondition(PlotViewport([0, 0, 1e-320, 1]) == nil)
        precondition(NotebookPlotScene.tick(0.0012345, span: 1e-7) != NotebookPlotScene.tick(0.0012346, span: 1e-7))
        precondition(NotebookPlotScene.tick(0, span: 1e-320) == "0")
        let clip = CGRect(x: 0, y: 0, width: 100, height: 100)
        let clipped = NotebookPlotScene.clipSegment(CGPoint(x: -10000, y: 50), CGPoint(x: 10000, y: 50), to: clip)!
        near(clipped.0.x, 0); near(clipped.1.x, 100)
        precondition(NotebookPlotScene.clipSegment(CGPoint(x: -100, y: -100), CGPoint(x: -50, y: -50), to: clip) == nil)
        let polygon = NotebookPlotScene.clipPolygon([CGPoint(x: -10000, y: -10000), CGPoint(x: 10000, y: -10000),
            CGPoint(x: 10000, y: 10000), CGPoint(x: -10000, y: 10000)], to: clip)
        precondition(polygon.count == 4 && polygon.allSatisfy { $0.x >= 0 && $0.x <= 100 && $0.y >= 0 && $0.y <= 100 })

        let source = try JSONSerialization.jsonObject(with: Data(panel.plot_scene!.utf8)) as! [String: Any]
        func changed(_ change: (inout [String: Any]) -> Void) -> NotebookPlotScene? {
            var object = source; change(&object)
            return NotebookPlotScene.decode(String(data: try! JSONSerialization.data(withJSONObject: object), encoding: .utf8), width: 800, height: 540)
        }
        let down = changed { $0["y_down"] = true }!
        near(down.dataPoint(frame.origin, view: down.initialViewport, frame: frame).y, 0)
        near(down.screenPoint(CGPoint(x: 0, y: 16), view: down.initialViewport, frame: frame).y, frame.maxY)
        let equal = changed { $0["aspect_equal"] = true }!
        let equalFrame = equal.plotFrame(view: view, size: size)
        near(equalFrame.width / equalFrame.height, 2.0 / 10)
        precondition(changed { $0["version"] = 99 } == nil)
        precondition(changed { $0["bounds"] = [0, 0, 0, 0] } == nil)
        precondition(changed { $0["frame"] = [0, 0, 100000, 10] } == nil)
        precondition(changed { $0["layers"] = [["kind": "line", "commands": [[99]]]] } == nil)
        precondition(changed { $0["layers"] = [["kind": "line", "commands": [[2, "#000", 1, 1, [[0]], []]]]] } == nil)
        precondition(changed { $0["layers"] = [["kind": "line", "commands": [[2, "#000", 1, 1, [[0, 0]], [0]]]]] } == nil)
        precondition(NotebookPlotScene.decode(String(repeating: "x", count: 4 * 1024 * 1024 + 1), width: 800, height: 540) == nil)
        precondition(NotebookPlotScene.decode(panel.plot_scene, width: Int.max, height: Int.max) == nil)

        let originalPNG = scene.png(view: scene.initialViewport, width: 800, height: 540)!
        let zoomPNG = scene.png(view: view, width: 800, height: 540)!
        precondition(originalPNG != zoomPNG && zoomPNG.count > 1000)
        let bitmap = NSBitmapImageRep(data: zoomPNG)!
        precondition(bitmap.pixelsWide == 800 && bitmap.pixelsHigh == 540)
        // The line starts outside the zoom. Clipping must not paint the margin.
        let pixel = bitmap.colorAt(x: 40, y: 460)!.usingColorSpace(.deviceRGB)!
        precondition(pixel.redComponent > 0.98 && pixel.greenComponent > 0.98 && pixel.blueComponent > 0.98)
        try originalPNG.write(to: fixture.deletingLastPathComponent().appendingPathComponent("plot-native-original.png"))
        try zoomPNG.write(to: fixture.deletingLastPathComponent().appendingPathComponent("plot-native-view.png"))

        // Exercise native mouse coordinate conversion at a non-image view size.
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 400, height: 270),
                              styleMask: [.titled], backing: .buffered, defer: false)
        let canvas = PlotCanvasView(frame: CGRect(x: 0, y: 0, width: 400, height: 270))
        canvas.scene = scene; canvas.imageSize = size; canvas.viewport = scene.initialViewport
        var captured: PlotViewport?, opened = 0, resets = 0
        canvas.onZoom = { captured = $0 }; canvas.onOpen = { opened += 1 }; canvas.onReset = { resets += 1 }
        window.contentView = canvas
        func event(_ type: NSEvent.EventType, _ point: CGPoint, clicks: Int = 1) -> NSEvent {
            NSEvent.mouseEvent(with: type, location: canvas.convert(point, to: nil), modifierFlags: [], timestamp: 0,
                windowNumber: window.windowNumber, context: nil, eventNumber: 0, clickCount: clicks, pressure: 1)!
        }
        let start = CGPoint(x: (frame.minX + frame.width / 4) / 2, y: (frame.minY + frame.height / 4) / 2)
        let end = CGPoint(x: (frame.maxX - frame.width / 4) / 2, y: (frame.maxY - frame.height / 4) / 2)
        canvas.mouseDown(with: event(.leftMouseDown, start))
        canvas.mouseDragged(with: event(.leftMouseDragged, end))
        canvas.mouseUp(with: event(.leftMouseUp, end))
        precondition(captured == selected && opened == 0 && canvas.dragStart == nil)
        canvas.mouseDown(with: event(.leftMouseDown, start, clicks: 2))
        precondition(resets == 1)
        canvas.mouseDown(with: event(.leftMouseDown, start)); canvas.mouseUp(with: event(.leftMouseUp, start))
        RunLoop.main.run(until: Date().addingTimeInterval(NSEvent.doubleClickInterval + 0.05))
        precondition(opened == 1)
        canvas.mouseDown(with: event(.leftMouseDown, start)); canvas.mouseUp(with: event(.leftMouseUp, start))
        canvas.mouseDown(with: event(.leftMouseDown, start, clicks: 2))
        RunLoop.main.run(until: Date().addingTimeInterval(NSEvent.doubleClickInterval + 0.05))
        precondition(opened == 1 && resets == 2, "double click resets without opening the lightbox")
        window.orderOut(nil)

        // A live scene is a new immutable snapshot, but SwiftUI must retain
        // the user's selected domain at the same panel position.
        let store = PlotFixtureStore(panels)
        let hosting = NSHostingView(rootView: LivePlotFixture(store: store))
        let liveWindow = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1000, height: 700),
                                  styleMask: [.titled], backing: .buffered, defer: false)
        liveWindow.contentView = hosting
        func settle() {
            hosting.layoutSubtreeIfNeeded()
            RunLoop.main.run(until: Date().addingTimeInterval(0.08))
            hosting.layoutSubtreeIfNeeded()
        }
        func canvases(_ view: NSView) -> [PlotCanvasView] {
            (view as? PlotCanvasView).map { [$0] } ?? view.subviews.flatMap(canvases)
        }
        settle()
        precondition(canvases(hosting).count == 2, "both native canvases are mounted")
        canvases(hosting)[0].onZoom?(view); settle()
        precondition(canvases(hosting)[0].viewport == view)
        store.panels[0].plot_scene = panel.plot_scene!.replacingOccurrences(of: "Viewport regression", with: "Live frame updated")
        settle()
        precondition(canvases(hosting)[0].scene?.title == "Live frame updated" && canvases(hosting)[0].viewport == view,
                     "new live geometry retains the selected range")
        canvases(hosting)[0].onReset?(); settle()
        precondition(canvases(hosting)[0].viewport == scene.initialViewport)
        liveWindow.orderOut(nil)
        print("notebook_macos_plot_tests: ok")

        if CommandLine.arguments.contains("--interactive") || Bundle.main.bundleIdentifier == "org.amberlang.plot-preview-test" {
            NSApp.setActivationPolicy(.regular)
            let fixtureWindow = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1080, height: 720),
                                         styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
            fixtureWindow.title = "Plot View · UI test"; fixtureWindow.isReleasedWhenClosed = false
            fixtureWindow.contentView = NSHostingView(rootView: ScrollView {
                NotebookFigureView(panels: panels, stale: false).padding(24)
            })
            fixtureWindow.center(); fixtureWindow.makeKeyAndOrderFront(nil)
            NSApp.activate(ignoringOtherApps: true); NSApp.run()
        }
    }
}
