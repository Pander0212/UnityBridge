// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI
import UIKit

/// M2b: checks that FEXCore runs x86-64 code on this iPhone.
///
/// JIT on iOS 26 (TXM devices) needs a debugger that stays attached and answers the app's JIT
/// requests (brk #0xf00d). Enabling JIT from StikDebug's own app list attaches and leaves, so the
/// app asks StikDebug itself, passing its process ID and the debugger script (madeira-jit.js).
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

    static func write(_ line: String) {
        FileHandle.standardError.write(Data(("UB_APP " + line + "\n").utf8))
    }
}

enum JITState: Equatable {
    case off            // never debugged
    case noDebugger     // CS_DEBUGGED set, but no debugger attached now
    case ready          // debugger attached: JIT requests are answered

    static var current: JITState {
        if !ub_probe_jit_attached() { return .off }
        return ub_probe_debugger_attached() ? .ready : .noDebugger
    }
}

enum StikDebug {
    static var scriptData: Data? {
        Bundle.main.url(forResource: "madeira-jit", withExtension: "js").flatMap { try? Data(contentsOf: $0) }
    }

    /// stikdebug://enable-jit?bundle-id=…&pid=…&script-data=<base64>; StikDebug attaches to this very
    /// process and keeps the script running.
    static func requestURL() -> URL? {
        guard let bundleID = Bundle.main.bundleIdentifier, let script = scriptData else { return nil }
        var components = URLComponents()
        components.scheme = "stikdebug"
        components.host = "enable-jit"
        components.queryItems = [
            URLQueryItem(name: "bundle-id", value: bundleID),
            URLQueryItem(name: "pid", value: String(getpid())),
            URLQueryItem(name: "script-data", value: script.base64EncodedString()),
        ]
        return components.url
    }
}

struct ProbeView: View {
    @State private var report = ""
    @State private var running = false
    @State private var jit = JITState.current
    @State private var waiting = false
    @State private var message = ""

    var body: some View {
        NavigationStack {
            VStack(alignment: .leading, spacing: 12) {
                statusLabel
                if jit != .ready {
                    Button(waiting ? "Waiting for StikDebug…" : "Enable JIT", action: enableJIT)
                        .buttonStyle(.bordered)
                        .disabled(waiting)
                }
                if !message.isEmpty {
                    Text(message).font(.footnote).foregroundStyle(.secondary)
                }
                HStack {
                    Button(running ? "Running…" : "Run x86-64 probe", action: run)
                        .buttonStyle(.borderedProminent)
                        .disabled(running || jit != .ready)
                    Button("Run Windows DLL test", action: runWindowsDLL)
                        .buttonStyle(.bordered)
                        .disabled(running || jit != .ready)
                }
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
                jit = JITState.current
            }
        }
    }

    @ViewBuilder private var statusLabel: some View {
        switch jit {
        case .ready:
            Label("JIT ready: StikDebug is attached", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
        case .noDebugger:
            Label("JIT flag set, but StikDebug is not attached any more. Tap Enable JIT.",
                  systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange)
        case .off:
            Label("JIT is off. Tap Enable JIT (needs StikDebug, its pairing file and LocalDevVPN).",
                  systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange)
        }
    }

    private func enableJIT() {
        guard let url = StikDebug.requestURL() else {
            message = "The JIT script is missing from this build."
            return
        }
        ProbeLog.write("opening StikDebug for pid \(getpid())")
        waiting = true
        message = "StikDebug opens and attaches to this app. Come back here when it is done."
        UIApplication.shared.open(url) { opened in
            guard opened else {
                waiting = false
                message = "StikDebug could not be opened. Is it installed?"
                return
            }
            let deadline = Date().addingTimeInterval(90)
            Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { timer in
                jit = JITState.current
                if jit == .ready || Date() >= deadline {
                    timer.invalidate()
                    waiting = false
                    message = jit == .ready ? "" : "StikDebug did not attach within 90 seconds. Check LocalDevVPN and the pairing file."
                    ProbeLog.write("JIT state after waiting: \(jit)")
                }
            }
        }
    }

    private func run() {
        jit = JITState.current
        guard jit == .ready else { return }
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

    /// M2d: our own Windows test DLL (tests/testdll, bundled) under FEXCore against the mini Win32.
    private func runWindowsDLL() {
        jit = JITState.current
        guard jit == .ready else { return }
        guard let dll = Bundle.main.url(forResource: "ubtest", withExtension: "dll") else {
            report = "ubtest.dll is missing from this build."
            return
        }
        let profile = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Profile", isDirectory: true)
        try? FileManager.default.createDirectory(at: profile, withIntermediateDirectories: true)
        running = true
        report = ""
        Thread.detachNewThread {
            var buffer = [CChar](repeating: 0, count: 16384)
            let status = ub_win32_device_run(dll.path, profile.path, &buffer, buffer.count)
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
