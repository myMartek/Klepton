import Foundation

// Vivecraft's "Seated" play mode must not lock the player out: before every
// start of the game the profile's stored play mode is put back to standing.
//
// Why. In seated mode Vivecraft takes its input from keyboard and mouse and
// skips the VR controllers (MCVR.triggerHapticPulse / processBindings return
// early, MCOpenXR.poll leaves the controller block out; VRSettings.java:
// `@SettingField(VrOptions.PLAY_MODE_SEATED) public boolean seated`, JSON key
// "seated", default false). In the immersive space on a Vision Pro there is
// neither keyboard nor mouse, so a player who picked Play Mode: Seated in the
// game's VR settings had no input left at all — not even to switch back. Game
// parts built from Vivecraft patch 0007 on no longer offer the mode; this guard
// covers the parts built before it, and a config that arrives with the value
// already in it (a copied profile, an edited file).
//
// What it does. The ONE line `"seated": "true"` becomes `"seated": "false"`.
// Vivecraft reads a boolean as value.equals("true") (VRSettings.loadOption), so
// "true" is the only spelling that means seated; any other value already is
// standing and stays as it is. The related keys (seatedhmd, seatedHudAltMode,
// seatedFreeMove, …) only act in seated mode and are not touched.
//
// Rules, the ones MCTurningPreferences.swift follows: a line-wise edit in the
// shape Gson writes (one `"key": "value"` per line, every value a string — the
// shape kl_app.c's vrEnabled edit relies on too), verified by re-parse: every
// other key byte for byte, written atomically. Not seated -> no write. Key
// missing or JSON unparseable -> no write, logged. No config file yet -> no
// write and NO partial seed (a partial file would make MCUIComfort read its
// missing HUD keys as "customized"); Vivecraft's default is standing. Never
// throws; the launch goes on whatever the outcome.
//
// Fixtures: `swiftc -parse-as-library -D MC_SEATED_GUARD_TEST MCSeatedGuard.swift -o seated && ./seated`

enum MCSeatedGuard {
    static let configKey = "seated"
    static let configRelative = "config/vivecraft-client-config.json"
    static let seated = "true"
    static let standing = "false"

    // --- pure -------------------------------------------------------------

    enum Decision: Equatable {
        case standing(String)     // the stored value; not "true", so nothing to do
        case rewrite(String)      // the new text
        case keyMissing
        case malformed(String)
    }

    static func decide(_ text: String) -> Decision {
        guard let dict = parse(text) else { return .malformed("not a JSON object of settings") }
        guard let current = dict[configKey] as? String else { return .keyMissing }
        if current != seated { return .standing(current) }
        let from = "\"\(configKey)\": \"\(seated)\"", to = "\"\(configKey)\": \"\(standing)\""
        let n = text.components(separatedBy: from).count - 1
        guard n == 1 else { return .malformed("expected exactly one `\(from)` line, found \(n)") }
        let out = text.replacingOccurrences(of: from, with: to)
        guard let after = parse(out) else { return .malformed("the rewritten text does not parse") }
        var expect = dict; expect[configKey] = standing
        guard NSDictionary(dictionary: after).isEqual(to: expect) else { return .malformed("the rewrite changed another key") }
        return .rewrite(out)
    }

    private static func parse(_ text: String) -> [String: Any]? {
        guard let d = text.data(using: .utf8), let o = try? JSONSerialization.jsonObject(with: d) else { return nil }
        return o as? [String: Any]
    }

    // --- the profile ------------------------------------------------------

    enum Outcome: Equatable {
        case applied               // was seated, is standing now
        case standing(String)      // was not seated
        case noConfig              // first launch of a new profile: Vivecraft's default is standing
        case keyMissing
        case skipped(String)       // unreadable / malformed; nothing written
        case writeFailed(String)

        var summary: String {
            switch self {
            case .applied: return "play mode was Seated (no input in the space): \(configKey) true -> false"
            case .standing(let v): return "standing (\(configKey)=\(v))"
            case .noConfig: return "no config yet (Vivecraft's default is standing)"
            case .keyMissing: return "config has no \(configKey) key (Vivecraft's default is standing)"
            case .skipped(let why): return "skipped: \(why)"
            case .writeFailed(let why): return "FAILED to write: \(why)"
            }
        }
    }

    /// Put `<gameDir>/config/vivecraft-client-config.json` back to standing. Never throws.
    static func apply(gameDir: String) -> Outcome {
        let fm = FileManager.default
        let cfg = gameDir + "/" + configRelative
        guard fm.fileExists(atPath: cfg) else { return .noConfig }
        guard let data = fm.contents(atPath: cfg), let text = String(data: data, encoding: .utf8) else {
            return .skipped("could not read the config")
        }
        switch decide(text) {
        case .standing(let v): return .standing(v)
        case .keyMissing: return .keyMissing
        case .malformed(let why): return .skipped(why)
        case .rewrite(let out):
            do { try Data(out.utf8).write(to: URL(fileURLWithPath: cfg), options: .atomic) }
            catch { return .writeFailed(error.localizedDescription) }
            return .applied
        }
    }

    // --- fixtures ---------------------------------------------------------

    /// Gson shape with the play mode at Seated, plus what a rewrite must leave
    /// alone: the other seated keys, another "true", escapes.
    static let seatedFixture = """
    {
      "mrMovingCamOffsetZ": "0.0",
      "seatedhmd": "true",
      "seated": "true",
      "keyboardKeys": "`1234567890-=qwertyuiop[]\\\\asdfghjkl;':\\"zxcvbnm,./?<>",
      "QUICKCOMMAND_9": "\\"seated\\": \\"true\\"",
      "seatedHudAltMode": "true",
      "seatedFreeMove": "true",
      "vrWorldRotationIncrement": "45.0",
      "vrHudLockMode": "WRIST",
      "vrEnabled": "true"
    }
    """

