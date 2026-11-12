//
//  KleptonControllerRecovery.swift — getting spatial controllers back after a
//  system interruption.
//
//  Sits beside #113 (KleptonControllers.swift, KLControllerFocus): that one
//  keeps buttons arriving while another window has the focus. This one repairs
//  the stream after it has already stopped.
//
//  The trigger is the return to the foreground, and the answer is one pinch.
//  There is no heuristics in between, because there cannot be: measured on
//  device 2026-11-06, an idle Sense pair delivers 0 input states a second —
//  exactly like a dead one — while its anchors stay fresh every 3 to 34 ms even
//  when the input is gone. Every liveness check built on those two signals
//  either missed a dead controller or raised a prompt over a working one.
//
//  The crown HUD in `.mixed` is not covered: it takes the input and reports no
//  scene, focus or controller change at all. That needs the progressive path,
//  where the crown's immersion change is the signal.
//

import Foundation
import QuartzCore
import SwiftUI

/// The prompt window's lifetime, and the only code that touches the accessory
/// list on the recovery's behalf.
@MainActor
enum KLControllerRecovery {
    /// A `WindowGroup`, not a `Window`: this is opened and dismissed repeatedly
    /// and never restored at launch.
    static let promptWindowID = "ctrlprompt"

    /// A window opens where the gaze points at that instant, so raising it
    /// mid-turn puts it behind the user: 15° counts as a turn, 0.8 s without
    /// one is "still". Same numbers as the local plugin's play-area path.
    private static let stillDegrees: Double = 15
    private static let stillSeconds: Double = 0.8
    private static let stillTimeout: Double = 6

    private static var policy = ControllerRecoveryPolicy()
    private static var openWindow: OpenWindowAction?
    private static var dismissWindow: DismissWindowAction?
    private static var started = false
    private static var promptUp = false
    /// True while an accessory hand-over or a probe is running, so a second
    /// return cannot start a second recovery on top of the first.
    private static var busy = false

    /// When the last take-back proved itself. A residual immersion change
    /// arrives within a second of the pinch that dismissed the prompt (the HUD
    /// closing), and without this it released the controllers and raised a new
    /// prompt straight away — measured 2026-11-06: prompt at 14:50:05.445,
    /// back at 07.406, prompt again at 08.234.
    private static var lastTakeBackAt: TimeInterval = 0
    private static var referenceYaw: Double = 0
    private static var haveYaw = false
    private static var lastTurnAt: TimeInterval = 0

    /// The window actions, taken from the start window when the immersive
    /// space opened. They stay usable after that window has gone.
    static func capture(openWindow: OpenWindowAction, dismissWindow: DismissWindowAction) {
        Self.openWindow = openWindow
        Self.dismissWindow = dismissWindow
    }

    /// Called once at app start, next to `KLControllerFocus.start()`.
    static func start() {
        guard !started else { return }
        started = true
        guard klEnvOn("KL_CTRL_RECOVERY", default: true) else {
            NSLog("[ctrl] recovery off (KL_CTRL_RECOVERY=0)")
            return
        }
        NSLog("[ctrl] recovery armed — checked after a return to the foreground")
    }

    /// The app came back to the front. This is the only trigger: it is the one
    /// moment the question "does the stream still deliver" has an answer, and
    /// the answer is the pinch.
    static func focusReturned() {
        guard klEnvOn("KL_CTRL_RECOVERY", default: true),
              !busy, !promptUp,
              // The space has to be open: a focus change while the start
              // window is up is not a game that lost its controllers.
              KleptonCompositor.live != nil,
              let controllers = KleptonControllers.active,
              controllers.mode == .standard,
              !KleptonControllers.spatialControllers().isEmpty else { return }

        busy = true
        Task { await recover(controllers) }
    }

    /// One crown change. The prompt waits until the turn is over. The crown is
    /// a continuous control here, and a gesture that goes down and back up used
    /// to raise the first prompt mid-turn and a second one after the pinch
    /// (measured 2026-11-06: two prompts for one gesture). A second without a
    /// change means the HUD is about to close.
    private static var crownGeneration = 0

    static func crownChanged() {
        guard KleptonImmersion.enabled else { return }
        crownGeneration += 1
        let generation = crownGeneration
        Task { @MainActor in
            try? await Task.sleep(for: .milliseconds(1000))
            guard generation == crownGeneration else { return }
            crownTick()
        }
    }

