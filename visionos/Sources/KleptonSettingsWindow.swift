// Every game's settings, in the layout the owner liked in LambdaVision's
// Half-Life (2026-11-16: "Das settings Menü für Half Life gefällt mir sehr von
// der Struktur … mit seinem zwei bzw drei geteilten Layout und den presets"):
// pages in a sidebar on the left, the page's controls in the middle, and on
// the right what the setting you are on does, with a picture of it. The first
// page is Presets (KleptonPictureProfile.swift): three that come with the game
// and the player's own, which can be shared as text.
//
// Written for Klepton's own state, not copied: the controls are the same
// stored settings as before (KleptonRendering, KleptonMetalFX, KleptonEyeMax,
// Alyx's dials), so nothing a player chose changes with the new window, and a
// game's pages are its own – Alyx gets its dials, a launcher game its launcher.
//
// The pictures on the right are general, the same for every game: one scene
// (made for this, no game's art) shown with and without the setting.
import SwiftUI
import Darwin

// MARK: - The headset

enum KLDevice {
    /// Which Vision Pro this is (KLDeviceKind). KL_DEVICE_KIND=m2|m5 overrides, to try the other's window.
    static let kind: KLDeviceKind = {
        var size = 0
        sysctlbyname("hw.machine", nil, &size, nil, 0)
        var raw = [CChar](repeating: 0, count: max(size, 1))
        sysctlbyname("hw.machine", &raw, &size, nil, 0)
        return KLDeviceKind.from(model: String(cString: raw),
                                 override: ProcessInfo.processInfo.environment["KL_DEVICE_KIND"])
    }()
}

// MARK: - The picture as stored, and the presets made from it

enum KLPictureNow {
    /// The game's own cap per eye, by name (the window asks before the game is loaded).
    static var titleCap: Int { Int(kl_ovrp_title_eye_cap(klTargetName())) }

    private static func alyxDials(_ d: AlyxDials) -> KLAlyxPicture {
        KLAlyxPicture(quality: d.quality.rawValue, edges: d.edges.rawValue, view: d.view.rawValue,
                      foveation: d.foveation.rawValue, output: d.output.rawValue, cadence: d.cadence.rawValue,
                      effects: d.effects.rawValue, texturePool: d.texturePool.rawValue,
                      textureCap: d.textureCap.rawValue)
    }

    /// What a new installation of this game starts with: the preset "Vision Pro M5".
    static var standard: KLPicture {
        KLPicture(renderScale: KleptonRendering.targetRenderScale,
                  metalFX: KleptonMetalFX.onFromInstall,
                  eyeMax: KLPictureRules.eyeMaxByTitle,
                  foveation: KleptonRendering.targetFoveation,
                  vkFoveation: KleptonRendering.targetVkFoveation,
                  msaa: 0,
                  vkMsaa: KleptonRendering.targetVkMsaa,
                  output: KleptonRendering.targetOutput.rawValue,
                  alyx: klIsAlyx() ? alyxDials(AlyxPreset.balanced.dials) : nil)
    }

    /// The picture as it is set now.
    static func read() -> KLPicture {
        let r = KleptonRendering.shared
        var alyx: KLAlyxPicture?
        if klIsAlyx() {
            let d = UserDefaults.standard
            let std = alyxDials(AlyxPreset.balanced.dials)
            func v(_ k: String, _ s: Int) -> Int { d.object(forKey: k) as? Int ?? s }
            alyx = KLAlyxPicture(quality: v(AlyxQuality.storageKey, std.quality), edges: v(AlyxEdges.storageKey, std.edges),
                                 view: v(AlyxView.storageKey, std.view), foveation: v(AlyxFoveation.storageKey, std.foveation),
                                 output: v(AlyxOutput.storageKey, std.output), cadence: v(AlyxCadence.storageKey, std.cadence),
                                 effects: v(AlyxEffects.storageKey, std.effects),
                                 texturePool: v(AlyxTexturePool.storageKey, std.texturePool),
                                 textureCap: v(AlyxTextureCap.storageKey, std.textureCap))
        }
        return KLPicture(renderScale: (r.renderScale * 20).rounded() / 20, metalFX: KleptonMetalFX.shared.enabled,
                         eyeMax: KleptonEyeMax().choice, foveation: r.foveation, vkFoveation: r.vkFoveation,
                         msaa: r.msaa, vkMsaa: r.vkMsaa, output: r.output, alyx: alyx)
    }

    /// Sets the picture. Like every picture setting, it applies from the next start.
    static func apply(_ p: KLPicture) {
        let r = KleptonRendering.shared
        r.renderScale = min(max(p.renderScale, KleptonRendering.minRenderScale), 1.0)
        // The resolution is the preset's now, not one the MetalFX switch lowered.
        KleptonMetalFXPreset().movedByHand()
        KleptonMetalFX.shared.enabled = p.metalFX
        KleptonEyeMax().choice = p.eyeMax
        r.foveation = p.foveation
        r.vkFoveation = p.vkFoveation
        r.msaa = p.msaa
        r.vkMsaa = p.vkMsaa
        r.output = p.output
        if let a = p.alyx, klIsAlyx() {
            let d = UserDefaults.standard
            d.set(a.quality, forKey: AlyxQuality.storageKey)
            d.set(a.edges, forKey: AlyxEdges.storageKey)
            d.set(a.view, forKey: AlyxView.storageKey)
            d.set(a.foveation, forKey: AlyxFoveation.storageKey)
            d.set(a.output, forKey: AlyxOutput.storageKey)
            d.set(a.cadence, forKey: AlyxCadence.storageKey)
            d.set(a.effects, forKey: AlyxEffects.storageKey)
            d.set(a.texturePool, forKey: AlyxTexturePool.storageKey)
            d.set(a.textureCap, forKey: AlyxTextureCap.storageKey)
            d.set(false, forKey: "alyx.custom")
        }
        NSLog("[app] picture preset applied: %.0f %%, MetalFX %@, eye max %d, output %d",
              p.renderScale * 100, p.metalFX ? "on" : "off", p.eyeMax, p.output)
        KLSettingsState.shared.changed()
    }

    static func builtIn(_ kind: KLBuiltInPreset) -> KLPicture {
        KLPictureRules.picture(kind, standard: standard, titleCap: titleCap)
    }

    /// The name of the preset the picture is set to, or nil.
    static func activeName() -> String? {
        let store = KLPresetStore()
        switch KLPictureRules.matching(read(), standard: standard, titleCap: titleCap, custom: store.presets) {
        case .builtIn(let k)?: return klT(k.nameEN, k.nameDE)
        case .custom(let id)?: return store.presets.first { $0.id == id }?.name
        case nil: return nil
        }
    }
}

/// Redraws the window after a change made outside SwiftUI's own state (a preset, the eye size).
final class KLSettingsState: ObservableObject {
    static let shared = KLSettingsState()
    @Published private(set) var generation = 0
    func changed() { DispatchQueue.main.async { self.generation += 1 } }
}

// MARK: - What the right-hand column shows

final class KLSettingsFocus: ObservableObject {
    @Published var key: String?
}

private struct KLHelpModifier: ViewModifier {
    @EnvironmentObject private var focus: KLSettingsFocus
    let key: String
    func body(content: Content) -> some View {
        content
            .contentShape(Rectangle())
            .onHover { if $0 { focus.key = key } }
            .simultaneousGesture(TapGesture().onEnded { focus.key = key })
    }
}

extension View {
    /// Shows `key`'s explanation on the right while this is looked at or touched.
    func klHelp(_ key: String) -> some View { modifier(KLHelpModifier(key: key)) }
}

struct KLHelpPicture: Hashable {
    let en: String
    let de: String
    let asset: String
    var label: String { klT(en, de) }
}

