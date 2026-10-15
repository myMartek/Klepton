// Live controller-alignment tuning — the values in DEBUG_ENV_VARS.md's
// KL_SENSE_* and KL_XR_GRIP_* entries, made adjustable while wearing the
// headset instead of only at launch.
//
// **This exists because these numbers cannot be derived, only judged.** They
// are the gap between Apple's grip convention and the pose each guest was built
// against, and every guest applies its own controller-model transform on top —
// so the residual differs per title and the only instrument is a person looking
// at their hand. At a relaunch per candidate that is two minutes an iteration,
// which is why three targets were tuned by eye and none of them converged.
//
// The environment still sets the starting values, so a run that never opens the
// panel behaves exactly as it did before this file existed.
import SwiftUI
import simd

/// One complete set. Value semantics on purpose: the render thread reads a
/// whole snapshot, so it can never see one slider's new value beside another's
/// old one.
struct KLTune: Equatable {
    var sensePitch: Float = -37
    var sensePos   = SIMD3<Float>(-0.004, 0.02, -0.02)
    var sensePivot = SIMD3<Float>(0, 0, 0)
    var gripPitch: Float = 37
    var aimPitch:  Float = 0
    var gripPivot  = SIMD3<Float>(0, 0, 0)
    var gripPos    = SIMD3<Float>(0, 0, 0)
}

final class KleptonTuning: ObservableObject {
    static let shared = KleptonTuning()

    /// What the UI edits. Main thread only — SwiftUI requires it, and the
    /// render thread never touches it.
    @Published var ui = KLTune() { didSet { publish() } }

    /// ...and what the render thread reads, behind a lock. Two copies rather
    /// than one atomic because a tune is seven values and a controller drawn
    /// from half of one set and half of another is exactly the incoherence this
    /// whole area is about.
    private let lock = NSLock()
    private var live = KLTune()

    private init() {
        // Seeded from whatever is actually in force, not from these defaults:
        // a panel showing its own idea of the numbers while the runtime uses
        // the environment's is an instrument that lies on the first frame.
        var t = KLTune()
        t.sensePitch = KLSenseTune.hiltPitch
        t.sensePos   = KLSenseTune.shared[1].pos
        t.sensePivot = KLSenseTune.shared[1].pivot
        var gp: Float = 0, ap: Float = 0
        var pivot = [Float](repeating: 0, count: 3)
        var pos   = [Float](repeating: 0, count: 3)
        kl_openxr_grip_tune(&gp, &ap, &pivot, &pos)
        t.gripPitch = gp
        t.aimPitch  = ap
        t.gripPivot = SIMD3<Float>(pivot[0], pivot[1], pivot[2])
        t.gripPos   = SIMD3<Float>(pos[0], pos[1], pos[2])
        ui = t
        live = t
    }

    /// The snapshot the pose path uses. Called once per controller pose.
    func snapshot() -> KLTune {
        lock.lock(); defer { lock.unlock() }
        return live
    }

    private func publish() {
        lock.lock(); live = ui; lock.unlock()
        var pivot = [ui.gripPivot.x, ui.gripPivot.y, ui.gripPivot.z]
        var pos   = [ui.gripPos.x,   ui.gripPos.y,   ui.gripPos.z]
        kl_openxr_set_grip_tune(ui.gripPitch, ui.aimPitch, &pivot, &pos)
    }

    /// The current set as the environment lines that would reproduce it —
    /// which is the whole point of tuning live. A value found by dragging and
    /// not written down is a value found twice.
    func asEnvironment() -> String {
        func v(_ s: SIMD3<Float>) -> String {
            String(format: "%.3f,%.3f,%.3f", s.x, s.y, s.z)
        }
        return """
        KL_SENSE_PITCH=\(String(format: "%.3f", ui.sensePitch)) \
        KL_SENSE_POS="\(v(ui.sensePos))" \
        KL_SENSE_PIVOT="\(v(ui.sensePivot))" \
        KL_XR_GRIP_PITCH=\(String(format: "%.3f", ui.gripPitch)) \
        KL_XR_AIM_PITCH=\(String(format: "%.3f", ui.aimPitch)) \
        KL_XR_GRIP_PIVOT="\(v(ui.gripPivot))" \
        KL_XR_GRIP_POS="\(v(ui.gripPos))"
        """
    }
}

/// The panel. Lives in the boot window, which stays open beside the immersive
/// space — so the guest keeps rendering while these move.
struct TuningView: View {
    @ObservedObject private var tune = KleptonTuning.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Controller alignment").font(.headline)
            Text("The X term is mirrored for the left hand. Grip-frame axes: "
                 + "−Z points, +Y up, +X right.")
                .font(.caption).foregroundStyle(.secondary)

            Group {
                deg("Sense pitch", $tune.ui.sensePitch, -90 ... 90)
                vec("Sense pivot", $tune.ui.sensePivot)
                vec("Sense offset", $tune.ui.sensePos)
            }
            Divider()
            Group {
                deg("OpenXR grip pitch", $tune.ui.gripPitch, -90 ... 90)
                deg("OpenXR aim pitch",  $tune.ui.aimPitch,  -90 ... 90)
                vec("OpenXR grip pivot", $tune.ui.gripPivot)
                vec("OpenXR grip offset", $tune.ui.gripPos)
            }
            Divider()
            HStack {
                Button("Log as environment") { NSLog("[tune] \(tune.asEnvironment())") }
                Button("Reset") { tune.ui = KLTune() }
            }
            // Shown as well as logged: reading a value off the panel beats
            // going to find the log for it, and this is what gets pasted.
            Text(tune.asEnvironment())
                .font(.system(.caption2, design: .monospaced))
                .textSelection(.enabled)
                .foregroundStyle(.secondary)
        }
        .padding()
    }

    private func deg(_ label: String, _ value: Binding<Float>,
                     _ range: ClosedRange<Float>) -> some View {
        HStack {
            Text(label).frame(width: 170, alignment: .leading)
            Slider(value: value, in: range, step: 0.5)
            Text(String(format: "%6.1f°", value.wrappedValue))
                .font(.system(.body, design: .monospaced)).frame(width: 70)
        }
    }

    private func vec(_ label: String, _ value: Binding<SIMD3<Float>>) -> some View {
        HStack {
            Text(label).frame(width: 170, alignment: .leading)
            // ±12 cm covers every offset measured so far (the largest was 8 cm
            // on BONELAB) with room to find out it is not enough.
            axis("x", value.x); axis("y", value.y); axis("z", value.z)
        }
    }

    private func axis(_ name: String, _ value: Binding<Float>) -> some View {
        VStack(spacing: 2) {
            Slider(value: value, in: -0.12 ... 0.12, step: 0.005)
            Text("\(name) \(String(format: "%+.3f", value.wrappedValue))")
                .font(.system(.caption2, design: .monospaced))
        }
    }
}

// ---- Rendering: foveated guest rendering as a setting ----------------------
//
// KL_VRR, with a memory. Off by default on a headset: the guest's rate map is
// built once, before the guest boots, from wherever the display's own curve
// happened to be dense at that moment, and then it stays there — a fixed sharp
// patch that does not follow the eyes. Cheaper (about a third of the fragments)
// and visibly so. A person who wants the frame time back turns it on here.
//
// Applied at boot, because that is when the map is built; flipping the toggle
// in a running session is recorded for the next one.
final class KleptonRendering: ObservableObject {
    static let shared = KleptonRendering()
    private static let key = "klepton.foveation"
    private static let resetKey = "klepton.foveation.reset"