    /// The Digital Crown's immersion HUD is up. Unlike a return to the
    /// foreground, this is certain: the system reported the amount change, so
    /// the list is handed over without asking whether the stream is dead.
    static func crownTick() {
        guard klEnvOn("KL_CTRL_RECOVERY", default: true),
              KleptonImmersion.enabled,
              !busy, !promptUp,
              KleptonCompositor.live != nil,
              let controllers = KleptonControllers.active,
              controllers.mode == .standard,
              !KleptonControllers.spatialControllers().isEmpty else { return }
        // A crown tick inside an `.inactive` window belongs to the system, not
        // to the game: the double-click peek and a crown press both land there.
        guard UIApplication.shared.applicationState == .active else { return }
        // The space reports its own opening as an immersion change, 0 and then
        // back to 1.0 within a quarter second (measured twice on 2026-11-06:
        // 14:26:54.084 -> .320 and 14:35:04.572 -> .809). Without this the
        // prompt came up on a launch where nobody had touched the crown. Five
        // seconds is the same grace the local plugin gives its start.
        guard CACurrentMediaTime() - KleptonCompositor.spaceOpenedAt > 5 else { return }
        // A residual tick arrives within a second of the pinch that dismissed
        // the prompt. Ignoring those keeps the panel from reappearing right
        // after it was dismissed, exactly as the local plugin does.
        guard CACurrentMediaTime() - lastTakeBackAt > 1.0 else { return }

        busy = true
        Task { await recover(controllers, waitForStill: false) }
    }

    /// Wait for the head to settle, hand the accessories to the system, raise
    /// the prompt, wait for the pinch.
    private static func recover(_ controllers: KleptonControllers,
                                waitForStill: Bool = true) async {
        defer { busy = false }

        // The head settles first only where the trigger is a scene change: a
        // window opens where the gaze points, and raising it mid-turn puts it
        // behind the user. A crown turn is not that — the local plugin raises
        // its prompt at the tick, and waiting here made the panel arrive seconds
        // after the turn, which read as "it needs a button press" (2026-11-06).
        if waitForStill {
            haveYaw = false
            noteHead()
            let deadline = CACurrentMediaTime() + stillTimeout
            while CACurrentMediaTime() < deadline, !headStill() {
                try? await Task.sleep(for: .milliseconds(100))
                noteHead()
            }
        }

        guard await controllers.suspendAccessories() else {
            NSLog("[ctrl] release failed — not prompting")
            return
        }
        NSLog("[ctrl] stream dead — prompt up")
        KLControllerPromptState.shared.visible = true
        openWindow?(id: promptWindowID)
        promptUp = true
    }

    private static func noteHead() {
        guard KleptonCompositor.headYawAt > 0 else { return }
        let now = CACurrentMediaTime()
        let y = Double(KleptonCompositor.headYaw)
        guard haveYaw else {
            haveYaw = true
            referenceYaw = y
            lastTurnAt = now
            return
        }
        var delta = abs(y - referenceYaw)
        if delta > .pi { delta = 2 * .pi - delta }
        guard delta * 180 / .pi > stillDegrees else { return }
        referenceYaw = y
        lastTurnAt = now
    }

    private static func headStill() -> Bool {
        haveYaw && CACurrentMediaTime() - lastTurnAt >= stillSeconds
    }

    private static func lowerPrompt(why: String) {
        guard promptUp else { return }
        NSLog("[ctrl] prompt lowered (\(why))")
        KLControllerPromptState.shared.waiting = false
        KLControllerPromptState.shared.failed = false
        KLControllerPromptState.shared.visible = false
        dismissWindow?(id: promptWindowID)
        promptUp = false
    }

    /// The pinch on the prompt. One take-back per prompt, whatever the button
    /// does while it runs.
    static func promptTapped() {
        guard promptUp, let controllers = KleptonControllers.active else { return }
        guard !KLControllerPromptState.shared.waiting else { return }
        busy = true
        KLControllerPromptState.shared.waiting = true
        KLControllerPromptState.shared.failed = false
        Task {
            let count = await attemptTakeBack(controllers)
            busy = false
            KLControllerPromptState.shared.waiting = false
            guard count != nil else {
                KLControllerPromptState.shared.failed = true
                return
            }
            promptUp = false
            KLControllerPromptState.shared.visible = false
            KLControllerPromptState.shared.failed = false
            dismissWindow?(id: promptWindowID)
        }
    }

