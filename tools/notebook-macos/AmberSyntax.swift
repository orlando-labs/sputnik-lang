import Foundation

#if canImport(AppKit)
import AppKit
#endif

/// The small, lossless lexical surface used by the notebook editor.
///
/// AmberSyntax intentionally does not ask the compiler to parse the source:
/// editors must be useful while a cell is incomplete or temporarily invalid.
public enum AmberSyntaxCategory: String, CaseIterable {
    case keyword
    case constant
    case className
    case number
    case string
    case comment
    case variable
    case identifier
    case `operator`
    case punctuation
}

public struct AmberSyntaxToken: Equatable {
    public let range: NSRange
    public let category: AmberSyntaxCategory
    public let text: String

    public init(range: NSRange, category: AmberSyntaxCategory, text: String = "") {
        self.range = range
        self.category = category
        self.text = text
    }

    /// A spelling that is convenient for callers that use the token as a
    /// lexical token rather than as a styling instruction.
    public var lexeme: String { text }
    public var kind: AmberSyntaxCategory { category }
}

public enum AmberSyntax {
    public typealias Token = AmberSyntaxToken
    public typealias Category = AmberSyntaxCategory

    public static func tokens(in source: String) -> [AmberSyntaxToken] {
        Scanner(source).scan()
    }

#if canImport(AppKit)
    public static let defaultFont = NSFont.monospacedSystemFont(ofSize: 13,
                                                                 weight: .regular)
    public static let defaultColor = NSColor(calibratedRed: 0.153,
                                             green: 0.161,
                                             blue: 0.166,
                                             alpha: 1)

    /// Applies syntax colors without changing the string. Callers that own an
    /// NSTextView should disable undo registration around this method; this
    /// method itself only changes attributes and never calls a text mutation
    /// API.
    public static func apply(to textStorage: NSTextStorage,
                             source: String? = nil,
                             font: NSFont? = nil,
                             baseColor: NSColor? = nil) {
        let source = source ?? textStorage.string
        let stringLength = (source as NSString).length
        guard textStorage.length == stringLength else { return }

        let baseFont = font ?? defaultFont
        let ink = baseColor ?? defaultColor
        let baseAttributes: [NSAttributedString.Key: Any] = [
            .font: baseFont,
            .foregroundColor: ink,
        ]
        if stringLength > 0 {
            textStorage.setAttributes(baseAttributes,
                                      range: NSRange(location: 0, length: stringLength))
        }

        for token in tokens(in: source) {
            guard token.range.location >= 0,
                  NSMaxRange(token.range) <= stringLength else { continue }
            textStorage.addAttribute(.foregroundColor,
                                     value: color(for: token.category),
                                     range: token.range)
        }
    }

    public static func color(for category: AmberSyntaxCategory) -> NSColor {
        switch category {
        case .keyword:
            return NSColor(calibratedRed: 0.665, green: 0.255, blue: 0.158, alpha: 1)
        case .constant:
            return NSColor(calibratedRed: 0.470, green: 0.290, blue: 0.475, alpha: 1)
        case .className:
            return NSColor(calibratedRed: 0.185, green: 0.365, blue: 0.505, alpha: 1)
        case .number:
            return NSColor(calibratedRed: 0.645, green: 0.350, blue: 0.145, alpha: 1)
        case .string:
            return NSColor(calibratedRed: 0.205, green: 0.435, blue: 0.285, alpha: 1)
        case .comment:
            return NSColor(calibratedRed: 0.455, green: 0.455, blue: 0.405, alpha: 1)
        case .variable:
            return NSColor(calibratedRed: 0.585, green: 0.310, blue: 0.150, alpha: 1)
        case .identifier:
            return defaultColor
        case .operator:
            return NSColor(calibratedRed: 0.315, green: 0.290, blue: 0.270, alpha: 1)
        case .punctuation:
            return NSColor(calibratedRed: 0.390, green: 0.365, blue: 0.335, alpha: 1)
        }
    }
#endif
}

private struct AmberScalar {
    let value: UInt32
    let utf16Start: Int
    let utf16End: Int
}

private final class Scanner {
    private let source: String
    private let scalars: [AmberScalar]
    private var index = 0
    private var lineStart = true
    private var lineIndent = 0
    private var bracketDepth = 0
    private var macroState = 0
    private var macroHeaderIndent = 0
    private var macroBodyIndent = 0
    private var macroInlineTokens = 0
    private var expectingClassName = false
    private var result: [AmberSyntaxToken] = []

