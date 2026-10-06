import SwiftUI

/// Secondary object actions share one quiet, consistently placed entry point.
struct NotebookObjectMenu<Content: View>: View {
    let title: String
    @ViewBuilder var content: () -> Content

    var body: some View {
        Menu(content: content) {
            Image(systemName: "ellipsis")
                .font(.system(size: 14, weight: .semibold))
                .frame(width: 26, height: 24)
                .contentShape(Rectangle())
        }
        .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
        .foregroundStyle(.secondary)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 5))
        .help(title).accessibilityLabel(title)
    }
}
