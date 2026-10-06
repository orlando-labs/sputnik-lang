import AppKit
import Foundation

private struct RichTextTestFailure: Error, CustomStringConvertible {
    let description: String
}

@main
private struct NotebookMacOSRichTextTests {
    @MainActor
    static func main() {
        let application = NSApplication.shared
        application.setActivationPolicy(.prohibited)
        do {
            try testUnicodeAndPlainText()
            try testFormattingRoundTrip()
            try testInvalidRangesAndVersions()
            try testStyleOnlyFormatting()
            try testArbitraryAttributesAreSanitized()
            try testNativeStyleUndoRedo()
            try testTypingStylesAndEditorGuards()
            try testLongPlainTextAndSplitAttributes()
            try testParagraphHeadingAtCaret()
            try testStructuredLists()
            try testNativeTables()
            try testInterpolation()
            try testTableBoundaryProtection()
            try testStructuralEdgeCases()
            print("notebook macOS rich-text tests passed")
        } catch {
            fputs("notebook macOS rich-text tests failed: \(error)\n", stderr)
            exit(1)
        }
    }

    private static func testUnicodeAndPlainText() throws {
        let source = "Café 👩🏽‍💻 — plain\n第二行"
        let decoded = try RichTextCodec.decode(text: source, formatting: "")
        try require(decoded.string == source, "plain Unicode text must survive decode")
        let encoded = try RichTextCodec.encode(decoded)
        try require(encoded.text == source, "plain Unicode text must survive encode")
        try require(encoded.formatting.isEmpty, "plain text should have empty formatting")
        try require((source as NSString).length > source.count,
                    "the fixture must exercise UTF-16 offsets")
    }

    private static func testFormattingRoundTrip() throws {
        let source = "A🙂B — heading and code"
        // A is UTF-16 offset 0, the emoji occupies offsets 1...2, and B is
        // offset 3. The heading run deliberately begins and ends on scalar
        // boundaries around the surrogate pair.
        let formatting = "{\"version\":1,\"runs\":[{\"start\":1,\"length\":2,\"style\":\"heading1\",\"bold\":false,\"italic\":true,\"underline\":false},{\"start\":6,\"length\":4,\"style\":\"code\",\"bold\":false,\"italic\":false,\"underline\":true}]}"
        let decoded = try RichTextCodec.decode(text: source, formatting: formatting)
        let encoded = try RichTextCodec.encode(decoded)
        try require(encoded.text == source, "formatted source must survive round-trip")
        try require(encoded.formatting == formatting,
                    "portable formatting should round-trip deterministically")

        let second = try RichTextCodec.decode(text: encoded.text, formatting: encoded.formatting)
        let secondEncoding = try RichTextCodec.encode(second)
        try require(secondEncoding.formatting == formatting,
                    "a second codec pass must not drift formatting")
    }

    private static func testInvalidRangesAndVersions() throws {
        try requireThrows(
            try RichTextCodec.decode(text: "🙂", formatting: "{\"version\":3,\"runs\":[]}"),
            "unsupported versions must be rejected") { error in
                if case .unsupportedVersion(3) = error { return true }
                return false
            }
        try requireThrows(
            try RichTextCodec.decode(text: "🙂", formatting: "{\"version\":1,\"runs\":[{\"start\":1,\"length\":1,\"style\":\"body\"}]}"),
            "ranges inside surrogate pairs must be rejected") { error in
                if case .nonScalarBoundary = error { return true }
                return false
            }
        try requireThrows(
            try RichTextCodec.decode(text: "abcd", formatting: "{\"version\":1,\"runs\":[{\"start\":0,\"length\":3,\"style\":\"body\"},{\"start\":2,\"length\":1,\"style\":\"code\"}]}"),
            "overlapping ranges must be rejected") { error in
                if case .overlappingRanges = error { return true }
                return false
            }
        try requireThrows(
            try RichTextCodec.decode(text: "abc", formatting: "{\"version\":1,\"runs\":[],\"future\":true}"),
            "unknown persisted fields must not be silently discarded") { error in
                if case .invalidSchema = error { return true }
                return false
            }
        try requireThrows(
            try RichTextCodec.decode(text: "abc", formatting: "{\"version\":1,\"runs\":[{\"start\":9223372036854775808,\"length\":1}]}"),
            "out-of-range integer offsets must be rejected without trapping") { error in
                if case .invalidSchema = error { return true }
                return false
            }
        let defaultStyle = try RichTextCodec.decode(
            text: "abc",
            formatting: "{\"version\":1,\"runs\":[{\"start\":0,\"length\":3,\"bold\":true}]}"
        )
        let defaultStyleEncoding = try RichTextCodec.encode(defaultStyle)
        try require(defaultStyleEncoding.formatting.contains("\"style\":\"body\""),
                    "an omitted style must default to body")
    }