    private static let scaleKey = "klepton.renderScale"
    private static let videoKey = "klepton.video"

    /// Try in-game video even where the runtime would refuse it (a Vulkan
    /// guest — see klm_extractor_enabled). Starts at the target's own default.
    @Published var video: Bool {
        didSet { UserDefaults.standard.set(video, forKey: Self.videoKey) }
    }

    static let targetFoveation: Bool =
        (Bundle.main.object(forInfoDictionaryKey: "KLFoveationDefault") as? String) == "1"

    /// The target's default (targets.py "video", baked into Info.plist as
    /// KLVideoDefault). Off for a bundle without the key.
    static let targetVideo: Bool =
        (Bundle.main.object(forInfoDictionaryKey: "KLVideoDefault") as? String) == "1"

    /// The number a target raises to call a stored choice back once (targets.py
    /// "foveation_reset", KLFoveationReset in Info.plist). 0 for a bundle
    /// without the key: nothing is ever called back.
    static let targetFoveationReset: Int =
        (Bundle.main.object(forInfoDictionaryKey: "KLFoveationReset") as? String).flatMap(Int.init) ?? 0

    @Published var foveation: Bool {
        didSet { UserDefaults.standard.set(foveation, forKey: Self.key) }
    }

    /// Edge smoothing for a guest that draws its eyes into a multisampled
    /// multiview target (kl_glfb_multiview_ms): 0 is off, 2 and 4 are the
    /// sample counts. Beat Saber from 1.45 on is the one such guest; the two
    /// rows it comes under are named here because the version is only known
    /// once the game's files are read, after this page has been shown.
    private static let msaaKey = "klepton.msaa"
    static let targetHasMSAA: Bool = {
        let t = String(cString: kl_app_target_name_or_default())
        // Beat Saber (from 1.45 on) and Maestro: the guests kl_glfb.c's
        // klfb_msaa_guest names. Doom 3 was one of them for a release.
        return t == "beatsaber140" || t == "beatsaber" || t == "x6963344583787383"
    }()
    static let targetIsBeatSaber: Bool = {
        let t = String(cString: kl_app_target_name_or_default())
        return t == "beatsaber140" || t == "beatsaber"
    }()
    @Published var msaa: Int {
        didSet { UserDefaults.standard.set(msaa, forKey: Self.msaaKey) }
    }

    /// The frame rate a game is shown at, by way of the kind of immersive
    /// space it opens (Immersive.mixed): 100 is the mixed space, where this
    /// headset's panel runs at up to 100 Hz, and what every game starts from;
    /// 120 is the fully immersive one, at up to 120 Hz. visionOS halves either
    /// when a game cannot keep up (to 50 and to 60). The owner first had the
    /// switch taken out when the mixed space became the rule (1.1.7) and then
    /// asked for it back for all games: "den optionalen Switch zwischen 100hz
    /// (standard in mixed) und 120hz".
    ///
    /// Not for a mixed-reality game ("Nur für Mixed reality spiele will ich
    /// das nicht"): its room shows through a mixed space only. Those are the
    /// games passthrough is offered to (kl_ovrp.c, KL_OVRP_PASSTHROUGH) —
    /// TRIANGLE STRATEGY and Demeo.
    private static let rateKey = "klepton.maxRate"
    static let targetIsMixedReality: Bool = {
        let t = String(cString: kl_app_target_name_or_default())
        return t == "x5340130476095884" || t == "x3634830803298285"
            || klEnvOn("KL_OVRP_PASSTHROUGH", default: false)
    }()
    @Published var maxRate: Int {
        didSet { UserDefaults.standard.set(maxRate, forKey: Self.rateKey) }
    }
    /// The full space, as the person chose it. Never for a mixed-reality game.
    var wantsFullSpace: Bool { !Self.targetIsMixedReality && maxRate == 120 }

    /// The same for a Vulkan guest, which multisamples by itself and with a
    /// count of its own choosing: 0 leaves that, 4 raises it to four
    /// (kl_vulkan_set_force_samples_default). Which API a game renders with,
    /// and with how many samples, is only known once it has run — so a run
    /// leaves that number behind (`rememberRun`), and the picker is offered
    /// from the next start on, to a game that was seen with fewer than four.
    private static let vkMsaaKey = "klepton.vk.msaa"
    private static let vkGuestSamplesKey = "klepton.vk.guestSamples"
    static var vkGuestSamples: Int { UserDefaults.standard.integer(forKey: vkGuestSamplesKey) }
    /// What it starts at (targets.py "vk_msaa", KLVkMsaaDefault in Info.plist):
    /// 4 for a game that has been played that way, else the game's own count.
    static let targetVkMsaa: Int =
        (Bundle.main.object(forInfoDictionaryKey: "KLVkMsaaDefault") as? String) == "4" ? 4 : 0
    @Published var vkMsaa: Int {
        didSet { UserDefaults.standard.set(vkMsaa, forKey: Self.vkMsaaKey) }
    }
    /// Only where the setting does something: the game was seen multisampling
    /// with fewer than four samples (and is not one that is held at two).
    var offersVkMsaa: Bool { (2...3).contains(Self.vkGuestSamples) }

    /// Foveated rendering for a Vulkan guest (KL_FOVEATION, kl_fove.m): the
    /// level as a choice, where a GL guest has the one switch above. "off",
    /// "low", "medium" or "high" — "high" with the smaller sharp box and the
    /// lower floor rebelancap runs his Unreal and Unity titles with. Starts at
    /// the target's own (targets.py "vk_foveation", KLVkFoveationDefault): off
    /// unless a game was looked at on a headset with it.
    /// "ultra" is a level of this setting only: kl_fove is told "high" with a
    /// smaller sharp box, a shorter ramp and a lower floor (asked for after
    /// Moss at "high", 2026-10-13: "könnte auch noch stärker sein").
    static let vkFoveationLevels = ["off", "low", "medium", "high", "ultra"]
    /// The number that drops a stored resolution and foveation level once
    /// (targets.py "picture_reset", KLPictureReset in Info.plist); 0 for a
    /// target that never changed its picture defaults.
    private static let pictureResetKey = "klepton.picture.reset"
    static let targetPictureReset: Int =
        Int((Bundle.main.object(forInfoDictionaryKey: "KLPictureReset") as? String) ?? "") ?? 0
    private static let vkFoveationKey = "klepton.vk.foveation"
    static let targetVkFoveation: String = {
        let v = (Bundle.main.object(forInfoDictionaryKey: "KLVkFoveationDefault") as? String) ?? "off"
        return vkFoveationLevels.contains(v) ? v : "off"
    }()
    @Published var vkFoveation: String {
        didSet { UserDefaults.standard.set(vkFoveation, forKey: Self.vkFoveationKey) }
    }
    /// Where the choice is shown in place of the switch: a game known to draw
    /// with Vulkan — a run left its sample count behind (`rememberRun`), or the
    /// target itself starts foveated.
    var offersVkFoveation: Bool { Self.vkGuestSamples > 0 || Self.targetVkFoveation != "off" }

    /// How finely the compositor puts the finished picture on the display
    /// (KL_CP_QUALITY; `AlyxOutput` has the values and what was measured).
    /// Starts at the target's own (targets.py "output", KLOutputDefault): light
    /// for nearly every game. Alyx has its own in its panel.
    private static let outputKey = "klepton.output"
    static let targetOutput: AlyxOutput = {
        switch Bundle.main.object(forInfoDictionaryKey: "KLOutputDefault") as? String {
        case "light": return .light
        case "standard": return .standard
        default: return .full          // also a bundle without the key
        }
    }()
    @Published var output: Int {
        didSet { UserDefaults.standard.set(output, forKey: Self.outputKey) }
    }

