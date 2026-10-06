import AppKit
import SwiftUI

/// A small AppKit-backed source editor. Keeping NSTextView alive across SwiftUI
/// snapshots preserves its native selection and undo history while the text is
/// unchanged.
struct CodeEditor: NSViewRepresentable {
    @Binding var text: String
    var editable: Bool = true
    var onFocus: () -> Void = {}
    var onRun: () -> Void = {}

    func makeCoordinator() -> Coordinator {
        Coordinator(self)
    }

    func makeNSView(context: Context) -> NSScrollView {
        let scrollView = NSScrollView()
        scrollView.drawsBackground = true
        scrollView.backgroundColor = NSColor(calibratedRed: 0.995, green: 0.990, blue: 0.976, alpha: 1)
        scrollView.borderType = .noBorder
        scrollView.hasVerticalScroller = true
        scrollView.hasHorizontalScroller = false
        scrollView.autohidesScrollers = true

        let textView = FocusTrackingTextView(frame: NSRect(x: 0, y: 0, width: 640, height: 100))
        textView.delegate = context.coordinator
        textView.onFocus = context.coordinator.parent.onFocus
        textView.string = text
        textView.isRichText = false
        textView.isEditable = editable
        textView.isSelectable = true
        textView.allowsUndo = true
        textView.drawsBackground = false
        textView.textColor = .labelColor
        textView.insertionPointColor = NSColor(calibratedRed: 0.72, green: 0.39, blue: 0.12, alpha: 1)
        textView.font = NSFont.monospacedSystemFont(ofSize: 13, weight: .regular)
        textView.textContainerInset = NSSize(width: 16, height: 14)
        textView.isHorizontallyResizable = false
        textView.isVerticallyResizable = true
        textView.minSize = NSSize(width: 0, height: 0)
        textView.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
        textView.textContainer?.widthTracksTextView = true
        textView.textContainer?.heightTracksTextView = false
        textView.textContainer?.containerSize = NSSize(width: 0, height: CGFloat.greatestFiniteMagnitude)
        textView.isAutomaticQuoteSubstitutionEnabled = false
        textView.isAutomaticDashSubstitutionEnabled = false
        textView.isAutomaticTextReplacementEnabled = false
        textView.isAutomaticSpellingCorrectionEnabled = false
        textView.isContinuousSpellCheckingEnabled = false
        textView.isGrammarCheckingEnabled = false
        textView.smartInsertDeleteEnabled = false
        textView.typingAttributes = Self.editorTypingAttributes
        if let storage = textView.textStorage {
            AmberSyntax.apply(to: storage,
                              source: text,
                              font: Self.editorFont,
                              baseColor: Self.editorColor)
        }
        textView.setAccessibilityLabel(editable ? "Amber source editor" : "Read-only Amber source")
        scrollView.documentView = textView
        return scrollView
    }

    func updateNSView(_ scrollView: NSScrollView, context: Context) {
        context.coordinator.parent = self
        guard let textView = scrollView.documentView as? NSTextView else { return }

        textView.isEditable = editable
        textView.setAccessibilityLabel(editable ? "Amber source editor" : "Read-only Amber source")
        (textView as? FocusTrackingTextView)?.onFocus = onFocus

        // Runtime snapshots arrive frequently. Assign only when the source
        // itself changed so the normal snapshot path never disturbs selection
        // or the user's undo stack.
        if textView.string != text {
            context.coordinator.replaceExternalSource(text, in: textView)
        } else {
            context.coordinator.flushPendingSource(in: textView)
        }
    }

    func sizeThatFits(_ proposal: ProposedViewSize, nsView: NSScrollView, context: Context) -> CGSize? {
        let width = max(240, proposal.width ?? 640)
        let usableWidth = max(160, width - 44)
        let approximateCharacterWidth: CGFloat = 7.7
        let sourceLines = text.components(separatedBy: .newlines)
        let visualLineCount = sourceLines.reduce(0) { count, line in
            let wrapped = max(1, Int(ceil(CGFloat(line.count) * approximateCharacterWidth / usableWidth)))
            return count + wrapped
        }
        let height = min(320, max(96, CGFloat(visualLineCount) * 19 + 30))
        return CGSize(width: width, height: height)
    }

