import AppKit

/// SwiftUI probes multiple sizes, including zero. Measurement must never
/// mutate the live container or a last min-width probe narrows visible text.
enum RichTextLayout {
    static func height(_ value: NSAttributedString, width: CGFloat) -> CGFloat {
        let storage = NSTextStorage(attributedString: value)
        let manager = NSLayoutManager()
        let container = NSTextContainer(size: NSSize(width: max(100, width - 26), height: CGFloat.greatestFiniteMagnitude))
        storage.addLayoutManager(manager)
        manager.addTextContainer(container)
        manager.ensureLayout(for: container)
        return min(640, max(120, manager.usedRect(for: container).height + 26))
    }
}

final class RichTextScrollView: NSScrollView {
    override func layout() {
        super.layout()
        guard let view = documentView as? NSTextView else { return }
        let width = contentSize.width
        if width > 0 {
            view.setFrameSize(NSSize(width: width, height: max(contentSize.height, view.frame.height)))
            view.textContainer?.containerSize = NSSize(width: max(1, width - 2 * view.textContainerInset.width), height: CGFloat.greatestFiniteMagnitude)
        }
    }
}

/// Portable paragraph/table semantics. AppKit objects never cross the file
/// boundary: only checked UTF-16 ranges and rectangular cell order do.
enum RichTextStructure {
    static let headingKey = NSAttributedString.Key("com.amber.rich-text.paragraph")
    static let listKey = NSAttributedString.Key("com.amber.rich-text.list")
    enum ListKind: String { case bullet, numbered }

    struct Table {
        let object: NSTextTable
        var range: NSRange
        var cells: [NSRange]
    }

    static func paragraphs(_ text: String, includingTrailing: Bool = false) -> [NSRange] {
        let text = text as NSString
        var result: [NSRange] = [], start = 0
        while start < text.length {
            let newline = text.range(of: "\n", range: NSRange(location: start, length: text.length - start))
            let end = newline.location == NSNotFound ? text.length : NSMaxRange(newline)
            result.append(NSRange(location: start, length: end - start))
            start = end
        }
        if includingTrailing && (text.length == 0 || text.character(at: text.length - 1) == 10) {
            result.append(NSRange(location: text.length, length: 0))
        }
        return result
    }

    static func paragraph(at position: Int, in text: String) -> NSRange {
        let ranges = paragraphs(text, includingTrailing: true)
        return ranges.first { position >= $0.location && position < NSMaxRange($0) }
            ?? ranges.last ?? NSRange(location: 0, length: 0)
    }

    static func list(in attrs: [NSAttributedString.Key: Any]) -> ListKind? {
        (attrs[listKey] as? String).flatMap(ListKind.init(rawValue:))
    }

    static func tableBlock(in attrs: [NSAttributedString.Key: Any]) -> NSTextTableBlock? {
        (attrs[.paragraphStyle] as? NSParagraphStyle)?.textBlocks.last as? NSTextTableBlock
    }

    static func tables(in value: NSAttributedString) -> [Table] {
        var result: [Table] = []
        var previousBlock: NSTextTableBlock?
        for range in paragraphs(value.string) {
            guard let block = tableBlock(in: value.attributes(at: range.location, effectiveRange: nil)) else {
                previousBlock = nil
                continue
            }
            if let last = result.last, last.object === block.table, NSMaxRange(last.range) == range.location {
                let i = result.count - 1
                result[i].range.length += range.length
                if previousBlock === block { result[i].cells[result[i].cells.count - 1].length += range.length }
                else { result[i].cells.append(range) }
            } else {
                result.append(Table(object: block.table, range: range, cells: [range]))
            }
            previousBlock = block
        }
        return result
    }

    static func table(at position: Int, in value: NSAttributedString) -> Table? {
        tables(in: value).first { NSLocationInRange(position, $0.range) }
    }

    static func paragraphStyle(heading: String? = nil, list: ListKind? = nil,
                               block: NSTextTableBlock? = nil) -> NSParagraphStyle {
        let style = NSMutableParagraphStyle()
        style.lineSpacing = 3
        style.paragraphSpacing = heading == nil ? 5 : 9
        style.paragraphSpacingBefore = heading == nil ? 0 : 8
        if list != nil {
            style.firstLineHeadIndent = 0
            style.headIndent = 32
            style.tabStops = [NSTextTab(textAlignment: .left, location: 32)]
            style.defaultTabInterval = 32
        }
        if let block { style.textBlocks = [block]; style.paragraphSpacing = 3 }
        return style
    }

