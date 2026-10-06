import AppKit

/// Paragraph operations are single undoable transactions. List markers belong
/// to paragraphs; table cell separators are protected from ordinary deletion.
enum RichTextEditing {
    typealias List = RichTextStructure.ListKind
    static var body: [NSAttributedString.Key: Any] {
        var attrs = RichTextCodec.attributes(style: .body, bold: false, italic: false, underline: false)
        attrs[.paragraphStyle] = RichTextStructure.paragraphStyle()
        return attrs
    }

    static func transaction(_ view: RichTextTextView, name: String,
                            change: (NSMutableAttributedString, inout NSRange) -> Void) {
        guard view.isEditable, !view.hasMarkedText(), !view.isStructuralEdit else { return }
        let next = NSMutableAttributedString(attributedString: view.attributedString())
        var selection = view.selectedRange()
        change(next, &selection)
        normalizeLists(next, selection: &selection)
        view.breakUndoCoalescing()
        view.undoManager?.beginUndoGrouping()
        install(next, selection: selection, typing: nil, in: view)
        view.undoManager?.setActionName(name)
        view.undoManager?.endUndoGrouping()
    }

    private static func install(_ value: NSAttributedString, selection: NSRange,
                                typing: [NSAttributedString.Key: Any]?, in view: RichTextTextView) {
        let previous = NSAttributedString(attributedString: view.attributedString())
        let oldSelection = view.selectedRange(), oldTyping = view.typingAttributes
        view.undoManager?.registerUndo(withTarget: view) { target in
            install(previous, selection: oldSelection, typing: oldTyping, in: target)
        }
        view.isStructuralEdit = true
        view.textStorage?.setAttributedString(value)
        let location = min(selection.location, value.length)
        view.setSelectedRange(NSRange(location: location, length: min(selection.length, value.length - location)))
        view.typingAttributes = typing ?? attributes(at: location, in: value)
        view.isStructuralEdit = false
        view.didChangeText()
    }

    static func attributes(at position: Int, in value: NSAttributedString) -> [NSAttributedString.Key: Any] {
        let paragraph = RichTextStructure.paragraph(at: position, in: value.string)
        guard paragraph.length > 0 else { return body }
        return value.attributes(at: min(position, NSMaxRange(paragraph) - 1), effectiveRange: nil)
    }

    private static func selectedParagraphs(_ selection: NSRange, in value: NSMutableAttributedString) -> [NSRange] {
        if selection.location == value.length && (value.length == 0 || (value.string as NSString).character(at: value.length - 1) == 10) {
            value.append(NSAttributedString(string: "\n", attributes: body))
        }
        let last = selection.length == 0 ? selection.location : NSMaxRange(selection) - 1
        return RichTextStructure.paragraphs(value.string).filter {
            NSMaxRange($0) > selection.location && $0.location <= last
        }
    }

    static func heading(_ style: RichTextCodec.Style, in view: RichTextTextView) {
        transaction(view, name: "Paragraph Style") { value, selection in
            for range in selectedParagraphs(selection, in: value) {
                setHeading(style, range: range, in: value)
            }
        }
    }

    private static func setHeading(_ style: RichTextCodec.Style, range: NSRange, in value: NSMutableAttributedString) {
        let heading = style == .body ? nil : style.rawValue
        let attrs = value.attributes(at: range.location, effectiveRange: nil)
        let list = RichTextStructure.list(in: attrs), block = RichTextStructure.tableBlock(in: attrs)
        value.enumerateAttributes(in: range, options: []) { attrs, segment, _ in
            let font = RichTextCodec.font(style: style,
                bold: attrs[NSAttributedString.Key("com.amber.rich-text.bold")] as? Bool ?? false,
                italic: attrs[NSAttributedString.Key("com.amber.rich-text.italic")] as? Bool ?? false)
            value.addAttributes([.font: font, RichTextCodec.styleAttributeKey: style.rawValue], range: segment)
        }
        value.removeAttribute(RichTextStructure.headingKey, range: range)
        if let heading, block == nil { value.addAttribute(RichTextStructure.headingKey, value: heading, range: range) }
        value.addAttribute(.paragraphStyle, value: RichTextStructure.paragraphStyle(heading: heading, list: list, block: block), range: range)
    }