    /// Raised when the two settings above get new defaults that copies with a
    /// stored choice should take once (the owner's wish for the first ones:
    /// "einmal beim ersten Installieren mit dem neuen Toolchain auf diese
    /// Einstellung setzen"). A choice made after that start stays.
    private static let pictureEpoch = 1
    private static let pictureEpochKey = "klepton.picture.epoch"

    /// Fraction of the eye-texture size the guest is told, 0.5...1.0. See
    /// kl_ovrp_set_render_scale_default for why this and not foveation is the
    /// lever for a Vulkan guest.
    @Published var renderScale: Double {
        didSet { UserDefaults.standard.set(renderScale, forKey: Self.scaleKey) }
    }

    /// How far down "Resolution" goes. Half for everything but Beat Saber from
    /// 1.45 on: with foveated rendering that game starts from the display's own
    /// size (5740 instead of 4000 across), where half is still a fine picture,
    /// and with edge smoothing a coarser one is worth trying.
    static var minRenderScale: Double { targetIsBeatSaber ? 0.25 : 0.5 }

    /// The target's own default (targets.py "render_scale", baked into
    /// Info.plist as KLRenderScaleDefault). 1.0 for a bundle without the key.
    static let targetRenderScale: Double = {
        let s = Bundle.main.object(forInfoDictionaryKey: "KLRenderScaleDefault") as? String
        let v = s.flatMap(Double.init) ?? 1.0
        return (0.5...1.0).contains(v) ? v : 1.0
    }()

    private init() {
        // A target whose default changed drops what this copy stored, once:
        // the number is remembered, so a choice made after that stays. A copy
        // that never stored one gets the new default without this.
        let applied = UserDefaults.standard.integer(forKey: Self.resetKey)
        if Self.targetFoveationReset > applied {
            let had = UserDefaults.standard.object(forKey: Self.key) as? Bool
            UserDefaults.standard.removeObject(forKey: Self.key)
            UserDefaults.standard.set(Self.targetFoveationReset, forKey: Self.resetKey)
            NSLog("[cp] foveated rendering: stored choice "
                  + (had.map { $0 ? "on" : "off" } ?? "none")
                  + " dropped once for this target's new default "
                  + (Self.targetFoveation ? "on" : "off")
                  + " (reset \(Self.targetFoveationReset), was \(applied))")
        }
        // Never chosen means the target's default (targets.py "foveation",
        // KLFoveationDefault in Info.plist), which is off unless the target
        // says otherwise.
        foveation = (UserDefaults.standard.object(forKey: Self.key) as? Bool)
            ?? Self.targetFoveation
        // Never chosen is 100; so is anything a test build stored besides 120.
        maxRate = (UserDefaults.standard.object(forKey: Self.rateKey) as? Int) == 120 ? 120 : 100
        // Never chosen is off (see kl_glfb_multiview_ms for why).
        msaa = (UserDefaults.standard.object(forKey: Self.msaaKey) as? Int)
            .flatMap { [0, 2, 4].contains($0) ? $0 : nil } ?? 0
        if UserDefaults.standard.integer(forKey: Self.pictureEpochKey) < Self.pictureEpoch {
            let had = (UserDefaults.standard.object(forKey: Self.vkMsaaKey) as? Int,
                       UserDefaults.standard.object(forKey: Self.outputKey) as? Int)
            UserDefaults.standard.removeObject(forKey: Self.vkMsaaKey)
            UserDefaults.standard.removeObject(forKey: Self.outputKey)
            UserDefaults.standard.set(Self.pictureEpoch, forKey: Self.pictureEpochKey)
            if had.0 != nil || had.1 != nil {
                NSLog("[cp] edge smoothing and display output: stored choices dropped once for "
                      + "this target's defaults (\(Self.targetVkMsaa == 0 ? "the game's own" : "4x"), "
                      + "\(Self.targetOutput.title))")
            }
        }
        // A target whose picture defaults were changed for everyone can ask for
        // the stored resolution and Vulkan foveation level to be dropped once
        // (targets.py "picture_reset", KLPictureReset): a copy that was set up
        // under the old defaults would otherwise keep them for good.
        let pictureApplied = UserDefaults.standard.integer(forKey: Self.pictureResetKey)
        if Self.targetPictureReset > pictureApplied {
            let had = (UserDefaults.standard.object(forKey: Self.scaleKey) as? Double,
                       UserDefaults.standard.object(forKey: Self.vkFoveationKey) as? String)
            UserDefaults.standard.removeObject(forKey: Self.scaleKey)
            UserDefaults.standard.removeObject(forKey: Self.vkFoveationKey)
            UserDefaults.standard.set(Self.targetPictureReset, forKey: Self.pictureResetKey)
            NSLog("[cp] resolution and foveated rendering (Vulkan): stored choices "
                  + "(\(had.0.map { String(format: "%.0f%%", $0 * 100) } ?? "none"), \(had.1 ?? "none")) "
                  + "dropped once for this target's new defaults "
                  + String(format: "(%.0f%%, ", Self.targetRenderScale * 100) + "\(Self.targetVkFoveation); "
                  + "reset \(Self.targetPictureReset), was \(pictureApplied))")
        }
        vkMsaa = (UserDefaults.standard.object(forKey: Self.vkMsaaKey) as? Int)
            .map { $0 == 4 ? 4 : 0 } ?? Self.targetVkMsaa
        vkFoveation = (UserDefaults.standard.object(forKey: Self.vkFoveationKey) as? String)
            .flatMap { Self.vkFoveationLevels.contains($0) ? $0 : nil } ?? Self.targetVkFoveation
        output = (UserDefaults.standard.object(forKey: Self.outputKey) as? Int)
            .flatMap { AlyxOutput(rawValue: $0)?.rawValue } ?? Self.targetOutput.rawValue
        // object(forKey:), so that "never chosen" is the target's default
        // rather than off.
        video = (UserDefaults.standard.object(forKey: Self.videoKey) as? Bool)
            ?? Self.targetVideo
        // object(forKey:) rather than double(forKey:): 0 is "never set" there,
        // and never set has to mean the target's default, not the minimum.
        let stored = UserDefaults.standard.object(forKey: Self.scaleKey) as? Double
        renderScale = stored.map { min(max($0, Self.minRenderScale), 1.0) } ?? Self.targetRenderScale
    }

