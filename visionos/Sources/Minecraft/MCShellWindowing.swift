import SwiftUI
import GameController

// What the window does differently while MINECRAFT's picture is in it (windowed
// play), as one modifier on the shared ShellWindow's surface. From
// KleptonShell.swift of rebelancap's vivecraft-visionos 1.0.3, where these were
// lines of ShellWindow itself; here ShellWindow stays as it is for every other
// target and takes this modifier behind `#if KL_MACHO_GUEST`.
//
//  - The gamepad belongs to the game. Without a GCEventInteraction on the view
//    that hosts the picture, visionOS turns a paired gamepad into gaze-targeted
//    pinch events for whatever the person looks at, so the sticks read nothing
//    useful and Options/Create are left to the system. MCWindowedInput reads
//    the pad through GameController and needs to know which window is active.
//  - No letterbox. The game's framebuffer is fixed for the process
//    (KL_NATIVE_SIZE, 1280x720 unless said otherwise), so the WINDOW is held to
//    the game's shape instead of the picture being fitted into the window: one
//    resize to that aspect at the current width, then uniform-only resizing,
//    which visionOS enforces on every drag of the window's corner. Back to the
//    launcher's shape and free resizing when the picture leaves.
struct MCShellWindowing: ViewModifier {
    let box: ShellSurface.Box
    @State private var inputWindowID = UUID()
    @State private var lock = Lock()
    @Environment(\.scenePhase) private var scenePhase
    private let padInput = MCWindowedInput.shared
    // The game's own surface size is read from its frames (box.view.guestW/H).
    // Until the first frame the window keeps the launcher's shape.
    private let sizePoll = Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()

    /// The window scene whose shape was locked to the game's aspect, so the
    /// lock can be undone, and the view the gamepad was claimed for.
    final class Lock {
        weak var scene: UIWindowScene?
        var aspect: CGSize = .zero
        weak var claimed: UIView?
    }

    func body(content: Content) -> some View {
        content
            .onAppear { padInput.window(inputWindowID, active: scenePhase == .active) }
            .onChange(of: scenePhase) { _, phase in padInput.window(inputWindowID, active: phase == .active) }
            .onDisappear { padInput.removeWindow(inputWindowID) }
            .onReceive(sizePoll) { _ in
                guard let v = box.view else { return }
                claimGamepad(for: v)
                guard v.guestW > 0, v.guestH > 0 else { return }
                lockWindow(to: CGSize(width: v.guestW, height: v.guestH))
            }
            .onDisappear { unlockWindow() }
    }

    /// Once per view. Pinches with the hands are not gamepad events and still
    /// reach ShellWindow's drag gesture.
    private func claimGamepad(for view: UIView) {
        guard lock.claimed !== view else { return }
        lock.claimed = view
        let gc = GCEventInteraction()
        gc.handledEventTypes = .gamepad
        view.addInteraction(gc)
    }

    private func lockWindow(to guest: CGSize) {
        guard let window = box.view?.window, let scene = window.windowScene else { return }
        if lock.scene === scene, lock.aspect == guest { return }
        let width = max(640, window.bounds.width)
        let size = CGSize(width: width, height: (width * guest.height / guest.width).rounded())
        lock.scene = scene
        lock.aspect = guest
        NSLog("[shell] window locked to the game's aspect: \(Int(guest.width))x\(Int(guest.height)) -> \(Int(size.width))x\(Int(size.height)) pt, uniform resizing")
        scene.requestGeometryUpdate(.Vision(size: size, resizingRestrictions: .uniform)) { error in
            NSLog("[shell] window aspect lock refused: \(error)")
        }
    }

    /// Back to the launcher's shape and free resizing (KleptonApp's
    /// .defaultSize, 1280x800) once the game's picture leaves this window.
    private func unlockWindow() {
        guard let scene = lock.scene else { return }
        lock.scene = nil
        lock.aspect = .zero
        NSLog("[shell] window unlocked: 1280x800 pt, freeform resizing")
        scene.requestGeometryUpdate(.Vision(size: CGSize(width: 1280, height: 800), resizingRestrictions: .freeform)) { error in
            NSLog("[shell] window unlock refused: \(error)")
        }
    }
}