    static func toggleList(_ kind: List, in view: RichTextTextView) {
        transaction(view, name: kind == .bullet ? "Bullet List" : "Numbered List") { value, selection in
            let ranges = selectedParagraphs(selection, in: value)
            guard !ranges.contains(where: { RichTextStructure.tableBlock(in: value.attributes(at: $0.location, effectiveRange: nil)) != nil }) else { return }
            let remove = ranges.allSatisfy { RichTextStructure.list(in: value.attributes(at: $0.location, effectiveRange: nil)) == kind }
            for range in ranges.reversed() {
                let attrs = value.attributes(at: range.location, effectiveRange: nil)
                if let old = RichTextStructure.list(in: attrs) {
                    let length = RichTextStructure.prefixLength((value.string as NSString).substring(with: range), kind: old)
                    replace(NSRange(location: range.location, length: length), with: NSAttributedString(string: ""), in: value, selection: &selection)
                }
                let updated = RichTextStructure.paragraph(at: range.location, in: value.string)
                value.removeAttribute(RichTextStructure.listKey, range: updated)
                if !remove { value.addAttribute(RichTextStructure.listKey, value: kind.rawValue, range: updated) }
                value.addAttribute(.paragraphStyle, value: RichTextStructure.paragraphStyle(list: remove ? nil : kind), range: updated)
            }
        }
    }

    /// Rebuild derived prefixes and hanging indents after splits, joins or
    /// multi-line paste. This never interprets arbitrary body text as a list.
    static func normalizeLists(_ value: NSMutableAttributedString, selection: inout NSRange) {
        var offset = 0, number = 0
        var previousKind: List?
        while offset < value.length {
            var range = RichTextStructure.paragraph(at: offset, in: value.string)
            let attrs = value.attributes(at: offset, effectiveRange: nil)
            guard let kind = RichTextStructure.list(in: attrs), RichTextStructure.tableBlock(in: attrs) == nil else {
                previousKind = nil; number = 0; offset = NSMaxRange(range); continue
            }
            number = previousKind == kind ? number + 1 : 1
            previousKind = kind
            let prefix = kind == .bullet ? "•\t" : "\(number).\t"
            let oldPrefix = RichTextStructure.prefixLength((value.string as NSString).substring(with: range), kind: kind)
            if (value.string as NSString).substring(with: NSRange(location: offset, length: oldPrefix)) != prefix {
                replace(NSRange(location: offset, length: oldPrefix), with: NSAttributedString(string: prefix, attributes: attrs),
                        in: value, selection: &selection)
            }
            range = RichTextStructure.paragraph(at: offset, in: value.string)
            value.addAttribute(RichTextStructure.listKey, value: kind.rawValue, range: range)
            value.addAttribute(.paragraphStyle, value: RichTextStructure.paragraphStyle(heading: attrs[RichTextStructure.headingKey] as? String, list: kind), range: range)
            offset = NSMaxRange(range)
        }
    }

    static func shouldManage(_ range: NSRange, in view: RichTextTextView) -> Bool {
        let value = view.attributedString()
        let attrs = attributes(at: range.location, in: value)
        return RichTextStructure.list(in: attrs) != nil || attrs[RichTextStructure.headingKey] != nil ||
            RichTextStructure.tableBlock(in: attrs) != nil ||
            RichTextStructure.tables(in: value).contains {
                NSIntersectionRange($0.range, range).length > 0 ||
                (range.length > 0 && NSMaxRange(range) == $0.range.location)
            } ||
            (range.length > 0 && RichTextStructure.list(in: attributes(at: NSMaxRange(range), in: value)) != nil)
    }