    private static let keywords: Set<String> = [
        "and", "attr", "break", "catch", "case", "case!", "class",
        "class_method", "class_prop", "def", "do", "elif", "else",
        "elsif", "export", "ensure", "extend", "false", "from", "if",
        "import", "in", "include", "loop", "mixin", "not", "null", "or",
        "package", "prop", "raise", "rescue", "return", "throw", "try",
        "true", "unless", "until", "when", "while",
    ]

    // These words are contextual in the parser, but highlighting them makes
    // the editor agree with Amber's shipped Vim/VS Code grammars.
    private static let contextualKeywords: Set<String> = [
        "as", "get", "set", "var", "native", "macro", "string_tag", "next",
        "pattern", "then", "with", "numeric", "quote", "owned", "borrowed",
        "collected",
    ]

    init(_ source: String) {
        self.source = source
        var offset = 0
        self.scalars = source.unicodeScalars.map { scalar in
            let width = scalar.value > 0xFFFF ? 2 : 1
            defer {
                offset += width
            }
            return AmberScalar(value: scalar.value,
                               utf16Start: offset,
                               utf16End: offset + width)
        }
    }

    func scan() -> [AmberSyntaxToken] {
        while index < scalars.count {
            if lineStart {
                prepareLine()
            }

            let value = current
            if value == 32 || value == 9 {
                index += 1
                continue
            }
            if value == 10 || value == 13 {
                index += 1
                if value == 13, current == 10 { index += 1 }
                if macroState == 2 || (macroState == 3 && macroInlineTokens > 0) {
                    macroState = 0
                }
                lineStart = true
                continue
            }

            if value == 35, peekValue == 123, macroIsActive {
                emit(start: index, end: index + 2, category: .punctuation)
                index += 2
                bracketDepth += 1
                recordMacroToken()
                continue
            }
            if value == 35, commentStartsHere {
                let start = index
                while index < scalars.count && current != 10 && current != 13 {
                    index += 1
                }
                emit(start: start, end: index, category: .comment)
                continue
            }

            if isIdentifierStart(value) {
                scanIdentifier()
                continue
            }
            if isDigit(value) {
                scanNumber()
                continue
            }
            if value == 34 {
                scanString(start: index, quoteIndex: index, raw: false)
                continue
            }
            if value == 39 {
                scanString(start: index, quoteIndex: index, raw: false)
                continue
            }
            if value == 64, scanAtVariable() {
                continue
            }
            if value == 36, scanDollarVariable() {
                continue
            }

            if let (length, category) = operatorOrPunctuationLength(at: index) {
                let start = index
                index += length
                if category == .punctuation {
                    if value == 40 || value == 91 || value == 123 { bracketDepth += 1 }
                    if value == 41 || value == 93 || value == 125 { bracketDepth = max(0, bracketDepth - 1) }
                }
                emit(start: start, end: index, category: category)
                if value == 58, macroState == 2, bracketDepth == 0 {
                    macroState = 3
                    macroInlineTokens = 0
                } else {
                    recordMacroToken()
                }
                continue
            }

            // Keep malformed input visible and keep scanning after it. Amber's
            // compiler reports the error later; an editor should not stop.
            emit(start: index, end: index + 1, category: .punctuation)
            index += 1
        }
        return result
    }

    private var current: UInt32 { index < scalars.count ? scalars[index].value : 0 }
    private var peekValue: UInt32 { index + 1 < scalars.count ? scalars[index + 1].value : 0 }
    private var macroIsActive: Bool { macroState == 3 || macroState == 4 }

    private var commentStartsHere: Bool {
        guard current == 35 else { return false }
        if macroIsActive && peekValue == 123 { return false }
        if index == 0 { return true }
        // A first-non-space hash is necessarily preceded by indentation or a
        // newline. Avoid rescanning the whole line for each callable #method.
        let previous = scalars[index - 1].value
        return previous == 32 || previous == 9 || previous == 10 || previous == 13
    }

    private func prepareLine() {
        var cursor = index
        var indent = 0
        while cursor < scalars.count && (scalars[cursor].value == 32 || scalars[cursor].value == 9) {
            indent += 1
            cursor += 1
        }
        lineIndent = indent
        let first = cursor < scalars.count ? scalars[cursor].value : 0
        let lineLeadingSplice = first == 35 && cursor + 1 < scalars.count &&
            scalars[cursor + 1].value == 123
        let blankOrComment = first == 0 || first == 10 || first == 13 ||
            (first == 35 && !lineLeadingSplice)
        if macroState == 4 && !blankOrComment && indent <= macroHeaderIndent {
            macroState = 0
        } else if macroState == 3 && !blankOrComment && indent > macroHeaderIndent {
            macroState = 4
            macroBodyIndent = indent
        }
        lineStart = false
    }

