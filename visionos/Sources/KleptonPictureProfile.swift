// The picture presets of every game's settings window (KleptonSettingsWindow):
// what a preset holds, the three that come with the game, the player's own,
// and which Vision Pro the game runs on. Plain values and rules only, no
// SwiftUI and no runtime calls, so the rules are checked on the Mac
// (tests/t_presets.swift, `make presets`).
//
// The owner's brief (2026-11-16): the game's present default is the preset
// "Vision Pro M5" and stays what a new installation starts with. "Performance"
// is that with the resolution at 75 % and MetalFX upscaling on. "Quality" is
// that with the next higher resolution (unless it is the highest already) and
// the whole display drawn – nothing trimmed, the finest output. Nobody has
// played these on an M2 yet; its owners are asked to make presets of their own,
// which is why a preset can be saved, shared as text and read back in.
//
// A preset sets the picture only. Frame rate and frame generation stay as the
// player chose them: they are a matter of taste, and a preset that moved them
// would make "which preset is this" depend on a choice it is not about.
import Foundation

/// Half-Life: Alyx's nine picture dials (AlyxDials), by their stored numbers.
struct KLAlyxPicture: Codable, Equatable {
    var quality: Int        // AlyxQuality: smooth 0, balanced 1, sharp 2, maximum 3
    var edges: Int          // AlyxEdges: 2, 4
    var view: Int           // AlyxView: full 0, trimmed 1
    var foveation: Int      // AlyxFoveation: off 0, on 1, strong 2, strongest 3
    var output: Int         // AlyxOutput: full 0, standard 1, light 2
    var cadence: Int        // AlyxCadence: 1, 2
    var effects: Int        // AlyxEffects
    var texturePool: Int    // AlyxTexturePool, MB or 0
    var textureCap: Int     // AlyxTextureCap, pixels

    static let qualityMaximum = 3
    static let viewFull = 0
    static let outputFull = 0
}

/// Everything a preset decides. The stored numbers of KleptonRendering,
/// KleptonMetalFX and KleptonEyeMax, so a preset is read back exactly.
struct KLPicture: Codable, Equatable {
    /// "Resolution", a fraction of the eye picture, in steps of 0.05.
    var renderScale: Double
    /// MetalFX spatial upscaling.
    var metalFX: Bool
    /// "Max resolution per eye" (KleptonEyeMax): 0 the game's own cap, -1 none.
    var eyeMax: Int
    /// Foveated rendering of a GL game (a switch) …
    var foveation: Bool
    /// … and of a Vulkan game (a level: off, low, medium, high, ultra).
    var vkFoveation: String
    /// Edge smoothing of a GL game that has it (0, 2, 4) and of a Vulkan game (0 = its own, 4).
    var msaa: Int
    var vkMsaa: Int
    /// "Display output" (AlyxOutput's numbers: full 0, standard 1, light 2).
    var output: Int
    /// Alyx's own dials; nil for every other game.
    var alyx: KLAlyxPicture?

    static let outputFull = 0

    /// A copy within the ranges the window offers – for a preset read from text.
    func clamped(minScale: Double) -> KLPicture {
        var p = self
        p.renderScale = (min(max(renderScale, minScale), 1.0) * 20).rounded() / 20
        if !KLPictureRules.eyeMaxOptions.contains(p.eyeMax) { p.eyeMax = KLPictureRules.eyeMaxByTitle }
        if !["off", "low", "medium", "high", "ultra"].contains(p.vkFoveation) { p.vkFoveation = "off" }
        if ![0, 2, 4].contains(p.msaa) { p.msaa = 0 }
        if p.vkMsaa != 4 { p.vkMsaa = 0 }
        if !(0...2).contains(p.output) { p.output = Self.outputFull }
        return p
    }
}

/// The presets that come with every game.
enum KLBuiltInPreset: String, CaseIterable, Identifiable {
    case standard = "m5"
    case performance = "m5.performance"
    case quality = "m5.quality"
    var id: String { rawValue }