    /// Hand the stored choice to the runtime. Before kl_app_boot, so every
    /// reader of kl_glfb_foveation_wanted sees one answer for the whole run.
    func apply() {
        kl_glfb_set_foveation_default(foveation ? 1 : 0)
        let env = ProcessInfo.processInfo.environment["KL_VRR"]
        NSLog("[cp] foveated rendering: setting \(foveation ? "on" : "off")"
              + (env.map { " — overridden by KL_VRR=\($0)" } ?? ""))
        if Self.targetHasMSAA {
            kl_glfb_set_msaa_default(Int32(msaa))
            let envAA = ProcessInfo.processInfo.environment["KL_GL_MSAA_MV"]
            NSLog("[cp] edge smoothing: setting " + (msaa == 0 ? "off" : "\(msaa)x")
                  + (envAA.map { " — overridden by KL_GL_MSAA_MV=\($0)" } ?? ""))
        }
        kl_vulkan_set_force_samples_default(Int32(vkMsaa))
        let envVk = ProcessInfo.processInfo.environment["KL_VK_FORCE_SAMPLES"]
        NSLog("[cp] edge smoothing (Vulkan): setting " + (vkMsaa == 0 ? "as the game makes it" : "\(vkMsaa)x")
              + " (target default " + (Self.targetVkMsaa == 0 ? "as the game makes it" : "\(Self.targetVkMsaa)x")
              + (Self.vkGuestSamples > 0 ? "; the game was last seen with \(Self.vkGuestSamples)x)" : ")")
              + (envVk.map { " — overridden by KL_VK_FORCE_SAMPLES=\($0)" } ?? ""))
        kl_ovrp_set_render_scale_default(Float(renderScale))
        kl_mediandk_set_video_default(video ? 1 : -1)
        NSLog("[cp] in-game video: \(video ? "on" : "runtime default") (target default \(Self.targetVideo ? "on" : "off"))")
        let envScale = ProcessInfo.processInfo.environment["KL_OVRP_RENDER_SCALE"]
        NSLog(String(format: "[cp] render resolution: setting %.0f%% (target default %.0f%%)",
                     renderScale * 100, Self.targetRenderScale * 100)
              + (envScale.map { " — overridden by KL_OVRP_RENDER_SCALE=\($0)" } ?? ""))
    }
}

// The guest renders small (the Render resolution slider); the LINEAR stretch
// to the eye texture is then replaced by Apple's spatial scaler: same pixels
// in, sharper picture out. Single frame in/out, no motion data.
// Read live in the compositor, so the switch applies to the next frame.
// "Never chosen" means what the installation started with: off, unless the
// installer named KL_METALFX_UPSCALE among the switches it wants on
// (KLEPTON_TEST_ON, KLTestOn in Info.plist — AVP Play's experimental setting
// "MetalFX"). A choice made here then stays, the way a Test switch's does.
final class KleptonMetalFX: ObservableObject {
    static let shared = KleptonMetalFX()

    static let key = "klepton.metalfxUpscale"
    /// The name an installation switches this on from the start with.
    static let installSwitch = "KL_METALFX_UPSCALE"

    /// On from the installation (KleptonTestSwitches.onFromInstall).
    static var onFromInstall: Bool { KleptonTestSwitches.onFromInstall.contains(installSwitch) }

    /// What the compositor reads every frame: the choice made in the game, else
    /// what the installation started with.
    static var isOn: Bool {
        (UserDefaults.standard.object(forKey: key) as? Bool) ?? onFromInstall
    }

    @Published var enabled: Bool {
        didSet {
            // Stored only where it differs from what the installation starts
            // with (KleptonTestSwitches.set), so a switch put back follows a
            // later installation that starts it differently.
            if enabled == Self.onFromInstall { UserDefaults.standard.removeObject(forKey: Self.key) }
            else { UserDefaults.standard.set(enabled, forKey: Self.key) }
            NSLog("[metalfx] spatial upscale: \(enabled ? "on" : "off")")
        }
    }

    private init() {
        enabled = Self.isOn
    }
}

/// "Record what the game waits for": the runtime's account of blocked threads
/// (KL_TRACE_CONDDUMP — kl_pthread_stuck_diag has what it writes), as a switch
/// a player can turn on for a game that stops in a loading screen. Off unless
/// chosen, and the choice stays with the game across updates of it.
enum KleptonDiagnostics {
    static let waitersKey = "klepton.diag.waiters"

    /// Into the environment, before kl_app_configure (see
    /// KleptonRendering.applyEnvironment). Never over the launch line's value.
    static func applyEnvironment() {
        guard UserDefaults.standard.bool(forKey: waitersKey) else { return }
        setenv("KL_TRACE_CONDDUMP", "1", 0)
        NSLog("[app] diagnostics: recording what the game waits for (KL_TRACE_CONDDUMP)")
    }
}

/// Behaviour that is in the runtime but not yet the default: each entry is one
/// environment switch, off unless chosen, so a change taken over from another
/// fork can be tried on the headset without a build per trial. The group is in
/// every build, released ones too (the owner's decision, 2026-10-13, after
/// looking at the test copies: "Lass die Flags dann noch in der Test Sektion
/// auch später im Release") — a player with a game that misbehaves can try a
/// switch and say which one helped.
enum KleptonTestSwitches {
    struct Entry: Identifiable {
        let env: String          // the variable
        let value: String        // what it is set to when the switch is on
        let title: String
        let note: String
        /// Further variables that belong to the same switch.
        var also: [(String, String)] = []
        var id: String { env + "=" + value }
        var key: String { "klepton.test." + env + (also.isEmpty ? "" : "." + value) }
    }

    /// The switches this installation has on from the start: named by whoever
    /// installed it (KLEPTON_TEST_ON at build time, KLTestOn in Info.plist) —
    /// AVP Play does, for someone who wants a change that is built in but off
    /// in all games at once ("as if the old way were gone") without a visit to
    /// each. A switch named here is on until it is turned off in this window,
    /// and that choice then stays.
    static let onFromInstall: Set<String> = Set(
        ((Bundle.main.object(forInfoDictionaryKey: "KLTestOn") as? String) ?? "")
            .split(separator: ",").map(String.init))

    /// Whether a switch is on: what was chosen in the window, else what the
    /// installation started with.
    static func isOn(_ e: Entry) -> Bool {
        (UserDefaults.standard.object(forKey: e.key) as? Bool) ?? onFromInstall.contains(e.env)
    }
    /// Stores a choice — only where it differs from what the installation
    /// starts with, so that a switch put back follows the installation again
    /// when a later one starts it differently.
    static func set(_ e: Entry, on: Bool) {
        if on == onFromInstall.contains(e.env) { UserDefaults.standard.removeObject(forKey: e.key) }
        else { UserDefaults.standard.set(on, forKey: e.key) }
    }

    /// KL_TEST_SWITCHES=0 on the launch line hides the group and ignores what
    /// it stored, for a run that must not depend on a leftover choice.
    static var available: Bool { klEnvOn("KL_TEST_SWITCHES", default: true) }

