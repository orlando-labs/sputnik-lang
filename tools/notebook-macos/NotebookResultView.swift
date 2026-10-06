import AppKit
import SwiftUI

enum NotebookResultSearch {
    // Literal, non-overlapping matches, in NSTextView's UTF-16 coordinates.
    static func matches(in text: String, query: String, matchCase: Bool = false) -> [NSRange] {
        guard !query.isEmpty else { return [] }
        let source = text as NSString
        var matches: [NSRange] = []
        var offset = 0
        let options: NSString.CompareOptions = matchCase ? [.literal] : [.literal, .caseInsensitive]
        while offset < source.length {
            let range = source.range(of: query, options: options,
                                     range: NSRange(location: offset, length: source.length - offset))
            guard range.location != NSNotFound, range.length > 0 else { break }
            matches.append(range)
            offset = NSMaxRange(range)
        }
        return matches
    }

    static func preview(_ text: String) -> String {
        // Bound both newlines and characters before SwiftUI measures the text.
        let prefix = String(text.prefix(600).split(separator: "\n", maxSplits: 6,
                                                  omittingEmptySubsequences: false).prefix(6).joined(separator: "\n"))
        return prefix == text ? prefix : prefix + "…"
    }

    static func step(_ current: Int, by delta: Int, count: Int) -> Int {
        guard count > 0 else { return 0 }
        return ((current + delta) % count + count) % count
    }
}

enum NotebookResultLayout {
    static func canExpand(text: String, naturalHeight: CGFloat, previewHeight: CGFloat) -> Bool {
        NotebookResultSearch.preview(text) != text ||
            (previewHeight > 0 && naturalHeight > previewHeight + 0.5)
    }
}

private struct ResultPreviewSizeKey: PreferenceKey {
    static var defaultValue: [String: CGFloat] = [:]
    static func reduce(value: inout [String: CGFloat], nextValue: () -> [String: CGFloat]) {
        value.merge(nextValue(), uniquingKeysWith: { _, next in next })
    }
}

/// Measure the same SwiftUI text/font/width as the actual preview. Character
/// count alone cannot tell whether wrapping hides lines in a narrow board.
struct NotebookResultPreview: View {
    let text: String
    let measured: (CGFloat, CGFloat) -> Void
    private var preview: String { NotebookResultSearch.preview(text) }
    var body: some View {
        Text(preview).lineLimit(6).fixedSize(horizontal: false, vertical: true)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(GeometryReader { proxy in
                Color.clear.preference(key: ResultPreviewSizeKey.self, value: ["preview": proxy.size.height])
            })
            .overlay(alignment: .topLeading) {
                Text(preview).lineLimit(nil).fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(GeometryReader { proxy in
                        Color.clear.preference(key: ResultPreviewSizeKey.self, value: ["natural": proxy.size.height])
                    })
                    .hidden().accessibilityHidden(true).allowsHitTesting(false)
            }
            .font(.system(size: 12, design: .monospaced))
            .foregroundStyle(.primary).textSelection(.enabled)
            .onPreferenceChange(ResultPreviewSizeKey.self) { sizes in
                if let natural = sizes["natural"], let preview = sizes["preview"] { measured(natural, preview) }
            }
    }
}

struct NotebookResultView: View {
    let title: String
    let text: String
    let pretty: String?
    let truncated: Bool
    let prettyTruncated: Bool
    let tint: Color
    @State private var usePretty = false
    @State private var expanded = false
    @State private var finding = false
    @State private var query = ""
    @State private var matchCase = false
    @State private var selectedMatch = 0
    @State private var naturalHeight: CGFloat = 0
    @State private var previewHeight: CGFloat = 0
    @FocusState private var searchFocused: Bool

    private var renderedText: String { usePretty ? (pretty ?? text) : text }
    private var limited: Bool { usePretty && pretty != nil ? prettyTruncated : truncated }
    private var canExpand: Bool {
        NotebookResultLayout.canExpand(text: renderedText, naturalHeight: naturalHeight, previewHeight: previewHeight)
    }
    private var showingFullText: Bool { (expanded && canExpand) || finding }

