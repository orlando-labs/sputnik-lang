import AppKit
import SwiftUI

// MARK: - Portable rich-text format

/// The only attributes which are allowed to cross the notebook boundary are
/// the attributes represented by this small value format.  In particular, an
/// attributed string is never archived as RTF/HTML: those formats carry font,
/// URL, attachment, and platform-specific information which cannot be made
/// portable.
enum RichTextCodecError: Error, LocalizedError, Equatable {
    case invalidJSON(String)
    case unsupportedVersion(Int)
    case invalidSchema(String)
    case invalidRange(start: Int, length: Int)
    case overlappingRanges
    case nonScalarBoundary(Int)
    case textTooLong
    case unsupportedContent(String)

    var errorDescription: String? {
        switch self {
        case .invalidJSON(let reason): return "Invalid rich-text formatting JSON: \(reason)."
        case .unsupportedVersion(let version):
            return "Rich-text formatting version \(version) is not supported."
        case .invalidSchema(let reason): return "Invalid rich-text formatting: \(reason)."
        case .invalidRange(let start, let length):
            return "Invalid rich-text range (start \(start), length \(length))."
        case .overlappingRanges: return "Rich-text ranges must be sorted and non-overlapping."
        case .nonScalarBoundary(let offset):
            return "Rich-text range ends at UTF-16 offset \(offset), inside a Unicode scalar."
        case .textTooLong: return "Rich-text cells are limited to 16 MiB of UTF-16 text."
        case .unsupportedContent(let content): return "Unsupported rich-text content: \(content)."
        }
    }
}

/// A deliberately small, deterministic codec for notebook text cells.
enum RichTextCodec {
    static let maxUTF16Length = 16 * 1024 * 1024
    static let maxRunCount = 20_000

    // These attributes are deliberately private to the native editor.  They
    // make encode(decode(...)) lossless without making the persisted format
    // depend on AppKit fonts.
    static let styleAttributeKey = NSAttributedString.Key("com.amber.rich-text.style")
    private static let boldAttributeKey = NSAttributedString.Key("com.amber.rich-text.bold")
    private static let italicAttributeKey = NSAttributedString.Key("com.amber.rich-text.italic")
    private static let underlineAttributeKey = NSAttributedString.Key("com.amber.rich-text.underline")

    enum Style: String, CaseIterable {
        case body
        case heading1
        case heading2
        case heading3
        case code
    }

    struct Run: Equatable {
        let start: Int
        let length: Int
        let style: Style
        let bold: Bool
        let italic: Bool
        let underline: Bool
    }

    /// Decode the portable representation into a sanitized AppKit string.
    /// Unknown schema fields are rejected instead of being silently discarded.
    static func decode(text: String, formatting: String) throws -> NSAttributedString {
        let utf16Length = (text as NSString).length
        guard utf16Length <= maxUTF16Length else { throw RichTextCodecError.textTooLong }

        let (inlineFormatting, structure) = try RichTextStructure.baseFormatting(formatting)
        let runs: [Run]
        if inlineFormatting.isEmpty {
            runs = []
        } else {
            runs = try parseRuns(inlineFormatting, text: text)
        }

        let result = NSMutableAttributedString(
            string: text,
            attributes: attributes(style: .body, bold: false, italic: false, underline: false)
        )
        for run in runs {
            result.setAttributes(
                attributes(style: run.style,
                           bold: run.bold,
                           italic: run.italic,
                           underline: run.underline),
                range: NSRange(location: run.start, length: run.length)
            )
        }
        if let structure { try RichTextStructure.apply(structure, to: result) }
        return result.copy() as! NSAttributedString
    }

    /// Encode an attributed string as plain UTF-8 text plus deterministic JSON.
    /// Arbitrary display attributes are intentionally sanitized. Attachments
    /// and links are rejected because converting them to text would lose data.
    static func encode(_ value: NSAttributedString) throws -> (text: String, formatting: String) {
        let text = value.string
        let utf16Length = (text as NSString).length
        guard utf16Length <= maxUTF16Length else { throw RichTextCodecError.textTooLong }
        guard utf16Length > 0 else { return (text: text, formatting: "") }

        var encodedRuns: [Run] = []
        var currentDescriptor: Descriptor?
        var currentStart = 0
        var currentEnd = 0

        var descriptorError: Error?
        value.enumerateAttributes(in: NSRange(location: 0, length: utf16Length), options: []) {
            attrs, range, _ in
            guard descriptorError == nil else { return }
            guard isScalarBoundary(range.location, in: text) else {
                descriptorError = RichTextCodecError.nonScalarBoundary(range.location)
                return
            }
            guard isScalarBoundary(NSMaxRange(range), in: text) else {
                descriptorError = RichTextCodecError.nonScalarBoundary(NSMaxRange(range))
                return
            }
            do {
                let descriptor = try descriptor(for: attrs)
                let candidate = descriptor.isDefault ? nil : descriptor
                if candidate == currentDescriptor && currentEnd == range.location {
                    currentEnd = NSMaxRange(range)
                } else {
                    if let currentDescriptor {
                        encodedRuns.append(Run(start: currentStart,
                                               length: currentEnd - currentStart,
                                               style: currentDescriptor.style,
                                               bold: currentDescriptor.bold,
                                               italic: currentDescriptor.italic,
                                               underline: currentDescriptor.underline))
                    }
                    currentDescriptor = candidate
                    currentStart = range.location
                    currentEnd = NSMaxRange(range)
                }
            } catch {
                descriptorError = error
            }
        }
        if let descriptorError { throw descriptorError }
        if let currentDescriptor {
            encodedRuns.append(Run(start: currentStart,
                                   length: currentEnd - currentStart,
                                   style: currentDescriptor.style,
                                   bold: currentDescriptor.bold,
                                   italic: currentDescriptor.italic,
                                   underline: currentDescriptor.underline))
        }

        guard encodedRuns.count <= maxRunCount else {
            throw RichTextCodecError.invalidSchema("too many formatting ranges")
        }
        let inlineFormatting = encodedRuns.isEmpty ? "" : json(for: encodedRuns)
        return (text: text, formatting: try RichTextStructure.encode(value, inlineFormatting: inlineFormatting))
    }