    private func scanIdentifier() {
        let start = index
        while index < scalars.count && isIdentifierPart(current) { index += 1 }
        let word = substring(start, index)

        // In Amber `r` is a lexical raw-string tag only when immediately
        // adjacent to its delimiter. Treat the whole spelling as a string so
        // neither a keyword nor a comment can leak out of it.
        if word == "r", index < scalars.count, current == 34 || current == 39 {
            scanString(start: start, quoteIndex: index, raw: true)
            return
        }

        let category: AmberSyntaxCategory
        if expectingClassName {
            category = .className
            expectingClassName = false
        } else if Self.keywords.contains(word) || Self.contextualKeywords.contains(word) {
            category = word == "true" || word == "false" || word == "null" ? .constant : .keyword
        } else if startsWithUppercase(word) {
            category = .className
        } else if word == "self" {
            category = .constant
        } else if isPlaceholder(word) {
            category = .variable
        } else {
            category = .identifier
        }
        emit(start: start, end: index, category: category)

        if word == "class" || word == "mixin" { expectingClassName = true }
        recordMacroIdentifier(word, category: category)
    }

    private func scanNumber() {
        let start = index
        var isFloat = false
        if current == 48 && (peekValue == 120 || peekValue == 88 ||
                             peekValue == 98 || peekValue == 66 ||
                             peekValue == 111 || peekValue == 79) {
            index += 2
            while index < scalars.count && (isDigit(current) || isHexDigit(current) || current == 95) {
                index += 1
            }
            emit(start: start, end: index, category: .number)
            recordMacroToken()
            return
        }
        while index < scalars.count && (isDigit(current) || current == 95) { index += 1 }
        if current == 46 && peekValue != 46 && (isDigit(peekValue) || peekValue == 95) {
            isFloat = true
            index += 1
            while index < scalars.count && (isDigit(current) || current == 95) { index += 1 }
        }
        if current == 101 || current == 69 {
            isFloat = true
            index += 1
            if current == 43 || current == 45 { index += 1 }
            while index < scalars.count && (isDigit(current) || current == 95) { index += 1 }
        }
        _ = isFloat // Both numeric forms share the notebook's warm number color.
        emit(start: start, end: index, category: .number)
        recordMacroToken()
    }

    private func scanString(start: Int, quoteIndex: Int, raw: Bool) {
        let triple = quoteIndex + 2 < scalars.count &&
            scalars[quoteIndex].value == 34 && scalars[quoteIndex + 1].value == 34 &&
            scalars[quoteIndex + 2].value == 34
        let quote = scalars[quoteIndex].value
        index = quoteIndex + (triple ? 3 : 1)
        while index < scalars.count {
            if triple && quote == 34 && hasTripleQuote {
                index += 3
                break
            }
            if !triple && current == quote {
                index += 1
                break
            }
            if current == 92 {
                index += 1
                if index < scalars.count { index += 1 }
                continue
            }
            if !raw && quote == 34 && current == 35 && peekValue == 123 {
                consumeInterpolation()
                continue
            }
            if !triple && (current == 10 || current == 13) { break }
            index += 1
        }
        emit(start: start, end: index, category: .string)
        recordMacroToken()
    }

    private var hasTripleQuote: Bool {
        index + 2 < scalars.count && current == 34 && peekValue == 34 &&
            scalars[index + 2].value == 34
    }

    private func consumeInterpolation() {
        index += 2
        var depth = 0
        while index < scalars.count {
            if current == 92 {
                index += 1
                if index < scalars.count { index += 1 }
                continue
            }
            if current == 34 || current == 39 {
                consumeNestedString(quote: current)
                continue
            }
            if current == 123 {
                depth += 1
            } else if current == 125 {
                if depth == 0 {
                    index += 1
                    return
                }
                depth -= 1
            }
            index += 1
        }
    }

    private func consumeNestedString(quote: UInt32) {
        index += 1
        while index < scalars.count {
            if current == 92 {
                index += 1
                if index < scalars.count { index += 1 }
            } else if current == quote {
                index += 1
                return
            } else {
                index += 1
            }
        }
    }

    private func scanAtVariable() -> Bool {
        let start = index
        var cursor = index + 1
        if cursor < scalars.count && scalars[cursor].value == 64 { cursor += 1 }
        guard cursor < scalars.count, isIdentifierStart(scalars[cursor].value) else { return false }
        cursor += 1
        while cursor < scalars.count && isIdentifierPart(scalars[cursor].value) { cursor += 1 }
        if cursor < scalars.count && (scalars[cursor].value == 63 || scalars[cursor].value == 33) { cursor += 1 }
        index = cursor
        emit(start: start, end: index, category: .variable)
        recordMacroToken()
        return true
    }