    static var entries: [Entry] {
        [
            Entry(env: "KL_GLFB_DEPTH_LINEAR", value: "0",
                  title: klT("Depth textures: nearest instead of linear without compare",
                             "Tiefentexturen: nächster Wert statt linear ohne Vergleich"),
                  note: klT("GL games. Changes what a game reads from a depth texture it filters linearly; look at lights, fog and particles in a level.",
                            "GL-Spiele. Ändert, was ein Spiel aus einer linear gefilterten Tiefentextur liest; in einem Level auf Lichter, Nebel und Partikel achten.")),
            Entry(env: "KL_VRR_FLOOR", value: "0.50",
                  title: klT("Foveation: no zone below half resolution",
                             "Foveation: keine Zone unter halber Auflösung"),
                  note: klT("GL games with foveated rendering. The edge of the picture gets sharper textures and costs about 70% more fragments; watch the frame rate.",
                            "GL-Spiele mit Foveated Rendering. Der Bildrand bekommt schärfere Texturen und kostet etwa 70 % mehr Bildpunkte; auf die Bildrate achten.")),
            Entry(env: "KL_OVRP_ONE_POSE", value: "1",
                  title: klT("Head pose: one per frame of the game",
                             "Kopfpose: eine pro Frame des Spiels"),
                  note: klT("Games that talk to Meta's own interface, such as Moss or Tetris Effect. The game gets one head pose for a whole frame instead of a newer one several times within it. Does nothing in games that come through OpenXR (Half-Life: Alyx, Steam Link, Doom 3, Walkabout, Beat Saber 1.45). Turn the head quickly in a level and compare: does the picture stay steadier, or does it lag?",
                            "Spiele, die über Metas eigene Schnittstelle laufen, etwa Moss oder Tetris Effect. Das Spiel bekommt für einen ganzen Frame eine Kopfpose statt mehrmals darin eine neuere. Bewirkt nichts in Spielen, die über OpenXR kommen (Half-Life: Alyx, Steam Link, Doom 3, Walkabout, Beat Saber 1.45). Im Level den Kopf schnell drehen und vergleichen: steht das Bild ruhiger, oder hängt es nach?")),
            Entry(env: "KL_OVRP_EYE_DESC_CAP", value: "1",
                  title: klT("Resolution: the game's own scale stays under the limit",
                             "Auflösung: der eigene Faktor des Spiels bleibt unter der Obergrenze"),
                  note: klT("Unity and Unreal games that enlarge the picture themselves. Moss asks for 1.5 times the resolution in a level and falls to 45 frames a second; with this it is drawn at the size it is offered. Watch the frame rate and the sharpness.",
                            "Unity- und Unreal-Spiele, die das Bild selbst vergrößern. Moss verlangt im Level die 1,5-fache Auflösung und fällt auf 45 Bilder pro Sekunde; hiermit wird es in der angebotenen Größe gezeichnet. Auf Bildrate und Schärfe achten.")),
            Entry(env: "KL_STATS_CARD", value: "1",
                  title: klT("Show performance in the headset from the start",
                             "Leistungsanzeige im Headset von Anfang an einblenden"),
                  note: klT("A small panel just above the middle of the view, like the performance overlay of a Quest: frames a second, pictures that had to be shown twice, GPU and CPU load, memory, heat and picture size, with the last minute as graphs. In every game, holding Create and Options on the controllers together for a second shows it and hides it again; this switch only decides whether it is there when the game starts. Half-Life: Alyx has this as a setting of its own.",
                            "Eine kleine Tafel knapp über der Blickmitte, wie die Leistungsanzeige einer Quest: Bilder pro Sekunde, Bilder, die doppelt gezeigt werden mussten, Auslastung von GPU und CPU, Speicher, Wärme und Bildgröße, dazu die letzte Minute als Verlauf. In jedem Spiel blendet Create und Options an den Controllern, eine Sekunde lang zusammen gehalten, sie ein und wieder aus; dieser Schalter bestimmt nur, ob sie beim Start des Spiels schon zu sehen ist. Half-Life: Alyx hat dafür eine eigene Einstellung.")),
            Entry(env: "KL_VK_ANISO", value: "16",
                  title: klT("Textures: 16x anisotropic filtering",
                             "Texturen: 16-fache anisotrope Filterung"),
                  note: klT("Vulkan games. Surfaces seen at a shallow angle – floors, table tops – stay sharp further away. Games built for a Quest often ask for none at all (Moss does). Look at such surfaces and at the frame rate.",
                            "Vulkan-Spiele. Flächen, die man flach ansieht – Böden, Tischplatten –, bleiben weiter hinten scharf. Für die Quest gebaute Spiele verlangen oft gar keine (Moss etwa). Auf solche Flächen und auf die Bildrate achten.")),
            Entry(env: "KL_GUEST_PACE_DIV", value: "2",
                  title: klT("Frame rate: the game draws every second picture of the display",
                             "Bildrate: das Spiel zeichnet jedes zweite Bild des Displays"),
                  note: klT("An even 60 of 120 (or 50 of 100) frames a second, with the head movement still corrected at the full rate – for a game that does not quite hold the full rate and stutters unevenly. Not applied while the system itself shows the game at half rate. Half-Life: Alyx has this as a setting of its own.",
                            "Gleichmäßige 60 von 120 (oder 50 von 100) Bildern pro Sekunde, die Kopfbewegung wird weiter mit voller Rate ausgeglichen – für ein Spiel, das die volle Rate nicht ganz hält und ungleichmäßig ruckelt. Greift nicht, solange das System das Spiel selbst mit halber Rate zeigt. Half-Life: Alyx hat dafür eine eigene Einstellung.")),
            Entry(env: "KL_FOVE_BOX", value: "0.30",
                  title: klT("Foveated rendering “Strong”: the profile it had before",
                             "Foveated Rendering „Stark“: das frühere Profil"),
                  note: klT("Vulkan games with foveated rendering at Strong. Strong has a smaller sharp middle (18% of the picture's height) and a finer edge now; this brings back the larger middle (30%) with the coarser edge. Turn it on if a game looked better before.",
                            "Vulkan-Spiele mit Foveated Rendering auf „Stark“. „Stark“ hat jetzt eine kleinere scharfe Mitte (18 % der Bildhöhe) und einen feineren Rand; das hier bringt die größere Mitte (30 %) mit dem gröberen Rand zurück. Einschalten, wenn ein Spiel vorher besser aussah."),
                  also: [("KL_FOVE_FLOOR", "0.08")]),
            Entry(env: "KL_OVRP_EYE_MAX", value: "4608",
                  title: klT("Resolution: the limit is 4608 pixels an eye (3072 for most games)",
                             "Auflösung: die Obergrenze liegt bei 4608 Bildpunkten je Auge (sonst meist 3072)"),
                  note: klT("Unity and Unreal games. Draws about twice as many pixels; meant to be used with foveated rendering (start window) at Medium or Strong. Watch the frame rate.",
                            "Unity- und Unreal-Spiele. Zeichnet etwa doppelt so viele Bildpunkte; gedacht zusammen mit Foveated Rendering (Startfenster) auf Mittel oder Stark. Auf die Bildrate achten.")),
            Entry(env: "KL_OVRP_EYE_ALIGN", value: "4",
                  title: klT("Resolution: the eye size is a multiple of 4",
                             "Auflösung: die Augengröße ist ein Vielfaches von 4"),
                  note: klT("Unreal games that draw with Vulkan. Unreal rounds its own buffers up to a multiple of 4; when the eye picture is one or two rows smaller, foveated rendering is on and does nothing. On for Moss.",
                            "Unreal-Spiele, die mit Vulkan zeichnen. Unreal rundet seine eigenen Puffer auf ein Vielfaches von 4; ist das Augenbild ein, zwei Zeilen kleiner, ist Foveated Rendering eingeschaltet und bewirkt nichts. Bei Moss an.")),
            Entry(env: "KL_AAUDIO_OLD_BURST", value: "1",
                  title: klT("Sound: the old buffer size (240 frames)",
                             "Ton: die alte Puffergröße (240 Frames)"),
                  note: klT("Games that play through AAudio (most Unreal and newer Unity games) are handed 256 frames at a time now, which divides the blocks sound engines mix in; 240 could be heard as a fine crackle. Turn this on if a game sounds worse than it used to.",
                            "Spiele, die über AAudio spielen (die meisten Unreal- und neueren Unity-Spiele), bekommen jetzt 256 Frames auf einmal; das teilt die Blöcke, in denen Sound-Engines mischen, 240 konnte als feines Knistern hörbar sein. Einschalten, wenn ein Spiel schlechter klingt als früher.")),
            Entry(env: "KL_XR_BOOT_GATE", value: "1",
                  title: klT("OpenXR: the first frame waits for head and hands",
                             "OpenXR: das erste Bild wartet auf Kopf und Hände"),
                  note: klT("OpenXR games (Steam Link, Counter-Strike, Doom 3). For a game that starts with a tilted floor or the controllers in the wrong place: it is held up to four seconds until the headset has reported both.",
                            "OpenXR-Spiele (Steam Link, Counter-Strike, Doom 3). Für ein Spiel, das mit schiefem Boden oder falsch liegenden Controllern startet: es wird bis zu vier Sekunden gehalten, bis das Headset beides gemeldet hat."),
                  also: [("KL_XR_HONEST_HEAD", "1")]),
            Entry(env: "KL_CROWN_SILENCE", value: "0",
                  title: klT("Sound: do not go silent when the game leaves the view",
                             "Ton: nicht stummschalten, wenn das Spiel die Sicht verlässt"),
                  note: klT("The sound stops at once on a press of the Digital Crown and comes back with the game. Switch this on if a game stays silent after a system window was open.",
                            "Beim Druck auf die Digital Crown verstummt der Ton sofort und kommt mit dem Spiel zurück. Einschalten, falls ein Spiel stumm bleibt, nachdem ein Systemfenster offen war.")),
            Entry(env: "KL_FMOD_BLOCK", value: "256",
                  title: klT("Sound: FMOD is told a block of 256 frames (512 so far)",
                             "Ton: FMOD bekommt 256 Frames als Blockgröße genannt (bisher 512)"),
                  note: klT("Games whose sound engine is FMOD (Asgard's Wrath 2, Vampire). Listen for dropouts and for sound that lags.",
                            "Spiele mit FMOD als Sound-Engine (Asgard's Wrath 2, Vampire). Auf Aussetzer und nachhängenden Ton achten.")),
            Entry(env: "KL_AUDIO_ONE_OUTPUT", value: "1",
                  title: klT("Sound: the game is shown one audio output instead of none",
                             "Ton: dem Spiel wird ein Tonausgang gezeigt statt keinem"),
                  note: klT("For a game that stays silent: some sound engines pick their output from the list of devices and give up on an empty one.",
                            "Für ein Spiel, das stumm bleibt: manche Sound-Engines wählen ihren Ausgang aus der Geräteliste und geben bei einer leeren auf.")),
            Entry(env: "KL_AUDIO_SINK2", value: "1",
                  title: klT("Sound: the new audio path",
                             "Ton: die neue Audio-Implementierung"),
                  note: klT("Every game with sound. Everything between the game's sound interface and the output is a second implementation, from rebelancap's fork: every sample format is converted properly, one of the game's sound outputs owns the device (the first one that is heard, instead of two being mixed), and the pauses between a sound engine's blocks are waited out instead of played as gaps. Listen for crackle, dropouts, sound that lags behind the picture, and whether music, voices and effects are all there. The switch for the old buffer size does nothing with it. Steam Link plays through two outputs at once; with this on only one of them is heard at a time.",
                            "Jedes Spiel mit Ton. Alles zwischen der Tonschnittstelle des Spiels und der Ausgabe ist eine zweite Implementierung aus rebelancaps Fork: jedes Sample-Format wird richtig umgewandelt, einer der Tonausgänge des Spiels besitzt das Gerät (der erste, der zu hören ist, statt dass zwei gemischt werden), und die Pausen zwischen den Blöcken einer Sound-Engine werden abgewartet statt als Lücken gespielt. Auf Knistern, Aussetzer, nachhängenden Ton achten und darauf, ob Musik, Stimmen und Effekte alle da sind. Der Schalter für die alte Puffergröße bewirkt damit nichts. Steam Link spielt über zwei Ausgänge zugleich; hiermit ist immer nur einer davon zu hören.")),
            Entry(env: "KL_EGL_CREATE_CONTEXT_EXT", value: "1",
                  title: klT("EGL: name EGL_KHR_create_context",
                             "EGL: EGL_KHR_create_context nennen"),
                  note: klT("Every game. Engines then ask for another GL context: Beat Saber for GLES 3.2, Asgard's Wrath 2 for GLES 3.2 where it took GLES 2. Watch the start and the loading screens.",
                            "Jedes Spiel. Engines fragen dann einen anderen GL-Kontext an: Beat Saber GLES 3.2, Asgard's Wrath 2 GLES 3.2 statt bisher GLES 2. Auf Start und Ladebildschirme achten.")),
            Entry(env: "KL_GL_BACKEND_EXT", value: "1",
                  title: klT("GL: also offer anisotropic filtering, DXT and BGRA",
                             "GL: auch anisotrope Filterung, DXT und BGRA anbieten"),
                  note: klT("GL games. A Unity game may then filter textures anisotropically: sharper floors at flat angles.",
                            "GL-Spiele. Ein Unity-Spiel filtert Texturen dann womöglich anisotrop: schärfere Böden bei flachem Blick.")),
        ]
    }

