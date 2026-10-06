import Foundation

struct NotebookProjectInput: Decodable, Identifiable {
    let id: String, title: String, type: String, value_json: String
    let minimum: Double?, maximum: Double?

    var editingText: String {
        if type == "string", let value = try? JSONDecoder().decode(String.self, from: Data(value_json.utf8)) {
            return value
        }
        return value_json
    }
    static func valueJSON(_ text: String, type: String) throws -> String {
        if type == "string" { return String(decoding: try JSONEncoder().encode(text), as: UTF8.self) }
        // Preserve the spelling of integers. The backend validates types,
        // range and finite numeric values before publishing anything.
        return text.trimmingCharacters(in: .whitespacesAndNewlines)
    }
}

struct NotebookBoardComponent: Codable, Identifiable {
    var id: String, kind: String, title: String
    var input: String?, cell: String?, binding: String?
}

struct NotebookBoard: Codable, Identifiable {
    var id: String, title: String, sheet: String
    var columns: Int
    var components: [NotebookBoardComponent]

    static func empty(sheet: String) -> NotebookBoard {
        NotebookBoard(id: "board_" + UUID().uuidString.lowercased().replacingOccurrences(of: "-", with: "_"), title: "New Board",
                      sheet: sheet, columns: 2, components: [])
    }
}
