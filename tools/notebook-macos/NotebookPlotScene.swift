import AppKit

struct PlotViewport: Equatable {
    var xMin: Double, yMin: Double, xMax: Double, yMax: Double
    var values: [Double] { [xMin, yMin, xMax, yMax] }
    init?(_ values: [Double]) {
        guard values.count == 4, values.allSatisfy({ $0.isFinite && abs($0) < 1e150 }),
              values[2] > values[0], values[3] > values[1],
              (values[2] - values[0]).isFinite, (values[3] - values[1]).isFinite else { return nil }
        // Avoid unstable inversions and indistinguishable tick labels after
        // repeated rubber-band zooms at machine precision.
        guard values[2] - values[0] > max(1e-150, max(abs(values[0]), abs(values[2])) * 1e-13),
              values[3] - values[1] > max(1e-150, max(abs(values[1]), abs(values[3])) * 1e-13) else { return nil }
        xMin = values[0]; yMin = values[1]; xMax = values[2]; yMax = values[3]
    }
}

private enum PlotSceneError: Error { case invalid }

struct PlotCommand: Decodable {
    let kind: Int
    var fill = "none", stroke = "none", text = ""
    var width = 0.0, opacity = 1.0, radius = 0.0, size = 12.0, align = 0
    var points: [[Double]] = [], rings: [[[Double]]] = [], dash: [Double] = []
    var position: [Double] = []

    init(from decoder: Decoder) throws {
        var c = try decoder.unkeyedContainer()
        kind = try c.decode(Int.self)
        switch kind {
        case 1:
            fill = try c.decode(String.self); stroke = try c.decode(String.self)
            width = try c.decode(Double.self); opacity = try c.decode(Double.self)
            rings = try c.decode([[[Double]]].self)
        case 2:
            stroke = try c.decode(String.self); width = try c.decode(Double.self)
            opacity = try c.decode(Double.self); points = try c.decode([[Double]].self)
            if !c.isAtEnd { dash = try c.decode([Double].self) }
        case 3:
            fill = try c.decode(String.self); stroke = try c.decode(String.self)
            width = try c.decode(Double.self); opacity = try c.decode(Double.self)
            radius = try c.decode(Double.self); points = try c.decode([[Double]].self)
        case 4:
            fill = try c.decode(String.self); size = try c.decode(Double.self)
            position = [try c.decode(Double.self), try c.decode(Double.self)]
            align = try c.decode(Int.self); text = try c.decode(String.self)
        default: throw PlotSceneError.invalid
        }
        guard c.isAtEnd, [width, opacity, radius, size].allSatisfy(\.isFinite),
              (0...1024).contains(width), (0...1).contains(opacity), (0...1024).contains(radius),
              (1...512).contains(size), (0...2).contains(align), text.utf8.count <= 65536,
              Self.color(fill) != nil, Self.color(stroke) != nil,
              dash.count <= 64, dash.allSatisfy({ $0.isFinite && $0 >= 0.1 && $0 <= 10000 }),
              (points + rings.flatMap { $0 } + (position.isEmpty ? [] : [position])).allSatisfy(Self.validPoint)
        else { throw PlotSceneError.invalid }
    }

    static func validPoint(_ p: [Double]) -> Bool { p.count == 2 && p.allSatisfy { $0.isFinite && abs($0) < 1e150 } }
    static func color(_ value: String) -> NSColor? {
        if value == "none" || value == "transparent" { return .clear }
        guard value.first == "#" else { return nil }
        var hex = String(value.dropFirst())
        if hex.count == 3 { hex = hex.map { "\($0)\($0)" }.joined() }
        guard hex.count == 6 || hex.count == 8, let v = UInt64(hex, radix: 16) else { return nil }
        let rgba = hex.count == 6 ? (v << 8) | 255 : v
        return NSColor(srgbRed: Double((rgba >> 24) & 255) / 255,
                       green: Double((rgba >> 16) & 255) / 255,
                       blue: Double((rgba >> 8) & 255) / 255, alpha: Double(rgba & 255) / 255)
    }
}