    var body: some View {
        let ranges = NotebookResultSearch.matches(in: renderedText, query: finding ? query : "", matchCase: matchCase)
        VStack(alignment: .leading, spacing: 9) {
            HStack(spacing: 12) {
                Label(title, systemImage: "equal")
                    .font(.system(size: 9, weight: .bold, design: .rounded))
                    .tracking(0.7).foregroundStyle(tint)
                Spacer(minLength: 8)
                NotebookObjectMenu(title: "Result actions") {
                    if let pretty, !pretty.isEmpty {
                        Picker("Result format", selection: $usePretty) {
                            Text("Inspect").tag(false)
                            Text("Pretty").tag(true)
                        }.pickerStyle(.inline)
                        Divider()
                    }
                    Button(finding ? "Close result search" : "Find in result…") {
                        finding.toggle()
                        searchFocused = finding
                    }
                    Button("Copy captured result") {
                        NSPasteboard.general.clearContents()
                        NSPasteboard.general.setString(renderedText, forType: .string)
                    }
                }
            }
            .buttonStyle(.plain)

            if finding {
                HStack(spacing: 8) {
                    TextField("Find substring in result", text: $query)
                        .textFieldStyle(.roundedBorder).focused($searchFocused)
                        .onSubmit { selectedMatch = NotebookResultSearch.step(selectedMatch, by: 1, count: ranges.count) }
                        .onChange(of: query) { selectedMatch = 0 }
                    Toggle("Aa", isOn: $matchCase).toggleStyle(.button)
                        .help("Match case").accessibilityLabel("Match case")
                        .onChange(of: matchCase) { selectedMatch = 0 }
                    Text(query.isEmpty ? "" : ranges.isEmpty ? "No matches" : "\(min(selectedMatch + 1, ranges.count)) / \(ranges.count)")
                        .font(.system(size: 11, design: .monospaced)).foregroundStyle(.secondary)
                    Button { selectedMatch = NotebookResultSearch.step(selectedMatch, by: -1, count: ranges.count) }
                        label: { Image(systemName: "chevron.up") }
                        .disabled(ranges.isEmpty).help("Previous match").accessibilityLabel("Previous match")
                    Button { selectedMatch = NotebookResultSearch.step(selectedMatch, by: 1, count: ranges.count) }
                        label: { Image(systemName: "chevron.down") }
                        .disabled(ranges.isEmpty).help("Next match").accessibilityLabel("Next match")
                    Button { finding = false; searchFocused = false }
                        label: { Image(systemName: "xmark") }
                        .help("Close result search").accessibilityLabel("Close result search")
                }
                .controlSize(.small)
            }

            ZStack(alignment: .topLeading) {
                // Keep measuring while expanded so a resize or shorter result
                // can remove the disclosure rather than leave a stale control.
                NotebookResultPreview(text: renderedText) { natural, preview in
                    naturalHeight = natural; previewHeight = preview
                }
                .opacity(showingFullText ? 0 : 1).accessibilityHidden(showingFullText)
                .allowsHitTesting(!showingFullText)
                if showingFullText {
                    NotebookResultText(text: renderedText, matches: ranges, selectedMatch: selectedMatch)
                        .frame(height: canExpand ? 260 : max(28, previewHeight + 8))
                }
            }
            if canExpand || limited {
              HStack {
                if canExpand {
                    Button(showingFullText ? "Collapse" : "Expand result") {
                        if showingFullText { expanded = false; finding = false; searchFocused = false }
                        else { expanded = true }
                    }
                    .buttonStyle(.plain).foregroundStyle(tint)
                }
                if limited {
                    Label("Result limited · search covers captured text only", systemImage: "ellipsis.circle")
                        .foregroundStyle(.secondary)
                        .help("Snapshot budget: 64 KiB per format, 4,096 values, nesting depth 16. The value itself is unchanged.")
                }
                Spacer(minLength: 0)
              }.font(.system(size: 10))
            }
        }
        .onChange(of: renderedText) { selectedMatch = 0 }
        .onChange(of: canExpand) { if !canExpand { expanded = false } }
        .padding(.horizontal, 12).padding(.vertical, 10)
        .background(tint.opacity(0.045), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
    }
}

/// Selectable native text with bounded height. Searching never replaces the
/// source string, and navigation does not steal focus from the search field.
struct NotebookResultText: NSViewRepresentable {
    let text: String
    let matches: [NSRange]
    let selectedMatch: Int

    func makeCoordinator() -> Coordinator { Coordinator() }
    func makeNSView(context: Context) -> NSScrollView {
        let scroll = NSScrollView()
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        let view = NSTextView(frame: NSRect(x: 0, y: 0, width: 640, height: 260))
        view.isEditable = false
        view.isSelectable = true
        view.isRichText = false
        view.drawsBackground = false
        view.isVerticallyResizable = true
        view.isHorizontallyResizable = false
        view.autoresizingMask = [.width]
        view.minSize = .zero
        view.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
        view.textContainer?.widthTracksTextView = true
        view.textContainer?.heightTracksTextView = false
        view.textContainer?.containerSize = NSSize(width: 640, height: CGFloat.greatestFiniteMagnitude)
        view.textContainerInset = NSSize(width: 0, height: 4)
        view.setAccessibilityLabel("Result text")
        scroll.documentView = view
        return scroll
    }
    func updateNSView(_ scroll: NSScrollView, context: Context) {
        guard let view = scroll.documentView as? NSTextView else { return }
        context.coordinator.update(view, text: text, matches: matches, selected: selectedMatch)
    }

    final class Coordinator {
        private var previousText: String?
        private var previousMatches: [NSRange] = []
        private var previousSelection = -1

        func update(_ view: NSTextView, text: String, matches: [NSRange], selected: Int) {
            let replaced = previousText != text
            guard replaced || previousMatches != matches || previousSelection != selected else { return }
            let attributes: [NSAttributedString.Key: Any] = [
                .font: NSFont.monospacedSystemFont(ofSize: 12, weight: .regular),
                .foregroundColor: NSColor.labelColor
            ]
            let storage = view.textStorage!
            storage.beginEditing()
            if replaced { storage.setAttributedString(NSAttributedString(string: text, attributes: attributes)) }
            else { storage.removeAttribute(.backgroundColor, range: NSRange(location: 0, length: storage.length)) }
            for range in matches where NSMaxRange(range) <= storage.length {
                storage.addAttribute(.backgroundColor, value: NSColor.systemYellow.withAlphaComponent(0.3), range: range)
            }
            let active = matches.indices.contains(selected) ? matches[selected] : nil
            if let active, NSMaxRange(active) <= storage.length {
                storage.addAttribute(.backgroundColor, value: NSColor.systemOrange.withAlphaComponent(0.6), range: active)
            }
            storage.endEditing()
            if let active { view.scrollRangeToVisible(active) }
            else if replaced { view.scrollRangeToVisible(NSRange(location: 0, length: 0)) }
            previousText = text
            previousMatches = matches
            previousSelection = selected
        }
    }
}