    private static func testStyleOnlyFormatting() throws {
        let body = try RichTextCodec.decode(text: "same", formatting: "")
        let styled = try RichTextCodec.decode(
            text: "same",
            formatting: "{\"version\":1,\"runs\":[{\"start\":0,\"length\":4,\"style\":\"heading2\",\"bold\":false,\"italic\":false,\"underline\":false}]}"
        )
        let bodyEncoding = try RichTextCodec.encode(body)
        let styledEncoding = try RichTextCodec.encode(styled)
        try require(bodyEncoding.text == styledEncoding.text,
                    "style-only edits must keep plaintext unchanged")
        try require(bodyEncoding.formatting != styledEncoding.formatting,
                    "style-only edits must still emit formatting")
    }

    private static func testArbitraryAttributesAreSanitized() throws {
        let value = NSMutableAttributedString(string: "safe")
        value.addAttribute(.foregroundColor, value: NSColor.red,
                           range: NSRange(location: 0, length: value.length))
        let encoded = try RichTextCodec.encode(value)
        try require(encoded.text == "safe" && encoded.formatting.isEmpty,
                    "unsupported visual attributes should not leak into persistence")

        value.addAttribute(.link, value: URL(string: "https://example.com")!,
                           range: NSRange(location: 0, length: value.length))
        try requireThrows(try RichTextCodec.encode(value),
                          "links must never be persisted") { error in
            if case .unsupportedContent("links") = error { return true }
            return false
        }
    }

    private static func testNativeStyleUndoRedo() throws {
        let fixture = try NativeEditorFixture(text: "undo me")
        let textView = fixture.view
        textView.setSelectedRange(NSRange(location: 0, length: 7))
        fixture.apply(.style(.heading1))
        let formatted = try RichTextCodec.encode(textView.attributedString())
        try require(formatted.formatting.contains("\"style\":\"heading1\""),
                    "native heading action should serialize formatting")
        try require(fixture.lastFormatting == formatted.formatting,
                    "a style-only edit must reach the model callback")

        textView.undoManager?.undo()
        let undone = try RichTextCodec.encode(textView.attributedString())
        try require(undone.formatting.isEmpty && fixture.lastFormatting == "",
                    "native undo must restore plain formatting and notify the model")

        textView.undoManager?.redo()
        let redone = try RichTextCodec.encode(textView.attributedString())
        try require(redone.formatting == formatted.formatting,
                    "native redo should restore the exact formatting")

        // A second undo/redo cycle verifies that each inverse action registers
        // its own inverse rather than working only for the first cycle.
        textView.undoManager?.undo()
        let secondUndo = try RichTextCodec.encode(textView.attributedString())
        try require(secondUndo.formatting.isEmpty,
                    "second native undo should still restore plain formatting")
        textView.undoManager?.redo()
        let secondRedo = try RichTextCodec.encode(textView.attributedString())
        try require(secondRedo.formatting == formatted.formatting && fixture.lastFormatting == formatted.formatting,
                    "second native redo should still restore heading formatting")
        try require(textView.string == "undo me" && textView.selectedRange() == NSRange(location: 0, length: 7),
                    "format and undo must preserve source and selection")
    }