    static func insert(_ text: String, replacing range: NSRange, in view: RichTextTextView) {
        let value = view.attributedString()
        let range = joiningRange(protectedListRange(range, in: value), replacement: text, in: value)
        guard safeReplacement(range, in: value) else { NSSound.beep(); return }
        var attrs = attributes(at: range.location, in: value)
        if let table = RichTextStructure.table(at: range.location, in: value),
           range.location <= table.range.location && NSMaxRange(range) >= NSMaxRange(table.range) {
            // Replacing a complete table produces ordinary prose, not a
            // surviving cell with an invalid, incomplete row structure.
            attrs = body
        }
        // Keep inline typing toggles, but take paragraph/cell ownership from
        // the destination, never from the previous first responder's caret.
        for (key, attr) in view.typingAttributes where key != .paragraphStyle && key != RichTextStructure.listKey && key != RichTextStructure.headingKey {
            attrs[key] = attr
        }
        let content = RichTextStructure.tableBlock(in: attrs) == nil ? text : text.replacingOccurrences(of: "\r\n", with: "\u{2028}").replacingOccurrences(of: "\n", with: "\u{2028}").replacingOccurrences(of: "\r", with: "\u{2028}")
        transaction(view, name: "Typing") { next, selection in
            next.replaceCharacters(in: range, with: NSAttributedString(string: content, attributes: attrs))
            selection = NSRange(location: range.location + (content as NSString).length, length: 0)
        }
    }

    static func newline(in view: RichTextTextView) -> Bool {
        let value = view.attributedString(), selection = view.selectedRange()
        let attrs = attributes(at: selection.location, in: value)
        if RichTextStructure.tableBlock(in: attrs) != nil {
            insert("\u{2028}", replacing: selection, in: view)
            return true
        }
        let kind = RichTextStructure.list(in: attrs)
        guard kind != nil || attrs[RichTextStructure.headingKey] != nil else { return false }
        guard safeReplacement(selection, in: value) else { NSSound.beep(); return true }
        transaction(view, name: "New Paragraph") { next, caret in
            let paragraph = RichTextStructure.paragraph(at: caret.location, in: next.string)
            let source = (next.string as NSString).substring(with: paragraph)
            if let kind {
                let prefixLength = RichTextStructure.prefixLength(source, kind: kind)
                let content = (source as NSString).substring(from: prefixLength).trimmingCharacters(in: .whitespacesAndNewlines)
                if content.isEmpty {
                    next.removeAttribute(RichTextStructure.listKey, range: paragraph)
                    replace(NSRange(location: paragraph.location, length: prefixLength), with: NSAttributedString(string: ""), in: next, selection: &caret)
                    let remaining = RichTextStructure.paragraph(at: paragraph.location, in: next.string)
                    if remaining.length > 0 { next.setAttributes(body, range: remaining) }
                    caret = NSRange(location: paragraph.location, length: 0)
                    return
                }
                let prefix = kind == .bullet ? "•\t" : "1.\t"
                // A click before/inside the derived marker means the start
                // of the item's content, never a split inside its marker.
                let end = max(NSMaxRange(caret), paragraph.location + prefixLength)
                caret.location = max(caret.location, paragraph.location + prefixLength)
                caret.length = end - caret.location
                caret = joiningRange(caret, replacement: "\n" + prefix, in: next)
                next.replaceCharacters(in: caret, with: NSAttributedString(string: "\n" + prefix, attributes: attrs))
                caret = NSRange(location: caret.location + 1 + (prefix as NSString).length, length: 0)
            } else {
                caret = joiningRange(caret, replacement: "\n", in: next)
                next.replaceCharacters(in: caret, with: NSAttributedString(string: "\n", attributes: attrs))
                caret = NSRange(location: caret.location + 1, length: 0)
                let following = RichTextStructure.paragraph(at: caret.location, in: next.string)
                if following.length > 0 { setHeading(.body, range: following, in: next) }
            }
        }
        return true
    }