struct KLHelpEntry {
    let en: String
    let de: String
    let textEN: String
    let textDE: String
    var pictures: [KLHelpPicture] = []
    /// When it applies, or what it costs.
    var noteEN: String? = nil
    var noteDE: String? = nil
    var title: String { klT(en, de) }
    var text: String { klT(textEN, textDE) }
    var note: String? { noteEN.map { klT($0, noteDE ?? $0) } }
}

enum KLHelp {
    static let nextStartEN = "Applies the next time the game starts."
    static let nextStartDE = "Gilt ab dem nächsten Start des Spiels."

    private static func pair(_ name: String, _ offEN: String, _ offDE: String, _ onEN: String, _ onDE: String) -> [KLHelpPicture] {
        [KLHelpPicture(en: offEN, de: offDE, asset: "KLHelp-\(name)-a"),
         KLHelpPicture(en: onEN, de: onDE, asset: "KLHelp-\(name)-b")]
    }

    static func entry(_ key: String) -> KLHelpEntry? {
        let rates = KLDevice.kind.rates
        switch key {
        // Presets
        case "presets", "preset.m5":
            return KLHelpEntry(en: "Vision Pro M5", de: "Vision Pro M5",
                               textEN: "The settings this game was tuned with on a Vision Pro with the M5 chip. A new installation starts with them, and choosing it again puts every picture setting back.",
                               textDE: "Die Einstellungen, mit denen dieses Spiel auf einer Vision Pro mit M5-Chip abgestimmt wurde. Eine neue Installation startet damit, und wer es wieder wählt, setzt alle Bildeinstellungen zurück.",
                               pictures: [KLHelpPicture(en: "M5", de: "M5", asset: "KLHelp-scene")],
                               noteEN: "A preset sets the picture only. Frame rate and frame generation stay as you chose them. " + nextStartEN,
                               noteDE: "Ein Preset setzt nur das Bild. Bildrate und Zwischenbilder bleiben, wie du sie gewählt hast. " + nextStartDE)
        case "preset.m5.performance":
            return KLHelpEntry(en: "Performance", de: "Leistung",
                               textEN: "The game draws its picture at 75 % and MetalFX enlarges it to full size. The graphics chip has a good third less to do, so the frame rate holds better and the headset stays cooler; the picture is a little softer.",
                               textDE: "Das Spiel zeichnet sein Bild mit 75 %, und MetalFX vergrößert es auf volle Größe. Der Grafikchip hat gut ein Drittel weniger zu tun, die Bildrate hält besser und das Headset bleibt kühler; das Bild ist etwas weicher.",
                               pictures: pair("metalfx", "75 % without MetalFX", "75 % ohne MetalFX", "75 % with MetalFX", "75 % mit MetalFX"),
                               noteEN: nextStartEN, noteDE: nextStartDE)
        case "preset.m5.quality":
            return KLHelpEntry(en: "Quality", de: "Qualität",
                               textEN: "The next larger picture per eye (unless it is the largest already), drawn at 100 %, over the whole display and put on it at the finest output. Sharper, especially text and distant detail – and more work: a game that already runs at its limit may halve its frame rate.",
                               textDE: "Das nächstgrößere Bild je Auge (wenn es nicht schon das größte ist), mit 100 % gezeichnet, übers ganze Display und in der feinsten Ausgabe darauf gebracht. Schärfer, vor allem Schrift und Details in der Ferne – und mehr Arbeit: Ein Spiel, das schon an der Grenze läuft, kann seine Bildrate halbieren.",
                               pictures: pair("resolution", "Vision Pro M5", "Vision Pro M5", "Quality", "Qualität"),
                               noteEN: nextStartEN, noteDE: nextStartDE)
        case "preset.own":
            return KLHelpEntry(en: "Your own presets", de: "Eigene Presets",
                               textEN: "Save the picture as it is set now under a name of your own, and switch between your presets later. Share sends a preset as text – to a friend, or to the AVP Play community on GitHub – and Paste reads one back in. Owners of a Vision Pro with the M2 are especially welcome to make and share presets for it: the ones that come with the game were tuned on an M5.",
                               textDE: "Speichere das Bild, wie es gerade eingestellt ist, unter einem eigenen Namen und wechsle später zwischen deinen Presets. Teilen schickt ein Preset als Text – an Freunde oder an die AVP-Play-Community auf GitHub –, Einfügen liest eins wieder ein. Besitzer einer Vision Pro mit M2 sind ausdrücklich eingeladen, Presets dafür zu bauen und zu teilen: Die mitgelieferten sind auf einer M5 abgestimmt.")
        case "device":
            return KLHelpEntry(en: "Your Vision Pro", de: "Deine Vision Pro",
                               textEN: KLDevice.kind == .m2
                               ? "This is a Vision Pro with the M2 chip. Its display runs at up to 100 Hz, so the frame rate choices are 90 and 100. The presets that come with the game were tuned on an M5 and may be too demanding here – start from \"Performance\" and save what works as a preset of your own."
                               : "This is a Vision Pro with the M5 chip, the one the presets were tuned on. Its display runs at up to 120 Hz.",
                               textDE: KLDevice.kind == .m2
                               ? "Das ist eine Vision Pro mit M2-Chip. Ihr Display läuft mit bis zu 100 Hz, deshalb stehen bei der Bildrate 90 und 100 zur Wahl. Die mitgelieferten Presets sind auf einer M5 abgestimmt und können hier zu anspruchsvoll sein – fang mit „Leistung“ an und speichere, was gut läuft, als eigenes Preset."
                               : "Das ist eine Vision Pro mit M5-Chip, auf der die Presets abgestimmt wurden. Ihr Display läuft mit bis zu 120 Hz.")
        // Picture
        case "resolution":
            return KLHelpEntry(en: "Resolution", de: "Auflösung",
                               textEN: "How finely the game draws its picture, as a share of the size below. Lower is faster and softer. Lower it if the picture stutters when you look around; with MetalFX on it is the size MetalFX enlarges.",
                               textDE: "Wie fein das Spiel sein Bild zeichnet, als Anteil der Größe darunter. Niedriger ist schneller und weicher. Verringern, wenn das Bild beim Umsehen ruckelt; mit MetalFX die Größe, die MetalFX vergrößert.",
                               pictures: pair("resolution", "Lower", "Niedriger", "Higher", "Höher"),
                               noteEN: nextStartEN, noteDE: nextStartDE)
        case "metalfx":
            return KLHelpEntry(en: "MetalFX upscaling", de: "MetalFX-Hochskalieren",
                               textEN: "Apple's scaler enlarges a picture drawn below 100 % instead of a plain stretch, which keeps edges and text sharper. Costs a little graphics time. Turning it on at 100 % sets the resolution to 75 %.",
                               textDE: "Apples Scaler vergrößert ein Bild, das unter 100 % gezeichnet wurde, statt es einfach zu strecken – Kanten und Schrift bleiben schärfer. Kostet etwas Grafikzeit. Einschalten bei 100 % setzt die Auflösung auf 75 %.",
                               pictures: pair("metalfx", "Off", "Aus", "On", "An"),
                               noteEN: "Switching applies at once, a new resolution with the next start.",
                               noteDE: "Das Umschalten wirkt sofort, eine neue Auflösung beim nächsten Start.")
        case "eyeMax":
            return KLHelpEntry(en: "Max resolution per eye", de: "Maximale Auflösung je Auge",
                               textEN: "The longer side of the picture for each eye, in pixels – the size Resolution and MetalFX scale down from. Default is what this game was tested with; Native is the full size the headset asks for. Above the tested size a game may stutter or not start.",
                               textDE: "Die längere Seite des Bildes je Auge, in Bildpunkten – die Größe, von der Auflösung und MetalFX verkleinern. Standard ist, womit dieses Spiel getestet wurde; Nativ ist die volle Größe, die das Headset verlangt. Über der getesteten Größe kann ein Spiel ruckeln oder nicht starten.",
                               pictures: pair("resolution", "Smaller", "Kleiner", "Larger", "Größer"),
                               noteEN: nextStartEN, noteDE: nextStartDE)
        case "foveation":
            return KLHelpEntry(en: "Foveated rendering", de: "Foveated Rendering",
                               textEN: "Keeps the middle of the picture sharp and draws its edges more coarsely, where the eye sees less. The game runs more smoothly; the stronger the level, the smaller the sharp middle.",
                               textDE: "Hält die Bildmitte scharf und zeichnet die Ränder gröber, wo das Auge weniger sieht. Das Spiel läuft flüssiger; je stärker die Stufe, desto kleiner die scharfe Mitte.",
                               pictures: pair("foveation", "Off", "Aus", "On", "An"),
                               noteEN: nextStartEN, noteDE: nextStartDE)
        case "msaa":
            return KLHelpEntry(en: "Edge smoothing", de: "Kantenglättung",
                               textEN: "Smooths jagged, stair-stepped edges (MSAA). 4× is cleaner than 2×. It costs graphics time and, in some games, memory; the first start after a change may stutter while shaders are prepared again.",
                               textDE: "Glättet gezackte, treppenartige Kanten (MSAA). 4× ist sauberer als 2×. Kostet Grafikzeit und bei manchen Spielen Speicher; der erste Start nach einer Änderung kann ruckeln, während Shader neu vorbereitet werden.",
                               pictures: pair("msaa", "Off", "Aus", "4×", "4×"),
                               noteEN: nextStartEN, noteDE: nextStartDE)
        case "output":
            return KLHelpEntry(en: "Display output", de: "Ausgabe aufs Display",
                               textEN: "How finely the finished picture is put on the display. Light leaves the graphics chip more time for the game; Full is the finest, and small text stays crisper.",
                               textDE: "Wie fein das fertige Bild aufs Display kommt. Leicht lässt dem Grafikchip mehr Zeit fürs Spiel; Voll ist am feinsten, kleine Schrift bleibt knackiger.",
                               pictures: pair("output", "Light", "Leicht", "Full", "Voll"),
                               noteEN: nextStartEN, noteDE: nextStartDE)
        case "video":
            return KLHelpEntry(en: "In-game videos", de: "Videos im Spiel",
                               textEN: "Tutorial and cutscene clips in games that draw with Vulkan. On where it has been confirmed to work; turn it off if a game crashes when a clip starts.",
                               textDE: "Tutorial- und Zwischensequenz-Clips in Spielen, die mit Vulkan zeichnen. An, wo es bestätigt funktioniert; ausschalten, wenn ein Spiel beim Start eines Clips abstürzt.",
                               noteEN: nextStartEN, noteDE: nextStartDE)
        // Frame rate
        case "frameRate":
            return KLHelpEntry(en: "Frame rate", de: "Bildrate",
                               textEN: "Up to \(rates.mixed) is the default: the game is shown in a mixed space, black around its picture. Up to \(rates.full) uses the fully immersive space and is smoother where the game manages it. When a game cannot keep up, the Vision Pro halves the rate for a while – to \(rates.mixed / 2) or \(rates.full / 2)."
                               + (KLDevice.kind == .m2 ? " This Vision Pro has the M2: its display has no 120 Hz." : ""),
                               textDE: "Bis \(rates.mixed) ist der Standard: Das Spiel erscheint in einem gemischten Raum, schwarz um sein Bild. Bis \(rates.full) nutzt den voll immersiven Raum und ist flüssiger, wo das Spiel es schafft. Kommt ein Spiel nicht mit, halbiert die Vision Pro die Rate eine Weile – auf \(rates.mixed / 2) oder \(rates.full / 2)."
                               + (KLDevice.kind == .m2 ? " Diese Vision Pro hat den M2: Ihr Display kann keine 120 Hz." : ""),
                               pictures: [KLHelpPicture(en: "Frame rate", de: "Bildrate", asset: "KLHelp-framerate")],
                               noteEN: nextStartEN, noteDE: nextStartDE)
        case "frameGen":
            return KLHelpEntry(en: "Frame generation", de: "Zwischenbilder",
                               textEN: "For a game that draws fewer pictures than the display shows. Ahead: where the game has no new picture, one is made from its last two, moved on to the moment it is shown. In between: the middle between the game's last two pictures – smoother, and what the game draws comes 10 ms later; your head's movement stays as quick. Not together with MetalFX.",
                               textDE: "Für ein Spiel, das weniger Bilder zeichnet, als das Display zeigt. Vorausberechnet: Wo das Spiel kein neues Bild hat, wird eins aus seinen letzten beiden errechnet und bis zum Moment des Zeigens weitergerückt. Dazwischen: die Mitte zwischen den letzten beiden Bildern – weicher, und was das Spiel zeichnet, kommt 10 ms später; die Kopfbewegung bleibt so schnell. Nicht zusammen mit MetalFX.",
                               pictures: [KLHelpPicture(en: "Frame generation", de: "Zwischenbilder", asset: "KLHelp-framegen")])
        // Immersion and hands
        case "progressive":
            return KLHelpEntry(en: "Progressive immersion", de: "Progressive Immersion",
                               textEN: "Starts fully immersed. Turning the Digital Crown down draws the game into a portal over your room; at 0 % only the room is left and the sound fades out.",
                               textDE: "Startet voll immersiv. Dreh die Digital Crown herunter, dann zieht sich das Spiel in ein Portal über deinem Raum zusammen; bei 0 % bleibt nur der Raum, und der Ton blendet aus.",
                               pictures: pair("progressive", "Full", "Voll", "Portal", "Portal"),
                               noteEN: nextStartEN, noteDE: nextStartDE)
        case "hands":
            return KLHelpEntry(en: "Your real hands", de: "Deine echten Hände",
                               textEN: "Shows your real hands over the game. Most games draw their own hands or controllers, so your real ones would be a second pair in the same place.",
                               textDE: "Zeigt deine echten Hände über dem Spiel. Die meisten Spiele zeichnen eigene Hände oder Controller – deine echten wären dann ein zweites Paar an derselben Stelle.")
        case "pinch":
            return KLHelpEntry(en: "Pinch pulls the trigger", de: "Pinch zieht den Trigger",
                               textEN: "On: pinching or tapping your fingers pulls the trigger (in games played with hands only, it presses A or X). Off: your hands only aim.",
                               textDE: "Ein: Pinch oder Tippen mit den Fingern zieht den Trigger (in Spielen nur mit Händen drückt es A oder X). Aus: Die Hände zielen nur.",
                               noteEN: nextStartEN, noteDE: nextStartDE)
        case "chroma":
            return KLHelpEntry(en: "Passthrough and chroma key", de: "Durchsicht und Chroma-Key",
                               textEN: "Lets the room show through where the game draws a chosen colour – for mixed-reality games and recordings.",
                               textDE: "Lässt den Raum dort durchscheinen, wo das Spiel eine gewählte Farbe zeichnet – für Mixed-Reality-Spiele und Aufnahmen.")
        // Controls, room, game
        case "controllers":
            return KLHelpEntry(en: "Controllers", de: "Controller",
                               textEN: "Which controllers the game is played with, and how they line up with the game's hands. PlayStation VR2 Sense controllers and an ordinary gamepad work; Surreal Touch where AVP Play offers it.",
                               textDE: "Mit welchen Controllern gespielt wird und wie sie zu den Händen im Spiel passen. PlayStation-VR2-Sense-Controller und ein gewöhnlicher Gamecontroller funktionieren; Surreal Touch, wo AVP Play es anbietet.")
        case "alignment":
            return KLHelpEntry(en: "Controller alignment", de: "Ausrichtung der Controller",
                               textEN: "Moves and tilts the game's hands against your controllers, live while you play – for a game whose hands sit a little off.",
                               textDE: "Verschiebt und neigt die Hände im Spiel gegenüber deinen Controllern, live beim Spielen – für ein Spiel, dessen Hände etwas daneben sitzen.")
        case "room":
            return KLHelpEntry(en: "Play area", de: "Spielbereich",
                               textEN: "The space you can move in. Room scale measures your room once for all games and shows a wall when you come close to its edge; stationary or seated play needs nothing measured.",
                               textDE: "Der Raum, in dem du dich bewegen kannst. Raumgröße vermisst deinen Raum einmal für alle Spiele und zeigt eine Wand, wenn du seinem Rand nahe kommst; stationär oder sitzend muss nichts vermessen werden.")
        case "language":
            return KLHelpEntry(en: "Game language", de: "Sprache des Spiels",
                               textEN: "The language the game is told the headset speaks. Many games choose their text and voices from it.",
                               textDE: "Die Sprache, die dem Spiel als Sprache des Headsets gemeldet wird. Viele Spiele wählen danach Texte und Stimmen.",
                               noteEN: nextStartEN, noteDE: nextStartDE)
        case "game":
            return KLHelpEntry(en: "The game", de: "Das Spiel",
                               textEN: "What is particular to this game: its language, your name in it, saves in Steam Cloud and the mods it runs with.",
                               textDE: "Was zu diesem Spiel gehört: seine Sprache, dein Name darin, Spielstände in der Steam Cloud und die Mods, mit denen es läuft.")
        case "advanced":
            return KLHelpEntry(en: "Advanced", de: "Erweitert",
                               textEN: "Dials of the port itself and switches that are built in but not on yet. Nothing here is needed to play.",
                               textDE: "Regler der Portierung selbst und Schalter, die eingebaut, aber noch nicht eingeschaltet sind. Nichts davon braucht man zum Spielen.")
        case "diagnostics":
            return KLHelpEntry(en: "Diagnostics", de: "Diagnose",
                               textEN: "The log of this run, to pass on when something goes wrong.",
                               textDE: "Das Protokoll dieses Laufs, zum Weitergeben, wenn etwas nicht stimmt.")
        // Half-Life: Alyx
        case "alyx.size":
            return KLHelpEntry(en: "Picture size", de: "Bildgröße",
                               textEN: "How many pixels the game draws. Larger is sharper, slower and takes more memory.",
                               textDE: "Wie viele Bildpunkte das Spiel zeichnet. Größer ist schärfer, langsamer und braucht mehr Speicher.",
                               pictures: pair("resolution", "Smaller", "Kleiner", "Larger", "Größer"),
                               noteEN: nextStartEN, noteDE: nextStartDE)
        case "alyx.edges":
            return KLHelpEntry(en: "Edge smoothing", de: "Kantenglättung",
                               textEN: "How slanted edges are smoothed. 4× is cleaner, 2× is faster.",
                               textDE: "Wie schräge Kanten geglättet werden. 4× ist sauberer, 2× ist schneller.",
                               pictures: pair("msaa", "2×", "2×", "4×", "4×"), noteEN: nextStartEN, noteDE: nextStartDE)
        case "alyx.view":
            return KLHelpEntry(en: "Field of view", de: "Sichtfeld",
                               textEN: "\"Trimmed\" leaves the outermost edge of view dark and saves about a sixth of the work; \"Full\" draws the whole display.",
                               textDE: "„Beschnitten“ lässt den äußersten Rand des Sichtfelds dunkel und spart etwa ein Sechstel der Arbeit; „Voll“ zeichnet das ganze Display.",
                               pictures: pair("view", "Trimmed", "Beschnitten", "Full", "Voll"), noteEN: nextStartEN, noteDE: nextStartDE)
        case "alyx.foveation":
            return KLHelpEntry(en: "Foveated rendering", de: "Foveated Rendering",
                               textEN: "Keeps the middle of the picture sharp and draws its edge more coarsely. \"Strongest\" saves the most – about three quarters of the work – and the edge then looks soft when you look at it.",
                               textDE: "Hält die Bildmitte scharf und zeichnet den Rand gröber. „Sehr stark“ spart am meisten – rund drei Viertel der Arbeit –, der Rand wirkt dann weich, wenn man hinsieht.",
                               pictures: pair("foveation", "Off", "Aus", "Strongest", "Sehr stark"), noteEN: nextStartEN, noteDE: nextStartDE)
        case "alyx.output":
            return entry("output")
        case "alyx.cadence":
            return KLHelpEntry(en: "Frame cadence", de: "Bildtakt",
                               textEN: "\"Every second refresh\" draws an even 50 pictures a second and leaves the graphics chip time to cool; \"Every refresh\" draws as many as it manages, which is uneven when a picture takes a little too long.",
                               textDE: "„Jedes zweite Bild“ zeichnet gleichmäßig 50 Bilder pro Sekunde und lässt dem Grafikchip Zeit zum Abkühlen; „Jedes Bild“ zeichnet so viele, wie es schafft – ungleichmäßig, wenn ein Bild etwas zu lange braucht.",
                               noteEN: nextStartEN, noteDE: nextStartDE)
        case "alyx.effects":
            return KLHelpEntry(en: "Effects", de: "Effekte",
                               textEN: "\"Fewer\" leaves out bloom and ambient occlusion, the two effects that are passes of their own.",
                               textDE: "„Weniger“ lässt Bloom und Umgebungsverdeckung weg, die beiden Effekte, die eigene Durchgänge sind.",
                               noteEN: nextStartEN, noteDE: nextStartDE)
        case "alyx.pool":
            return KLHelpEntry(en: "Texture memory", de: "Texturspeicher",
                               textEN: "How much memory the game may fill with textures; more keeps distant surfaces finer, and every step up takes that much from the memory left. With too little left the system ends the game.",
                               textDE: "Wie viel Speicher das Spiel mit Texturen füllen darf; mehr hält entfernte Flächen feiner, und jeder Schritt nach oben nimmt so viel vom freien Speicher. Bleibt zu wenig, beendet das System das Spiel.",
                               noteEN: nextStartEN, noteDE: nextStartDE)
        case "alyx.cap":
            return KLHelpEntry(en: "Finest texture", de: "Feinste Textur",
                               textEN: "The finest any single texture is loaded at, in pixels along its edge. 4096 only pays with a very large texture memory and has run the headset out of memory; 1024 is coarser and frees memory.",
                               textDE: "Wie fein eine einzelne Textur höchstens geladen wird, in Bildpunkten Kantenlänge. 4096 lohnt nur mit sehr großem Texturspeicher und hat dem Headset schon den Speicher ausgehen lassen; 1024 ist gröber und spart Speicher.",
                               noteEN: nextStartEN, noteDE: nextStartDE)
        case "alyx.controls":
            return KLHelpEntry(en: "Controls", de: "Steuerung",
                               textEN: "Your controllers, how fast you walk with the stick, and whether a loaded level waits for the trigger.",
                               textDE: "Deine Controller, wie schnell du mit dem Stick läufst und ob ein geladenes Level auf den Trigger wartet.")
        case "alyx.language":
            return KLHelpEntry(en: "Language", de: "Sprache",
                               textEN: "Menus and subtitles come with the game in many languages; the voices were recorded in English only. A fan-made voice-over can be added when the game is installed from the Mac.",
                               textDE: "Menüs und Untertitel bringt das Spiel in vielen Sprachen mit; die Stimmen wurden nur englisch aufgenommen. Eine Fan-Vertonung lässt sich bei der Installation vom Mac aus hinzufügen.",
                               noteEN: nextStartEN, noteDE: nextStartDE)
        default:
            return nil
        }
    }
}