    /// Into the environment, before kl_app_configure. Never over the launch
    /// line's value.
    static func applyEnvironment() {
        guard available else { return }
        // setenv with overwrite 0: of two switches for one variable the first
        // listed wins, and the launch line wins over both.
        for e in entries where isOn(e) {
            setenv(e.env, e.value, 0)
            for (k, v) in e.also { setenv(k, v, 0) }
            NSLog("[app] test switch: \(e.env)=\(e.value)"
                  + e.also.map { " \($0.0)=\($0.1)" }.joined()
                  + (UserDefaults.standard.object(forKey: e.key) == nil ? " (on from the installation)" : ""))
        }
    }
}

struct TestSwitchesView: View {
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(klT("Changes that are built in but not switched on yet. Each applies from the next start of the game.",
                     "Änderungen, die eingebaut, aber noch nicht eingeschaltet sind. Jede gilt ab dem nächsten Start des Spiels."))
                .font(.caption).foregroundStyle(.secondary)
            ForEach(KleptonTestSwitches.entries) { e in
                TestSwitchRow(entry: e)
            }
        }
    }
}

private struct TestSwitchRow: View {
    let entry: KleptonTestSwitches.Entry
    @State private var on: Bool
    init(entry: KleptonTestSwitches.Entry) {
        self.entry = entry
        _on = State(initialValue: KleptonTestSwitches.isOn(entry))
    }
    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Toggle(entry.title, isOn: $on)
                .onChange(of: on) { _, v in KleptonTestSwitches.set(entry, on: v) }
            Text(entry.note + " (" + entry.env + "=" + entry.value
                 + entry.also.map { " " + $0.0 + "=" + $0.1 }.joined() + ")")
                .font(.caption).foregroundStyle(.secondary)
            if KleptonTestSwitches.onFromInstall.contains(entry.env) {
                Text(klT("On from the start in this installation: AVP Play's experimental setting asked for it.",
                         "In dieser Installation von Anfang an eingeschaltet: Die experimentelle Einstellung von AVP Play hat es so verlangt."))
                    .font(.caption).foregroundStyle(.orange)
            }
        }
    }
}