    final class Coordinator: NSObject, NSTextViewDelegate {
        var parent: CodeEditor
        private(set) var isApplyingExternalSource = false
        private(set) var isApplyingSyntax = false
        private var pendingExternalSource: String?

        init(_ parent: CodeEditor) {
            self.parent = parent
        }

        func replaceExternalSource(_ source: String, in textView: NSTextView) {
            // AppKit owns an IME's marked range. Replacing text while it is
            // marked discards the composition, so wait for the next SwiftUI
            // update after the composition commits.
            if textView.hasMarkedText() {
                pendingExternalSource = source
                return
            }
            pendingExternalSource = nil

            let oldSelection = textView.selectedRange()
            let newLength = (source as NSString).length
            let undoManager = textView.undoManager
            guard let storage = textView.textStorage else { return }
            undoManager?.disableUndoRegistration()
            isApplyingExternalSource = true
            storage.beginEditing()
            storage.replaceCharacters(
                in: NSRange(location: 0, length: storage.length),
                with: NSAttributedString(string: source,
                                          attributes: CodeEditor.editorTypingAttributes))
            storage.endEditing()
            isApplyingExternalSource = false
            undoManager?.enableUndoRegistration()

            let location = min(max(0, oldSelection.location), newLength)
            let available = max(0, newLength - location)
            let length = min(max(0, oldSelection.length), available)
            textView.setSelectedRange(NSRange(location: location, length: length))
            highlight(textView)
        }

        func flushPendingSource(in textView: NSTextView) {
            guard let pending = pendingExternalSource, !textView.hasMarkedText() else { return }
            pendingExternalSource = nil
            guard pending == parent.text else { return }
            guard pending != textView.string else {
                return
            }
            replaceExternalSource(pending, in: textView)
        }

        private func highlight(_ textView: NSTextView) {
            guard !textView.hasMarkedText(), let storage = textView.textStorage else { return }
            let undoManager = textView.undoManager
            undoManager?.disableUndoRegistration()
            isApplyingSyntax = true
            AmberSyntax.apply(to: storage,
                              source: textView.string,
                              font: CodeEditor.editorFont,
                              baseColor: CodeEditor.editorColor)
            isApplyingSyntax = false
            undoManager?.enableUndoRegistration()
            textView.typingAttributes = CodeEditor.editorTypingAttributes
        }

        func textDidChange(_ notification: Notification) {
            guard let textView = notification.object as? NSTextView,
                  !isApplyingExternalSource, !isApplyingSyntax else { return }
            if parent.text != textView.string {
                parent.text = textView.string
            }
            // Do not touch attributes belonging to an active IME composition.
            // The next non-marked textDidChange recolors the complete source.
            if !textView.hasMarkedText() {
                highlight(textView)
            }
        }

        func textDidEndEditing(_ notification: Notification) {
            guard let textView = notification.object as? NSTextView else { return }
            flushPendingSource(in: textView)
        }

        func textView(_ textView: NSTextView, doCommandBy commandSelector: Selector) -> Bool {
            if commandSelector == #selector(NSResponder.insertTab(_:)) {
                textView.insertText("  ", replacementRange: textView.selectedRange())
                return true
            }

            if commandSelector == #selector(NSResponder.insertNewline(_:)),
               textView.window?.currentEvent?.modifierFlags.contains(.command) == true {
                parent.onRun()
                return true
            }

            return false
        }
    }

    fileprivate static let editorFont = NSFont.monospacedSystemFont(ofSize: 13,
                                                                     weight: .regular)
    fileprivate static let editorColor = NSColor(calibratedRed: 0.153,
                                                 green: 0.161,
                                                 blue: 0.166,
                                                 alpha: 1)
    fileprivate static let editorTypingAttributes: [NSAttributedString.Key: Any] = [
        .font: editorFont,
        .foregroundColor: editorColor,
    ]
}

private final class FocusTrackingTextView: NSTextView {
    var onFocus: () -> Void = {}

    override func becomeFirstResponder() -> Bool {
        let didBecomeFirstResponder = super.becomeFirstResponder()
        if didBecomeFirstResponder { onFocus() }
        return didBecomeFirstResponder
    }
}
