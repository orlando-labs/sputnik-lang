import SwiftUI

/// Progress is presentation, not a committed notebook variable. Stopping never
/// paints an unfinished bar as 100%; its last count stays inspectable.
struct NotebookProgressView: View {
    let items: [NotebookProgress]
    let running: Bool

    private func number(_ value: Double) -> String {
        value.formatted(.number.precision(.fractionLength(0...1)))
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            ForEach(items) { item in
                let complete = item.done && (item.total == 0 || item.current >= item.total)
                VStack(alignment: .leading, spacing: 7) {
                    HStack {
                        Label(item.description.isEmpty ? "Progress" : item.description,
                              systemImage: complete ? "checkmark.circle.fill" : "waveform.path")
                            .font(.system(size: 12, weight: .semibold))
                        Spacer()
                        Text(complete ? "Finished" : item.done || !running ? "Stopped" : "Running")
                            .font(.system(size: 10, weight: .medium))
                            .foregroundStyle(.secondary)
                    }
                    if item.total > 0 {
                        ProgressView(value: min(item.current, item.total), total: item.total)
                            .tint(complete ? Color.green : Color.accentColor)
                    } else if running && !item.done {
                        ProgressView().controlSize(.small)
                    }
                    HStack {
                        Text(item.total > 0 ? "\(number(item.current)) / \(number(item.total))" : number(item.current))
                        if item.total > 0 {
                            Text("· \(number(min(100, item.current / item.total * 100)))%")
                        }
                        Spacer()
                        if item.elapsed_ms > 0 {
                            let seconds = Double(item.elapsed_ms) / 1000
                            Text("\(number(seconds)) s · \(number(item.current / seconds))/s")
                        }
                    }
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundStyle(.secondary)
                }
                .accessibilityElement(children: .combine)
                .accessibilityLabel("\(item.description): \(number(item.current)) of \(number(item.total))")
            }
        }
        .padding(13)
        .background(Color.accentColor.opacity(0.055), in: RoundedRectangle(cornerRadius: 9))
    }
}