    /// One take-back, then real input states as the proof.
    ///
    /// A single state is not proof — a half-closed stream delivered exactly one
    /// — so the bar is ten, far below the ~85/s a working pair produces. The
    /// window is polled rather than sampled once: after a scene transition the
    /// hot-swap needs between 1.1 s and 3.3 s to come back.
    ///
    /// The hot-swap alone is not always enough: it can hand the anchor stream
    /// back while GameController still gets no buttons (`anchor 0.01 s`,
    /// `states 0/91` for eight seconds). `refreshAccessories` re-asserts the
    /// list without emptying it, so it is tried once, a second in.
    private static func attemptTakeBack(_ controllers: KleptonControllers) async -> Int? {
        let before = KleptonControllers.inputStates
        guard let count = await controllers.resumeAccessories() else {
            NSLog("[ctrl] take-back found no provider")
            return nil
        }
        var states = 0
        var refreshed = false
        for step in 0..<14 {
            try? await Task.sleep(for: .milliseconds(250))
            states = KleptonControllers.inputStates - before
            if states >= policy.healthyStates { break }
            if !refreshed, step == 3 {
                refreshed = true
                let ok = await controllers.refreshAccessories()
                NSLog("[ctrl] button stream still quiet — accessory list re-asserted "
                      + "(ok: \(ok))")
            }
        }
        guard policy.inputReturned(before: before, after: before + states) else {
            NSLog("[ctrl] take-back did not restore input (\(states) states) — prompt stays")
            return nil
        }
        NSLog("[ctrl] controllers back (\(count)), \(states) states in the probe")
        lastTakeBackAt = CACurrentMediaTime()
        return count
    }
}

/// What the prompt's circle is showing. A pinch takes 1 to 3 seconds, which is
/// long enough that a button that does not move reads as a dead button. A
/// take-back that came back without input gets the retry glyph instead of the
/// controller, exactly as the local plugin did.
@MainActor
final class KLControllerPromptState: ObservableObject {
    static let shared = KLControllerPromptState()
    @Published var waiting = false
    @Published var failed = false
    /// False while the window is only there to give the scene its focus back.
    @Published var visible = false
}

/// The prompt itself. It is a window, but it does not look like one:
/// `.windowStyle(.plain)` drops the plate and `.persistentSystemOverlays`
/// drops the close, resize and drag chrome. What is left is the glass circle.
///
/// The window is 184 x 304 with a 160 pt circle 52 pt above the bottom edge, so
/// the circle sits below the gaze centre. The clear area above it is invisible
/// and still hit-testable: a pinch that misses the circle by a few points has
/// to run the take-back anyway.
struct KLControllerPromptView: View {
    @ObservedObject private var state = KLControllerPromptState.shared

    private let width: CGFloat = 184
    private let height: CGFloat = 304
    private let circle: CGFloat = 160

    var body: some View {
        Group {
            if state.visible {
                Button {
                    KLControllerRecovery.promptTapped()
                } label: {
                    VStack(spacing: 0) {
                        Color.clear
                        circleView
                            .frame(width: circle, height: circle)
                            .hoverEffect(.highlight)
                            .padding(.bottom, 52)
                    }
                    .frame(width: width, height: height)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                // The plain button's own gaze effect highlights the whole label
                // as a rectangle. The circle keeps its explicit highlight.
                .hoverEffectDisabled(true)
                .accessibilityLabel(klT("Get your controllers back", "Steuerung zurückholen"))
                .accessibilityHint(klT("Pinch to reconnect the controllers",
                                       "Pinch, um die Controller wieder zu verbinden"))
            } else {
                Color.clear
            }
        }
        .frame(width: width, height: height)
        .persistentSystemOverlays(.hidden)
    }

    @ViewBuilder
    private var circleView: some View {
        ZStack {
            if state.waiting {
                ProgressView()
                    .controlSize(.large)
                    .scaleEffect(1.8)
                    .tint(.primary)
                    .transition(.opacity)
            } else {
                Image(systemName: state.failed ? "arrow.clockwise" : "gamecontroller.fill")
                    .font(.system(size: 60, weight: .semibold))
                    .symbolRenderingMode(.hierarchical)
                    .foregroundStyle(.primary)
                    .contentTransition(.symbolEffect(.replace))
                    .transition(.opacity)
            }
        }
        .frame(width: circle, height: circle)
        .glassBackgroundEffect(in: Circle())
        .animation(.easeInOut(duration: 0.15), value: state.waiting)
        .animation(.easeInOut(duration: 0.15), value: state.failed)
    }
}