    // MARK: Parsing

    private static func parseRuns(_ formatting: String, text: String) throws -> [Run] {
        let data = Data(formatting.utf8)
        let object: Any
        do {
            object = try JSONSerialization.jsonObject(with: data, options: [.fragmentsAllowed])
        } catch {
            throw RichTextCodecError.invalidJSON(error.localizedDescription)
        }
        guard let dictionary = object as? [String: Any] else {
            throw RichTextCodecError.invalidSchema("top-level value must be an object")
        }
        let allowedTopLevel: Set<String> = ["version", "runs"]
        guard Set(dictionary.keys) == allowedTopLevel else {
            throw RichTextCodecError.invalidSchema("expected only version and runs")
        }
        guard let versionValue = dictionary["version"], let version = integer(versionValue) else {
            throw RichTextCodecError.invalidSchema("version must be an integer")
        }
        guard version == 1 else { throw RichTextCodecError.unsupportedVersion(version) }
        guard let rawRuns = dictionary["runs"] as? [Any] else {
            throw RichTextCodecError.invalidSchema("runs must be an array")
        }
        guard rawRuns.count <= maxRunCount else {
            throw RichTextCodecError.invalidSchema("too many formatting ranges")
        }

        let textLength = (text as NSString).length
        var previousEnd = 0
        var result: [Run] = []
        result.reserveCapacity(rawRuns.count)

        for rawRun in rawRuns {
            guard let run = rawRun as? [String: Any] else {
                throw RichTextCodecError.invalidSchema("each run must be an object")
            }
            let allowedRunKeys: Set<String> = ["start", "length", "style", "bold", "italic", "underline"]
            guard Set(run.keys).isSubset(of: allowedRunKeys) else {
                throw RichTextCodecError.invalidSchema("run contains an unknown field")
            }
            guard let startValue = run["start"], let start = integer(startValue), start >= 0,
                  let lengthValue = run["length"], let length = integer(lengthValue), length > 0 else {
                throw RichTextCodecError.invalidSchema("run requires non-negative start, positive length, and a supported style")
            }
            let style: Style
            if let rawStyleValue = run["style"] {
                guard let rawStyle = rawStyleValue as? String,
                      let parsedStyle = Style(rawValue: rawStyle) else {
                    throw RichTextCodecError.invalidSchema("run style must be supported")
                }
                style = parsedStyle
            } else {
                style = .body
            }
            let bold = try bool(run["bold"], field: "bold")
            let italic = try bool(run["italic"], field: "italic")
            let underline = try bool(run["underline"], field: "underline")
            guard start <= Int.max - length, start + length <= textLength else {
                throw RichTextCodecError.invalidRange(start: start, length: length)
            }
            guard isScalarBoundary(start, in: text) else {
                throw RichTextCodecError.nonScalarBoundary(start)
            }
            guard isScalarBoundary(start + length, in: text) else {
                throw RichTextCodecError.nonScalarBoundary(start + length)
            }
            guard start >= previousEnd else { throw RichTextCodecError.overlappingRanges }
            result.append(Run(start: start,
                              length: length,
                              style: style,
                              bold: bold,
                              italic: italic,
                              underline: underline))
            previousEnd = start + length
        }
        return result
    }

    private static func integer(_ value: Any) -> Int? {
        guard let number = value as? NSNumber,
              CFGetTypeID(number) == CFNumberGetTypeID() else { return nil }
        // NSNumber.doubleValue cannot distinguish Int.max from 2^63 after
        // rounding. Parsing its exact decimal spelling avoids a trap when a
        // hostile document supplies an offset just beyond Int.max.
        return Int(number.stringValue)
    }

    private static func bool(_ value: Any?, field: String) throws -> Bool {
        guard let value else { return false }
        guard let number = value as? NSNumber,
              CFGetTypeID(number) == CFBooleanGetTypeID() else {
            throw RichTextCodecError.invalidSchema("\(field) must be a boolean")
        }
        return number.boolValue
    }

    private static func isScalarBoundary(_ offset: Int, in text: String) -> Bool {
        let length = (text as NSString).length
        guard offset > 0, offset < length else { return true }
        let nsText = text as NSString
        let previous = nsText.character(at: offset - 1)
        let next = nsText.character(at: offset)
        let isHighSurrogate = previous >= 0xD800 && previous <= 0xDBFF
        let isLowSurrogate = next >= 0xDC00 && next <= 0xDFFF
        return !(isHighSurrogate && isLowSurrogate)
    }

