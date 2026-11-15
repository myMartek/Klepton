import Foundation

// Small value-only mapper so held input, disconnect releases and frame-time
// limits can be exercised without a controller, a guest or a headset.
struct MCWindowedPadSample {
    var leftX: Double = 0, leftY: Double = 0
    var rightX: Double = 0, rightY: Double = 0
    var jump = false, inventory = false, pause = false
    var attack = false, use = false, previousSlot = false, nextSlot = false
    var sprint = false
    var dpadUp = false, dpadDown = false, dpadLeft = false, dpadRight = false
}

enum MCWindowedPadEvent: Equatable {
    case key(Int32, Bool), button(Int32, Bool)
    case move(Double, Double), scroll(Double)
}

struct MCWindowedPadState {
    private(set) var keys = Set<Int32>()
    private(set) var buttons = Set<Int32>()
    private var previousSlot = false, nextSlot = false

    static func limitedDelta(_ dt: Double) -> Double {
        dt.isFinite ? max(0, min(0.05, dt)) : 0
    }
    private func axis(_ value: Double, key: Int32) -> Bool {
        value.isFinite && value > (keys.contains(key) ? 0.18 : 0.28)
    }
    private func look(_ value: Double) -> Double {
        guard value.isFinite else { return 0 }
        let v = min(1, abs(value))
        guard v > 0.18 else { return 0 }
        return (value < 0 ? -1 : 1) * pow(min(1, (v - 0.18) / 0.82), 1.5)
    }
    mutating func step(_ s: MCWindowedPadSample, dt: Double, grabbed: Bool) -> [MCWindowedPadEvent] {
        var result: [MCWindowedPadEvent] = []
        let wants: [(Int32, Bool)] = [
            (87, axis(s.leftY, key: 87)), (83, axis(-s.leftY, key: 83)),
            (65, axis(-s.leftX, key: 65)), (68, axis(s.leftX, key: 68)),
            (32, s.jump), (69, s.inventory), (256, s.pause),
            (341, s.sprint),                                   // L3 -> left Control (sprint)
            (265, s.dpadUp), (264, s.dpadDown), (263, s.dpadLeft), (262, s.dpadRight)
        ]
        for (key, down) in wants where down != keys.contains(key) {
            if down { keys.insert(key) } else { keys.remove(key) }
            result.append(.key(key, down))
        }
        for (button, down): (Int32, Bool) in [(0, s.attack), (1, s.use)] where down != buttons.contains(button) {
            if down { buttons.insert(button) } else { buttons.remove(button) }
            result.append(.button(button, down))
        }
        let slot = (s.previousSlot && !previousSlot ? 1.0 : 0) - (s.nextSlot && !nextSlot ? 1.0 : 0)
        previousSlot = s.previousSlot; nextSlot = s.nextSlot
        if slot != 0 { result.append(.scroll(slot)) }
        let speed = grabbed ? 900.0 : 700.0
        let dx = look(s.rightX) * speed * Self.limitedDelta(dt)
        let dy = -look(s.rightY) * speed * Self.limitedDelta(dt)
        if dx != 0 || dy != 0 { result.append(.move(dx, dy)) }
        return result
    }
    mutating func release() -> [MCWindowedPadEvent] {
        let out = keys.sorted().map { MCWindowedPadEvent.key($0, false) }
            + buttons.sorted().map { MCWindowedPadEvent.button($0, false) }
        keys.removeAll(); buttons.removeAll(); previousSlot = false; nextSlot = false
        return out
    }
}

#if os(visionOS)
import GameController
import SwiftUI

// One input owner for all windows, with no immersive compositor or ARKit
// session. Inactive/removed windows, disconnects and game exit all release
// held state before the next owner can send anything.
//
// Two kinds of controller feed the ONE pad sample: a conventional gamepad
// (`extendedGamepad`: DualSense, Backbone, Xbox) and the PSVR2 Sense pair.
// visionOS 26 publishes each Sense controller as its own GCController with
// productCategory GCProductCategorySpatialController and NO extendedGamepad;
// its elements are the unprefixed spatial names (GCInputNames.h, visionOS 26:
// GCInputThumbstick "Thumbstick", GCInputTrigger "Trigger", GCInputGripButton
// "Grip", GCInputThumbstickButton "Thumbstick Button", GCInputButtonA/B,
// GCInputButtonMenu), the hand being the device ("… (L)" / "… (R)"). In VR
// those same controllers belong to KleptonControllers (OpenXR); this pump
// only sends while kl_app_macho_windowed_ready(), which is 0 in a VR run, so
// the two never feed the game at once.
@MainActor
final class MCWindowedInput {
    static let shared = MCWindowedInput()
    /// KL_GAMEPAD_DEBUG=1: an element census per Sense controller and one line
    /// per pressed element edge. The per-connect line is always on.
    static let debug = ProcessInfo.processInfo.environment["KL_GAMEPAD_DEBUG"] == "1"
    private var controllerName: String?
    private var windows: [UUID: Bool] = [:]
    private var timer: Timer?
    private var observers: [NSObjectProtocol] = []
    private weak var controller: GCController?
    private var senses: [GCController] = []
    private var sensesClaimed = false
    private var sensePressed = Set<String>()
    private var state = MCWindowedPadState()
    private var previousTime: TimeInterval?
    private var sending = false

