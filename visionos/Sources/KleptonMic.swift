// Microphone capture — a single opt-in toggle that lets a guest (Steam Link's
// voice chat) reach the headset's microphone. Modelled on KleptonChroma: a
// persisted Codable setting, an env override, and a small panel in the boot
// window, so it is remembered across launches and adjustable while wearing the
// headset.
//
// OFF by default, and deliberately so. A microphone is a privacy surface: the
// session category it forces (.playAndRecord) also hands the ringer switch a
// veto over the music this app exists to play, and touching the device at all
// makes visionOS show a permission prompt. Neither belongs in a title that
// never asks for the mic. So nothing here engages the microphone — no category
// change, no permission prompt, no capture device presented to the guest —
// until a person turns it on. The C half is kl_audio.c's capture path, armed by
// kl_audio_mic_set_enabled and read by kl_aaudio.c / kl_opensl.c when a guest
// opens an input stream.
import SwiftUI
import AVFAudio

/// The one field, kept as a struct for the same reason KLChromaSettings is: a
/// single JSON blob in UserDefaults migrates cleanly when a field is added
/// later, rather than a loose default key nothing writes.
struct KLMicSettings: Codable, Equatable {
    var enabled = false
}

/// Whether this target's guest has voice chat. Such a target arms the microphone
/// on the first launch after an install — that is what makes visionOS ask its
/// permission question, the same way every other app that needs the microphone
/// does. Everything else keeps the original opt-in: no category change, no
/// prompt, no capture device for a title that never asks.
func klTargetUsesVoiceChat(_ name: String) -> Bool {
    ["x2497738113633933"].contains(name)      // Bigscreen
}

final class KleptonMic: ObservableObject {
    static let shared = KleptonMic()

    private static let defaultsKey = "klepton.mic"

    @Published var settings = KLMicSettings() { didSet { publish(); save() } }

    private init() {
        // Same precedence as KleptonChroma: environment wins where explicitly
        // set, then the saved setting, then the default (off) — except on a voice
        // target's first launch after an install, where there is no saved answer
        // yet and the default has to be "ask". Arming here is what makes visionOS
        // put its permission question on screen; the answer is saved below, so a
        // "no" stays a no until someone turns it back on in Settings.
        var s = KLMicSettings()
        var saved = false
        if let d = UserDefaults.standard.data(forKey: Self.defaultsKey),
           let store = try? JSONDecoder().decode(KLMicSettings.self, from: d) {
            s = store
            saved = true
            NSLog("[mic] restored: \(Self.describe(s))")
        }
        let env = ProcessInfo.processInfo.environment
        if let v = env["KL_MIC"] { s.enabled = v != "0"; saved = true }
        if !saved && klTargetUsesVoiceChat(klTargetName()) {
            s.enabled = true
            NSLog("[mic] first launch on a voice target — asking for the microphone")
        }
        settings = s          // didSet arms the C side and (if on) the session
    }

    /// visionOS said no. The system will not ask again, so the switch goes back to
    /// off and the panel says where to allow it; turning the toggle on here would
    /// only light up a capture that hardware permission keeps silent.
    func disableAfterDenial() {
        guard settings.enabled else { return }
        settings.enabled = false
        NSLog("[mic] permission denied — capture off (Settings > Apps > Klepton > Microphone)")
    }

    private func publish() {
        // Arm the capture side. It stays lazy — the microphone is not actually
        // opened until a guest asks for an input stream — but the session
        // category has to be right first, so both are done here.
        kl_audio_mic_set_enabled(settings.enabled ? 1 : 0)
        KleptonAudio.applyMicCategory(settings.enabled)
    }

    private func save() {
        guard let d = try? JSONEncoder().encode(settings) else { return }
        UserDefaults.standard.set(d, forKey: Self.defaultsKey)
    }

    /// The environment line that reproduces this, for pasting into a
    /// build_run_vpro.sh invocation — the same affordance the other panels have.
    static func describe(_ s: KLMicSettings) -> String {
        "KL_MIC=\(s.enabled ? 1 : 0)"
    }
}

/// The panel, a sibling of ChromaView in the boot window.
struct MicView: View {
    @ObservedObject private var mic = KleptonMic.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(klT("Microphone", "Mikrofon")).font(.headline)
            Text(klT("Let the game use the headset microphone — for voice chat, such as Steam "
                     + "Link's or the players' in a multiplayer room. Off by default. Turning it on asks for "
                     + "microphone permission and switches audio to play-and-record, "
                     + "which lets the silent switch mute app audio.",
                     "Lässt das Spiel das Mikrofon der Brille benutzen – für den Sprachchat, etwa in Steam "
                     + "Link oder mit den Mitspielern in einem Multiplayer-Raum. Standardmäßig aus. Beim Einschalten "
                     + "fragt visionOS nach dem Mikrofon, und der Ton läuft dann über Wiedergabe und Aufnahme, "
                     + "wodurch der Stummschalter den Ton der App stumm schalten kann."))
                .font(.caption).foregroundStyle(.secondary)

            Toggle(klT("Allow microphone", "Mikrofon erlauben"), isOn: $mic.settings.enabled)

            if AVAudioApplication.shared.recordPermission == .denied {
                Text(klT("visionOS has denied microphone access for Klepton. Allow it under Settings → Apps → Klepton → Microphone, then switch this on again.",
                         "visionOS hat den Mikrofon-Zugriff für Klepton verweigert. Erlaube ihn unter Einstellungen → Apps → Klepton → Mikrofon und schalte den Regler danach wieder ein."))
                    .font(.caption).foregroundStyle(.orange)
            }

            Text(KleptonMic.describe(mic.settings))
                .font(.system(.caption2, design: .monospaced))
                .textSelection(.enabled)
                .foregroundStyle(.secondary)
        }
        .padding()
    }
}
