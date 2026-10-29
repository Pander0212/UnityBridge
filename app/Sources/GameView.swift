// SPDX-License-Identifier: GPL-3.0-or-later
import QuartzCore
import SwiftUI
import UIKit

/// M5: the game on the screen. A view backed by a CAMetalLayer, which the Unity engine (through Husk
/// and ANGLE) presents into instead of writing BMP frames, and whose touches go to the engine.
final class GameLayerView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }

    /// One view for the whole session: the engine keeps presenting into its layer after the game
    /// screen is closed, and "Show game" puts the same view back.
    static let shared = GameLayerView(frame: .zero)

    /// Called once, with the layer and its size in pixels, when the view is laid out in landscape
    /// (or, if the screen does not turn, when `reportNow` is called).
    var onReady: ((CAMetalLayer, Int32, Int32) -> Void)?
    private var reported = false
    private var fingers: [ObjectIdentifier: Int32] = [:]
    private var nextFinger: Int32 = 0

    override init(frame: CGRect) {
        super.init(frame: frame)
        isMultipleTouchEnabled = true
        backgroundColor = .black
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    override func layoutSubviews() {
        super.layoutSubviews()
        let scale = window?.screen.scale ?? UIScreen.main.scale
        contentScaleFactor = scale
        layer.contentsScale = scale
        (layer as? CAMetalLayer)?.drawableSize = CGSize(width: bounds.width * scale, height: bounds.height * scale)
        if bounds.width > bounds.height { report() }  // Unity's games are landscape
    }

    /// Hands the layer over in whatever orientation the screen has now.
    func reportNow() { report() }

    private func report() {
        guard !reported, bounds.width > 0, bounds.height > 0, let metal = layer as? CAMetalLayer else { return }
        reported = true
        let scale = contentScaleFactor
        let width = Int32(bounds.width * scale), height = Int32(bounds.height * scale)
        DispatchQueue.main.async { [weak self] in self?.onReady?(metal, width, height) }  // not inside a layout pass
    }

    // Each finger keeps its number from down to up; positions in the layer's pixels, y down.
    private func send(_ touches: Set<UITouch>, phase: Int32) {
        let scale = contentScaleFactor
        for touch in touches {
            let key = ObjectIdentifier(touch)
            let finger: Int32
            if let known = fingers[key] {
                finger = known
            } else {
                finger = nextFinger
                nextFinger = (nextFinger + 1) % 10
                fingers[key] = finger
            }
            let p = touch.location(in: self)
            ub_unity_touch(phase, finger, Float(p.x * scale), Float(p.y * scale))
            if phase == 2 { fingers[key] = nil }
        }
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) { send(touches, phase: 0) }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) { send(touches, phase: 1) }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) { send(touches, phase: 2) }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) { send(touches, phase: 2) }
}

struct GameView: UIViewRepresentable {
    let onReady: (CAMetalLayer, Int32, Int32) -> Void

    func makeUIView(context: Context) -> GameLayerView {
        let view = GameLayerView.shared
        if view.onReady == nil { view.onReady = onReady }
        // If the screen does not turn to landscape (rotation is refused), start in what it has.
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { view.reportNow() }
        return view
    }

    func updateUIView(_ uiView: GameLayerView, context: Context) {}
}

/// The full-screen game: the view, and a small button back to the probe's log (the game keeps running).
struct GameScreen: View {
    let onReady: (CAMetalLayer, Int32, Int32) -> Void
    let onClose: () -> Void

    var body: some View {
        ZStack(alignment: .topLeading) {
            GameView(onReady: onReady).ignoresSafeArea()
            Button(action: onClose) {
                Image(systemName: "xmark.circle.fill")
                    .font(.title2)
                    .foregroundStyle(.white.opacity(0.35))
                    .padding(10)
            }
        }
        .background(Color.black)
        .statusBarHidden(true)
        .persistentSystemOverlays(.hidden)
        .onAppear { Orientation.request(.landscapeRight) }
    }
}

enum Orientation {
    static func request(_ mask: UIInterfaceOrientationMask) {
        guard let scene = UIApplication.shared.connectedScenes.first(where: { $0 is UIWindowScene }) as? UIWindowScene
        else { return }
        scene.requestGeometryUpdate(.iOS(interfaceOrientations: mask)) { error in
            ProbeLog.write("orientation request refused: \(error.localizedDescription)")
        }
    }
}