    // MARK: Attribute normalization

    private struct Descriptor: Equatable {
        let style: Style
        let bold: Bool
        let italic: Bool
        let underline: Bool

        var isDefault: Bool {
            style == .body && !bold && !italic && !underline
        }
    }

    private static func descriptor(for attrs: [NSAttributedString.Key: Any]) throws -> Descriptor {
        if attrs[.attachment] != nil {
            throw RichTextCodecError.unsupportedContent("attachments")
        }
        if attrs[.link] != nil {
            throw RichTextCodecError.unsupportedContent("links")
        }

        let style: Style
        if let rawStyle = attrs[styleAttributeKey] as? String,
           let explicitStyle = Style(rawValue: rawStyle) {
            style = explicitStyle
        } else if let font = attrs[.font] as? NSFont {
            style = inferredStyle(from: font)
        } else {
            style = .body
        }

        let bold = (attrs[boldAttributeKey] as? Bool) ??
            ((attrs[.font] as? NSFont)?.fontDescriptor.symbolicTraits.contains(.bold) ?? false)
        let italic = (attrs[italicAttributeKey] as? Bool) ??
            ((attrs[.font] as? NSFont)?.fontDescriptor.symbolicTraits.contains(.italic) ?? false)
        let underline: Bool
        if let explicit = attrs[underlineAttributeKey] as? Bool {
            underline = explicit
        } else if let number = attrs[.underlineStyle] as? NSNumber {
            underline = number.intValue != NSUnderlineStyle.byWord.rawValue && number.intValue != 0
        } else {
            underline = false
        }
        return Descriptor(style: style, bold: bold, italic: italic, underline: underline)
    }

    private static func inferredStyle(from font: NSFont) -> Style {
        let traits = font.fontDescriptor.symbolicTraits
        if traits.contains(.monoSpace) { return .code }
        if font.pointSize >= 23 { return .heading1 }
        if font.pointSize >= 19 { return .heading2 }
        if font.pointSize >= 16 { return .heading3 }
        return .body
    }

    static func attributes(style: Style, bold: Bool, italic: Bool, underline: Bool) -> [NSAttributedString.Key: Any] {
        var result: [NSAttributedString.Key: Any] = [
            .font: font(style: style, bold: bold, italic: italic),
            styleAttributeKey: style.rawValue,
            boldAttributeKey: bold,
            italicAttributeKey: italic,
            underlineAttributeKey: underline
        ]
        if underline {
            result[.underlineStyle] = NSUnderlineStyle.single.rawValue
        }
        return result
    }

    static func font(style: Style, bold: Bool, italic: Bool) -> NSFont {
        let size: CGFloat
        let weight: NSFont.Weight
        let base: NSFont
        switch style {
        case .body: size = 14; weight = .regular; base = NSFont.systemFont(ofSize: size, weight: weight)
        case .heading1: size = 24; weight = .bold; base = NSFont.systemFont(ofSize: size, weight: weight)
        case .heading2: size = 20; weight = .semibold; base = NSFont.systemFont(ofSize: size, weight: weight)
        case .heading3: size = 17; weight = .semibold; base = NSFont.systemFont(ofSize: size, weight: weight)
        case .code: size = 13; weight = .regular; base = NSFont.monospacedSystemFont(ofSize: size, weight: weight)
        }
        var traits: NSFontDescriptor.SymbolicTraits = []
        if bold || style == .heading1 || style == .heading2 || style == .heading3 { traits.insert(.bold) }
        if italic { traits.insert(.italic) }
        let descriptor = base.fontDescriptor.withSymbolicTraits(traits)
        return NSFont(descriptor: descriptor, size: size) ?? base
    }

    private static func json(for runs: [Run]) -> String {
        var output = "{\"version\":1,\"runs\":["
        for (index, run) in runs.enumerated() {
            if index > 0 { output.append(",") }
            let bold = run.bold ? "true" : "false"
            let italic = run.italic ? "true" : "false"
            let underline = run.underline ? "true" : "false"
            output += "{\"start\":\(run.start),\"length\":\(run.length),\"style\":\"\(run.style.rawValue)\",\"bold\":\(bold),\"italic\":\(italic),\"underline\":\(underline)}"
        }
        output += "]}"
        return output
    }
}

// MARK: - SwiftUI editor

struct RichTextEditor: View {
    var text: String
    var formatting: String
    var editable: Bool = true
    var variables: [NotebookTextVariable] = []
    var onChange: (String, String) -> Void = { _, _ in }
    var onFocus: () -> Void = {}
    var onError: (String?) -> Void = { _ in }

    @StateObject private var controller: RichTextEditorController
    @State private var preview = false
    @State private var choosingVariable = false

    init(text: String,
         formatting: String,
         editable: Bool = true,
         variables: [NotebookTextVariable] = [],
         onChange: @escaping (String, String) -> Void = { _, _ in },
         onFocus: @escaping () -> Void = {},
         onError: @escaping (String?) -> Void = { _ in }) {
        self.text = text
        self.formatting = formatting
        self.editable = editable
        self.variables = variables
        self.onChange = onChange
        self.onFocus = onFocus
        self.onError = onError
        _controller = StateObject(wrappedValue: RichTextEditorController(onChange: onChange,
                                                                           onFocus: onFocus))
    }