/// The right-hand column: the picture of the setting you are on (with a choice
/// between its states where there are two), what it does, and when it applies.
struct KLHelpPane: View {
    @EnvironmentObject private var focus: KLSettingsFocus
    let pageKey: String
    /// The explanation shown large (the owner's wish, 2026-11-16: "Wenn man drauf klickt kommt ein größeres
    /// Bild ebenfalls mit der Beschreibung, wo man dann gern wechseln kann zwischen beiden Varianten").
    /// KL_SETTINGS_ZOOM=1 opens it from the start, for a scripted photograph.
    @State private var enlarged: String? = klEnvOn("KL_SETTINGS_ZOOM", default: false)
        ? ProcessInfo.processInfo.environment["KL_SETTINGS_HELP"] : nil

    var body: some View {
        let key = focus.key ?? pageKey
        let shownKey = KLHelp.entry(key) != nil ? key : pageKey
        if let e = KLHelp.entry(shownKey) {
            ScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    // A picture only where there is something to compare (the owner's word: where there is
                    // no comparison, no picture).
                    if e.pictures.count >= 2 {
                        Button { enlarged = shownKey } label: {
                            VStack(alignment: .leading, spacing: 6) {
                                KLHelpPictures(pictures: e.pictures)
                                Label(klT("Tap to enlarge", "Zum Vergrößern antippen"), systemImage: "arrow.up.left.and.arrow.down.right")
                                    .font(.caption).foregroundStyle(.secondary)
                            }
                        }
                        .buttonStyle(.plain)
                        .hoverEffect()
                    }
                    KLHelpText(entry: e)
                }
                .padding(20)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .sheet(isPresented: Binding(get: { enlarged != nil }, set: { if !$0 { enlarged = nil } })) {
                if let k = enlarged, let big = KLHelp.entry(k) {
                    KLHelpLarge(entry: big) { enlarged = nil }
                }
            }
        } else {
            ContentUnavailableView(klT("Look at a setting", "Schau auf eine Einstellung"), systemImage: "eye",
                                   description: Text(klT("What it does appears here.", "Hier steht, was sie bewirkt.")))
        }
    }
}

