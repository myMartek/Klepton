import SwiftUI

// Minecraft's picture and performance settings: this tree's own, in a sheet of
// the Minecraft launcher.
//
// The reference launcher (vivecraft-visionos 1.0.3) had one choice of its own
// here, "Resolution: Balanced / Sharp / Max", written as KL_OVRP_EYE_MAX into
// klepton-env.txt. In this tree every game's picture is set in its start window
// — how finely it is drawn, MetalFX upscaling, foveated rendering, the frame
// rate, in-between pictures, the performance panel — and stored per game, so
// Minecraft takes those instead of a second, competing dial. The views are the
// tree's (KleptonTuning.swift, KleptonApp.swift) and the stored choices are the
// same keys, so what is set here is what MCBootView applies on the way into a
// boot (applyStartSettings, publishStartSettings).
//
// What is NOT here, because it would be a switch that does not do what it says
// for this game:
//
//  - "Progressive (Digital Crown)". The picture half would work; the sound half
//    fades kl_audio's output, and Minecraft's sound is OpenAL's own device, so
//    at 0 % the room would be all that is left to see while the game played on
//    loudly.
//  - "Passthrough and chroma key", edge smoothing, in-game video, the game
//    language and the player name: each belongs to a kind of guest this is not
//    (a room-showing one, a multiview or Vulkan one, an Android one).
//  - The play area. It is asked for by the start window's Start button of the
//    other games and is not wired into this launcher's Play.
struct MCPictureSettings: View {
    /// Locked while a game is starting or running: the picture is fixed when
    /// the game starts, and a dial that moves under it would only say something
    /// the running game is not doing.
    var locked: Bool
    @Environment(\.dismiss) private var dismiss
    @ObservedObject private var rendering = KleptonRendering.shared
    @ObservedObject private var chroma = KleptonChroma.shared
    @AppStorage(KleptonControllerMode.preferenceKey) private var controllerMode = KleptonControllerMode.selected().rawValue

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    GroupBox(klT("Picture", "Bild")) {
                        VStack(alignment: .leading, spacing: 10) {
                            Toggle(klT("Foveated rendering", "Foveated Rendering"), isOn: $rendering.foveation)
                            Text(klT("Keeps the middle of the picture sharp and draws the edges more coarsely, through the display's own map. Minecraft is drawn at a larger size with it (4864 pixels across an eye instead of 3072) for about half the work; off, the whole picture is drawn evenly at the smaller size.",
                                     "Hält die Bildmitte scharf und zeichnet die Ränder gröber, nach der Karte des Displays selbst. Minecraft wird damit größer gezeichnet (4864 statt 3072 Pixel je Auge in der Breite) bei etwa der halben Arbeit; aus wird das ganze Bild gleichmäßig in der kleineren Größe gezeichnet."))
                                .font(.callout).foregroundStyle(.secondary)
                            PicturePrimary(hintFont: .callout)
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    GroupBox(klT("Frame rate", "Bildrate")) {
                        MCFrameRatePicker().frame(maxWidth: .infinity, alignment: .leading)
                    }
                    GroupBox(klT("Frame generation", "Zwischenbilder")) {
                        MCFrameGenPicker().frame(maxWidth: .infinity, alignment: .leading)
                    }
                    GroupBox(klT("Your hands", "Deine Hände")) {
                        VStack(alignment: .leading, spacing: 10) {
                            Toggle(klT("Show my real hands over the game", "Meine echten Hände über dem Spiel zeigen"),
                                   isOn: $chroma.settings.handMatting)
                            Text(klT("Your real arms, hands and the controllers in them, over the world. On for Minecraft unless turned off.",
                                     "Deine echten Arme, Hände und die Controller darin, über der Welt. Für Minecraft an, solange nicht ausgeschaltet."))
                                .font(.callout).foregroundStyle(.secondary)
                            HandPinchView()
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    KleptonControllerSettingsView()
                    if controllerMode == KleptonControllerMode.standard.rawValue {
                        GroupBox(klT("Game controller", "Gamecontroller")) { GamepadStatusView() }
                    }
                    DisclosureGroup(klT("Advanced", "Erweitert")) {
                        VStack(alignment: .leading, spacing: 14) {
                            GroupBox(klT("Max resolution per eye", "Maximale Auflösung je Auge")) { EyeMaxView() }
                            GroupBox(klT("Display output", "Ausgabe aufs Display")) {
                                VStack(alignment: .leading, spacing: 8) {
                                    Picker(klT("Display output", "Ausgabe aufs Display"), selection: $rendering.output) {
                                        ForEach(AlyxOutput.ordered) { Text($0.name).tag($0.rawValue) }
                                    }
                                    .pickerStyle(.segmented)
                                    .labelsHidden()
                                    Text(AlyxOutput.note)
                                        .font(.caption).foregroundStyle(.secondary)
                                }
                                .frame(maxWidth: .infinity, alignment: .leading)
                            }
                            DisclosureGroup(klT("Controller alignment", "Ausrichtung der Controller")) { TuningView() }
                            // The performance panel is the first of these ("Show
                            // performance in the headset from the start"); in the
                            // game, Create and Options held together for a second
                            // show and hide it either way.
                            if KleptonTestSwitches.available {
                                DisclosureGroup(klT("Test", "Test")) { TestSwitchesView() }
                            }
                        }
                        .padding(.top, 8)
                    }
                    Label(klT("These apply the next time Minecraft is started with Play. In the game, hold Create and Options together for a second to show the performance panel.",
                              "Das gilt ab dem nächsten Start von Minecraft mit Play. Im Spiel Create und Options eine Sekunde zusammen halten, um die Leistungsanzeige einzublenden."),
                          systemImage: "info.circle")
                        .font(.callout).foregroundStyle(.secondary)
                }
                .padding(24)
                .disabled(locked)
            }
            .navigationTitle(klT("Picture and performance", "Bild und Leistung"))
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button(klT("Done", "Fertig")) { dismiss() }
                }
            }
        }
        .frame(minWidth: 620, minHeight: 640)
    }

    /// One line for the launcher's row: what the next Play will be drawn with.
    static var summary: String {
        let r = KleptonRendering.shared
        var parts = [String(format: "%.0f %%", r.renderScale * 100)]
        if KleptonMetalFX.shared.enabled, r.renderScale < 0.999 { parts.append("MetalFX") }
        parts.append(r.foveation ? klT("foveated", "foveated") : klT("not foveated", "ohne Foveation"))
        parts.append(klT("up to \(r.maxRate) Hz", "bis \(r.maxRate) Hz"))
        if let m = KleptonFrameGen.Mode(rawValue: r.frameGen), m != .off {
            parts.append(klT("in-between pictures", "Zwischenbilder"))
        }
        return parts.joined(separator: " · ")
    }
}

