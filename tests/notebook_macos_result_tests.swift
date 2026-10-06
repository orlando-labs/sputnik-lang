import AppKit
import SwiftUI

@main
private enum NotebookResultTests {
    static func main() {
        let text = "😀 :loss: 0.2, :LOSS: 0.1, [a.*], Жж"
        let matches = NotebookResultSearch.matches(in: text, query: "loss")
        precondition(matches == [NSRange(location: 4, length: 4), NSRange(location: 16, length: 4)])
        precondition(NotebookResultSearch.matches(in: text, query: "loss", matchCase: true).count == 1)
        precondition(NotebookResultSearch.matches(in: text, query: "a.*").count == 1, "no regex interpretation")
        precondition(NotebookResultSearch.matches(in: text, query: "ж").count == 2)
        precondition(NotebookResultSearch.matches(in: text, query: "").isEmpty)
        precondition(NotebookResultSearch.matches(in: text, query: "absent").isEmpty)
        precondition(NotebookResultSearch.matches(in: "aaaa", query: "aa").count == 2)
        precondition(NotebookResultSearch.step(0, by: -1, count: 2) == 1)
        precondition(NotebookResultSearch.step(1, by: 1, count: 2) == 0)
        precondition(NotebookResultSearch.step(0, by: 1, count: 0) == 0)
        let long = String(repeating: "😀", count: 4000) + "needle"
        let preview = NotebookResultSearch.preview(long)
        precondition(preview.count == 601 && preview.hasSuffix("…"))
        precondition(!preview.contains("needle") && NotebookResultSearch.matches(in: long, query: "needle").count == 1,
                     "search includes the text outside collapsed preview")
        precondition(NotebookResultSearch.preview("1\n2\n3\n4\n5\n6\n7") == "1\n2\n3\n4\n5\n6…")
        precondition(NotebookResultSearch.preview("small") == "small")

        _ = NSApplication.shared
        precondition(!NotebookResultLayout.canExpand(text: "42", naturalHeight: 14, previewHeight: 14))
        precondition(!NotebookResultLayout.canExpand(text: "42", naturalHeight: 14.1, previewHeight: 14), "ignore subpixel rounding")
        precondition(NotebookResultLayout.canExpand(text: long, naturalHeight: 84, previewHeight: 84), "character budget hides content")
        precondition(NotebookResultLayout.canExpand(text: "1\n2\n3\n4\n5\n6\n7", naturalHeight: 84, previewHeight: 84))
        // Measure actual SwiftUI wrapping, including width changes while the
        // same preview stays mounted (as it does underneath an expanded view).
        var measuredNatural: CGFloat = 0, measuredPreview: CGFloat = 0
        func previewView(_ text: String) -> NotebookResultPreview {
            NotebookResultPreview(text: text) { measuredNatural = $0; measuredPreview = $1 }
        }
        let wrapping = String(repeating: "word ", count: 80)
        let host = NSHostingView(rootView: previewView(wrapping))
        let measurementWindow = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 900, height: 400),
                                         styleMask: [.titled, .resizable], backing: .buffered, defer: false)
        measurementWindow.contentView = host
        func settle() {
            host.layoutSubtreeIfNeeded()
            RunLoop.main.run(until: Date().addingTimeInterval(0.08))
            host.layoutSubtreeIfNeeded()
        }
        settle()
        precondition(measuredPreview > 0 && !NotebookResultLayout.canExpand(text: wrapping, naturalHeight: measuredNatural, previewHeight: measuredPreview),
                     "fully visible wrapped text has no disclosure")
        measurementWindow.setContentSize(NSSize(width: 170, height: 400)); settle()
        precondition(NotebookResultLayout.canExpand(text: wrapping, naturalHeight: measuredNatural, previewHeight: measuredPreview),
                     "narrow width hides lines even below the character budget")
        measurementWindow.setContentSize(NSSize(width: 900, height: 400)); settle()
        precondition(!NotebookResultLayout.canExpand(text: wrapping, naturalHeight: measuredNatural, previewHeight: measuredPreview),
                     "widening removes obsolete disclosure")
        host.rootView = previewView("42"); settle()
        precondition(!NotebookResultLayout.canExpand(text: "42", naturalHeight: measuredNatural, previewHeight: measuredPreview),
                     "a shorter replacement cannot retain disclosure state")
        measurementWindow.orderOut(nil)
        let view = NSTextView(frame: NSRect(x: 0, y: 0, width: 500, height: 260))
        let coordinator = NotebookResultText.Coordinator()
        coordinator.update(view, text: text, matches: matches, selected: 0)
        precondition(view.string == text)
        let first = view.textStorage!.attribute(.backgroundColor, at: matches[0].location, effectiveRange: nil) as! NSColor
        coordinator.update(view, text: text, matches: matches, selected: 1)
        let second = view.textStorage!.attribute(.backgroundColor, at: matches[1].location, effectiveRange: nil) as! NSColor
        precondition(first == second, "active highlight follows navigation")
        coordinator.update(view, text: text, matches: [], selected: 0)
        precondition(view.textStorage!.attribute(.backgroundColor, at: matches[0].location, effectiveRange: nil) == nil,
                     "closing search clears highlights")
        view.setSelectedRange(NSRange(location: 0, length: 2))
        coordinator.update(view, text: text, matches: [], selected: 0)
        precondition(view.selectedRange() == NSRange(location: 0, length: 2), "unchanged snapshots preserve selection")
        coordinator.update(view, text: "42", matches: [], selected: 0)
        precondition(view.string == "42", "new execution replaces text")
        print("notebook_macos_result_tests: ok")
        if CommandLine.arguments.contains("--interactive") || Bundle.main.bundleIdentifier == "org.amberlang.result-preview-test" {
            // A disposable native fixture for visual/manual regression, never
            // opens or alters the user's running notebook or runtime results.
            NSApp.setActivationPolicy(.regular)
            let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 900, height: 700),
                                  styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
            window.title = "Result Preview · UI test"
            window.isReleasedWhenClosed = false
            let compact = "{:accuracy: 98.69, :test_loss: 0.0445108, :seconds: 81.5271, :steps: 2345, :parameters: 19706, :train_count: 60000, :test_count: 10000, :device: \"mps\", :epochs: 5}"
            let pretty = "{\n  :accuracy: 98.69,\n  :test_loss: 0.0445108,\n  :seconds: 81.5271,\n  :steps: 2345,\n  :parameters: 19706,\n  :train_count: 60000,\n  :test_count: 10000,\n  :device: \"mps\",\n  :epochs: 5\n}"
            window.contentView = NSHostingView(rootView: VStack(spacing: 20) {
                NotebookResultView(title: "RESULT · SCALAR", text: "42", pretty: nil,
                                   truncated: false, prettyTruncated: false, tint: .green)
                NotebookResultView(title: "RESULT · MNIST", text: compact, pretty: pretty,
                                   truncated: false, prettyTruncated: false, tint: .green)
                NotebookResultView(title: "RESULT · LARGE CONTAINER", text: "[" + String(repeating: "\"item\", ", count: 500) + "\"needle\", …]",
                                   pretty: nil, truncated: true, prettyTruncated: false, tint: .green)
                Spacer()
            }.padding(24))
            window.center(); window.makeKeyAndOrderFront(nil)
            NSApp.activate(ignoringOtherApps: true)
            NSApp.run()
        }
    }
}