    private init() {
        let center = NotificationCenter.default
        for name in [Notification.Name.GCControllerDidConnect, .GCControllerDidDisconnect] {
            observers.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] notification in
                let gc = notification.object as? GCController
                let disconnected = notification.name == .GCControllerDidDisconnect
                    ? gc.map(ObjectIdentifier.init) : nil
                let what = disconnected == nil ? "connected" : "disconnected"
                Task { @MainActor in
                    if let gc { Self.logController(gc, what) }
                    self?.chooseController(excluding: disconnected)
                }
            })
        }
        for gc in GCController.controllers() { Self.logController(gc, "already attached") }
    }

    private static func isSpatial(_ c: GCController) -> Bool {
        KleptonControllers.spatialControllers().contains { $0 === c }
    }
    /// Which windowed profile a controller gets; the same rule chooseController applies.
    private static func profile(_ c: GCController) -> String {
        if isSpatial(c) { return KleptonControllers.hand(of: c) == 0 ? "spatial-left" : "spatial-right" }
        if c.extendedGamepad == nil { return "ignored(no extendedGamepad)" }
        let name = (c.vendorName ?? "").lowercased()
        if name.contains("psvr2") || name.contains("ps vr2") { return "ignored(psvr2 name)" }
        return "extendedGamepad"
    }
    /// One line per controller connect/disconnect (and per controller already
    /// attached when the first game window opens), whatever KL_GAMEPAD_DEBUG says:
    /// it is what a device boot log needs to explain a pad that does nothing.
    private static func logController(_ c: GCController, _ what: String) {
        NSLog("[gamepad] %@: vendor=\"%@\" category=\"%@\" extendedGamepad=%@ profile=%@",
              what, c.vendorName ?? "?", c.productCategory, c.extendedGamepad == nil ? "no" : "yes",
              what == "disconnected" ? "-" : profile(c))
        if debug, isSpatial(c), what != "disconnected" {
            let p = c.physicalInputProfile
            NSLog("[gamepad] %@ elements: buttons=[%@] dpads=[%@] axes=[%@]", c.vendorName ?? "?",
                  p.buttons.keys.sorted().joined(separator: ", "),
                  p.dpads.keys.sorted().joined(separator: ", "),
                  p.axes.keys.sorted().joined(separator: ", "))
        }
    }

    func window(_ id: UUID, active: Bool) {
        windows[id] = active
        if !active { release(force: true) }
        updatePump()
    }
    func removeWindow(_ id: UUID) {
        windows.removeValue(forKey: id)
        release(force: true)
        updatePump()
    }
    private func updatePump() {
        guard windows.values.contains(true) else {
            timer?.invalidate(); timer = nil
            release(force: true)
            claimSystemButtons(controller, false)
            controller = nil
            claimSenses(false)
            senses = []
            return
        }
        guard timer == nil else { return }
        chooseController()
        previousTime = nil
        let t = Timer(timeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.tick() }
        }
        timer = t
        RunLoop.main.add(t, forMode: .common)
    }
    private func chooseController(excluding disconnected: ObjectIdentifier? = nil) {
        guard windows.values.contains(true) else { return }
        let all = GCController.controllers().filter { ObjectIdentifier($0) != disconnected }
        let spatial = KleptonControllers.spatialControllers().filter { ObjectIdentifier($0) != disconnected }
        let candidates = all.filter { c in
            !spatial.contains { $0 === c } && Self.profile(c) == "extendedGamepad"
        }
        let keepPad = controller.map { c in candidates.contains { $0 === c } } ?? candidates.isEmpty
        let sameSenses = spatial.count == senses.count && zip(spatial, senses).allSatisfy { $0 === $1 }
        if keepPad && sameSenses { return }
        release()
        if !keepPad {
            claimSystemButtons(controller, false)
            controller = candidates.first
            claimSystemButtons(controller, true)
        }
        if !sameSenses {
            claimSenses(false)          // re-claimed by tick() once the game is windowed-ready
            senses = spatial
        }
        var names: [String] = []
        if let controller { names.append(controller.vendorName ?? "Game controller") }
        if !senses.isEmpty {
            let hands = senses.map { KleptonControllers.hand(of: $0) == 0 ? "L" : "R" }.sorted().joined(separator: "+")
            names.append("PS VR2 Sense (\(hands))")
        }
        controllerName = names.isEmpty ? nil : names.joined(separator: " + ")
        NSLog("[windowed-pad] controller: %@ [%@] (of %d connected: %@)", controllerName ?? "none",
              controller?.productCategory ?? (senses.isEmpty ? "-" : "Spatial Controller"), all.count,
              all.map { "\($0.vendorName ?? "?")/\($0.productCategory)/\($0.extendedGamepad == nil ? "no-extended" : "extended")" }
                .joined(separator: ", "))
    }
    /// Menu/Options, Create/Share/View and a pad's own capture button
    /// (Backbone, Switch Pro) are bound to system gestures by default — the
    /// screenshot/recording capture — so the game never saw a pause press.
    /// Every element except Home is claimed while a game window owns the pad,
    /// and handed back when it stops. Home (PS / Xbox / Backbone) stays the
    /// system's, so the person can always leave.
    private func claimSystemButtons(_ c: GCController?, _ claim: Bool) {
        guard let c, let pad = c.extendedGamepad else { return }
        let state: GCControllerElement.SystemGestureState = claim ? .disabled : .enabled
        let home = pad.buttonHome
        var bound: [String] = []
        for element in c.physicalInputProfile.allElements where element !== home {
            if claim, element.isBoundToSystemGesture { bound.append(element.localizedName ?? "?") }
            element.preferredSystemGestureState = state
        }
        if claim { NSLog("[windowed-pad] system gestures disabled (bound before: %@)", bound.joined(separator: ", ")) }
    }
    /// The same claim for the Sense pair (Options/Create), but only while a
    /// windowed game is actually running — never in a VR run, where these
    /// controllers are KleptonControllers' and their system gestures are left
    /// exactly as they were. Home stays the system's.
    private func claimSenses(_ claim: Bool) {
        guard claim != sensesClaimed else { return }
        sensesClaimed = claim
        let state: GCControllerElement.SystemGestureState = claim ? .disabled : .enabled
        for c in senses {
            let profile = c.physicalInputProfile
            let home = profile.buttons["Button Home"]
            var bound: [String] = []
            for element in profile.allElements where element !== home {
                if claim, element.isBoundToSystemGesture { bound.append(element.localizedName ?? "?") }
                element.preferredSystemGestureState = state
            }
            if claim {
                NSLog("[windowed-pad] %@: system gestures disabled (bound before: %@)", c.vendorName ?? "?",
                      bound.joined(separator: ", "))
            }
        }
    }
    /// One Sense controller's half of the pad. Left: stick -> left stick,
    /// Square (Button A) -> X, Triangle (Button B) -> Y, L2 trigger, L1 grip,
    /// L3, Create (Button Menu) -> menu. Right: stick -> right stick, Cross
    /// (Button A) -> A, Circle (Button B) -> B, R2 trigger, R1 grip, R3,
    /// Options (Button Menu) -> menu. Live input, not the state queue: the
    /// queue is KleptonControllers' in VR and a diff stream besides.
    @available(visionOS 26.0, *)
    private func mergeSense(_ c: GCController, into s: inout MCWindowedPadSample) {
        let left = KleptonControllers.hand(of: c) == 0
        let input: any GCDevicePhysicalInputState = c.input
        let b = input.buttons, dp = input.dpads, ax = input.axes
        func pressed(_ n: String) -> Bool { b[n]?.pressedInput.isPressed ?? false }
        var stick = SIMD2<Double>(0, 0)
        if let d = dp["Thumbstick"] ?? dp[left ? "Left Thumbstick" : "Right Thumbstick"] {
            stick = SIMD2(Double(d.xAxis.value), Double(d.yAxis.value))
        } else {
            stick = SIMD2(Double(ax["Thumbstick X Axis"]?.absoluteInput?.value ?? 0),
                          Double(ax["Thumbstick Y Axis"]?.absoluteInput?.value ?? 0))
        }
        func merge(_ x: inout Double, _ y: inout Double) {
            if stick.x * stick.x + stick.y * stick.y > x * x + y * y { x = stick.x; y = stick.y }
        }
        let face1 = pressed("Button A"), face2 = pressed("Button B")
        let trigger = pressed("Trigger"), grip = pressed("Grip")
        let click = pressed("Thumbstick Button")
        let menu = pressed("Button Menu") || pressed("Button Options") || pressed("Button Share")
        if left {
            merge(&s.leftX, &s.leftY)
            // Square (X) has no windowed binding, as on a regular pad.
            if face2 { s.inventory = true }         // Triangle -> Y
            if trigger { s.use = true }             // L2
            if grip { s.previousSlot = true }       // L1
            if click { s.sprint = true }            // L3
        } else {
            merge(&s.rightX, &s.rightY)
            if face1 { s.jump = true }              // Cross -> A
            if face2 { s.pause = true }             // Circle -> B
            if trigger { s.attack = true }          // R2
            if grip { s.nextSlot = true }           // R1
            // R3 has no windowed binding, as on a regular pad.
        }
        if menu { s.pause = true }                  // Options / Create -> menu
        if Self.debug {
            let hand = left ? "L" : "R"
            for el in b where el.pressedInput.isPressed {
                let key = "\(hand):\(el.aliases.sorted().first ?? "?")"
                if sensePressed.insert(key).inserted { NSLog("[gamepad] press %@", key) }
            }
            for key in sensePressed where key.hasPrefix(hand + ":")
                && !(b[String(key.dropFirst(2))]?.pressedInput.isPressed ?? false) {
                sensePressed.remove(key)
            }
        }
    }
    private func tick() {
        let gamepad = controller?.extendedGamepad
        guard windows.values.contains(true), kl_app_macho_windowed_ready() != 0,
              gamepad != nil || !senses.isEmpty else {
            if sending { release() }
            return
        }
        sending = true
        let now = ProcessInfo.processInfo.systemUptime
        let dt = previousTime.map { now - $0 } ?? (1.0 / 60.0)
        previousTime = now
        // Resume after a delayed main thread with releases, rather than a
        // backlog of look or held inputs from before focus was lost.
        if dt > 0.25 { release(); previousTime = now; return }
        var sample = MCWindowedPadSample()
        if let gamepad {
            sample.leftX = Double(gamepad.leftThumbstick.xAxis.value)
            sample.leftY = Double(gamepad.leftThumbstick.yAxis.value)
            sample.rightX = Double(gamepad.rightThumbstick.xAxis.value)
            sample.rightY = Double(gamepad.rightThumbstick.yAxis.value)
            sample.jump = gamepad.buttonA.isPressed
            sample.inventory = gamepad.buttonY.isPressed
            sample.pause = gamepad.buttonB.isPressed || gamepad.buttonMenu.isPressed
                || (gamepad.buttonOptions?.isPressed ?? false)
            sample.sprint = gamepad.leftThumbstickButton?.isPressed ?? false
            sample.dpadUp = gamepad.dpad.up.isPressed
            sample.dpadDown = gamepad.dpad.down.isPressed
            sample.dpadLeft = gamepad.dpad.left.isPressed
            sample.dpadRight = gamepad.dpad.right.isPressed
            sample.attack = gamepad.rightTrigger.isPressed
            sample.use = gamepad.leftTrigger.isPressed
            sample.previousSlot = gamepad.leftShoulder.isPressed
            sample.nextSlot = gamepad.rightShoulder.isPressed
        }
        if !senses.isEmpty, #available(visionOS 26.0, *) {
            claimSenses(true)
            for c in senses { mergeSense(c, into: &sample) }
        }
        for event in state.step(sample, dt: dt, grabbed: kl_app_macho_windowed_cursor_mode() == 0x34003) {
            send(event)
        }
    }
    private func send(_ event: MCWindowedPadEvent) {
        switch event {
        case let .key(key, down): kl_app_macho_windowed_key(key, down ? 1 : 0)
        case let .button(button, down): kl_app_macho_windowed_button(button, down ? 1 : 0)
        case let .move(x, y): kl_app_macho_windowed_move(x, y)
        case let .scroll(y): kl_app_macho_windowed_scroll(y)
        }
    }
    private func release(force: Bool = false) {
        for event in state.release() { send(event) }
        if sending || force { kl_app_macho_windowed_release_all() }
        sending = false
        previousTime = nil
    }
}
#endif
