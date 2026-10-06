import AppKit
import ImageIO

/// Bytes and captions are runtime output, not persisted project content.
struct NotebookFigurePanel: Decodable {
    var plot_scene: String? = nil
    let mime: String
    let caption: String
    let width: Int
    let height: Int
    let data: Data

    func image(maxPixelSize: Int = 1200) -> NSImage? {
        guard mime == "image/png", data.count <= 16 * 1024 * 1024,
              width > 0, height > 0, width <= 8192, height <= 8192, width * height <= 16_000_000,
              let source = CGImageSourceCreateWithData(data as CFData, nil),
              CGImageSourceGetType(source) as String? == "public.png",
              let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
              (properties[kCGImagePropertyPixelWidth] as? Int) == width,
              (properties[kCGImagePropertyPixelHeight] as? Int) == height else { return nil }
        // A notebook may contain many high-resolution panels. Decode bounded
        // previews; only the selected lightbox/share image needs full pixels.
        let options: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceThumbnailMaxPixelSize: max(1, min(8192, maxPixelSize)),
            kCGImageSourceShouldCacheImmediately: true
        ]
        guard let image = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary) else { return nil }
        return NSImage(cgImage: image, size: NSSize(width: image.width, height: image.height))
    }

    static func label(_ index: Int) -> String {
        var n = index + 1, label = ""
        while n > 0 { n -= 1; label = String(UnicodeScalar(97 + n % 26)!) + label; n /= 26 }
        return label + ")"
    }
}