    var nameEN: String {
        switch self {
        case .standard:    return "Vision Pro M5"
        case .performance: return "M5 Performance"
        case .quality:     return "M5 Quality"
        }
    }
    var nameDE: String {
        switch self {
        case .standard:    return "Vision Pro M5"
        case .performance: return "M5 Leistung"
        case .quality:     return "M5 Qualität"
        }
    }
}

enum KLPictureRules {
    static let eyeMaxByTitle = 0
    static let eyeMaxNative = -1
    /// KleptonEyeMax.options, in their order.
    static let eyeMaxOptions = [0, 2048, 2560, 3072, 3584, 4000, 4096, 4608, 5120, -1]
    /// What MetalFX upscaling is paired with (KleptonMetalFXPreset.renderScale).
    static let performanceScale = 0.75

    /// "Performance": the standard picture at 75 % with MetalFX making it sharp again.
    static func performance(from standard: KLPicture) -> KLPicture {
        var p = standard
        p.renderScale = performanceScale
        p.metalFX = true
        return p
    }

    /// "Quality": the next larger picture and the whole display, finely put on it.
    /// `titleCap` is the game's own cap per eye (kl_ovrp_title_eye_cap; 0 or less: none).
    static func quality(from standard: KLPicture, titleCap: Int) -> KLPicture {
        var p = standard
        p.renderScale = 1.0
        p.metalFX = false           // nothing to upscale at 100 %
        p.output = KLPicture.outputFull
        // Alyx's picture size is its own dial (its eye cap is set by it, klAlyxApply).
        if p.alyx == nil { p.eyeMax = nextEyeMax(after: standard.eyeMax, titleCap: titleCap) }
        if var a = p.alyx {
            a.quality = max(a.quality, KLAlyxPicture.qualityMaximum)
            a.view = KLAlyxPicture.viewFull
            a.output = KLAlyxPicture.outputFull
            p.alyx = a
        }
        return p
    }

    /// The eye size a step above `choice` (the game's own cap where nothing is chosen):
    /// the next size the window offers, or no limit above the largest. No limit stays.
    static func nextEyeMax(after choice: Int, titleCap: Int) -> Int {
        if choice == eyeMaxNative { return eyeMaxNative }
        let now = choice == eyeMaxByTitle ? titleCap : choice
        guard now > 0 else { return eyeMaxNative }      // no cap at all: already the largest
        let sizes = eyeMaxOptions.filter { $0 > 0 }
        return sizes.first { $0 > now } ?? eyeMaxNative
    }

    /// The built-in preset `kind` for a game whose standard picture is `standard`.
    static func picture(_ kind: KLBuiltInPreset, standard: KLPicture, titleCap: Int) -> KLPicture {
        switch kind {
        case .standard:    return standard
        case .performance: return performance(from: standard)
        case .quality:     return quality(from: standard, titleCap: titleCap)
        }
    }

    /// The preset the picture is set to: a built-in one first, then the player's own.
    static func matching(_ now: KLPicture, standard: KLPicture, titleCap: Int,
                         custom: [KLCustomPreset]) -> KLPresetID? {
        for kind in KLBuiltInPreset.allCases where picture(kind, standard: standard, titleCap: titleCap) == now {
            return .builtIn(kind)
        }
        return custom.first { $0.picture == now }.map { .custom($0.id) }
    }
}

enum KLPresetID: Hashable {
    case builtIn(KLBuiltInPreset)
    case custom(UUID)
}

/// A preset of the player's own: a name, the picture, and which headset it was made on.
struct KLCustomPreset: Codable, Equatable, Identifiable {
    var id: UUID
    var name: String
    var picture: KLPicture
    /// "M2" or "M5" – where it was saved, shown beside the name.
    var device: String
}

/// The player's own presets, per game (each game is its own app with its own defaults).
struct KLPresetStore {
    static let key = "klepton.presets.custom"
    /// The first line of a shared preset, so a pasted text is recognised.
    static let shareHeader = "AVP Play picture preset v1"

