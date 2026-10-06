import Foundation

struct NotebookTextVariable: Identifiable {
    let name: String
    let value: String
    let cellID: String
    let stale: Bool

    var id: String { name }
}