struct PlotSceneLayer: Decodable {
    let kind: String
    let commands: [PlotCommand]
    var guide_value: Double? = nil
    var anchor: [Double]? = nil
    var offset: [Double]? = nil
    var label_at: String? = nil
}

/// Portable closed drawing vocabulary; no code, callbacks, file references or
/// VM objects. Both preview and exported zoomed PNG use this same renderer.
struct NotebookPlotScene: Decodable {
    let version: Int, bounds: [Double], frame: [Double]
    let padding: Double, color_scale: Bool, aspect_equal: Bool
    let y_down: Bool, axes: Bool, background: String, title: String, x_label: String, y_label: String
    let layers: [PlotSceneLayer], decorations: [PlotCommand]

    static func decode(_ json: String?, width: Int, height: Int) -> NotebookPlotScene? {
        guard let json, !json.isEmpty, json.utf8.count <= 4 * 1024 * 1024,
              width > 0, height > 0, width <= 8192, height <= 8192, width * height <= 16_000_000,
              let scene = try? JSONDecoder().decode(Self.self, from: Data(json.utf8)),
              scene.version == 1, PlotViewport(scene.bounds) != nil, scene.frame.count == 4,
              scene.frame.allSatisfy(\.isFinite), scene.frame[0] >= 0, scene.frame[1] >= 0,
              scene.frame[2] > 0, scene.frame[3] > 0,
              scene.frame[0] + scene.frame[2] <= Double(width) + 0.001,
              scene.frame[1] + scene.frame[3] <= Double(height) + 0.001,
              scene.padding.isFinite, scene.padding >= 0, scene.padding < Double(min(width, height)) / 2,
              Double(width) - 2 * scene.padding - (scene.color_scale ? 72 : 0) > 0,
              Double(height) - 2 * scene.padding - (scene.title.isEmpty ? 0 : 24) > 0,
              PlotCommand.color(scene.background) != nil, scene.layers.count <= 10000,
              [scene.title, scene.x_label, scene.y_label].allSatisfy({ $0.utf8.count <= 65536 }) else { return nil }
        var nodes = 0
        for layer in scene.layers {
            guard ["line", "points", "polygons", "mesh", "guide", "vline", "hline", "annotation"].contains(layer.kind),
                  layer.guide_value.map({ $0.isFinite && abs($0) < 1e150 }) ?? true,
                  layer.anchor.map(PlotCommand.validPoint) ?? true,
                  layer.offset.map(PlotCommand.validPoint) ?? true else { return nil }
            if ["vline", "hline"].contains(layer.kind) && layer.guide_value == nil { return nil }
            if ["vline", "hline", "guide", "annotation"].contains(layer.kind) &&
                (layer.anchor == nil || layer.offset == nil || !["start", "middle", "end"].contains(layer.label_at ?? "")) { return nil }
            nodes += layer.commands.reduce(0) { $0 + 1 + $1.points.count + $1.rings.reduce(0) { $0 + $1.count } }
        }
        nodes += scene.decorations.reduce(0) { $0 + 1 + $1.points.count + $1.rings.reduce(0) { $0 + $1.count } }
        guard nodes <= 100000 else { return nil }
        return scene
    }

