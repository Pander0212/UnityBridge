// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI
import UIKit

/// M2b: checks that FEXCore runs x86-64 code on this iPhone. JIT must be enabled for the app with
/// StikDebug first; the probe refuses to start without it (the JIT request would kill the app).
@main
struct ProbeApp: App {
    init() { ProbeLog.redirectStderr() }

    var body: some Scene {
        WindowGroup { ProbeView() }
    }
}

enum ProbeLog {
    /// Documents/probe.log, visible under Files › On My iPhone › UB Probe.
    static let url = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("probe.log")

    /// FEXCore and the engine write diagnostics to stderr; keep them in a file the user can share.
    static func redirectStderr() {
        FileManager.default.createFile(atPath: url.path, contents: nil)
        freopen(url.path, "a", stderr)
        setvbuf(stderr, nil, _IONBF, 0)
    }
}

struct ProbeView: View {
    @State private var report = ""
    @State private var running = false
    @State private var jit = ub_probe_jit_attached()

    var body: some View {
        NavigationStack {
            VStack(alignment: .leading, spacing: 12) {
                Label(jit ? "JIT is enabled" : "JIT is off: enable it for this app in StikDebug",
                      systemImage: jit ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
                    .foregroundStyle(jit ? .green : .orange)
                Button(running ? "Running…" : "Run x86-64 probe", action: run)
                    .buttonStyle(.borderedProminent)
                    .disabled(running || !jit)
                ScrollView {
                    Text(report.isEmpty ? "No result yet." : report)
                        .font(.system(.footnote, design: .monospaced))
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .textSelection(.enabled)
                }
                HStack {
                    Button("Copy result") { UIPasteboard.general.string = report + "\n\n" + logTail() }
                    Spacer()
                    ShareLink(item: ProbeLog.url) { Label("Share log", systemImage: "square.and.arrow.up") }
                }
            }
            .padding()
            .navigationTitle("UnityBridge Probe")
            .onReceive(NotificationCenter.default.publisher(for: UIApplication.didBecomeActiveNotification)) { _ in
                jit = ub_probe_jit_attached()
            }
        }
    }

    private func run() {
        running = true
        report = ""
        Thread.detachNewThread {
            var buffer = [CChar](repeating: 0, count: 8192)
            let status = ub_probe_run(&buffer, buffer.count)
            let text = String(cString: buffer) + "\nexit status \(status)"
            DispatchQueue.main.async {
                report = text
                running = false
            }
        }
    }

    private func logTail() -> String {
        guard let text = try? String(contentsOf: ProbeLog.url, encoding: .utf8) else { return "" }
        return text.split(separator: "\n", omittingEmptySubsequences: false).suffix(80).joined(separator: "\n")
    }
}