    var body: some View {
        VStack(spacing: 0) {
            toolbar
            if let error = controller.error {
                HStack(alignment: .top, spacing: 7) {
                    Image(systemName: "exclamationmark.triangle.fill")
                    Text(error).fixedSize(horizontal: false, vertical: true)
                    Spacer(minLength: 0)
                }
                .font(.system(size: 11))
                .foregroundStyle(.orange)
                .padding(.horizontal, 11)
                .padding(.vertical, 7)
                .background(Color.orange.opacity(0.10))
            }
            RichTextEditorRepresentable(text: text,
                                        formatting: formatting,
                                        editable: editable && !preview,
                                        controller: controller,
                                        onChange: onChange,
                                        onFocus: onFocus,
                                        onError: onError)
                .frame(minHeight: 96)
                .frame(height: preview ? 0 : nil)
                .clipped()
                .opacity(preview ? 0 : 1)
                .accessibilityHidden(preview)
            if preview {
                RichTextPreview(text: text, formatting: formatting, variables: variables)
                    .frame(minHeight: 96)
            }
        }
        .overlay(RoundedRectangle(cornerRadius: 7).stroke(Color.secondary.opacity(0.22)))
        .clipShape(RoundedRectangle(cornerRadius: 7))
    }

    private var toolbar: some View {
        HStack(spacing: 4) {
            Group {
            Menu {
                styleButton("Body", .body)
                styleButton("Heading 1", .heading1)
                styleButton("Heading 2", .heading2)
                styleButton("Heading 3", .heading3)
                styleButton("Inline code", .code)
            } label: {
                Label("Style", systemImage: "textformat")
            }
            .menuStyle(.borderlessButton)

            Divider().frame(height: 18)
            formatButton("bold", help: "Bold") { controller.apply(.bold) }
            formatButton("italic", help: "Italic") { controller.apply(.italic) }
            formatButton("underline", help: "Underline") { controller.apply(.underline) }
            Divider().frame(height: 18)
            Button { controller.list(.bullet) } label: {
                Image(systemName: "list.bullet")
            }
            .buttonStyle(.borderless)
            .help("Bullet list · toggle selected paragraphs")
            Button { controller.list(.numbered) } label: { Image(systemName: "list.number") }
                .buttonStyle(.borderless)
                .help("Numbered list · toggle selected paragraphs")
            Menu {
                Button("Insert 2 × 2 table") { controller.insertTable(rows: 2, columns: 2) }
                Button("Insert 3 × 3 table") { controller.insertTable(rows: 3, columns: 3) }
                Divider()
                Button("Add row below") { controller.table(.addRow) }
                Button("Add column right") { controller.table(.addColumn) }
                Button("Remove current row") { controller.table(.removeRow) }
                Button("Remove current column") { controller.table(.removeColumn) }
                Button("Continue after table") { controller.table(.afterTable) }
                Button("Remove table") { controller.table(.removeTable) }
            } label: { Image(systemName: "tablecells") }
                .menuStyle(.borderlessButton)
                .fixedSize()
                .help("Table · Tab moves between cells")
            Button { choosingVariable = true } label: { Image(systemName: "curlybraces") }
                .buttonStyle(.borderless)
                .accessibilityLabel("Insert variable")
                .help("Insert notebook variable · {{name}}")
                .popover(isPresented: $choosingVariable) {
                    NotebookVariablePicker(variables: variables) { name in
                        choosingVariable = false
                        controller.insertVariable(name)
                    }
                }
            }.disabled(preview)
            Spacer(minLength: 0)
            Button(preview ? "Edit" : "Preview") { preview.toggle() }
                .buttonStyle(.borderless)
                .help("Switch between template editing and interpolated values")
        }
        .padding(.horizontal, 9)
        .padding(.vertical, 5)
        .background(Color.secondary.opacity(0.07))
        .disabled(!editable || controller.decodeFailed)
    }

    @ViewBuilder
    private func styleButton(_ title: String, _ style: RichTextCodec.Style) -> some View {
        Button(title) { controller.apply(.style(style)) }
    }

    private func formatButton(_ symbol: String, help: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .frame(width: 19, height: 19)
        }
        .buttonStyle(.borderless)
        .help(help)
    }
}

final class RichTextEditorController: ObservableObject {
    enum Action { case style(RichTextCodec.Style), bold, italic, underline }

    @Published var error: String?
    @Published var decodeFailed = false
    weak var textView: RichTextTextView?
    var onChange: (String, String) -> Void
    var onFocus: () -> Void
    var onAction: ((RichTextTextView) -> Void)?
    var rememberedSelection = NSRange(location: 0, length: 0)

    init(onChange: @escaping (String, String) -> Void, onFocus: @escaping () -> Void) {
        self.onChange = onChange
        self.onFocus = onFocus
        self.error = nil
    }

    func apply(_ action: Action) {
        guard let textView, textView.isEditable, !textView.hasMarkedText() else { return }
        let textLength = textView.textStorage?.length ?? 0
        let selection = boundedSelection(in: textView, length: textLength)
        textView.window?.makeFirstResponder(textView)
        textView.setSelectedRange(selection)
        if case .style(let style) = action, style != .code {
            RichTextEditing.heading(style, in: textView)
        } else {
            RichTextEditorSupport.apply(action, to: textView, selection: selection)
            textView.setSelectedRange(selection)
        }
        onAction?(textView)
    }