    private static func testTypingStylesAndEditorGuards() throws {
        let fixture = try NativeEditorFixture(text: "typing")
        fixture.view.setSelectedRange(NSRange(location: 6, length: 0))
        fixture.apply(.underline)
        try require((fixture.view.typingAttributes[.underlineStyle] as? NSNumber)?.intValue == 1,
                    "underline should apply to subsequent typing")
        fixture.apply(.underline)
        try require((fixture.view.typingAttributes[.underlineStyle] as? NSNumber)?.intValue == 0,
                    "turning underline off must remove the visible typing attribute")
        for _ in 0..<2 {
            fixture.view.undoManager?.undo()
            try require((fixture.view.typingAttributes[.underlineStyle] as? NSNumber)?.intValue == 1,
                        "typing-style undo should work repeatedly")
            fixture.view.undoManager?.redo()
            try require((fixture.view.typingAttributes[.underlineStyle] as? NSNumber)?.intValue == 0,
                        "typing-style redo should work repeatedly")
        }
        fixture.view.isEditable = false
        fixture.view.setSelectedRange(NSRange(location: 0, length: 6))
        fixture.apply(.bold)
        let readOnly = try RichTextCodec.encode(fixture.view.attributedString())
        try require(readOnly.formatting.isEmpty, "keyboard formatting must respect a busy/read-only editor")
        fixture.view.isEditable = true
        fixture.view.undoManager?.beginUndoGrouping()
        fixture.view.setMarkedText("かな", selectedRange: NSRange(location: 2, length: 0),
                                   replacementRange: NSRange(location: 0, length: 0))
        fixture.view.undoManager?.endUndoGrouping()
        let marked = fixture.view.attributedString().copy() as! NSAttributedString
        fixture.controller.apply(.italic)
        try require(fixture.view.attributedString().isEqual(to: marked),
                    "format actions must leave IME-owned attributes untouched")
        fixture.view.unmarkText()
    }

    private static func testLongPlainTextAndSplitAttributes() throws {
        let text = String(repeating: "ab🙂", count: 25_000)
        let plain = try RichTextCodec.encode(RichTextCodec.decode(text: text, formatting: ""))
        try require(plain.text == text && plain.formatting.isEmpty,
                    "20,000 is a run limit, not a text length limit")
        let split = NSMutableAttributedString(string: "🙂")
        split.addAttribute(.underlineStyle, value: 1, range: NSRange(location: 1, length: 1))
        try requireThrows(try RichTextCodec.encode(split), "split-surrogate native attributes must not be silently discarded") {
            if case .nonScalarBoundary = $0 { return true }
            return false
        }
    }

    private static func testParagraphHeadingAtCaret() throws {
        let structuralHeading = try RichTextCodec.decode(text: "Heading\n", formatting:
            "{\"version\":2,\"runs\":[],\"paragraphs\":[{\"start\":0,\"length\":8,\"style\":\"heading1\"}]}")
        try require((structuralHeading.attribute(.font, at: 0, effectiveRange: nil) as? NSFont)?.pointSize == 24,
                    "paragraph metadata alone must render a heading without redundant inline runs")
        let fixture = try NativeEditorFixture(text: "first paragraph\nsecond paragraph\n")
        fixture.view.setSelectedRange(NSRange(location: 4, length: 0))
        fixture.apply(.style(.heading1))
        try require((fixture.view.textStorage?.attribute(.font, at: 0, effectiveRange: nil) as? NSFont)?.pointSize == 24,
                    "heading at a caret must style the existing paragraph, not just future typing")
        try require((fixture.view.textStorage?.attribute(.font, at: 17, effectiveRange: nil) as? NSFont)?.pointSize == 14,
                    "heading must leave the next paragraph alone")
        fixture.view.setSelectedRange(NSRange(location: 15, length: 0))
        fixture.view.insertNewline(nil)
        fixture.view.undoManager?.beginUndoGrouping()
        fixture.view.insertText("body", replacementRange: fixture.view.selectedRange())
        fixture.view.undoManager?.endUndoGrouping()
        let encoded = try RichTextCodec.encode(fixture.view.attributedString())
        let decoded = try RichTextCodec.decode(text: encoded.text, formatting: encoded.formatting)
        try require((decoded.attribute(.font, at: 16, effectiveRange: nil) as? NSFont)?.pointSize == 14,
                    "Return at the end of a heading must start body text")
    }