    static func delete(backward: Bool, in view: RichTextTextView) -> Bool {
        let value = view.attributedString(), selected = view.selectedRange()
        let attrs = attributes(at: selected.location, in: value)
        if selected.length == 0, let kind = RichTextStructure.list(in: attrs) {
            let paragraph = RichTextStructure.paragraph(at: selected.location, in: value.string)
            let prefix = RichTextStructure.prefixLength((value.string as NSString).substring(with: paragraph), kind: kind)
            if backward && selected.location <= paragraph.location + prefix { toggleList(kind, in: view); return true }
        }
        var range = selected
        if range.length == 0 {
            let index = backward ? range.location - 1 : range.location
            guard index >= 0 && index < value.length else { return false }
            range = (value.string as NSString).rangeOfComposedCharacterSequence(at: index)
        }
        guard shouldManage(range, in: view) else { return false }
        range = joiningRange(protectedListRange(range, in: value), replacement: "", in: value)
        guard safeReplacement(range, in: value) else { NSSound.beep(); return true }
        transaction(view, name: "Delete") { next, caret in
            next.deleteCharacters(in: range)
            caret = NSRange(location: range.location, length: 0)
        }
        return true
    }

    private static func safeReplacement(_ range: NSRange, in value: NSAttributedString) -> Bool {
        guard range.location <= value.length && range.length <= value.length - range.location else { return false }
        for table in RichTextStructure.tables(in: value) {
            if range.location <= table.range.location && NSMaxRange(range) >= NSMaxRange(table.range) { continue }
            // Do not join outside prose to the first cell by deleting its
            // preceding paragraph boundary, or select across that boundary.
            if range.length > 0 && range.location < table.range.location && NSMaxRange(range) >= table.range.location { return false }
            for cell in table.cells where NSLocationInRange(NSMaxRange(cell) - 1, range) { return false }
        }
        return true
    }

    private static func protectedListRange(_ range: NSRange, in value: NSAttributedString) -> NSRange {
        let paragraph = RichTextStructure.paragraph(at: range.location, in: value.string)
        guard paragraph.length > 0,
              let kind = RichTextStructure.list(in: value.attributes(at: paragraph.location, effectiveRange: nil)) else { return range }
        let contentStart = paragraph.location + RichTextStructure.prefixLength((value.string as NSString).substring(with: paragraph), kind: kind)
        // Whole item selections may remove their marker; normalization adds
        // a new one if replacement text retains list ownership.
        if range.length > 0 && NSMaxRange(range) >= contentStart { return range }
        let start = max(range.location, contentStart)
        return NSRange(location: start, length: max(0, NSMaxRange(range) - start))
    }

    private static func joiningRange(_ range: NSRange, replacement: String, in value: NSAttributedString) -> NSRange {
        guard range.length > 0, NSMaxRange(range) <= value.length else { return range }
        let first = RichTextStructure.paragraph(at: range.location, in: value.string)
        let last = RichTextStructure.paragraph(at: NSMaxRange(range), in: value.string)
        guard range.location < last.location, last.length > 0,
              !replacement.hasSuffix("\n"),
              range.location > first.location || !replacement.isEmpty,
              let kind = RichTextStructure.list(in: value.attributes(at: last.location, effectiveRange: nil)) else { return range }
        // A joined paragraph keeps only its left marker; the right item's
        // derived marker is not user content and must not land mid-sentence.
        let contentStart = last.location + RichTextStructure.prefixLength((value.string as NSString).substring(with: last), kind: kind)
        return NSRange(location: range.location, length: max(NSMaxRange(range), contentStart) - range.location)
    }

    static func insertTable(rows: Int, columns: Int, in view: RichTextTextView) {
        guard rows > 0 && (1...12).contains(columns) && rows * columns <= 240 else { return }
        guard RichTextStructure.table(at: view.selectedRange().location, in: view.attributedString()) == nil else { NSSound.beep(); return }
        transaction(view, name: "Insert Table") { value, selection in
            let paragraph = RichTextStructure.paragraph(at: NSMaxRange(selection), in: value.string)
            var position = NSMaxRange(paragraph)
            if position > 0 && (value.string as NSString).character(at: position - 1) != 10 {
                value.insert(NSAttributedString(string: "\n", attributes: body), at: position)
                position += 1
            }
            let table = NSMutableAttributedString(string: String(repeating: "\n", count: rows * columns), attributes: body)
            RichTextStructure.applyTable(to: table, columns: columns, cells: (0..<(rows * columns)).map { NSRange(location: $0, length: 1) })
            table.append(NSAttributedString(string: "\n", attributes: body))
            value.insert(table, at: position)
            selection = NSRange(location: position, length: 0)
        }
    }