    private func structural(_ action: (RichTextTextView) -> Void) {
        guard let textView, textView.isEditable, !textView.hasMarkedText() else { return }
        let selection = boundedSelection(in: textView, length: textView.string.utf16.count)
        textView.window?.makeFirstResponder(textView)
        textView.setSelectedRange(selection)
        action(textView)
        rememberedSelection = textView.selectedRange()
        onAction?(textView)
    }
    func list(_ kind: RichTextStructure.ListKind) { structural { RichTextEditing.toggleList(kind, in: $0) } }
    func insertTable(rows: Int, columns: Int) { structural { RichTextEditing.insertTable(rows: rows, columns: columns, in: $0) } }
    func table(_ action: RichTextEditing.TableAction) { structural { RichTextEditing.tableAction(action, in: $0) } }
    func insertVariable(_ name: String) { structural { RichTextEditing.insert("{{\(name)}}", replacing: $0.selectedRange(), in: $0) } }

    private func boundedSelection(in view: NSTextView, length: Int) -> NSRange {
        let range = rememberedSelection.location <= length ? rememberedSelection : view.selectedRange()
        let start = min(range.location, length)
        return NSRange(location: start, length: min(range.length, length - start))
    }
}

struct RichTextEditorRepresentable: NSViewRepresentable {
    let text: String
    let formatting: String
    let editable: Bool
    let controller: RichTextEditorController
    let onChange: (String, String) -> Void
    let onFocus: () -> Void
    let onError: (String?) -> Void

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    func makeNSView(context: Context) -> NSScrollView {
        let scrollView = RichTextScrollView()
        scrollView.drawsBackground = true
        scrollView.backgroundColor = NSColor.textBackgroundColor
        scrollView.borderType = .noBorder
        scrollView.hasVerticalScroller = true
        scrollView.autohidesScrollers = true

        let textView = RichTextTextView(frame: NSRect(x: 0, y: 0, width: 640, height: 120))
        textView.delegate = context.coordinator
        textView.onFocus = onFocus
        textView.onFormatShortcut = { [weak controller] action in
            controller?.apply(action)
        }
        textView.isRichText = true
        textView.importsGraphics = false
        textView.usesFontPanel = false
        textView.usesRuler = false
        textView.isEditable = editable && !controller.decodeFailed
        textView.isSelectable = true
        textView.allowsUndo = true
        textView.drawsBackground = false
        textView.textContainerInset = NSSize(width: 13, height: 11)
        textView.isHorizontallyResizable = false
        textView.isVerticallyResizable = true
        textView.autoresizingMask = [.width]
        textView.minSize = NSSize(width: 0, height: 0)
        textView.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude,
                                  height: CGFloat.greatestFiniteMagnitude)
        textView.textContainer?.widthTracksTextView = true
        textView.textContainer?.containerSize = NSSize(width: 0, height: CGFloat.greatestFiniteMagnitude)
        textView.isAutomaticQuoteSubstitutionEnabled = false
        textView.isAutomaticDashSubstitutionEnabled = false
        textView.isAutomaticTextReplacementEnabled = false
        textView.isAutomaticSpellingCorrectionEnabled = false
        textView.isAutomaticLinkDetectionEnabled = false
        textView.isContinuousSpellCheckingEnabled = false
        textView.isGrammarCheckingEnabled = false
        textView.smartInsertDeleteEnabled = false
        textView.unregisterDraggedTypes()
        textView.setAccessibilityLabel(editable ? "Rich text editor" : "Read-only rich text")