    /// nil = every check passed.
    static func selfTest() -> String? {
        // 1. Seated becomes standing: exactly the one line.
        guard case .rewrite(let out) = decide(seatedFixture) else { return "seated: not rewritten: \(decide(seatedFixture))" }
        let expected = seatedFixture.replacingOccurrences(of: "  \"seated\": \"true\",\n", with: "  \"seated\": \"false\",\n")
        if expected == seatedFixture { return "fixture: the seated line was not found" }
        if out != expected { return "seated: text differs" }
        guard let d = parse(out) else { return "seated: output does not parse" }
        if d["seated"] as? String != "false" { return "seated: still seated" }
        for k in ["seatedhmd", "seatedHudAltMode", "seatedFreeMove", "vrEnabled"] where d[k] as? String != "true" {
            return "seated: \(k) touched"
        }
        if d["QUICKCOMMAND_9"] as? String != "\"seated\": \"true\"" { return "seated: a value that spells the line was touched" }
        if d["keyboardKeys"] as? String != "`1234567890-=qwertyuiop[]\\asdfghjkl;':\"zxcvbnm,./?<>" { return "seated: escapes damaged" }
        if d.count != 10 { return "seated: key count \(d.count)" }
        // 2. Idempotent, and every value but "true" is standing and stays.
        if decide(out) != .standing("false") { return "idempotent: \(decide(out))" }
        let odd = seatedFixture.replacingOccurrences(of: "\"seated\": \"true\"", with: "\"seated\": \"TRUE\"")
        if decide(odd) != .standing("TRUE") { return "another spelling was rewritten" }
        // 3. Missing key, malformed and a duplicate line are refused without a rewrite.
        let missing = seatedFixture.replacingOccurrences(of: "  \"seated\": \"true\",\n", with: "")
        if decide(missing) != .keyMissing { return "missing key: \(decide(missing))" }
        if case .malformed = decide("not json") {} else { return "malformed accepted" }
        let twice = seatedFixture.replacingOccurrences(of: "  \"vrHudLockMode\": \"WRIST\",\n", with: "  \"seated\": \"true\",\n")
        if case .malformed = decide(twice) {} else { return "duplicate line accepted: \(decide(twice))" }
        // 4. A value that is not a string (not what Vivecraft writes) is left alone.
        let bool = seatedFixture.replacingOccurrences(of: "\"seated\": \"true\"", with: "\"seated\": true")
        if decide(bool) != .keyMissing { return "a non-string value was rewritten: \(decide(bool))" }
        return nil
    }

    /// File half in a fresh temp dir: apply/idempotence/no-config/saves untouched.
    static func fileSelfTest(in dir: String) -> String? {
        let fm = FileManager.default
        func profile(_ name: String, _ text: String?) throws -> String {
            let p = dir + "/" + name
            try fm.createDirectory(atPath: p + "/config", withIntermediateDirectories: true)
            try fm.createDirectory(atPath: p + "/saves/World", withIntermediateDirectories: true)
            try Data("level".utf8).write(to: URL(fileURLWithPath: p + "/saves/World/level.dat"))
            try Data("guiScale:0\n".utf8).write(to: URL(fileURLWithPath: p + "/options.txt"))
            if let text { try Data(text.utf8).write(to: URL(fileURLWithPath: p + "/" + configRelative)) }
            return p
        }
        do {
            let a = try profile("seated", seatedFixture)
            let cfg = a + "/" + configRelative
            if apply(gameDir: a) != .applied { return "apply" }
            let expected = seatedFixture.replacingOccurrences(of: "  \"seated\": \"true\",\n", with: "  \"seated\": \"false\",\n")
            if (try? String(contentsOfFile: cfg, encoding: .utf8)) != expected { return "file is not the fixture with the one line changed" }
            if apply(gameDir: a) != .standing("false") { return "second apply not idempotent" }
            if (try? String(contentsOfFile: cfg, encoding: .utf8)) != expected { return "second apply wrote" }
            if !fm.fileExists(atPath: a + "/saves/World/level.dat") { return "saves touched" }
            if (try? String(contentsOfFile: a + "/options.txt", encoding: .utf8)) != "guiScale:0\n" { return "options.txt touched" }
            if (try? fm.contentsOfDirectory(atPath: a + "/config"))?.sorted() != ["vivecraft-client-config.json"] { return "another file was left in config/" }
            let b = try profile("empty", nil)
            if apply(gameDir: b) != .noConfig { return "noConfig" }
            if fm.fileExists(atPath: b + "/" + configRelative) { return "noConfig: a file was seeded" }
            let c = try profile("broken", "{ \"seated\": ")
            let before = try String(contentsOfFile: c + "/" + configRelative, encoding: .utf8)
            if case .skipped = apply(gameDir: c) {} else { return "broken: \(apply(gameDir: c))" }
            if (try? String(contentsOfFile: c + "/" + configRelative, encoding: .utf8)) != before { return "broken: file changed" }
        } catch { return "fixture setup failed: \(error)" }
        return nil
    }
}

#if MC_SEATED_GUARD_TEST
@main
struct MCSeatedGuardTestMain {
    static func main() {
        if let err = MCSeatedGuard.selfTest() { fputs("FAIL decide: \(err)\n", stderr); exit(1) }
        let dir = NSTemporaryDirectory() + "mc-seated-guard-\(getpid())"
        defer { try? FileManager.default.removeItem(atPath: dir) }
        if let err = MCSeatedGuard.fileSelfTest(in: dir) { fputs("FAIL apply: \(err)\n", stderr); exit(1) }
        print("ok")
    }
}
#endif