    enum TableAction { case addRow, removeRow, addColumn, removeColumn, afterTable, removeTable }
    static func tableAction(_ action: TableAction, in view: RichTextTextView) {
        guard let table = RichTextStructure.table(at: view.selectedRange().location, in: view.attributedString()),
              let current = table.cells.firstIndex(where: { NSLocationInRange(view.selectedRange().location, $0) }) else { NSSound.beep(); return }
        if action == .afterTable {
            view.setSelectedRange(NSRange(location: NSMaxRange(table.range), length: 0)); view.typingAttributes = body; return
        }
        transaction(view, name: "Edit Table") { value, selection in
            var columns = table.object.numberOfColumns
            var cells = table.cells.map { NSMutableAttributedString(attributedString: value.attributedSubstring(from: $0)) }
            let row = current / columns, column = current % columns
            switch action {
            case .addRow:
                guard cells.count + columns <= 240 else { return }
                cells.insert(contentsOf: (0..<columns).map { _ in NSMutableAttributedString(string: "\n", attributes: body) }, at: (row + 1) * columns)
            case .removeRow: cells.removeSubrange((row * columns)..<((row + 1) * columns))
            case .addColumn:
                let rows = cells.count / columns
                guard columns < 12 && rows * (columns + 1) <= 240 else { return }
                for r in (0..<rows).reversed() { cells.insert(NSMutableAttributedString(string: "\n", attributes: body), at: r * columns + column + 1) }
                columns += 1
            case .removeColumn:
                for r in (0..<(cells.count / columns)).reversed() { cells.remove(at: r * columns + column) }
                columns -= 1
            case .removeTable: cells.removeAll()
            case .afterTable: return
            }
            let replacement = NSMutableAttributedString(string: "")
            var ranges: [NSRange] = []
            for cell in cells {
                ranges.append(NSRange(location: replacement.length, length: cell.length))
                replacement.append(cell)
            }
            if !cells.isEmpty { RichTextStructure.applyTable(to: replacement, columns: columns, cells: ranges) }
            value.replaceCharacters(in: table.range, with: replacement)
            let target = ranges.isEmpty ? 0 : ranges[min(current, ranges.count - 1)].location
            selection = NSRange(location: table.range.location + target, length: 0)
        }
    }

    static func tab(backward: Bool, in view: RichTextTextView) -> Bool {
        guard let table = RichTextStructure.table(at: view.selectedRange().location, in: view.attributedString()),
              let index = table.cells.firstIndex(where: { NSLocationInRange(view.selectedRange().location, $0) }) else { return false }
        let next = index + (backward ? -1 : 1)
        if next >= 0 && next < table.cells.count {
            view.setSelectedRange(NSRange(location: table.cells[next].location, length: 0))
            view.typingAttributes = attributes(at: table.cells[next].location, in: view.attributedString())
        } else if next == table.cells.count {
            view.setSelectedRange(NSRange(location: NSMaxRange(table.range), length: 0)); view.typingAttributes = body
        }
        return true
    }

    private static func replace(_ range: NSRange, with replacement: NSAttributedString,
                                in value: NSMutableAttributedString, selection: inout NSRange) {
        func mapped(_ position: Int) -> Int {
            if position < range.location { return position }
            if position >= NSMaxRange(range) { return position + replacement.length - range.length }
            return range.location + replacement.length
        }
        let start = mapped(selection.location), end = mapped(NSMaxRange(selection))
        value.replaceCharacters(in: range, with: replacement)
        selection = NSRange(location: start, length: max(0, end - start))
    }
}