    private static func testStructuredLists() throws {
        let fixture = try NativeEditorFixture(text: "alpha\nbeta\n")
        fixture.view.setSelectedRange(NSRange(location: 0, length: 10))
        fixture.controller.rememberedSelection = fixture.view.selectedRange()
        fixture.controller.list(.numbered)
        try require(fixture.view.string == "1.\talpha\n2.\tbeta\n", "numbered list must transform whole paragraphs")
        let first = try RichTextCodec.encode(fixture.view.attributedString())
        let reopened = try RichTextCodec.decode(text: first.text, formatting: first.formatting)
        let encodedAgain = try RichTextCodec.encode(reopened)
        try require(first.formatting == encodedAgain.formatting, "list metadata must roundtrip")
        fixture.view.undoManager?.undo()
        try require(fixture.view.string == "alpha\nbeta\n", "list transform is one undo step")
        fixture.view.undoManager?.redo()
        fixture.view.setSelectedRange(NSRange(location: 8, length: 0))
        fixture.view.insertNewline(nil)
        try require(fixture.view.string == "1.\talpha\n2.\t\n3.\tbeta\n", "Return inserts a list item and renumbers downstream")
        fixture.view.insertText("new", replacementRange: fixture.view.selectedRange())
        fixture.view.insertNewline(nil)
        fixture.view.insertNewline(nil)
        try require(!fixture.view.string.contains("• •") && fixture.view.typingAttributes[RichTextStructure.listKey] == nil,
                    "Return on an empty item exits the list")
        _ = try RichTextCodec.encode(fixture.view.attributedString())
        let bullets = try NativeEditorFixture(text: "item")
        bullets.view.setSelectedRange(NSRange(location: 2, length: 0))
        bullets.controller.rememberedSelection = bullets.view.selectedRange()
        bullets.controller.list(.bullet)
        try require(bullets.view.string == "•\titem", "bullet is placed at paragraph start, never at caret")
        bullets.controller.list(.bullet)
        try require(bullets.view.string == "item", "clicking bullet again removes list instead of stacking markers")
    }

    private static func testNativeTables() throws {
        let fixture = try NativeEditorFixture(text: "Intro\n")
        fixture.view.setSelectedRange(NSRange(location: 6, length: 0))
        fixture.controller.rememberedSelection = fixture.view.selectedRange()
        fixture.controller.insertTable(rows: 2, columns: 2)
        try require(RichTextStructure.tables(in: fixture.view.attributedString()).first?.cells.count == 4,
                    "insert table creates four real NSTextTableBlock cells")
        fixture.view.insertText("first", replacementRange: fixture.view.selectedRange())
        fixture.view.insertTab(nil)
        fixture.view.insertText("second", replacementRange: fixture.view.selectedRange())
        fixture.controller.rememberedSelection = fixture.view.selectedRange()
        fixture.controller.table(.addRow)
        fixture.controller.table(.addColumn)
        let tables = RichTextStructure.tables(in: fixture.view.attributedString())
        try require(tables.first?.cells.count == 9 && tables.first?.object.numberOfColumns == 3,
                    "row/column actions preserve a rectangular editable table")
        let encoded = try RichTextCodec.encode(fixture.view.attributedString())
        let reopened = try RichTextCodec.decode(text: encoded.text, formatting: encoded.formatting)
        try require(reopened.string.contains("first") && reopened.string.contains("second") &&
                    RichTextStructure.tables(in: reopened).first?.cells.count == 9,
                    "table content and structure survive reopen")
        let again = try RichTextCodec.encode(reopened)
        try require(encoded.formatting == again.formatting, "native table schema is deterministic")
        fixture.controller.table(.removeTable)
        try require(RichTextStructure.tables(in: fixture.view.attributedString()).isEmpty, "remove table works")
        fixture.view.undoManager?.undo()
        try require(RichTextStructure.tables(in: fixture.view.attributedString()).first?.cells.count == 9,
                    "undo table deletion restores all cells")
    }