    static func newTable(columns: Int) -> NSTextTable {
        let table = NSTextTable()
        table.numberOfColumns = columns
        table.layoutAlgorithm = .fixedLayoutAlgorithm
        table.collapsesBorders = true
        table.hidesEmptyCells = false
        table.setContentWidth(100, type: .percentageValueType)
        return table
    }

    static func block(table: NSTextTable, index: Int) -> NSTextTableBlock {
        let block = NSTextTableBlock(table: table, startingRow: index / table.numberOfColumns,
                                    rowSpan: 1, startingColumn: index % table.numberOfColumns, columnSpan: 1)
        block.setContentWidth(100 / CGFloat(table.numberOfColumns), type: .percentageValueType)
        block.setWidth(7, type: .absoluteValueType, for: .padding)
        block.setWidth(0.75, type: .absoluteValueType, for: .border)
        block.setBorderColor(NSColor(calibratedWhite: 0.76, alpha: 1))
        block.setValue(24, type: .absoluteValueType, for: .minimumHeight)
        return block
    }

    static func applyTable(to value: NSMutableAttributedString, columns: Int, cells: [NSRange]) {
        let table = newTable(columns: columns)
        for (index, range) in cells.enumerated() {
            value.removeAttribute(listKey, range: range)
            value.removeAttribute(headingKey, range: range)
            value.addAttribute(.paragraphStyle, value: paragraphStyle(block: block(table: table, index: index)), range: range)
        }
    }

    static func baseFormatting(_ formatting: String) throws -> (String, [String: Any]?) {
        guard !formatting.isEmpty else { return (formatting, nil) }
        let object = try JSONSerialization.jsonObject(with: Data(formatting.utf8))
        guard let document = object as? [String: Any], integer(document["version"]) == 2 else { return (formatting, nil) }
        guard Set(document.keys).isSubset(of: ["version", "runs", "paragraphs", "tables"]),
              let runs = document["runs"] as? [Any] else { throw invalid("invalid version 2 document") }
        return (try json(["version": 1, "runs": runs]), document)
    }

    static func apply(_ document: [String: Any], to value: NSMutableAttributedString) throws {
        let text = value.string as NSString
        let paragraphRecords = try objects(document["paragraphs"], limit: 20_000)
        let tableRecords = try objects(document["tables"], limit: 100)
        var paragraphRanges: [NSRange] = [], previous = 0
        for record in paragraphRecords {
            guard Set(record.keys).isSubset(of: ["start", "length", "style", "list"]) else { throw invalid("unknown paragraph property") }
            let range = try range(record, text: text, after: previous)
            previous = NSMaxRange(range)
            paragraphRanges.append(range)
            let heading = record["style"] as? String ?? "body"
            guard ["body", "heading1", "heading2", "heading3"].contains(heading),
                  record["style"] == nil || record["style"] is String else { throw invalid("invalid paragraph style") }
            var kind: ListKind?
            if let raw = record["list"] {
                guard let name = raw as? String, let parsed = ListKind(rawValue: name) else { throw invalid("invalid list type") }
                kind = parsed
            }
            for paragraph in paragraphs(text.substring(with: range)) {
                let absolute = NSRange(location: range.location + paragraph.location, length: paragraph.length)
                if let kind, prefixLength(text.substring(with: absolute), kind: kind) == 0 { throw invalid("missing list marker") }
                if heading != "body", let style = RichTextCodec.Style(rawValue: heading) {
                    value.enumerateAttributes(in: absolute, options: []) { attrs, segment, _ in
                        // Inline code can coexist with a paragraph heading.
                        guard attrs[RichTextCodec.styleAttributeKey] as? String != "code" else { return }
                        value.addAttributes([
                            .font: RichTextCodec.font(style: style,
                                bold: attrs[NSAttributedString.Key("com.amber.rich-text.bold")] as? Bool ?? false,
                                italic: attrs[NSAttributedString.Key("com.amber.rich-text.italic")] as? Bool ?? false),
                            RichTextCodec.styleAttributeKey: heading
                        ], range: segment)
                    }
                    value.addAttribute(headingKey, value: heading, range: absolute)
                }
                if let kind { value.addAttribute(listKey, value: kind.rawValue, range: absolute) }
                value.addAttribute(.paragraphStyle, value: paragraphStyle(heading: heading == "body" ? nil : heading, list: kind), range: absolute)
            }
        }
        previous = 0
        for record in tableRecords {
            guard Set(record.keys) == ["start", "length", "columns", "cells"],
                  let columns = integer(record["columns"]), (1...12).contains(columns) else { throw invalid("invalid table") }
            let tableRange = try range(record, text: text, after: previous)
            previous = NSMaxRange(tableRange)
            guard !paragraphRanges.contains(where: { NSIntersectionRange($0, tableRange).length > 0 }) else { throw invalid("paragraph/table overlap") }
            let records = try objects(record["cells"], limit: 240)
            guard !records.isEmpty && records.count % columns == 0 else { throw invalid("table must be rectangular") }
            var end = tableRange.location, cells: [NSRange] = []
            for cell in records {
                guard Set(cell.keys) == ["start", "length"] else { throw invalid("invalid table cell") }
                let cellRange = try range(cell, text: text, after: end)
                guard cellRange.location == end, NSMaxRange(cellRange) <= NSMaxRange(tableRange),
                      text.character(at: NSMaxRange(cellRange) - 1) == 10 else { throw invalid("table cells must partition the table and end with a newline") }
                cells.append(cellRange)
                end = NSMaxRange(cellRange)
            }
            guard end == NSMaxRange(tableRange) else { throw invalid("incomplete table") }
            applyTable(to: value, columns: columns, cells: cells)
        }
    }