        controller.textView = textView
        controller.rememberedSelection = NSRange(location: 0, length: 0)
        context.coordinator.install(textView)
        applySnapshot(textView, context: context, preserveSelection: false)
        scrollView.documentView = textView
        return scrollView
    }

    func updateNSView(_ scrollView: NSScrollView, context: Context) {
        context.coordinator.parent = self
        guard let textView = scrollView.documentView as? RichTextTextView else { return }
        textView.onFocus = onFocus
        textView.isEditable = editable && !controller.decodeFailed
        textView.setAccessibilityLabel(editable ? "Rich text editor" : "Read-only rich text")
        if context.coordinator.representedText != text ||
            context.coordinator.representedFormatting != formatting {
            // Never replace an in-progress IME composition with a SwiftUI
            // snapshot. NSTextView will deliver the final unmarked text after
            // the composition commits.
            if textView.hasMarkedText() { return }
            applySnapshot(textView, context: context, preserveSelection: true)
        }
    }

    func sizeThatFits(_ proposal: ProposedViewSize, nsView: NSScrollView, context: Context) -> CGSize? {
        let width = max(240, proposal.width ?? 640)
        guard let view = nsView.documentView as? NSTextView else { return CGSize(width: width, height: 160) }
        return CGSize(width: width, height: RichTextLayout.height(view.attributedString(), width: width))
    }

    private func applySnapshot(_ textView: RichTextTextView,
                               context: Context,
                               preserveSelection: Bool) {
        let oldSelection = textView.selectedRange()
        let undo = textView.undoManager
        // A true external replacement is a new Undo baseline. Local edits
        // update representedText/Formatting before SwiftUI echoes them, so
        // they never enter this path and keep their entire Undo stack.
        undo?.removeAllActions()
        textView.breakUndoCoalescing()
        undo?.disableUndoRegistration()
        defer { undo?.enableUndoRegistration() }
        do {
            let decoded = try RichTextCodec.decode(text: text, formatting: formatting)
            context.coordinator.applyingSnapshot = true
            textView.textStorage?.setAttributedString(decoded)
            context.coordinator.applyingSnapshot = false
            context.coordinator.reportCodecState(error: nil, decodeFailed: false, deferred: true)
            textView.isEditable = editable
        } catch {
            context.coordinator.applyingSnapshot = false
            // Keep the user's source visible even when persisted formatting is
            // bad.  A read-only view prevents an invalid snapshot from being
            // accidentally rewritten while the native layer reports the error.
            context.coordinator.applyingSnapshot = true
            textView.string = text
            context.coordinator.applyingSnapshot = false
            context.coordinator.reportCodecState(error: error.localizedDescription,
                                                 decodeFailed: true, deferred: true)
            textView.isEditable = false
        }
        let length = textView.textStorage?.length ?? 0
        if preserveSelection {
            let location = min(oldSelection.location, length)
            let remaining = max(0, length - location)
            textView.setSelectedRange(NSRange(location: location,
                                               length: min(oldSelection.length, remaining)))
        }
        context.coordinator.representedText = text
        context.coordinator.representedFormatting = formatting
    }

    final class Coordinator: NSObject, NSTextViewDelegate {
        var parent: RichTextEditorRepresentable
        var representedText = ""
        var representedFormatting = ""
        var applyingSnapshot = false
        var snapshotGeneration = 0
        weak var textView: RichTextTextView?

        init(_ parent: RichTextEditorRepresentable) { self.parent = parent }

        func install(_ textView: RichTextTextView) {
            self.textView = textView
            parent.controller.onAction = { [weak self] view in
                self?.emitSnapshot(from: view)
            }
            textView.onUndoRedo = { [weak self, weak textView] in
                guard let textView else { return }
                self?.emitSnapshot(from: textView)
            }
        }

        func textDidChange(_ notification: Notification) {
            guard let textView = notification.object as? RichTextTextView,
                  !applyingSnapshot,
                  !textView.hasMarkedText() else { return }
            emitSnapshot(from: textView)
        }

        func textViewDidChangeSelection(_ notification: Notification) {
            guard let textView = notification.object as? RichTextTextView else { return }
            parent.controller.rememberedSelection = textView.selectedRange()
        }

        func reportCodecState(error: String?, decodeFailed: Bool, deferred: Bool = false) {
            snapshotGeneration += 1
            let generation = snapshotGeneration
            let controller = parent.controller
            let callback = parent.onError
            let publish = { [weak self] in
                guard let self, self.snapshotGeneration == generation else { return }
                controller.decodeFailed = decodeFailed
                controller.error = error
                callback(error)
            }
            // SwiftUI updateNSView must not publish synchronously. User edits
            // must: the very next Save/Close must see any unencodable buffer.
            if deferred { DispatchQueue.main.async(execute: publish) }
            else { publish() }
        }

        func emitSnapshot(from textView: RichTextTextView) {
            guard !applyingSnapshot, !textView.hasMarkedText() else { return }
            do {
                let snapshot = try RichTextCodec.encode(textView.attributedString())
                representedText = snapshot.text
                representedFormatting = snapshot.formatting
                reportCodecState(error: nil, decodeFailed: false)
                parent.onChange(snapshot.text, snapshot.formatting)
            } catch {
                // Leave the native buffer untouched. This is especially
                // important for pasted/constructed attributed strings: an
                // error must not turn the user's input into a lossy string.
                reportCodecState(error: error.localizedDescription, decodeFailed: false)
            }
        }
    }
}