    private func scanDollarVariable() -> Bool {
        let start = index
        guard peekValue == 95 || (peekValue == 105 && index + 2 < scalars.count && scalars[index + 2].value == 116) else { return false }
        index += peekValue == 95 ? 2 : 3
        if peekValue != 0 && isDigit(current) {
            while index < scalars.count && isDigit(current) { index += 1 }
        }
        emit(start: start, end: index, category: .variable)
        recordMacroToken()
        return true
    }

    private func operatorOrPunctuationLength(at position: Int) -> (Int, AmberSyntaxCategory)? {
        let operators = [".?.", "...", "<=>", "===", "**", "//", "=~", "==", "!~", "!=", "<<", ">>", "<=", ">=", "->", ".."]
        for spelling in operators {
            if matches(spelling, at: position) { return (spelling.unicodeScalars.count, .operator) }
        }
        switch current {
        case 46, 43, 45, 42, 47, 37, 61, 33, 60, 62, 38, 124, 94, 63, 35:
            return (1, .operator)
        case 40, 41, 91, 93, 123, 125, 58, 44, 59:
            return (1, .punctuation)
        default:
            return nil
        }
    }

    private func matches(_ spelling: String, at position: Int) -> Bool {
        let values = spelling.unicodeScalars.map(\.value)
        guard position + values.count <= scalars.count else { return false }
        for offset in 0..<values.count where scalars[position + offset].value != values[offset] { return false }
        return true
    }

    private func emit(start: Int, end: Int, category: AmberSyntaxCategory) {
        guard end > start, start < scalars.count else { return }
        let boundedEnd = min(end, scalars.count)
        let range = NSRange(location: scalars[start].utf16Start,
                            length: scalars[boundedEnd - 1].utf16End - scalars[start].utf16Start)
        result.append(AmberSyntaxToken(range: range,
                                       category: category,
                                       text: substring(start, boundedEnd)))
    }

    private func substring(_ start: Int, _ end: Int) -> String {
        let ns = source as NSString
        let location = start < scalars.count ? scalars[start].utf16Start : ns.length
        let finish = end > start && end - 1 < scalars.count ? scalars[end - 1].utf16End : location
        return ns.substring(with: NSRange(location: location, length: max(0, finish - location)))
    }

    private func recordMacroIdentifier(_ word: String, category: AmberSyntaxCategory) {
        switch macroState {
        case 0:
            if word == "macro" {
                macroState = 1
                macroHeaderIndent = lineIndent
            }
        case 1:
            if word == "def" { macroState = 2 }
            else if word != "macro" { macroState = 0 }
        case 2:
            if word == "macro" { macroState = 1 }
        case 3:
            _ = category
            macroInlineTokens += 1
        case 4:
            _ = category
        default:
            break
        }
    }

    private func recordMacroToken() {
        if macroState == 3 { macroInlineTokens += 1 }
    }

    private func isPlaceholder(_ word: String) -> Bool {
        guard word.count > 1, word.first == "_" else { return false }
        return word.dropFirst().allSatisfy { $0.isNumber }
    }

    private func startsWithUppercase(_ word: String) -> Bool {
        guard let first = word.unicodeScalars.first else { return false }
        if first.value >= 65 && first.value <= 90 { return true }
        guard let scalar = UnicodeScalar(first.value) else { return false }
        let spelling = String(scalar)
        return spelling != spelling.lowercased() && spelling == spelling.uppercased()
    }

    private func isIdentifierStart(_ value: UInt32) -> Bool {
        let ascii = (value >= 65 && value <= 90) || (value >= 97 && value <= 122)
        let cyrillic = (value >= 0x0400 && value <= 0x052F) ||
            (value >= 0x1C80 && value <= 0x1C8F) ||
            (value >= 0x2DE0 && value <= 0x2DFF) ||
            (value >= 0xA640 && value <= 0xA69F)
        let greek = (value >= 0x0370 && value <= 0x03FF) ||
            (value >= 0x1F00 && value <= 0x1FFF)
        return ascii || cyrillic || greek || value == 95
    }

    private func isIdentifierPart(_ value: UInt32) -> Bool {
        isIdentifierStart(value) || isDigit(value)
    }

    private func isDigit(_ value: UInt32) -> Bool { value >= 48 && value <= 57 }

    private func isHexDigit(_ value: UInt32) -> Bool {
        isDigit(value) || (value >= 65 && value <= 70) || (value >= 97 && value <= 102)
    }
}