    private static func testInterpolation() throws {
        let variables = [NotebookTextVariable(name: "amount", value: "42", cellID: "1", stale: false),
                         NotebookTextVariable(name: "greeting", value: "Привет 👋\nAmber {{amount}}", cellID: "1", stale: false),
                         NotebookTextVariable(name: "old", value: "7", cellID: "2", stale: true)]
        let template = "Total: {{ amount }}; {{greeting}}; {{missing}}; {{old}}; {{amount + 1}}"
        let source = try RichTextCodec.decode(text: template, formatting: "")
        let rendered = RichTextInterpolation.render(source, variables: variables)
        try require(rendered.string == "Total: 42; Привет 👋\u{2028}Amber {{amount}}; [missing: unavailable]; [old: stale — 7]; [amount + 1: unavailable]",
                    "interpolation substitutes names once, preserves Unicode and flags missing/stale values; never evaluates expressions")
        try require(source.string == template, "rendering never changes persisted placeholders")
        try require(RichTextInterpolation.matching(variables, query: " AMO ").map(\.name) == ["amount"],
                    "picker searches variable names case-insensitively")
        let fixture = try NativeEditorFixture(text: "Total: ")
        fixture.view.setSelectedRange(NSRange(location: 7, length: 0))
        fixture.controller.rememberedSelection = fixture.view.selectedRange()
        fixture.controller.insertVariable("amount")
        try require(fixture.view.string == "Total: {{amount}}", "picker inserts an editable placeholder at remembered caret")
        fixture.view.undoManager?.undo()
        try require(fixture.view.string == "Total: ", "variable insertion is one undoable action")
        fixture.view.undoManager?.redo()
        try require(fixture.view.string == "Total: {{amount}}", "variable insertion supports redo")

        let table = try NativeEditorFixture(text: "")
        table.controller.insertTable(rows: 1, columns: 2)
        table.controller.insertVariable("greeting")
        let preview = RichTextInterpolation.render(table.view.attributedString(), variables: variables)
        try require(RichTextStructure.tables(in: preview).first?.cells.count == 2,
                    "interpolation preserves real table cells even for multiline values")
        _ = try RichTextCodec.encode(table.view.attributedString())
    }

    private static func testTableBoundaryProtection() throws {
        let fixture = try NativeEditorFixture(text: "")
        fixture.controller.insertTable(rows: 1, columns: 2)
        let original = fixture.view.string
        fixture.view.deleteForward(nil)
        try require(fixture.view.string == original, "Delete must not merge table cell separators")
        fixture.view.insertText("a", replacementRange: fixture.view.selectedRange())
        fixture.view.insertNewline(nil)
        fixture.view.insertText("b", replacementRange: fixture.view.selectedRange())
        try require(fixture.view.string.hasPrefix("a\u{2028}b\n"), "Return inside a cell inserts an in-cell line break")
        fixture.view.insertTab(nil)
        fixture.view.insertBacktab(nil)
        try require(fixture.view.selectedRange().location == 0, "Shift-Tab returns to previous table cell")
        fixture.controller.rememberedSelection = fixture.view.selectedRange()
        fixture.controller.table(.removeColumn)
        try require(RichTextStructure.tables(in: fixture.view.attributedString()).first?.cells.count == 1, "remove column keeps remaining cells")
        fixture.controller.table(.removeRow)
        try require(RichTextStructure.tables(in: fixture.view.attributedString()).isEmpty, "removing last row removes empty table safely")
    }