// The two pickers below were shared views (FrameRatePicker, FrameGenPicker in KleptonApp.swift) until the
// settings window of 905 replaced them with rows that live in its pages (KLFrameRateRow, KLFrameGenRow).
// This sheet is not one of those pages, and the Minecraft app failed to build without them (avp-play #141),
// so it keeps its own copies: the same settings, the rates of this headset as the settings window shows them.

/// The frame-rate choice (KleptonRendering.maxRate) with what it does.
private struct MCFrameRatePicker: View {
    @ObservedObject private var rendering = KleptonRendering.shared

    var body: some View {
        let r = KLDevice.kind.rates
        VStack(alignment: .leading, spacing: 10) {
            Picker(klT("Frame rate", "Bildrate"), selection: $rendering.maxRate) {
                Text(klT("Up to \(r.mixed)", "Bis \(r.mixed)")).tag(100)
                Text(klT("Up to \(r.full)", "Bis \(r.full)")).tag(120)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            Text(klT("Up to \(r.mixed) is the default. Up to \(r.full) is smoother where the game manages it and the Vision Pro's display can do it. When a game cannot keep up, the Vision Pro halves the rate for a while.",
                     "Bis \(r.mixed) ist der Standard. Bis \(r.full) ist flüssiger, wo das Spiel es schafft und das Display der Vision Pro es kann. Kommt ein Spiel nicht mit, halbiert die Vision Pro die Rate eine Weile."))
                .font(.callout).foregroundStyle(.secondary)
        }
    }
}

/// The frame-generation choice (KleptonRendering.frameGen): its three modes and what they do.
private struct MCFrameGenPicker: View {
    @ObservedObject private var rendering = KleptonRendering.shared
    @ObservedObject private var metalfx = KleptonMetalFX.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Picker(klT("Frame generation", "Zwischenbilder"), selection: $rendering.frameGen) {
                Text(klT("Off", "Aus")).tag(KleptonFrameGen.Mode.off.rawValue)
                Text(klT("Ahead", "Vorausberechnet")).tag(KleptonFrameGen.Mode.ahead.rawValue)
                Text(klT("In between", "Dazwischen")).tag(KleptonFrameGen.Mode.between.rawValue)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            Text(klT("For a game that draws fewer pictures than the display shows. Ahead: where the game has no new picture, one is made from its last two, moved on to the moment it is shown. In between: the middle between the game's last two pictures – smoother, and what the game draws comes 10 ms later; the head's movement stays as quick.",
                     "Für ein Spiel, das weniger Bilder zeichnet, als das Display zeigt. Vorausberechnet: Wo das Spiel kein neues Bild hat, wird eins aus seinen letzten beiden errechnet, bis zu dem Moment weitergerückt, in dem es gezeigt wird. Dazwischen: die Mitte zwischen den letzten beiden Bildern des Spiels – weicher, und was das Spiel zeichnet, kommt 10 ms später; die Kopfbewegung bleibt so schnell."))
                .font(.callout).foregroundStyle(.secondary)
            // The compositor makes no in-between picture where MetalFX swapped the eye's picture.
            if rendering.frameGen != KleptonFrameGen.Mode.off.rawValue, metalfx.enabled {
                Text(klT("While MetalFX upscaling is on, no in-between pictures are made – choose one of the two.",
                         "Solange MetalFX-Hochskalieren an ist, entstehen keine Zwischenbilder – eins von beiden wählen."))
                    .font(.callout).foregroundStyle(.orange)
            }
        }
    }
}