    var initialViewport: PlotViewport { PlotViewport(bounds)! }
    var originalFrame: CGRect { CGRect(x: frame[0], y: frame[1], width: frame[2], height: frame[3]) }
    func plotFrame(view: PlotViewport, size: CGSize) -> CGRect {
        var f = CGRect(x: padding, y: padding + (title.isEmpty ? 0 : 24),
                       width: size.width - 2 * padding - (color_scale ? 72 : 0),
                       height: size.height - 2 * padding - (title.isEmpty ? 0 : 24))
        if aspect_equal {
            let scale = min(f.width / (view.xMax - view.xMin), f.height / (view.yMax - view.yMin))
            let w = (view.xMax - view.xMin) * scale, h = (view.yMax - view.yMin) * scale
            f = CGRect(x: f.minX + (f.width - w) / 2, y: f.minY + (f.height - h) / 2, width: w, height: h)
        }
        return f
    }
    func dataPoint(_ p: CGPoint, view: PlotViewport, frame f: CGRect) -> CGPoint {
        CGPoint(x: view.xMin + (p.x - f.minX) / f.width * (view.xMax - view.xMin),
                y: y_down ? view.yMin + (p.y - f.minY) / f.height * (view.yMax - view.yMin)
                          : view.yMax - (p.y - f.minY) / f.height * (view.yMax - view.yMin))
    }
    func screenPoint(_ p: CGPoint, view: PlotViewport, frame f: CGRect) -> CGPoint {
        CGPoint(x: f.minX + (p.x - view.xMin) / (view.xMax - view.xMin) * f.width,
                y: f.minY + (y_down ? (p.y - view.yMin) : (view.yMax - p.y)) / (view.yMax - view.yMin) * f.height)
    }
    func selectedViewport(_ rectangle: CGRect, view: PlotViewport, size: CGSize) -> PlotViewport? {
        let f = plotFrame(view: view, size: size), r = rectangle.intersection(f)
        guard !r.isNull, r.width >= 4, r.height >= 4 else { return nil }
        let a = dataPoint(r.origin, view: view, frame: f)
        let b = dataPoint(CGPoint(x: r.maxX, y: r.maxY), view: view, frame: f)
        return PlotViewport([min(a.x, b.x), min(a.y, b.y), max(a.x, b.x), max(a.y, b.y)])
    }

    static func tick(_ value: Double, span: Double) -> String {
        guard value.isFinite, span.isFinite, span > 0 else { return "—" }
        let precision = ceil(log10(max(abs(value), 1e-150)) - log10(span)) + 3
        let digits = Int(min(14, max(4, precision)))
        return String(format: "%.*g", locale: Locale(identifier: "en_US_POSIX"), digits, value == 0 ? 0 : value)
    }

    // Clip before handing paths to Core Graphics: a perfectly ordinary data
    // series can project far beyond the canvas after repeated zooms. The
    // expanded clip keeps artificial polygon edges outside the visible plot.
    static func clipSegment(_ a: CGPoint, _ b: CGPoint, to r: CGRect) -> (CGPoint, CGPoint)? {
        let dx = b.x - a.x, dy = b.y - a.y
        guard [a.x, a.y, b.x, b.y, dx, dy].allSatisfy(\.isFinite) else { return nil }
        var low = 0.0, high = 1.0
        for (p, q) in zip([-dx, dx, -dy, dy], [a.x - r.minX, r.maxX - a.x, a.y - r.minY, r.maxY - a.y]) {
            if p == 0 { if q < 0 { return nil } }
            else if p < 0 { low = max(low, q / p) }
            else { high = min(high, q / p) }
        }
        guard low <= high else { return nil }
        func at(_ t: Double) -> CGPoint {
            CGPoint(x: min(r.maxX, max(r.minX, a.x + dx * t)), y: min(r.maxY, max(r.minY, a.y + dy * t)))
        }
        return (at(low), at(high))
    }

    static func clipPolygon(_ source: [CGPoint], to r: CGRect) -> [CGPoint] {
        guard source.allSatisfy({ $0.x.isFinite && $0.y.isFinite }) else { return [] }
        var points = source
        for edge in 0..<4 {
            let horizontal = edge >= 2, limit = [r.minX, r.maxX, r.minY, r.maxY][edge]
            func coordinate(_ p: CGPoint) -> Double { horizontal ? p.y : p.x }
            func inside(_ p: CGPoint) -> Bool { edge % 2 == 0 ? coordinate(p) >= limit : coordinate(p) <= limit }
            func intersection(_ a: CGPoint, _ b: CGPoint) -> CGPoint {
                let t = (limit - coordinate(a)) / (coordinate(b) - coordinate(a))
                return horizontal ? CGPoint(x: a.x + (b.x - a.x) * t, y: limit)
                                  : CGPoint(x: limit, y: a.y + (b.y - a.y) * t)
            }
            guard var previous = points.last else { return [] }
            var output: [CGPoint] = []
            for point in points {
                if inside(previous) != inside(point) { output.append(intersection(previous, point)) }
                if inside(point) { output.append(point) }
                previous = point
            }
            points = output
        }
        return points
    }