extension KleptonRendering {
    /// The part of the settings the runtime reads from its environment, which
    /// it starts doing in kl_app_configure — so this runs before that, where
    /// `apply` runs after. Never over a value given on the launch line.
    func applyEnvironment() {
        guard !klIsAlyx() else { return }
        // Foveated rendering for a Vulkan guest. kl_fove reads KL_FOVEATION the
        // first time it is asked, which is after this. A GL guest never asks.
        let launchLine = ProcessInfo.processInfo.environment["KL_FOVEATION"]
        if vkFoveation != "off" {
            setenv("KL_FOVEATION", vkFoveation == "ultra" ? "high" : vkFoveation, 0)
            if vkFoveation == "high" {
                // Since 2026-10-13: a smaller sharp middle (0.18 of the height, was 0.30) and an edge twice
                // as fine (0.16, was 0.08) – about a fifth fewer pixels, and the coarse edge was what showed.
                // Asked for and kept on by the owner after Moss with it on the headset.
                setenv("KL_FOVE_BOX", "0.18", 0)
                setenv("KL_FOVE_FLOOR", "0.16", 0)
            } else if vkFoveation == "ultra" {
                setenv("KL_FOVE_BOX", "0.20", 0)
                setenv("KL_FOVE_RAMP", "0.15", 0)
                setenv("KL_FOVE_FLOOR", "0.06", 0)
            }
        }
        NSLog("[cp] foveated rendering (Vulkan): setting \(vkFoveation) (target default \(Self.targetVkFoveation))"
              + (launchLine.map { " — overridden by KL_FOVEATION=\($0)" } ?? ""))
        guard let o = AlyxOutput(rawValue: output), o != .full else { return }
        setenv("KL_CP_QUALITY", o.quality, 0)
        NSLog("[cp] display output: setting \(o.title) (render quality \(o.quality))")
    }

    /// As the app leaves: what this run learned that the next start window
    /// needs. Written through at once, because the process ends right after.
    func rememberRun() {
        let seen = Int(kl_vulkan_guest_samples())
        guard seen > 0, seen != Self.vkGuestSamples else { return }
        UserDefaults.standard.set(seen, forKey: Self.vkGuestSamplesKey)
        UserDefaults.standard.synchronize()
    }
}

struct RenderingView: View {
    @ObservedObject private var rendering = KleptonRendering.shared
    @ObservedObject private var metalfx = KleptonMetalFX.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Toggle("Foveated rendering (sharp centre, cheaper edges)",
                   isOn: $rendering.foveation)
            Text("Fixed, not eye-tracked: the sharp region stays in place. Off "
                 + "renders the whole picture at full rate. Takes effect the "
                 + "next time the app starts.")
                .font(.caption).foregroundStyle(.secondary)
            if KleptonRendering.targetHasMSAA {
                Picker("Edge smoothing (MSAA)", selection: $rendering.msaa) {
                    Text("Off").tag(0); Text("2x").tag(2); Text("4x").tag(4)
                }
                .pickerStyle(.segmented)
            }
            if !klIsAlyx() {
                Picker(klT("Display output", "Ausgabe aufs Display"), selection: $rendering.output) {
                    ForEach(AlyxOutput.ordered) { Text($0.name).tag($0.rawValue) }
                }
                .pickerStyle(.segmented)
                Text(AlyxOutput.note + " "
                     + klT("Takes effect the next time the app starts.", "Wirkt beim nächsten Start der App."))
                    .font(.caption).foregroundStyle(.secondary)
            }
            Toggle("In-game videos", isOn: $rendering.video)
            Text("Tutorial and cutscene clips in Vulkan games. On by default "
                 + "where it has been confirmed to work; turn it off if a game "
                 + "crashes at a clip. Takes effect the next time the app "
                 + "starts.")
                .font(.caption).foregroundStyle(.secondary)
            HStack {
                Text("Render resolution")
                Slider(value: $rendering.renderScale, in: KleptonRendering.minRenderScale...1.0, step: 0.05)
                Text(String(format: "%.0f%%", rendering.renderScale * 100))
                    .monospacedDigit().frame(width: 52, alignment: .trailing)
            }
            Text("Size of the picture the game renders, per axis. Lower it if "
                 + "looking around stutters: a game that saturates the GPU "
                 + "halves the rate head movement is corrected at. Takes "
                 + "effect the next time the app starts.")
                .font(.caption).foregroundStyle(.secondary)
            Toggle("MetalFX upscaling", isOn: $metalfx.enabled)
            Text(klT("Sharpens the picture when the resolution the game was started with is below 100%: the game draws small and Apple's scaler stretches it to eye size instead of a plain linear stretch. Costs some graphics time. Switching takes effect on the next frame; a new resolution only with the next start. Works in every game: Vulkan and GL, through Meta's interface or OpenXR, with foveated rendering too. The metrics panel shows whether it is upscaling.",
                     "Schärft das Bild, wenn die Auflösung, mit der das Spiel gestartet wurde, unter 100 % liegt: Das Spiel zeichnet klein, und Apples Scaler streckt es auf Augengröße statt einer einfachen linearen Streckung. Kostet etwas Grafikzeit. Das Umschalten wirkt ab dem nächsten Bild, eine neue Auflösung erst beim nächsten Start. Wirkt in jedem Spiel: Vulkan und GL, über Metas Schnittstelle oder OpenXR, auch mit Foveated Rendering. Ob hochskaliert wird, zeigt das Leistungs-Panel."))
                .font(.caption).foregroundStyle(.secondary)
            if KleptonMetalFX.onFromInstall {
                Text(klT("On from the start in this installation: AVP Play's experimental setting asked for it.",
                         "In dieser Installation von Anfang an eingeschaltet: Die experimentelle Einstellung von AVP Play hat es so verlangt."))
                    .font(.caption).foregroundStyle(.orange)
            }
        }
    }
}

// The language the game is told the device speaks.
//
// A Quest game reads the device's language and many decide everything from it:
// Asgard's Wrath 2 has no language menu at all, text and voices follow the
// device. Here nothing sets a language for the guest, so every game was told
// en_US — and a player with a German headset and the German voice pack on it
// heard English. The device's own language is passed along (KleptonApp.init,
// KL_DEVICE_LANG) and this says whether the guest hears about it
// (KL_LOCALE_DEVICE, read in kl_jni_services.c).
//
// The default is the target's: the device's language where a game is known to
// depend on it (wrath2), English — what every game got so far — elsewhere, so
// that nothing changes language unasked. Read once, at boot.
final class KleptonLanguage: ObservableObject {
    static let shared = KleptonLanguage()
    private static let key = "klepton.gameLanguage"      // "device" | "en"

    static let targetDefault: String =
        String(cString: kl_app_target_name_or_default()) == "wrath2" ? "device" : "en"

    /// The device's first language in its own words ("Deutsch"), for the picker.
    static let deviceLanguageName: String = {
        guard let first = Locale.preferredLanguages.first else { return "English" }
        let loc = Locale(identifier: first)
        let code = loc.language.languageCode?.identifier ?? "en"
        return (loc.localizedString(forLanguageCode: code) ?? code).capitalized(with: loc)
    }()
    static let deviceIsEnglish: Bool =
        Locale.preferredLanguages.first?.lowercased().hasPrefix("en") ?? true

    @Published var choice: String {
        didSet { UserDefaults.standard.set(choice, forKey: Self.key) }
    }

    private init() {
        let stored = UserDefaults.standard.string(forKey: Self.key)
        choice = (stored == "device" || stored == "en") ? stored! : Self.targetDefault
    }

    /// Before kl_app_boot. An explicit KL_LOCALE_DEVICE on a launch line wins.
    func apply() {
        setenv("KL_LOCALE_DEVICE", choice == "device" ? "1" : "0", 0)
        NSLog("[app] game language: \(choice == "device" ? "the device's (\(Self.deviceLanguageName))" : "English")"
              + " (target default \(Self.targetDefault))")
    }
}