private enum RichTextEditorSupport {
    static func apply(_ action: RichTextEditorController.Action,
                      to textView: RichTextTextView,
                      selection: NSRange) {
        let storage = textView.textStorage
        guard textView.isEditable, !textView.hasMarkedText() else { return }
        textView.breakUndoCoalescing()
        let textLength = storage?.length ?? 0
        let safeSelection = NSIntersectionRange(selection,
                                                 NSRange(location: 0, length: textLength))
        if safeSelection.length == 0 {
            let attrs = attributesForTyping(action: action, textView: textView)
            let oldTypingAttributes = textView.typingAttributes
            var newTypingAttributes = oldTypingAttributes
            for key in [NSAttributedString.Key.font,
                        RichTextCodec.styleAttributeKey,
                        NSAttributedString.Key("com.amber.rich-text.bold"),
                        NSAttributedString.Key("com.amber.rich-text.italic"),
                        NSAttributedString.Key("com.amber.rich-text.underline")] {
                if let value = attrs[key] { newTypingAttributes[key] = value }
            }
            newTypingAttributes[.underlineStyle] = attrs[.underlineStyle] ?? 0
            textView.typingAttributes = newTypingAttributes
            textView.undoManager?.registerUndo(withTarget: textView) { target in
                restoreTypingAttributes(oldTypingAttributes, on: target)
            }
            textView.undoManager?.setActionName("Format Text")
            return
        }
        guard let storage else { return }
        let oldAttributes = storage.attributedSubstring(from: safeSelection)
        var ranges: [NSRange] = []
        storage.enumerateAttributes(in: safeSelection, options: []) { _, range, _ in
            ranges.append(range)
        }
        storage.beginEditing()
        for range in ranges {
            let existing = storage.attributes(at: range.location, effectiveRange: nil)
            let descriptor = try? RichTextCodecDescriptor.from(existing)
            let current = descriptor ?? RichTextCodecDescriptor(style: .body,
                                                                  bold: false,
                                                                  italic: false,
                                                                  underline: false)
            let next: RichTextCodecDescriptor
            switch action {
            case .style(let style): next = RichTextCodecDescriptor(style: style, bold: current.bold,
                                                                    italic: current.italic, underline: current.underline)
            case .bold: next = RichTextCodecDescriptor(style: current.style, bold: !current.bold,
                                                       italic: current.italic, underline: current.underline)
            case .italic: next = RichTextCodecDescriptor(style: current.style, bold: current.bold,
                                                         italic: !current.italic, underline: current.underline)
            case .underline: next = RichTextCodecDescriptor(style: current.style, bold: current.bold,
                                                             italic: current.italic, underline: !current.underline)
            }
            let attrs = RichTextCodec.attributes(style: next.style,
                                                 bold: next.bold,
                                                 italic: next.italic,
                                                 underline: next.underline)
            // Inline marks must retain their owning paragraph/list/table.
            var preserved = existing
            for (key, value) in attrs { preserved[key] = value }
            preserved[.underlineStyle] = attrs[.underlineStyle] ?? 0
            storage.setAttributes(preserved, range: range)
        }
        storage.endEditing()
        textView.undoManager?.registerUndo(withTarget: textView) { target in
            restoreAttributes(oldAttributes, in: safeSelection, selection: selection, on: target)
        }
        textView.undoManager?.setActionName("Format Text")
        textView.didChangeText()
    }

    private static func restoreAttributes(_ snapshot: NSAttributedString,
                                          in range: NSRange,
                                          selection: NSRange,
                                          on textView: NSTextView) {
        guard let storage = textView.textStorage,
              NSMaxRange(range) <= storage.length else { return }
        let redoSnapshot = storage.attributedSubstring(from: range)
        textView.undoManager?.registerUndo(withTarget: textView) { target in
            restoreAttributes(redoSnapshot, in: range, selection: selection, on: target)
        }
        storage.beginEditing()
        snapshot.enumerateAttributes(in: NSRange(location: 0, length: snapshot.length), options: []) {
            attrs, subrange, _ in
            let targetRange = NSRange(location: range.location + subrange.location,
                                      length: subrange.length)
            storage.setAttributes(attrs, range: targetRange)
        }
        storage.endEditing()
        let location = min(selection.location, storage.length)
        let length = min(selection.length, storage.length - location)
        textView.setSelectedRange(NSRange(location: location, length: length))
        textView.didChangeText()
    }

    private static func restoreTypingAttributes(_ snapshot: [NSAttributedString.Key: Any],
                                                on textView: NSTextView) {
        let redoAttributes = textView.typingAttributes
        textView.undoManager?.registerUndo(withTarget: textView) { target in
            restoreTypingAttributes(redoAttributes, on: target)
        }
        textView.typingAttributes = snapshot
    }

    private static func attributesForTyping(action: RichTextEditorController.Action,
                                            textView: RichTextTextView) -> [NSAttributedString.Key: Any] {
        let current = (try? RichTextCodecDescriptor.from(textView.typingAttributes)) ??
            RichTextCodecDescriptor(style: .body, bold: false, italic: false, underline: false)
        let next: RichTextCodecDescriptor
        switch action {
        case .style(let style): next = RichTextCodecDescriptor(style: style, bold: current.bold,
                                                                italic: current.italic, underline: current.underline)
        case .bold: next = RichTextCodecDescriptor(style: current.style, bold: !current.bold,
                                                   italic: current.italic, underline: current.underline)
        case .italic: next = RichTextCodecDescriptor(style: current.style, bold: current.bold,
                                                     italic: !current.italic, underline: current.underline)
        case .underline: next = RichTextCodecDescriptor(style: current.style, bold: current.bold,
                                                         italic: current.italic, underline: !current.underline)
        }
        return RichTextCodec.attributes(style: next.style, bold: next.bold,
                                        italic: next.italic, underline: next.underline)
    }
}

// Small, framework-only hooks keep the native undo behavior testable without
// exposing the editor's AppKit implementation to the notebook model. They are
// intentionally useful only to the standalone macOS regression executable.
enum RichTextEditorTestHooks {
    static func makeTextView(text: String) -> NSTextView {
        let textView = RichTextTextView(frame: NSRect(x: 0, y: 0, width: 640, height: 120))
        textView.isRichText = true
        textView.allowsUndo = true
        if let value = try? RichTextCodec.decode(text: text, formatting: "") {
            textView.textStorage?.setAttributedString(value)
        }
        return textView
    }

    static func applyHeading1(to textView: NSTextView) {
        guard let richTextView = textView as? RichTextTextView else { return }
        let range = NSRange(location: 0, length: richTextView.textStorage?.length ?? 0)
        RichTextEditorSupport.apply(.style(.heading1), to: richTextView, selection: range)
    }