/// Title, explanation and when it applies – in the column and in the large view.
struct KLHelpText: View {
    let entry: KLHelpEntry
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(entry.title).font(.title3.weight(.semibold))
            Text(entry.text)
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            if let note = entry.note {
                Label(note, systemImage: "arrow.clockwise")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        }
    }
}

/// One picture, or two as a split picture: the first variant on the left half, the second on the right,
/// a line between them and each half named in its corner.
struct KLHelpPictures: View {
    let pictures: [KLHelpPicture]
    var body: some View {
        if pictures.count >= 2 {
            KLHelpImage(asset: pictures[1].asset)
                .overlay {
                    KLHelpImage(asset: pictures[0].asset)
                        .mask {
                            GeometryReader { g in
                                Rectangle().frame(width: g.size.width / 2).frame(maxWidth: .infinity, alignment: .leading)
                            }
                        }
                }
                .overlay { Rectangle().fill(.white.opacity(0.9)).frame(width: 2) }
                .overlay(alignment: .bottomLeading) { KLHelpTag(text: pictures[0].label) }
                .overlay(alignment: .bottomTrailing) { KLHelpTag(text: pictures[1].label) }
                .clipShape(RoundedRectangle(cornerRadius: 16))
        } else if let p = pictures.first {
            KLHelpImage(asset: p.asset)
        }
    }
}

