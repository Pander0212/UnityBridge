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
    /// Append (never truncate) so a crash's log survives the relaunch, and mark each launch so the
    /// latest run is easy to find. Unbuffered, so every line is on disk even if the app then dies.
    static func redirectStderr() {
        freopen(url.path, "a", stderr)  // "a" creates the file if missing; it is never truncated
        setvbuf(stderr, nil, _IONBF, 0)
        // Which build is running: commit and CI run, written into the bundle by CI "Probe IPA".
        let build = Bundle.main.url(forResource: "build-info", withExtension: "txt")
            .flatMap { try? String(contentsOf: $0, encoding: .utf8) }?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? "unknown build"
        FileHandle.standardError.write(Data("\n==== UB Probe launch \(Date()) (\(build)) ====\n".utf8))
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
                        .disabled(running || jit != .ready || il2cppStarted)
                }
                Button("Run il2cpp_init (GameAssembly.dll)", action: { runIl2cpp(engine: false) })
                    .buttonStyle(.bordered)
                    .disabled(running || jit != .ready || il2cppStarted || windowsDLLRan)
                // M3e: the Unity engine (under Husk) starts il2cpp itself, with the game's data.
                Button("Run Unity engine (libunity starts il2cpp)", action: { runIl2cpp(engine: true) })
                    .buttonStyle(.bordered)
                    .disabled(running || jit != .ready || il2cppStarted || windowsDLLRan)
                // The il2cpp runtime stays up once started, and the two tests share the mini Win32:
                // each needs a fresh launch after the other.
                if il2cppStarted {
                    Text("il2cpp is running in this app session. Close and reopen the app to run another test.")
                        .font(.footnote).foregroundStyle(.secondary)
                } else if windowsDLLRan {
                    Text("To run il2cpp_init, close and reopen the app first.")
                        .font(.footnote).foregroundStyle(.secondary)
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

    // Re-read on every render; `running` flips when a test finishes, which renders again.
    private var il2cppStarted: Bool { ub_il2cpp_probe_started() != 0 }
    private var windowsDLLRan: Bool { ub_win32_device_has_run() != 0 }

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

    /// First real IL2CPP probe: loads Fellworld's own GameAssembly.dll and calls il2cpp_init.
    /// The user places GameAssembly.dll and the game's *_Data folder in
    /// Files › On My iPhone › UB Probe › Game. No game file ships in the app.
    private func runIl2cpp(engine: Bool) {
        jit = JITState.current
        guard jit == .ready else { return }
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let gameDir = docs.appendingPathComponent("Game", isDirectory: true)
        try? FileManager.default.createDirectory(at: gameDir, withIntermediateDirectories: true)
        let dll = gameDir.appendingPathComponent("GameAssembly.dll")
        guard FileManager.default.fileExists(atPath: dll.path) else {
            report = """
            GameAssembly.dll not found.

            In the Files app: On My iPhone › UB Probe › Game
            Put there:
              • GameAssembly.dll
              • the game's *_Data folder (e.g. Fellworld_Data), with
                il2cpp_data/Metadata/global-metadata.dat inside

            Then tap this button again.
            Folder: \(gameDir.path)
            """
            return
        }
        // il2cpp appends "/Metadata/global-metadata.dat" to the data dir, and that file lives in
        // <Game>_Data/il2cpp_data (Unity 2021.3). So point at the folder that contains "Metadata":
        // prefer <Game>_Data/il2cpp_data, then <Game>_Data, then the Game folder.
        var dataDir = gameDir
        if let entries = try? FileManager.default.contentsOfDirectory(
            at: gameDir, includingPropertiesForKeys: [.isDirectoryKey]),
           let data = entries.first(where: { $0.lastPathComponent.hasSuffix("_Data") }) {
            dataDir = data
        }
        let gameData = dataDir  // the <Game>_Data folder: the engine's data (M3e)
        let il2cppData = dataDir.appendingPathComponent("il2cpp_data", isDirectory: true)
        if FileManager.default.fileExists(atPath: il2cppData.appendingPathComponent("Metadata").path) {
            dataDir = il2cppData
        }
        let profile = docs.appendingPathComponent("Profile", isDirectory: true)
        try? FileManager.default.createDirectory(at: profile, withIntermediateDirectories: true)
        // M3d: Unity's Android engine (libmain.so, libunity.so from Unity's Android player, supplied by
        // the user like the game files) under Husk's runtime, once il2cpp is up.
        let unityDir = docs.appendingPathComponent("Unity", isDirectory: true)
        try? FileManager.default.createDirectory(at: unityDir, withIntermediateDirectories: true)
        let unityData = docs.appendingPathComponent("UnityAppData", isDirectory: true)
        let haveUnity = FileManager.default.fileExists(atPath: unityDir.appendingPathComponent("libunity.so").path)
        running = true
        report = ""
        Thread.detachNewThread {
            // Husk's region first, while the address space is still unfragmented: its writable
            // view must end up within 3 GiB of the executable one.
            if haveUnity { _ = husk_ios_jit_get_mapping() }
            ub_il2cpp_probe_set_engine_mode(engine ? 1 : 0)
            var buffer = [CChar](repeating: 0, count: 65536)
            let status = ub_il2cpp_probe_run(dll.path, dataDir.path, profile.path, &buffer, buffer.count)
            var text = String(cString: buffer) + "\nexit status \(status)"
            if status == 0 && engine {
                var unity = [CChar](repeating: 0, count: 16384)
                let unityStatus = ub_unity_engine_run(unityDir.path, gameData.path, unityData.path, &unity, unity.count)
                text += "\n\n" + String(cString: unity) + "\nUnity exit status \(unityStatus)"
            } else if status == 0 {
                var unity = [CChar](repeating: 0, count: 16384)
                let unityStatus = ub_unity_probe_run(unityDir.path, unityData.path, &unity, unity.count)
                text += "\n\n" + String(cString: unity) + "\nUnity exit status \(unityStatus)"
            }
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