    let defaults: UserDefaults
    init(defaults: UserDefaults = .standard) { self.defaults = defaults }

    var presets: [KLCustomPreset] {
        get {
            guard let data = defaults.data(forKey: Self.key) else { return [] }
            return (try? JSONDecoder().decode([KLCustomPreset].self, from: data)) ?? []
        }
        nonmutating set {
            if newValue.isEmpty { defaults.removeObject(forKey: Self.key); return }
            if let data = try? JSONEncoder().encode(newValue) { defaults.set(data, forKey: Self.key) }
        }
    }

    /// Saves `picture` under `name`; a preset of that name is replaced. Returns it.
    @discardableResult
    func save(name: String, picture: KLPicture, device: String) -> KLCustomPreset {
        let clean = name.trimmingCharacters(in: .whitespacesAndNewlines)
        var all = presets
        if let i = all.firstIndex(where: { $0.name.caseInsensitiveCompare(clean) == .orderedSame }) {
            all[i].picture = picture
            all[i].device = device
            presets = all
            return all[i]
        }
        let p = KLCustomPreset(id: UUID(), name: clean, picture: picture, device: device)
        presets = all + [p]
        return p
    }

    func delete(_ id: UUID) { presets = presets.filter { $0.id != id } }

    func rename(_ id: UUID, to name: String) {
        let clean = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !clean.isEmpty else { return }
        presets = presets.map { var p = $0; if p.id == id { p.name = clean }; return p }
    }

    /// The text a preset is shared as: a header line, then JSON. `game` names the
    /// game it was made for (a preset fits the game it was made in best).
    static func shareText(_ p: KLCustomPreset, game: String) -> String {
        struct Shared: Codable { let name: String; let device: String; let game: String; let picture: KLPicture }
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        let body = (try? enc.encode(Shared(name: p.name, device: p.device, game: game, picture: p.picture)))
            .flatMap { String(data: $0, encoding: .utf8) } ?? "{}"
        return Self.shareHeader + "\n" + body
    }

    /// A shared preset read back: its name, device and picture, or nil for text that is not one.
    static func parse(_ text: String) -> (name: String, device: String, picture: KLPicture)? {
        struct Shared: Codable { let name: String; let device: String?; let picture: KLPicture }
        guard let start = text.firstIndex(of: "{"), let end = text.lastIndex(of: "}") else { return nil }
        let json = Data(text[start...end].utf8)
        guard let s = try? JSONDecoder().decode(Shared.self, from: json) else { return nil }
        let name = s.name.trimmingCharacters(in: .whitespacesAndNewlines)
        return (name.isEmpty ? "Preset" : String(name.prefix(40)), s.device ?? "", s.picture)
    }
}

/// Which Vision Pro this is: the first one, with the M2 ("RealityDevice14,1"), or
/// the one with the M5 ("RealityDevice17,1") or later.
enum KLDeviceKind: String {
    case m2 = "M2", m5 = "M5"

    /// From the model name (hw.machine). Anything else – the simulator, a Mac – counts as M5:
    /// the presets were made there, and nothing is taken away.
    static func from(model: String, override: String? = nil) -> KLDeviceKind {
        switch override?.lowercased() {
        case "m2": return .m2
        case "m5": return .m5
        default: break
        }
        guard model.hasPrefix("RealityDevice") else { return .m5 }
        let digits = model.drop { !$0.isNumber }.prefix { $0.isNumber }
        guard let major = Int(digits) else { return .m5 }
        return major < 17 ? .m2 : .m5
    }

    /// The two frame-rate choices as this headset can show them: the mixed space and the full one.
    /// The M5's display runs up to 100 Hz in the mixed space and 120 Hz in the full one; the M2's
    /// has no 120 Hz – 90 Hz is its usual rate and 100 Hz its highest.
    var rates: (mixed: Int, full: Int) { self == .m2 ? (90, 100) : (100, 120) }
}