struct KLHelpTag: View {
    let text: String
    var body: some View {
        Text(text)
            .font(.caption.weight(.semibold))
            .padding(.horizontal, 10).padding(.vertical, 5)
            .background(.black.opacity(0.55), in: Capsule())
            .foregroundStyle(.white)
            .padding(10)
    }
}

/// The large view of an explanation: the picture big, a switch between its variants, the same words.
struct KLHelpLarge: View {
    let entry: KLHelpEntry
    let close: () -> Void
    @State private var shown = 0

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            if entry.pictures.count >= 2 {
                let i = min(shown, entry.pictures.count - 1)
                KLHelpImage(asset: entry.pictures[i].asset)
                    .id(entry.pictures[i].asset)
                    .transition(.opacity)
                    .overlay(alignment: .bottomLeading) { KLHelpTag(text: entry.pictures[i].label) }
                    .frame(maxWidth: 880)
                if entry.pictures.count > 1 {
                    Picker(klT("Variant", "Variante"), selection: $shown) {
                        ForEach(entry.pictures.indices, id: \.self) { Text(entry.pictures[$0].label).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    .labelsHidden()
                    .frame(maxWidth: 520)
                }
            }
            KLHelpText(entry: entry)
                .frame(maxWidth: 880, alignment: .leading)
            HStack {
                Spacer()
                Button(klT("Close", "Schließen"), action: close)
            }
        }
        .padding(28)
        .frame(width: 940)
        .animation(.easeInOut(duration: 0.2), value: shown)
    }
}

/// A picture from the app's asset catalog, or a quiet placeholder while there is none.
struct KLHelpImage: View {
    let asset: String
    var body: some View {
        Group {
            if UIImage(named: asset) != nil {
                Image(asset).resizable().scaledToFill()
            } else {
                ZStack {
                    LinearGradient(colors: [.gray.opacity(0.35), .gray.opacity(0.15)],
                                   startPoint: .topLeading, endPoint: .bottomTrailing)
                    Image(systemName: "photo").font(.largeTitle).foregroundStyle(.secondary)
                }
            }
        }
        .aspectRatio(16.0 / 10.0, contentMode: .fit)
        .clipShape(RoundedRectangle(cornerRadius: 16))
    }
}

// MARK: - Page frame and rows

/// A page: its controls in the middle, the explanation column on the right.
struct KLSettingsPage<Content: View>: View {
    let title: String
    let help: String
    @ViewBuilder let content: Content