    static func encode(_ value: NSAttributedString, inlineFormatting: String) throws -> String {
        var paragraphsJSON: [[String: Any]] = []
        for range in paragraphs(value.string) {
            let attrs = value.attributes(at: range.location, effectiveRange: nil)
            guard tableBlock(in: attrs) == nil else { continue }
            let heading = attrs[headingKey] as? String
            let kind = list(in: attrs)
            if heading == nil && kind == nil { continue }
            var record: [String: Any] = ["start": range.location, "length": range.length]
            if let heading { record["style"] = heading }
            if let kind { record["list"] = kind.rawValue }
            paragraphsJSON.append(record)
        }
        let tablesJSON: [[String: Any]] = tables(in: value).map { table in
            ["start": table.range.location, "length": table.range.length, "columns": table.object.numberOfColumns,
             "cells": table.cells.map { ["start": $0.location, "length": $0.length] }]
        }
        if paragraphsJSON.isEmpty && tablesJSON.isEmpty { return inlineFormatting }
        let base = inlineFormatting.isEmpty ? ["runs": []] : try JSONSerialization.jsonObject(with: Data(inlineFormatting.utf8)) as! [String: Any]
        let document: [String: Any] = ["version": 2, "runs": base["runs"]!, "paragraphs": paragraphsJSON, "tables": tablesJSON]
        // Validate native edits before publication, not just files on open.
        let checked = NSMutableAttributedString(string: value.string)
        try apply(document, to: checked)
        return try json(document)
    }

    static func prefixLength(_ text: String, kind: ListKind) -> Int {
        if kind == .bullet { return text.hasPrefix("•\t") ? 2 : 0 }
        let chars = Array(text.utf16)
        var index = 0
        while index < chars.count && chars[index] >= 48 && chars[index] <= 57 { index += 1 }
        return index > 0 && chars[0] != 48 && index + 1 < chars.count && chars[index] == 46 && chars[index + 1] == 9 ? index + 2 : 0
    }

    private static func range(_ record: [String: Any], text: NSString, after previous: Int) throws -> NSRange {
        guard let start = integer(record["start"]), let length = integer(record["length"]),
              start >= previous, start <= text.length, length > 0, length <= text.length - start,
              start == 0 || text.character(at: start - 1) == 10,
              start + length == text.length || text.character(at: start + length - 1) == 10 else { throw invalid("invalid paragraph boundary") }
        return NSRange(location: start, length: length)
    }
    private static func integer(_ value: Any?) -> Int? {
        guard let number = value as? NSNumber, CFGetTypeID(number) == CFNumberGetTypeID() else { return nil }
        return Int(number.stringValue)
    }
    private static func objects(_ value: Any?, limit: Int) throws -> [[String: Any]] {
        guard let value else { return [] }
        guard let objects = value as? [[String: Any]], objects.count <= limit else { throw invalid("too many or malformed structural records") }
        return objects
    }
    private static func invalid(_ message: String) -> RichTextCodecError { .invalidSchema(message) }
    private static func json(_ object: [String: Any]) throws -> String {
        String(decoding: try JSONSerialization.data(withJSONObject: object, options: [.sortedKeys]), as: UTF8.self)
    }
}