    private static func testStructuralEdgeCases() throws {
        for kind in [RichTextStructure.ListKind.bullet, .numbered] {
            let fixture = try NativeEditorFixture(text: "alpha\nbeta\n")
            fixture.view.setSelectedRange(NSRange(location: 0, length: 10))
            fixture.controller.rememberedSelection = fixture.view.selectedRange()
            fixture.controller.list(kind)
            let prefix = kind == .bullet ? "•\t" : "1.\t"
            fixture.view.setSelectedRange(NSRange(location: prefix.utf16.count + 5, length: 0))
            fixture.view.deleteForward(nil)
            try require(fixture.view.string == prefix + "alphabeta\n", "joining list items must remove the right marker")
            fixture.view.undoManager?.undo()
            fixture.view.setSelectedRange(NSRange(location: 0, length: 0))
            fixture.view.insertNewline(nil)
            let second = kind == .bullet ? "•\t" : "2.\t"
            try require(fixture.view.string.hasPrefix(prefix + "\n" + second + "alpha\n"),
                        "Return before marker splits content, never duplicates markers inside an item")
            fixture.view.undoManager?.undo()
            fixture.view.setSelectedRange(NSRange(location: 0, length: 0))
            fixture.view.insertText("Z", replacementRange: fixture.view.selectedRange())
            try require(fixture.view.string.hasPrefix(prefix + "Zalpha"), "typing before marker is placed after the marker")
            _ = try RichTextCodec.encode(fixture.view.attributedString())
        }
        let table = try NativeEditorFixture(text: "Intro\n")
        table.view.setSelectedRange(NSRange(location: 6, length: 0))
        table.controller.rememberedSelection = table.view.selectedRange()
        table.controller.insertTable(rows: 2, columns: 2)
        table.view.setSelectedRange(NSRange(location: 5, length: 0))
        let before = table.view.string
        table.view.deleteForward(nil)
        try require(table.view.string == before, "outside prose must not merge with first table cell")
        let range = RichTextStructure.tables(in: table.view.attributedString())[0].range
        table.view.setSelectedRange(range)
        table.view.insertText("Replacement", replacementRange: range)
        try require(RichTextStructure.tables(in: table.view.attributedString()).isEmpty,
                    "whole-table replacement drops cell ownership")
        _ = try RichTextCodec.encode(table.view.attributedString())
        table.view.undoManager?.undo()
        try require(RichTextStructure.tables(in: table.view.attributedString()).first?.cells.count == 4,
                    "undo whole-table replacement restores original structure")
        table.controller.rememberedSelection = NSRange(location: table.view.string.utf16.count, length: 999)
        table.controller.insertVariable("x")
        try require(table.view.string.hasSuffix("{{x}}"), "toolbar clamps a stale selection length before editing")
    }

    private static func require(_ condition: @autoclosure () -> Bool,
                                _ message: String) throws {
        guard condition() else { throw RichTextTestFailure(description: message) }
    }

    private static func requireThrows<T>(_ operation: @autoclosure () throws -> T,
                                         _ message: String,
                                         matches: (RichTextCodecError) -> Bool) throws {
        do {
            _ = try operation()
        } catch let error as RichTextCodecError {
            try require(matches(error), message + " (wrong error: \(error))")
            return
        } catch {
            throw RichTextTestFailure(description: message + " (wrong error type: \(error))")
        }
        throw RichTextTestFailure(description: message + " (no error)")
    }
}

// Hidden native window, real NSTextView and the same coordinator as production.
// No process-global pasteboard or user document is modified by these tests.
private final class NativeEditorFixture {
    let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 500, height: 180),
                          styleMask: [.titled], backing: .buffered, defer: false)
    let view = RichTextTextView(frame: NSRect(x: 0, y: 0, width: 500, height: 180))
    let controller = RichTextEditorController(onChange: { _, _ in }, onFocus: {})
    var coordinator: RichTextEditorRepresentable.Coordinator!
    var lastFormatting: String?

    init(text: String) throws {
        window.isReleasedWhenClosed = false
        window.contentView = view
        view.isEditable = true
        view.isRichText = true
        view.allowsUndo = true
        view.textStorage?.setAttributedString(try RichTextCodec.decode(text: text, formatting: ""))
        view.typingAttributes = RichTextCodec.attributes(style: .body, bold: false, italic: false, underline: false)
        controller.textView = view
        let editor = RichTextEditorRepresentable(text: text, formatting: "", editable: true, controller: controller,
            onChange: { [weak self] _, formatting in self?.lastFormatting = formatting }, onFocus: {}, onError: { _ in })
        coordinator = editor.makeCoordinator()
        coordinator.install(view)
        view.delegate = coordinator
        window.makeFirstResponder(view)
        guard let undo = view.undoManager else { throw RichTextTestFailure(description: "native editor must have UndoManager") }
        undo.removeAllActions()
        undo.groupsByEvent = false
    }

    func apply(_ action: RichTextEditorController.Action) {
        controller.rememberedSelection = view.selectedRange()
        view.undoManager?.beginUndoGrouping()
        controller.apply(action)
        view.undoManager?.endUndoGrouping()
    }
}