    var body: some View {
        HStack(spacing: 0) {
            ScrollView {
                VStack(alignment: .leading, spacing: 18) { content }
                    .padding(.horizontal, 28).padding(.vertical, 20)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            Divider()
            KLHelpPane(pageKey: help)
                .frame(width: 380)
        }
        .navigationTitle(title)
    }
}

/// A box of rows with a title, as the pages group them.
struct KLSection<Content: View>: View {
    let title: String
    @ViewBuilder let content: Content
    var body: some View {
        GroupBox(title) {
            VStack(alignment: .leading, spacing: 12) { content }
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

/// One setting, explained in the column: its name above the control (a row of choices gets the
/// page's whole width, so long names are not cut), or beside it with `inline` (a menu, a switch).
struct KLRow<Control: View>: View {
    let title: String
    let help: String
    var inline = false
    @ViewBuilder let control: Control
    var body: some View {
        Group {
            if inline {
                HStack(alignment: .center, spacing: 16) {
                    Text(title)
                    Spacer(minLength: 12)
                    control
                }
            } else {
                VStack(alignment: .leading, spacing: 8) {
                    Text(title)
                    control
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .klHelp(help)
    }
}

// MARK: - The window

enum KLSettingsPageID: String, Hashable, CaseIterable {
    case presets, picture, motion, immersion, controls, room, game, advanced, diagnostics
}

struct KleptonSettingsWindow<Diagnostics: View>: View {
    /// True once the game was started: settings then apply from the next start.
    let started: Bool
    let measureRoom: () -> Void
    let done: () -> Void
    @ViewBuilder let diagnostics: Diagnostics

    @StateObject private var focus = KLSettingsFocus()
    @ObservedObject private var state = KLSettingsState.shared
    @ObservedObject private var rendering = KleptonRendering.shared
    @ObservedObject private var metalfx = KleptonMetalFX.shared
    /// A scripted launch can open a page and an explanation (KL_SETTINGS_PAGE=<page>, KL_SETTINGS_HELP=<key>; with
    /// KL_START_SETTINGS=1), to photograph them without a hand to tap.
    @State private var selection: KLSettingsPageID? =
        ProcessInfo.processInfo.environment["KL_SETTINGS_PAGE"].flatMap(KLSettingsPageID.init(rawValue:)) ?? .presets

    private var isAlyx: Bool { klIsAlyx() }

    var body: some View {
        NavigationSplitView {
            List(selection: $selection) {
                Section {
                    row(.presets, klT("Presets", "Presets"), "slider.horizontal.3",
                        KLPictureNow.activeName() ?? klT("Custom", "Eigene"))
                }
                Section(klT("Look", "Bild")) {
                    row(.picture, klT("Picture", "Bild"), "sparkles.tv", pictureValue)
                    if Immersive.wanted {
                        row(.motion, klT("Frame rate", "Bildrate"), "speedometer", motionValue)
                    }
                    if !isAlyx {
                        row(.immersion, klT("Immersion & hands", "Immersion & Hände"), "figure.stand")
                    }
                }
                Section(klT("Play", "Spielen")) {
                    row(.controls, klT("Controls", "Steuerung"), "gamecontroller")
                    row(.room, klT("Play area", "Spielbereich"), "square.dashed")
                    row(.game, isAlyx ? klT("Language & saves", "Sprache & Spielstände") : klT("Game", "Spiel"),
                        "character.bubble")
                }
                Section {
                    row(.advanced, klT("Advanced", "Erweitert"), "wrench.and.screwdriver")
                    row(.diagnostics, klT("Diagnostics", "Diagnose"), "doc.text.magnifyingglass")
                }
            }
            .navigationTitle(klT("Settings", "Einstellungen"))
            .navigationSplitViewColumnWidth(min: 260, ideal: 290, max: 340)
        } detail: {
            page(selection ?? .presets)
                .id(selection)
                .onAppear { focus.key = ProcessInfo.processInfo.environment["KL_SETTINGS_HELP"] }
        }
        .environmentObject(focus)
        .toolbar {
            ToolbarItem(placement: .confirmationAction) {
                Button(klT("Done", "Fertig")) { done() }
            }
        }
    }

    private var pictureValue: String {
        _ = state.generation
        if isAlyx { return AlyxQuality.stored.name }
        let scale = String(format: "%.0f %%", rendering.renderScale * 100)
        return metalfx.enabled ? scale + " · MetalFX" : scale
    }

    private var motionValue: String {
        let r = KLDevice.kind.rates
        return klT("Up to \(rendering.maxRate == 120 ? r.full : r.mixed)", "Bis \(rendering.maxRate == 120 ? r.full : r.mixed)")
    }

    private func row(_ id: KLSettingsPageID, _ title: String, _ symbol: String, _ value: String = "") -> some View {
        NavigationLink(value: id) {
            LabeledContent {
                Text(value).lineLimit(1)
            } label: {
                Label(title, systemImage: symbol)
            }
        }
    }

    @ViewBuilder
    private func page(_ id: KLSettingsPageID) -> some View {
        switch id {
        case .presets:
            KLSettingsPage(title: klT("Presets", "Presets"), help: "presets") { KLPresetsPage(started: started) }
        case .picture:
            KLSettingsPage(title: klT("Picture", "Bild"), help: isAlyx ? "alyx.size" : "resolution") {
                if isAlyx { AlyxPicturePage() } else { KLPicturePage() }
                startedNote
            }
        case .motion:
            KLSettingsPage(title: klT("Frame rate", "Bildrate"), help: "frameRate") {
                KLMotionPage()
                startedNote
            }
        case .immersion:
            KLSettingsPage(title: klT("Immersion & hands", "Immersion & Hände"), help: "progressive") {
                KLImmersionPage()
            }
        case .controls:
            KLSettingsPage(title: klT("Controls", "Steuerung"), help: isAlyx ? "alyx.controls" : "controllers") {
                if isAlyx { AlyxControlsView() } else { KLControlsPage() }
            }
        case .room:
            KLSettingsPage(title: klT("Play area", "Spielbereich"), help: "room") {
                RoomSettingsView(measure: measureRoom).klHelp("room")
            }
        case .game:
            KLSettingsPage(title: isAlyx ? klT("Language & saves", "Sprache & Spielstände") : klT("Game", "Spiel"),
                           help: isAlyx ? "alyx.language" : "game") {
                KLGamePage()
            }
        case .advanced:
            KLSettingsPage(title: klT("Advanced", "Erweitert"), help: "advanced") { KLAdvancedPage() }
        case .diagnostics:
            KLSettingsPage(title: klT("Diagnostics", "Diagnose"), help: "diagnostics") { diagnostics }
        }
    }

    @ViewBuilder private var startedNote: some View {
        if started {
            Label(klT("The game is running: picture settings apply the next time it is started.",
                      "Das Spiel läuft: Bildeinstellungen gelten ab dem nächsten Start."),
                  systemImage: "info.circle")
                .font(.callout).foregroundStyle(.secondary)
        }
    }
}

// MARK: - Presets

struct KLPresetsPage: View {
    let started: Bool
    @ObservedObject private var state = KLSettingsState.shared
    @ObservedObject private var rendering = KleptonRendering.shared
    @ObservedObject private var metalfx = KleptonMetalFX.shared
    @State private var newName = ""
    @State private var renaming: KLCustomPreset?
    @State private var renameText = ""
    @State private var pasteResult = ""

    private var store: KLPresetStore { KLPresetStore() }

    var body: some View {
        _ = state.generation
        let now = KLPictureNow.read()
        let custom = store.presets
        let active = KLPictureRules.matching(now, standard: KLPictureNow.standard,
                                             titleCap: KLPictureNow.titleCap, custom: custom)
        return VStack(alignment: .leading, spacing: 18) {
            deviceCard
            KLSection(title: klT("Come with the game", "Mitgeliefert")) {
                ForEach(KLBuiltInPreset.allCases) { kind in
                    presetRow(name: klT(kind.nameEN, kind.nameDE), detail: detail(kind),
                              active: active == .builtIn(kind), help: "preset.\(kind.rawValue)") {
                        KLPictureNow.apply(KLPictureNow.builtIn(kind))
                    }
                    if kind != KLBuiltInPreset.allCases.last { Divider() }
                }
            }
            KLSection(title: klT("Your own", "Eigene")) {
                if custom.isEmpty {
                    Text(klT("None yet. Set the picture the way you like it, then save it here.",
                             "Noch keine. Stell das Bild so ein, wie es dir gefällt, und speichere es hier."))
                        .font(.callout).foregroundStyle(.secondary)
                }
                ForEach(custom) { p in
                    HStack {
                        presetRow(name: p.name, detail: p.device.isEmpty ? "" : "Vision Pro \(p.device)",
                                  active: active == .custom(p.id), help: "preset.own") {
                            KLPictureNow.apply(p.picture.clamped(minScale: KleptonRendering.minRenderScale))
                        }
                        ShareLink(item: KLPresetStore.shareText(p, game: klLauncherTitle() ?? klTargetName())) {
                            Image(systemName: "square.and.arrow.up")
                        }
                        .buttonStyle(.borderless)
                        Menu {
                            Button(klT("Rename", "Umbenennen")) { renameText = p.name; renaming = p }
                            Button(klT("Save the current picture into it", "Aktuelles Bild hineinspeichern")) {
                                store.save(name: p.name, picture: now, device: KLDevice.kind.rawValue)
                                state.changed()
                            }
                            Button(klT("Delete", "Löschen"), role: .destructive) { store.delete(p.id); state.changed() }
                        } label: { Image(systemName: "ellipsis.circle") }
                        .buttonStyle(.borderless)
                    }
                    Divider()
                }
                HStack {
                    TextField(klT("Name for the current picture", "Name für das aktuelle Bild"), text: $newName)
                        .textFieldStyle(.roundedBorder)
                    Button(klT("Save", "Speichern")) {
                        store.save(name: newName, picture: now, device: KLDevice.kind.rawValue)
                        newName = ""
                        state.changed()
                    }
                    .disabled(newName.trimmingCharacters(in: .whitespaces).isEmpty)
                }
                .klHelp("preset.own")
                HStack {
                    PasteButton(payloadType: String.self) { texts in
                        guard let text = texts.first, let p = KLPresetStore.parse(text) else {
                            pasteResult = klT("That is not a shared preset.", "Das ist kein geteiltes Preset.")
                            return
                        }
                        let saved = store.save(name: p.name, picture: p.picture.clamped(minScale: KleptonRendering.minRenderScale),
                                               device: p.device)
                        pasteResult = klT("Added \"\(saved.name)\".", "„\(saved.name)“ hinzugefügt.")
                        state.changed()
                    }
                    if !pasteResult.isEmpty {
                        Text(pasteResult).font(.callout).foregroundStyle(.secondary)
                    }
                }
                .klHelp("preset.own")
            }
            Label(klT("A preset sets the picture; frame rate and frame generation stay as you chose them. Changes apply the next time the game starts.",
                      "Ein Preset setzt das Bild; Bildrate und Zwischenbilder bleiben, wie du sie gewählt hast. Änderungen gelten ab dem nächsten Start des Spiels."),
                  systemImage: "info.circle")
                .font(.callout).foregroundStyle(.secondary)
        }
        .alert(klT("Rename preset", "Preset umbenennen"), isPresented: Binding(get: { renaming != nil },
                                                                                 set: { if !$0 { renaming = nil } })) {
            TextField(klT("Name", "Name"), text: $renameText)
            Button(klT("Rename", "Umbenennen")) {
                if let p = renaming { store.rename(p.id, to: renameText); state.changed() }
                renaming = nil
            }
            Button(klT("Cancel", "Abbrechen"), role: .cancel) { renaming = nil }
        }
    }

    private var deviceCard: some View {
        HStack(alignment: .top, spacing: 14) {
            Image(systemName: "visionpro").font(.title)
            VStack(alignment: .leading, spacing: 4) {
                Text(klT("This Vision Pro: \(KLDevice.kind.rawValue)", "Diese Vision Pro: \(KLDevice.kind.rawValue)"))
                    .font(.headline)
                Text(KLDevice.kind == .m2
                     ? klT("The presets that come with the game were tuned on an M5. Start from \"Performance\", save what runs well as a preset of your own and share it – M2 presets are very welcome.",
                           "Die mitgelieferten Presets sind auf einer M5 abgestimmt. Fang mit „Leistung“ an, speichere, was gut läuft, als eigenes Preset und teile es – M2-Presets sind ausdrücklich erwünscht.")
                     : klT("The presets that come with the game were tuned on this kind of Vision Pro.",
                           "Die mitgelieferten Presets wurden auf dieser Art Vision Pro abgestimmt."))
                    .font(.callout).foregroundStyle(.secondary)
            }
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 20))
        .klHelp("device")
    }

    private func detail(_ kind: KLBuiltInPreset) -> String {
        switch kind {
        case .standard:    return klT("What this game was tuned with", "Womit dieses Spiel abgestimmt wurde")
        case .performance: return klT("75 % resolution, MetalFX upscaling", "75 % Auflösung, MetalFX-Hochskalieren")
        case .quality:     return klT("Next larger picture, whole display", "Nächstgrößeres Bild, ganzes Display")
        }
    }

    private func presetRow(name: String, detail: String, active: Bool, help: String,
                           apply: @escaping () -> Void) -> some View {
        Button(action: apply) {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text(name).font(.body.weight(.semibold))
                    if !detail.isEmpty { Text(detail).font(.footnote).foregroundStyle(.secondary) }
                }
                Spacer()
                if active { Image(systemName: "checkmark.circle.fill").foregroundStyle(.green) }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .klHelp(help)
    }
}

// MARK: - Picture (every game but Alyx)

struct KLPicturePage: View {
    @ObservedObject private var rendering = KleptonRendering.shared
    @ObservedObject private var state = KLSettingsState.shared
    @State private var eyeMax = KleptonEyeMax().choice
    private let titleCap = KLPictureNow.titleCap

    private func eyeLabel(_ v: Int) -> String {
        switch v {
        case KleptonEyeMax.byTitle where titleCap <= 0: return klT("Default (no limit)", "Standard (keine Grenze)")
        case KleptonEyeMax.byTitle: return klT("Default (\(titleCap))", "Standard (\(titleCap))")
        case KleptonEyeMax.native: return klT("Native (no limit)", "Nativ (keine Grenze)")
        default: return "\(v)"
        }
    }

    var body: some View {
        KLSection(title: klT("Size", "Größe")) {
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Text(klT("Resolution", "Auflösung"))
                    Slider(value: rendering.renderScaleByHand, in: KleptonRendering.minRenderScale...1.0, step: 0.05)
                    Text(String(format: "%.0f %%", rendering.renderScale * 100))
                        .monospacedDigit().frame(width: 64, alignment: .trailing)
                }
            }
            .klHelp("resolution")
            if KleptonMetalFX.deviceSupported {
                MetalFXSwitch(title: klT("MetalFX upscaling", "MetalFX-Hochskalieren"), hintFont: .footnote)
                    .klHelp("metalfx")
            }
            KLRow(title: klT("Max resolution per eye", "Maximale Auflösung je Auge"), help: "eyeMax", inline: true) {
                Picker(klT("Max resolution per eye", "Maximale Auflösung je Auge"), selection: $eyeMax) {
                    ForEach(KleptonEyeMax.options, id: \.self) { Text(eyeLabel($0)).tag($0) }
                }
                .labelsHidden()
            }
            if KleptonEyeMax.aboveTitle(eyeMax, titleCap: titleCap) {
                Text(klT("Above this game's tested size: may stutter or fail to start.",
                         "Über der getesteten Größe dieses Spiels: kann ruckeln oder nicht starten."))
                    .font(.footnote).foregroundStyle(.orange)
            }
        }
        .onChange(of: eyeMax) { _, v in KleptonEyeMax().choice = v; state.changed() }
        .onChange(of: state.generation) { _, _ in eyeMax = KleptonEyeMax().choice }

        KLSection(title: klT("Detail", "Details")) {
            if rendering.offersVkFoveation {
                KLRow(title: "Foveated Rendering", help: "foveation") {
                    Picker("Foveated Rendering", selection: $rendering.vkFoveation) {
                        Text(klT("Off", "Aus")).tag("off")
                        Text(klT("Light", "Leicht")).tag("low")
                        Text(klT("Medium", "Mittel")).tag("medium")
                        Text(klT("Strong", "Stark")).tag("high")
                        Text(klT("Very strong", "Sehr stark")).tag("ultra")
                    }
                    .pickerStyle(.segmented).labelsHidden()
                }
            } else {
                Toggle("Foveated Rendering", isOn: $rendering.foveation).klHelp("foveation")
            }
            if KleptonRendering.targetHasMSAA {
                KLRow(title: klT("Edge smoothing", "Kantenglättung"), help: "msaa") {
                    Picker(klT("Edge smoothing", "Kantenglättung"), selection: $rendering.msaa) {
                        Text(klT("Off", "Aus")).tag(0); Text("2×").tag(2); Text("4×").tag(4)
                    }
                    .pickerStyle(.segmented).labelsHidden()
                }
            }
            if rendering.offersVkMsaa {
                KLRow(title: klT("Edge smoothing", "Kantenglättung"), help: "msaa") {
                    Picker(klT("Edge smoothing", "Kantenglättung"), selection: $rendering.vkMsaa) {
                        Text(klT("As the game (\(KleptonRendering.vkGuestSamples)×)", "Wie das Spiel (\(KleptonRendering.vkGuestSamples)×)")).tag(0)
                        Text("4×").tag(4)
                    }
                    .pickerStyle(.segmented).labelsHidden()
                }
            }
            KLRow(title: klT("Display output", "Ausgabe aufs Display"), help: "output") {
                Picker(klT("Display output", "Ausgabe aufs Display"), selection: $rendering.output) {
                    ForEach(AlyxOutput.ordered) { Text($0.name).tag($0.rawValue) }
                }
                .pickerStyle(.segmented).labelsHidden()
            }
        }
    }
}

// MARK: - Frame rate

/// The frame-rate choice with the rates of this headset: 100/120 on an M5, 90/100 on an M2.
struct KLFrameRateRow: View {
    @ObservedObject private var rendering = KleptonRendering.shared
    var body: some View {
        let r = KLDevice.kind.rates
        KLRow(title: klT("Frame rate", "Bildrate"), help: "frameRate") {
            Picker(klT("Frame rate", "Bildrate"), selection: $rendering.maxRate) {
                Text(klT("Up to \(r.mixed)", "Bis \(r.mixed)")).tag(100)
                Text(klT("Up to \(r.full)", "Bis \(r.full)")).tag(120)
            }
            .pickerStyle(.segmented).labelsHidden()
        }
    }
}

struct KLFrameGenRow: View {
    @ObservedObject private var rendering = KleptonRendering.shared
    @ObservedObject private var metalfx = KleptonMetalFX.shared
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            KLRow(title: klT("Frame generation", "Zwischenbilder"), help: "frameGen") {
                Picker(klT("Frame generation", "Zwischenbilder"), selection: $rendering.frameGen) {
                    Text(klT("Off", "Aus")).tag(KleptonFrameGen.Mode.off.rawValue)
                    Text(klT("Ahead", "Vorausberechnet")).tag(KleptonFrameGen.Mode.ahead.rawValue)
                    Text(klT("In between", "Dazwischen")).tag(KleptonFrameGen.Mode.between.rawValue)
                }
                .pickerStyle(.segmented).labelsHidden()
            }
            if rendering.frameGen != KleptonFrameGen.Mode.off.rawValue, metalfx.enabled {
                Text(klT("While MetalFX upscaling is on, no in-between pictures are made – choose one of the two.",
                         "Solange MetalFX-Hochskalieren an ist, entstehen keine Zwischenbilder – eins von beiden wählen."))
                    .font(.footnote).foregroundStyle(.orange)
            }
        }
    }
}

struct KLMotionPage: View {
    var body: some View {
        KLSection(title: klT("Frame rate", "Bildrate")) {
            if !KleptonRendering.targetIsMixedReality { KLFrameRateRow() }
            KLFrameGenRow()
        }
        if KLDevice.kind == .m2 {
            Label(klT("This Vision Pro has the M2: its display runs at up to 100 Hz, so the choices are 90 and 100.",
                      "Diese Vision Pro hat den M2: Ihr Display läuft mit bis zu 100 Hz, deshalb 90 und 100."),
                  systemImage: "visionpro")
                .font(.callout).foregroundStyle(.secondary)
        }
    }
}

// MARK: - Immersion and hands

struct KLImmersionPage: View {
    @ObservedObject private var rendering = KleptonRendering.shared
    @ObservedObject private var chroma = KleptonChroma.shared
    var body: some View {
        if Immersive.wanted, !KleptonRendering.targetIsMixedReality {
            KLSection(title: klT("Digital Crown", "Digital Crown")) {
                Toggle(klT("Progressive immersion", "Progressive Immersion"), isOn: $rendering.crownImmersion)
                    .klHelp("progressive")
            }
        }
        KLSection(title: klT("Your hands", "Deine Hände")) {
            Toggle(klT("Show my real hands over the game", "Meine echten Hände über dem Spiel zeigen"),
                   isOn: $chroma.settings.handMatting)
                .klHelp("hands")
            HandPinchView().klHelp("pinch")
        }
        KLSection(title: klT("Passthrough and chroma key", "Durchsicht und Chroma-Key")) {
            DisclosureGroup(klT("Show", "Anzeigen")) { ChromaView() }.klHelp("chroma")
        }
    }
}

// MARK: - Controls, game, advanced

struct KLControlsPage: View {
    @AppStorage(KleptonControllerMode.preferenceKey) private var controllerMode = KleptonControllerMode.selected().rawValue
    var body: some View {
        KleptonControllerSettingsView().klHelp("controllers")
        if controllerMode == KleptonControllerMode.standard.rawValue {
            KLSection(title: klT("Game controller", "Gamecontroller")) { GamepadStatusView() }
                .klHelp("controllers")
        }
        KLSection(title: klT("Controller alignment", "Ausrichtung der Controller")) {
            DisclosureGroup(klT("Show", "Anzeigen")) { TuningView() }.klHelp("alignment")
        }
    }
}

struct KLGamePage: View {
    /// Walkabout and Realms of Flow use the microphone in play (BootView.micUpFront).
    private var micUpFront: Bool { ["walkabout", "x6062798767111272"].contains(klTargetName()) }
    var body: some View {
        if klIsLauncher() { LauncherPanel() }
        if klIsAlyx() {
            KLSteamCloudPanel()
            AlyxLanguageView().klHelp("alyx.language")
        } else {
            KLSection(title: klT("Language", "Sprache")) { GameLanguageView().klHelp("language") }
        }
        if KleptonPlayer.offered {
            KLSection(title: klT("Player", "Spieler")) { PlayerNameView() }
        }
        WalkaboutUnlockView()
        ModCreditsView()
        if micUpFront { KLSection(title: klT("Microphone", "Mikrofon")) { MicView() } }
    }
}

struct KLAdvancedPage: View {
    @ObservedObject private var rendering = KleptonRendering.shared
    private var showMic: Bool { ["steamlink-vr", "hl1", "cs1"].contains(klTargetName()) }
    var body: some View {
        if klIsAlyx() {
            KLSection(title: klT("Performance panel", "Leistungsanzeige")) { AlyxPerformancePanelView() }
            KLSection(title: klT("Memory", "Speicher")) { AlyxLeanDepthView() }
        } else {
            KLSection(title: klT("Picture, in detail", "Bild, im Einzelnen")) {
                Toggle(klT("In-game videos", "Videos im Spiel"), isOn: $rendering.video).klHelp("video")
            }
        }
        if KLRealityPresentSetting.offered {
            KLSection(title: klT("Eye-tracked sharpness", "Schärfe nach Blick")) { RealityPresentView() }
        }
        if showMic {
            KLSection(title: klT("Microphone", "Mikrofon")) { MicView() }
        }
        if KleptonTestSwitches.available {
            KLSection(title: klT("Test", "Test")) {
                DisclosureGroup(klT("Show", "Anzeigen")) { TestSwitchesView() }
            }
        }
    }
}
