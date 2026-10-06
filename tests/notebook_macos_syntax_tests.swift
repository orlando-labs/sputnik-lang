import AppKit
import Foundation
import SwiftUI

private struct SyntaxTestError: Error, CustomStringConvertible {
    let description: String
}

@main
private enum NotebookMacOSSyntaxTests {
    static func main() throws {
        try unicodeRangesAndNumbers()
        try commentsDoNotEatCode()
        try stringsWinOverKeywordsAndComments()
        try macroSplicesUseAmberHashRules()
        try applyingColorPreservesText()
        try editorCoordinatorPreservesSelectionUndoAndIME()
        print("notebook macOS syntax tests passed")
    }

    private static func unicodeRangesAndNumbers() throws {
        let source = "π = 0x2A + 1.5e+2\nЖар = true\n"
        let tokens = AmberSyntax.tokens(in: source)
        let pi = try token("π", in: tokens, category: .identifier)
        require(pi.range == NSRange(location: 0, length: 1),
                "Greek identifier must use its UTF-16 range")
        _ = try token("0x2A", in: tokens, category: .number)
        _ = try token("1.5e+2", in: tokens, category: .number)
        _ = try token("Жар", in: tokens, category: .className)
        _ = try token("true", in: tokens, category: .constant)

        let astralSource = "😀 + π = 1"
        let astralTokens = AmberSyntax.tokens(in: astralSource)
        let astralPi = try token("π", in: astralTokens, category: .identifier)
        require(astralPi.range == NSRange(location: 5, length: 1),
                "an astral scalar before a token must count as two UTF-16 units")

        let incomplete = AmberSyntax.tokens(in: "value = \"unterminated if # true")
        let literal = try token("\"unterminated if # true", in: incomplete, category: .string)
        require(literal.range.location == 8,
                "an incomplete string should still begin at its delimiter")
        require(!incomplete.contains { $0.category == .keyword || $0.category == .comment },
                "incomplete strings must not leak keyword/comment highlighting")
    }

    private static func commentsDoNotEatCode() throws {
        let source = "value = 1\n# if true\nvalue = 2 #{outside}\ncall#method\n"
        let tokens = AmberSyntax.tokens(in: source)
        let comment = try token("# if true", in: tokens, category: .comment)
        require(comment.text == "# if true", "comment should end at the physical newline")
        require(!tokens.contains { $0.category == .keyword && $0.text == "if" },
                "keywords inside comments must not be highlighted")
        require(!tokens.contains { $0.category == .comment && $0.text.contains("outside") == false && $0.text.contains("value") },
                "a preceding identifier must keep #method out of comment mode")
    }

    private static func stringsWinOverKeywordsAndComments() throws {
        let source = "single = 'if # false \\'quote\\''\n" +
            "double = \"if # true \\\"quote\\\"\"\n" +
            "raw = r\"if #{not_interpolated} # not a comment\"\n" +
            "block = \"\"\"\n  if true\n\"\"\"\n"
        let tokens = AmberSyntax.tokens(in: source)
        require(tokens.filter { $0.category == .string }.count == 4,
                "single, double, raw, and triple strings should each be one token")
        require(tokens.filter { $0.category == .keyword }.isEmpty,
                "keywords inside strings must not leak into the token stream")
        let raw = try token("r\"if #{not_interpolated} # not a comment\"",
                            in: tokens, category: .string)
        require(raw.text.hasPrefix("r\""), "raw string tag should be part of the literal token")
    }

    private static func macroSplicesUseAmberHashRules() throws {
        let source = "macro def inc(x):\n  #{x} + true\nplain = 1 #{outside_macro}\n"
        let tokens = AmberSyntax.tokens(in: source)
        let splice = try token("#{", in: tokens, category: .punctuation)
        require(splice.range.location == (source as NSString).range(of: "#{x}").location,
                "macro splice should be visible at its source location")
        let outside = try token("#{outside_macro}", in: tokens, category: .comment)
        require(outside.text == "#{outside_macro}",
                "outside a macro body #{ keeps its comment reading")
    }

    private static func applyingColorPreservesText() throws {
        let source = "answer = 42\n# note\n"
        let storage = NSTextStorage(string: source)
        AmberSyntax.apply(to: storage)
        require(storage.string == source,
                "syntax coloring must not mutate the editor source")
        require(storage.attribute(.foregroundColor, at: 0, effectiveRange: nil) != nil,
                "syntax coloring should install a foreground color")
    }

    private static func editorCoordinatorPreservesSelectionUndoAndIME() throws {
        // NSTextView lazily touches AppKit's application services even though
        // this test never opens a window. Keep that application prohibited so
        // the tokenizer and NSTextStorage tests remain headless.
        let application = NSApplication.shared
        application.setActivationPolicy(.prohibited)

        var source = "answer = 42\n"
        let binding = Binding<String>(
            get: { source },
            set: { source = $0 }
        )
        let editor = CodeEditor(text: binding)
        let coordinator = editor.makeCoordinator()
        let textView = UndoTextView(frame: NSRect(x: 0, y: 0, width: 500, height: 100))
        textView.delegate = coordinator
        textView.isRichText = false
        textView.allowsUndo = true
        textView.string = source
        textView.setSelectedRange(NSRange(location: 4, length: 0))
        coordinator.replaceExternalSource("answer = 43\n", in: textView)
        require(textView.string == "answer = 43\n",
                "external source replacement should update the text view")
        require(textView.selectedRange() == NSRange(location: 4, length: 0),
                "external source replacement should preserve the selection")

        textView.setSelectedRange(NSRange(location: 10, length: 0))
        textView.insertText("!", replacementRange: textView.selectedRange())
        require(textView.string == "answer = 4!3\n",
                "the real text view should accept an edit")
        textView.undoManager?.undo()
        require(textView.string == "answer = 43\n",
                "syntax attributes must not consume the user's undo edit")

        textView.setSelectedRange(NSRange(location: 0, length: 0))
        textView.setMarkedText("かな", selectedRange: NSRange(location: 2, length: 0),
                               replacementRange: NSRange(location: 0, length: 0))
        if textView.hasMarkedText() {
            coordinator.replaceExternalSource("runtime = true\n", in: textView)
            require(textView.string.hasPrefix("かな"),
                    "external snapshots must not replace active marked IME text")
            textView.unmarkText()
        }
    }

    private static func token(_ text: String,
                              in tokens: [AmberSyntaxToken],
                              category: AmberSyntaxCategory) throws -> AmberSyntaxToken {
        guard let token = tokens.first(where: { $0.text == text && $0.category == category }) else {
            throw SyntaxTestError(description: "missing " + category.rawValue + " token: " + text)
        }
        return token
    }

    private static func require(_ condition: @autoclosure () -> Bool,
                                _ message: String) {
        precondition(condition(), message)
    }
}

private final class UndoTextView: NSTextView {
    private let stableUndoManager = UndoManager()

    override var undoManager: UndoManager? {
        stableUndoManager
    }
}