struct GameLanguageView: View {
    @ObservedObject private var language = KleptonLanguage.shared

    var body: some View {
        // A device set to English has nothing to choose between.
        if !KleptonLanguage.deviceIsEnglish {
            VStack(alignment: .leading, spacing: 8) {
                Picker(klT("Game language", "Sprache des Spiels"), selection: $language.choice) {
                    Text(KleptonLanguage.deviceLanguageName).tag("device")
                    Text("English").tag("en")
                }
                .pickerStyle(.segmented)
                Text(klT("What the game is told this headset is set to. Games that offer the "
                         + "language use it for text and, where their voice pack is installed, "
                         + "for voices. Takes effect the next time the app starts.",
                         "Was dem Spiel als Sprache dieser Brille gemeldet wird. Spiele, die die "
                         + "Sprache anbieten, nutzen sie für Texte und – wenn ihr Sprachpaket "
                         + "installiert ist – für die Stimmen. Gilt ab dem nächsten Start der App."))
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
    }
}

// The name the game is told its player has.
//
// A Quest game takes the player's name from the Meta account and offers no way
// to change it — it is not the game's to change. Here the account is the
// offline user of kl_ovrplat.c, whose name was "kleptonplayer" for everybody.
// Demeo shows it as the player's name, and its owner asked for a way to set it
// (2026-10-13). Read once, at boot (KL_PLAT_USER_NAME).
//
// For a game that keeps an account of its own under the user's id (Demeo), the
// id is made here as well: one random number per installation instead of the
// same number for every player (KL_PLAT_USER_ID, kl_ovrplat_user_id_per_install).
final class KleptonPlayer: ObservableObject {
    static let shared = KleptonPlayer()
    private static let nameKey = "klepton.playerName"
    private static let idKey = "klepton.playerId"

    /// Only where a name goes anywhere: the games that get the offline user.
    static let offered: Bool = kl_ovrplat_offline_user_for(kl_app_target_name_or_default()) != 0
    static let ownId: Bool = kl_ovrplat_user_id_per_install(kl_app_target_name_or_default()) != 0

    @Published var name: String {
        didSet { UserDefaults.standard.set(name, forKey: Self.nameKey) }
    }

    private init() {
        name = UserDefaults.standard.string(forKey: Self.nameKey) ?? ""
    }

    /// Before kl_app_boot. A value on a launch line wins.
    func apply() {
        guard Self.offered else { return }
        let n = name.trimmingCharacters(in: .whitespacesAndNewlines)
        if !n.isEmpty { setenv("KL_PLAT_USER_NAME", n, 0) }
        if Self.ownId {
            var id = UInt64(UserDefaults.standard.string(forKey: Self.idKey) ?? "") ?? 0
            if id == 0 {
                id = UInt64.random(in: 1_000_000_000_001_000...8_999_999_999_999_999)
                UserDefaults.standard.set(String(id), forKey: Self.idKey)
            }
            setenv("KL_PLAT_USER_ID", String(id), 0)
            // The Steam build of such a game asks Steam's library instead of Meta's: the same number as a
            // Steam id – an individual account in the public universe, its account number taken from ours.
            setenv("KL_STEAM_ID", String(0x0110_0001_0000_0000 as UInt64 + id % 0xFFFF_FFFE + 1), 0)
        }
        if !n.isEmpty { setenv("KL_STEAM_NAME", n, 0) }
        NSLog("[app] player name: " + (n.isEmpty ? "not set (the runtime's own)" : "\"\(n)\"")
              + (Self.ownId ? "; the player id is this installation's own" : ""))
    }
}

struct PlayerNameView: View {
    @ObservedObject private var player = KleptonPlayer.shared

    var body: some View {
        if KleptonPlayer.offered {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text(klT("Player name", "Spielername"))
                    TextField("kleptonplayer", text: $player.name)
                        .textFieldStyle(.roundedBorder)
                        .autocorrectionDisabled()
                        .textInputAutocapitalization(.never)
                        .frame(maxWidth: 320)
                }
                Text(klT("The name the game is told you have. Games that show a player name use it; "
                         + "where a game wants a name without spaces, letters, digits, '.', '_' "
                         + "and '-' are kept. Takes effect the next time the app starts.",
                         "Der Name, der dem Spiel für dich gemeldet wird. Spiele, die einen "
                         + "Spielernamen anzeigen, nutzen ihn; wo ein Spiel einen Namen ohne "
                         + "Leerzeichen erwartet, bleiben Buchstaben, Ziffern, '.', '_' und '-'. "
                         + "Gilt ab dem nächsten Start der App."))
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
    }
}

// Caches of LINKED GL programs that a game keeps in its own files.
//
// Unreal on GLES stores the driver's program binaries
// (android-files/ProgramBinaryCache/GLSL_ES3_1_ANDROID_<hash>) and at the next
// start hands them back with glProgramBinary instead of compiling its shaders.
// It names the cache after the GL strings, which here are ours and never change
// — while what answers glProgramBinary is ANGLE, and ANGLE refuses a binary made
// by another build of itself (its ANGLE_PROGRAM_VERSION is a hash over its own
// sources). Unreal treats that refusal as fatal: it asks to quit and then runs
// into its own missing programs. That is how Maestro stopped at start after
// toolchain 344 brought the first new ANGLE build since the game was installed
// (seen on the owner's headset, 2026-10-12: no shader compiled before the first
// draw, GL_INVALID_OPERATION on every glUseProgram, a burst of ForceQuit).
// Unity keeps such a cache too and compiles again when one is refused.
//
// So the cache does not outlive the build of the app that made it: when the
// bundle's build (version code and toolchain) differs from the one remembered,
// every ProgramBinaryCache folder in the game's files is removed before the
// guest boots, and the game compiles its programs once more.
enum KleptonProgramCaches {
    private static let key = "klepton.programCacheBuild"

    static func dropIfBuildChanged() {
        let build = (Bundle.main.infoDictionary?["CFBundleVersion"] as? String) ?? ""
        let defaults = UserDefaults.standard
        guard !build.isEmpty, defaults.string(forKey: key) != build else { return }
        let fm = FileManager.default
        var dropped: [String] = []
        if let docs = fm.urls(for: .documentDirectory, in: .userDomainMask).first {
            let root = docs.appendingPathComponent("android-files")
            if let walk = fm.enumerator(at: root, includingPropertiesForKeys: [.isDirectoryKey],
                                        options: [.skipsHiddenFiles]) {
                for case let url as URL in walk {
                    let isDir = (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) ?? false
                    guard isDir else { continue }
                    if url.lastPathComponent == "ProgramBinaryCache" {
                        walk.skipDescendants()
                        if (try? fm.removeItem(at: url)) != nil {
                            dropped.append(url.path.replacingOccurrences(of: root.path + "/", with: ""))
                        }
                    } else if walk.level >= 5 {
                        // Unreal puts it at the top of the files or under
                        // UE4Game/<project>/<project>/Saved; no need to walk a
                        // game's whole content for it.
                        walk.skipDescendants()
                    }
                }
            }
        }
        defaults.set(build, forKey: key)
        NSLog("[app] program caches: build \(build) is new to this copy — "
              + (dropped.isEmpty ? "no ProgramBinaryCache found"
                                 : "removed \(dropped.joined(separator: ", "))"))
    }
}