    static func applyBold(to textView: NSTextView) {
        guard let richTextView = textView as? RichTextTextView else { return }
        let range = NSRange(location: 0, length: richTextView.textStorage?.length ?? 0)
        RichTextEditorSupport.apply(.bold, to: richTextView, selection: range)
    }
}

private struct RichTextCodecDescriptor {
    let style: RichTextCodec.Style
    let bold: Bool
    let italic: Bool
    let underline: Bool

    static func from(_ attrs: [NSAttributedString.Key: Any]) throws -> RichTextCodecDescriptor {
        let font = attrs[.font] as? NSFont
        let style: RichTextCodec.Style
        if let rawStyle = attrs[RichTextCodec.styleAttributeKey] as? String,
           let explicitStyle = RichTextCodec.Style(rawValue: rawStyle) {
            style = explicitStyle
        } else if let font {
            let traits = font.fontDescriptor.symbolicTraits
            if traits.contains(.monoSpace) {
                style = .code
            } else if font.pointSize >= 23 {
                style = .heading1
            } else if font.pointSize >= 19 {
                style = .heading2
            } else if font.pointSize >= 16 {
                style = .heading3
            } else {
                style = .body
            }
        } else {
            style = .body
        }
        let fontTraits = font?.fontDescriptor.symbolicTraits ?? []
        let bold = (attrs[NSAttributedString.Key("com.amber.rich-text.bold")] as? Bool) ??
            fontTraits.contains(.bold)
        let italic = (attrs[NSAttributedString.Key("com.amber.rich-text.italic")] as? Bool) ??
            fontTraits.contains(.italic)
        let underline = (attrs[NSAttributedString.Key("com.amber.rich-text.underline")] as? Bool) ??
            (((attrs[.underlineStyle] as? NSNumber)?.intValue ?? 0) != 0)
        return RichTextCodecDescriptor(style: style, bold: bold, italic: italic, underline: underline)
    }
}

final class RichTextTextView: NSTextView {
    var isStructuralEdit = false
    var onFocus: () -> Void = {}
    var onFormatShortcut: ((RichTextEditorController.Action) -> Void)?
    var onUndoRedo: (() -> Void)?

    override func insertText(_ insertString: Any, replacementRange: NSRange) {
        guard !isStructuralEdit, isEditable else { return }
        let range = replacementRange.location == NSNotFound ? selectedRange() : replacementRange
        if !hasMarkedText() && RichTextEditing.shouldManage(range, in: self) {
            let string = (insertString as? NSAttributedString)?.string ?? (insertString as? String) ?? ""
            RichTextEditing.insert(string, replacing: range, in: self)
        } else { super.insertText(insertString, replacementRange: replacementRange) }
    }

    override func insertNewline(_ sender: Any?) {
        guard isEditable else { return }
        if hasMarkedText() || !RichTextEditing.newline(in: self) { super.insertNewline(sender) }
    }
    override func insertTab(_ sender: Any?) {
        if !RichTextEditing.tab(backward: false, in: self) { super.insertTab(sender) }
    }
    override func insertBacktab(_ sender: Any?) {
        if !RichTextEditing.tab(backward: true, in: self) { super.insertBacktab(sender) }
    }
    override func deleteBackward(_ sender: Any?) {
        guard isEditable else { return }
        if hasMarkedText() || !RichTextEditing.delete(backward: true, in: self) { super.deleteBackward(sender) }
    }
    override func deleteForward(_ sender: Any?) {
        guard isEditable else { return }
        if hasMarkedText() || !RichTextEditing.delete(backward: false, in: self) { super.deleteForward(sender) }
    }
    override func cut(_ sender: Any?) {
        guard isEditable else { return }
        if RichTextEditing.shouldManage(selectedRange(), in: self) {
            copy(sender)
            RichTextEditing.insert("", replacing: selectedRange(), in: self)
        } else { super.cut(sender) }
    }

    override func becomeFirstResponder() -> Bool {
        let result = super.becomeFirstResponder()
        if result { onFocus() }
        return result
    }

    @objc func undo(_ sender: Any?) {
        guard isEditable, !hasMarkedText() else { return }
        undoManager?.undo()
        onUndoRedo?()
    }

    @objc func redo(_ sender: Any?) {
        guard isEditable, !hasMarkedText() else { return }
        undoManager?.redo()
        onUndoRedo?()
    }

    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        guard isEditable, !hasMarkedText() else { return super.performKeyEquivalent(with: event) }
        let flags = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        guard flags.contains(.command), !flags.contains(.option), !flags.contains(.control),
              let character = event.charactersIgnoringModifiers?.lowercased() else {
            return super.performKeyEquivalent(with: event)
        }
        switch character {
        case "b": onFormatShortcut?(.bold); return true
        case "i": onFormatShortcut?(.italic); return true
        case "u": onFormatShortcut?(.underline); return true
        default: return super.performKeyEquivalent(with: event)
        }
    }

    // Reading only the plain-string pasteboard type guarantees that HTML,
    // RTF, attachments, and URLs never enter the portable notebook document.
    override func paste(_ sender: Any?) {
        guard isEditable,
              let text = NSPasteboard.general.string(forType: .string) else { return }
        insertText(text, replacementRange: selectedRange())
    }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        // Drag-and-drop can carry file URLs and promised attachments. The
        // notebook editor accepts only explicit text paste.
        return false
    }
}
