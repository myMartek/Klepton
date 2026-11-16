// The picture presets (KleptonPictureProfile.swift): the three built-in ones
// made from a game's standard picture, the next higher eye size, which preset a
// picture is, the player's own presets in their store, shared text read back,
// and which Vision Pro a model name is. Run on the Mac: `make presets`, in
// `make check`.
import Foundation

@main
struct PresetsCheck {
    static var failed = 0
    static var suites: [String] = []

    static func freshStore() -> UserDefaults {
        let name = "klepton.t_presets.\(UUID().uuidString)"
        suites.append(name)
        return UserDefaults(suiteName: name)!
    }

    static func expect<T: Equatable>(_ what: String, _ got: T, _ want: T) {
        print("  \(got == want ? "ok  " : "FAIL") \(what) -> \(got)\(got == want ? "" : " (want \(want))")")
        if got != want { failed += 1 }
    }

    static let standard = KLPicture(renderScale: 1.0, metalFX: false, eyeMax: 0, foveation: true,
                                    vkFoveation: "off", msaa: 0, vkMsaa: 0, output: 2, alyx: nil)

    static func main() {
        print("=== Built-in presets ===")
        let perf = KLPictureRules.performance(from: standard)
        expect("performance: 75 %", perf.renderScale, 0.75)
        expect("performance: MetalFX on", perf.metalFX, true)
        expect("performance: rest as standard", perf.output, standard.output)

        let q = KLPictureRules.quality(from: standard, titleCap: 3072)
        expect("quality: 3072 game -> 3584", q.eyeMax, 3584)
        expect("quality: 100 %", q.renderScale, 1.0)
        expect("quality: MetalFX off", q.metalFX, false)
        expect("quality: full output", q.output, 0)
        expect("quality: foveation as standard", q.foveation, true)

        print("=== Next higher eye size ===")
        expect("4000 -> 4096", KLPictureRules.nextEyeMax(after: 0, titleCap: 4000), 4096)
        expect("3840 -> 4000", KLPictureRules.nextEyeMax(after: 0, titleCap: 3840), 4000)
        expect("5740 -> no limit", KLPictureRules.nextEyeMax(after: 0, titleCap: 5740), -1)
        expect("5120 chosen -> no limit", KLPictureRules.nextEyeMax(after: 5120, titleCap: 3072), -1)
        expect("no limit stays", KLPictureRules.nextEyeMax(after: -1, titleCap: 3072), -1)
        expect("no title cap -> no limit", KLPictureRules.nextEyeMax(after: 0, titleCap: 0), -1)
        expect("2560 chosen -> 3072", KLPictureRules.nextEyeMax(after: 2560, titleCap: 4000), 3072)

        print("=== Alyx ===")
        let alyx = KLAlyxPicture(quality: 3, edges: 2, view: 1, foveation: 3, output: 2,
                                 cadence: 2, effects: 1, texturePool: 2560, textureCap: 2048)
        var aStd = standard; aStd.alyx = alyx
        let aq = KLPictureRules.quality(from: aStd, titleCap: 3072)
        expect("alyx quality: view full", aq.alyx?.view, 0)
        expect("alyx quality: output full", aq.alyx?.output, 0)
        expect("alyx quality: picture maximum", aq.alyx?.quality, 3)
        expect("alyx quality: foveation kept", aq.alyx?.foveation, 3)
        expect("alyx quality: eye size left to its picture size", aq.eyeMax, 0)
        var smaller = alyx; smaller.quality = 2
        var aStd2 = standard; aStd2.alyx = smaller
        expect("alyx quality: Large -> Maximum", KLPictureRules.quality(from: aStd2, titleCap: 3072).alyx?.quality, 3)

        print("=== Which preset ===")
        expect("standard", KLPictureRules.matching(standard, standard: standard, titleCap: 3072, custom: []),
               .builtIn(.standard))
        expect("performance", KLPictureRules.matching(perf, standard: standard, titleCap: 3072, custom: []),
               .builtIn(.performance))
        expect("quality", KLPictureRules.matching(q, standard: standard, titleCap: 3072, custom: []),
               .builtIn(.quality))
        var mine = standard; mine.renderScale = 0.85
        expect("none", KLPictureRules.matching(mine, standard: standard, titleCap: 3072, custom: []), nil)

        print("=== Own presets ===")
        let store = KLPresetStore(defaults: freshStore())
        expect("empty", store.presets.count, 0)
        let saved = store.save(name: "  Mein M2 ", picture: mine, device: "M2")
        expect("saved, trimmed name", store.presets.map(\.name), ["Mein M2"])
        expect("matching own", KLPictureRules.matching(mine, standard: standard, titleCap: 3072, custom: store.presets),
               .custom(saved.id))
        var mine2 = mine; mine2.metalFX = true
        store.save(name: "mein m2", picture: mine2, device: "M2")
        expect("same name replaces", store.presets.count, 1)
        expect("replaced picture", store.presets.first?.picture.metalFX, true)
        store.rename(saved.id, to: "Abend")
        expect("renamed", store.presets.first?.name, "Abend")
        store.rename(saved.id, to: "   ")
        expect("blank name ignored", store.presets.first?.name, "Abend")

        print("=== Shared text ===")
        let text = KLPresetStore.shareText(store.presets[0], game: "Beat Saber")
        expect("header", text.hasPrefix(KLPresetStore.shareHeader), true)
        let back = KLPresetStore.parse("Schau mal:\n" + text + "\nGruß")
        expect("read back: name", back?.name, "Abend")
        expect("read back: device", back?.device, "M2")
        expect("read back: picture", back?.picture, mine2)
        expect("not a preset", KLPresetStore.parse("hallo") == nil, true)
        var odd = mine2; odd.renderScale = 0.12; odd.eyeMax = 1234; odd.vkFoveation = "max"; odd.output = 9
        let c = odd.clamped(minScale: 0.5)
        expect("clamped scale", c.renderScale, 0.5)
        expect("clamped eye size", c.eyeMax, 0)
        expect("clamped foveation", c.vkFoveation, "off")
        expect("clamped output", c.output, 0)

        store.delete(saved.id)
        expect("deleted", store.presets.count, 0)

        print("=== What changed since a preset ===")
        expect("no change", KLPictureDiff.changes(from: standard, to: standard).map(\.rawValue), [String]())
        var changed = standard; changed.renderScale = 0.8; changed.output = 0
        expect("resolution and output", KLPictureDiff.changes(from: standard, to: changed).map(\.rawValue), ["renderScale", "output"])
        expect("from performance", KLPictureDiff.changes(from: perf, to: standard).map(\.rawValue), ["renderScale", "metalFX"])
        var a2 = aStd; a2.alyx?.view = 0
        expect("alyx view", KLPictureDiff.changes(from: aStd, to: a2).map(\.rawValue), ["alyxView"])
        let last = KLLastPreset(defaults: freshStore())
        expect("no last preset", last.id, nil)
        last.id = .builtIn(.quality)
        expect("last built-in", last.id, .builtIn(.quality))
        let u = UUID(); last.id = .custom(u)
        expect("last own", last.id, .custom(u))

        print("=== Which Vision Pro ===")
        expect("RealityDevice14,1", KLDeviceKind.from(model: "RealityDevice14,1"), .m2)
        expect("RealityDevice17,1", KLDeviceKind.from(model: "RealityDevice17,1"), .m5)
        expect("simulator", KLDeviceKind.from(model: "arm64"), .m5)
        expect("override", KLDeviceKind.from(model: "RealityDevice17,1", override: "m2"), .m2)
        expect("M2 rates", KLDeviceKind.m2.rates.mixed * 1000 + KLDeviceKind.m2.rates.full, 90_100)
        expect("M5 rates", KLDeviceKind.m5.rates.mixed * 1000 + KLDeviceKind.m5.rates.full, 100_120)

        for s in suites { UserDefaults.standard.removePersistentDomain(forName: s) }
        print(failed == 0 ? "all presets checks passed" : "\(failed) presets checks FAILED")
        exit(failed == 0 ? 0 : 1)
    }
}