    func draw(view: PlotViewport, size: CGSize) {
        let f = plotFrame(view: view, size: size)
        guard f.width > 0, f.height > 0 else { return }
        PlotCommand.color(background)!.setFill(); NSBezierPath(rect: CGRect(origin: .zero, size: size)).fill()
        func label(_ text: String, _ p: CGPoint, size: Double, color: NSColor, align: Int, clipped: Bool = false) {
            let attributes: [NSAttributedString.Key: Any] = [.font: NSFont.systemFont(ofSize: size), .foregroundColor: color]
            let string = text as NSString, extent = string.size(withAttributes: attributes)
            let r = CGRect(x: p.x - extent.width * Double(align) / 2, y: p.y, width: extent.width, height: extent.height)
            if !clipped || f.contains(r) { string.draw(at: r.origin, withAttributes: attributes) }
        }
        func line(_ a: CGPoint, _ b: CGPoint, _ color: NSColor) {
            color.setStroke(); let path = NSBezierPath(); path.move(to: a); path.line(to: b); path.lineWidth = 1; path.stroke()
        }
        if !title.isEmpty { label(title, CGPoint(x: size.width / 2, y: 18), size: 20, color: .darkGray, align: 1) }
        if axes {
            for i in 0...5 {
                let t = Double(i) / 5, x = f.minX + t * f.width, y = f.minY + t * f.height
                line(CGPoint(x: x, y: f.minY), CGPoint(x: x, y: f.maxY), NSColor(white: 0.9, alpha: 1))
                line(CGPoint(x: f.minX, y: y), CGPoint(x: f.maxX, y: y), NSColor(white: 0.9, alpha: 1))
                let xv = view.xMin + t * (view.xMax - view.xMin)
                let yv = y_down ? view.yMin + t * (view.yMax - view.yMin) : view.yMax - t * (view.yMax - view.yMin)
                label(Self.tick(xv, span: view.xMax - view.xMin), CGPoint(x: x, y: f.maxY + 9), size: 11, color: .darkGray, align: 1)
                label(Self.tick(yv, span: view.yMax - view.yMin), CGPoint(x: f.minX - 8, y: y - 5), size: 11, color: .darkGray, align: 2)
            }
            label(x_label, CGPoint(x: f.midX, y: f.maxY + 33), size: 13, color: .darkGray, align: 1)
            label(y_label, CGPoint(x: f.minX, y: f.minY - 22), size: 13, color: .darkGray, align: 0)
        }
        func drawCommand(_ c: PlotCommand, layer: PlotSceneLayer?) {
            func point(_ p: [Double]) -> CGPoint {
                if layer == nil { return CGPoint(x: p[0] + f.maxX - originalFrame.maxX, y: p[1] + f.minY - originalFrame.minY) }
                return screenPoint(dataPoint(CGPoint(x: p[0], y: p[1]), view: initialViewport, frame: originalFrame), view: view, frame: f)
            }
            let path = NSBezierPath(); path.windingRule = .evenOdd; path.lineJoinStyle = .round; path.lineCapStyle = .round
            path.lineWidth = c.width
            if !c.dash.isEmpty { c.dash.map { CGFloat($0) }.withUnsafeBufferPointer { path.setLineDash($0.baseAddress!, count: $0.count, phase: 0) } }
            func ring(_ points: [[Double]], close: Bool) {
                let projected = points.map(point)
                let clip = f.insetBy(dx: -max(2, c.width), dy: -max(2, c.width))
                if close {
                    let clipped = layer == nil ? projected : Self.clipPolygon(projected, to: clip)
                    for (i, p) in clipped.enumerated() {
                        if i == 0 { path.move(to: p) } else { path.line(to: p) }
                    }
                    if !clipped.isEmpty { path.close() }
                } else {
                    for (a, b) in zip(projected, projected.dropFirst()) {
                        if let (start, end) = Self.clipSegment(a, b, to: clip) {
                            if path.isEmpty || path.currentPoint != start { path.move(to: start) }
                            path.line(to: end)
                        }
                    }
                }
            }
            switch c.kind {
            case 1: for r in c.rings { ring(r, close: true) }
            case 2:
                if let layer, let value = layer.guide_value {
                    if layer.kind == "vline" {
                        let x = screenPoint(CGPoint(x: value, y: view.yMin), view: view, frame: f).x
                        guard x.isFinite, x >= f.minX - c.width, x <= f.maxX + c.width else { return }
                        path.move(to: CGPoint(x: x, y: f.minY)); path.line(to: CGPoint(x: x, y: f.maxY))
                    } else {
                        let y = screenPoint(CGPoint(x: view.xMin, y: value), view: view, frame: f).y
                        guard y.isFinite, y >= f.minY - c.width, y <= f.maxY + c.width else { return }
                        path.move(to: CGPoint(x: f.minX, y: y)); path.line(to: CGPoint(x: f.maxX, y: y))
                    }
                } else { ring(c.points, close: false) }
            case 3:
                for p in c.points { let p = point(p); if f.insetBy(dx: -c.radius, dy: -c.radius).contains(p) {
                    path.appendOval(in: CGRect(x: p.x - c.radius, y: p.y - c.radius, width: c.radius * 2, height: c.radius * 2))
                } }
            case 4:
                var p = point(c.position)
                if let layer, let anchor = layer.anchor, let offset = layer.offset {
                    var a = CGPoint(x: anchor[0], y: anchor[1])
                    let t = layer.label_at == "start" ? 0.0 : layer.label_at == "end" ? 1.0 : 0.5
                    if layer.kind == "vline" { a.y = y_down ? view.yMin + t * (view.yMax - view.yMin) : view.yMax - t * (view.yMax - view.yMin) }
                    if layer.kind == "hline" { a.x = view.xMin + t * (view.xMax - view.xMin) }
                    p = screenPoint(a, view: view, frame: f); p.x += offset[0]; p.y += offset[1]
                }
                label(c.text, p, size: c.size, color: PlotCommand.color(c.fill)!, align: c.align, clipped: layer != nil)
                return
            default: return
            }
            let fill = PlotCommand.color(c.fill)!, stroke = PlotCommand.color(c.stroke)!
            fill.withAlphaComponent(fill.alphaComponent * c.opacity).setFill(); path.fill()
            if c.width > 0 { stroke.withAlphaComponent(stroke.alphaComponent * c.opacity).setStroke(); path.stroke() }
        }
        NSGraphicsContext.saveGraphicsState()
        NSBezierPath(rect: f).addClip()
        for layer in layers { for c in layer.commands { drawCommand(c, layer: layer) } }
        NSGraphicsContext.restoreGraphicsState()
        if axes { NSColor.lightGray.setStroke(); let border = NSBezierPath(rect: f); border.lineWidth = 1; border.stroke() }
        for c in decorations { drawCommand(c, layer: nil) }
    }

    func png(view: PlotViewport, width: Int, height: Int) -> Data? {
        guard width > 0, height > 0, width <= 8192, height <= 8192, width * height <= 16_000_000,
              let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: width, pixelsHigh: height,
                                            bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                                            colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0),
              let graphics = NSGraphicsContext(bitmapImageRep: bitmap) else { return nil }
        NSGraphicsContext.saveGraphicsState()
        let cg = graphics.cgContext
        cg.translateBy(x: 0, y: CGFloat(height)); cg.scaleBy(x: 1, y: -1)
        NSGraphicsContext.current = NSGraphicsContext(cgContext: cg, flipped: true)
        draw(view: view, size: CGSize(width: width, height: height))
        NSGraphicsContext.restoreGraphicsState()
        return bitmap.representation(using: .png, properties: [:])
    }
}
