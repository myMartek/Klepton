import Foundation

// Per-account game dirs for the native Minecraft guest (the vivecraft row).
//
// Every account plays in a game dir of its own, so its worlds, options and mod
// settings stay with it:
//
//   <instance>/profiles/<key>/      the account's --gameDir (and the guest's cwd)
//   <instance>/profiles/offline/    the Offline (Player) card — persistent too
//   <instance>/profiles/.migrated   the key that inherited the old shared data
//
// <instance> is the parent of launch.txt's --gameDir (root/inst-26.2 on 26.2;
// fgamedir stays where it was and becomes the SEED + the shared read-only trees).
// <key>: MSAStore.profileKey — "xuid-<Xbox user id>" (stable across Remove +
// Add again, and across a free account buying the game), "acct-<id>" for an
// account that has no xuid yet, "offline" for the Offline card. Pinned in
// accounts.json the first time, never recomputed.
//
// A new profile is created on its first Launch:
//   seeded (copied once, never overwritten later): options.txt,
//       keyboardtheme.txt, config/ (vivecraft-client-config.json, Sodium,
//       voicechat incl. the f24 push-to-talk pin…), input/ (1.21.x QC
//       Custom_Bindings.json; absent on 26.2). The options.txt copied here gets
//       a new player's start values (MCOptionDefaults below: render distance 5,
//       simulation distance 8); one that is already there is not opened.
//   symlinked to fgamedir (relative links; the game only reads them):
//       mods/        Fabric 0.19 scans <gameDir>/mods at runtime (the 83 mods it
//                    lists come from there; the image only holds their classes)
//       .fabric/     its processedMods cache (the nested jars baked.cp names)
//       resourcepacks/
//   everything else (saves/, screenshots/, logs/, crash-reports/, data/,
//       downloads/, servers.dat, usercache.json, command_history.txt…) is
//       created by the game inside the profile.
//
// Migration: the FIRST Launch of an account that owns the game, while
// .migrated is absent (and its profile holds no world yet), inherits the shared
// data: saves/*, screenshots/*, servers.dat, usercache.json and
// command_history.txt are MOVED from fgamedir into the profile (options.txt,
// config/, keyboardtheme.txt are COPIED by the seed step, so fgamedir keeps
// them as the seed for everyone else). .migrated is written first (contents:
// the key), so a crash half-way can never hand the leftovers to a second
// account; nothing is ever deleted. Free accounts and Offline never inherit.
//
// Removing an account keeps its profile dir. KL_MC_PROFILES=0 (env file) or
// KL_MSA=0 = the old single shared fgamedir.
//
// HUD comfort (MCUIComfort.swift): right after the profile is prepared,
// and only once per profile, the three Vivecraft HUD placement keys
// (vrHudLockMode, hudDistance, headHudScale) are moved from stock to a
// forward head lock at 2 m, slightly smaller, IF they are all still stock.
// Inventory / in-world menu keys and everything else are preserved exactly; a
// customized HUD is never touched. The outcome is one log line and never
// stops the launch.
//
// Play mode (MCSeatedGuard.swift): at EVERY launch, last of the config edits,
// a stored `"seated": "true"` in the game dir this start will use — the
// profile's, or the shared one when profiles are off or could not be
// prepared — is put back to "false". Seated mode reads keyboard and mouse
// only; in the space that leaves the player without any input.
//
// A fresh IPA (KleptonPayload marker "bundle") ships no world. If a world is
// in that game dir anyway, the first profile inherits it, Offline included,
// because there is no owning account yet. A devicectl-staged container
// (marker "external") does not: that world still goes only to the first
// account that owns the game.
//
// Swift prepares the dir on every Launch press and hands it to kl_app.c as
// KL_MC_GAMEDIR (absolute) + KL_MC_PROFILE_NOTE (one line for the boot log).
//
// Minecraft version upgrade (inheritPreviousVersion): the instance dir is per
// version (root/inst-26.2, root/inst-26.3), so a new version starts with no
// profiles. The first time an instance is looked at and has no profiles/ yet,
// the newest older sibling's profiles/ (inst-<older>/profiles) is COPIED in
// whole: every account's worlds, options.txt, config/ (Vivecraft, Sodium,
// voice chat, the comfort/pitch markers), servers.dat, screenshots, and the
// .migrated marker. APFS clonefile, so it costs no space until either side
// changes. The old instance is never touched; Minecraft upgrades the copied
// worlds in place when they are opened. The mods/.fabric/resourcepacks links
// are relative (../../fgamedir/…), so in the copy they point at the NEW
// instance's fgamedir without any rewrite.

enum MCProfileFS {
    static let seedCopy = ["options.txt", "keyboardtheme.txt", "config", "input"]
    static let shared = ["mods", ".fabric", "resourcepacks"]
    static let inheritMoveChildren = ["saves", "screenshots"]
    static let inheritMoveFiles = ["servers.dat", "usercache.json", "command_history.txt"]

    struct Result { var path: String; var note: String; var created: Bool; var migrated: Bool }

    /// The copy's log line, for the boot log's profile note: the copy usually
    /// happens at the launcher's first world list, before the boot log opens.
    nonisolated(unsafe) static var inheritedNote: String?

    /// One-time copy of the previous Minecraft version's profiles/ into
    /// <instance>/profiles. Does nothing once <instance>/profiles exists. The
    /// copy lands under a temporary name and is renamed into place, so a crash
    /// half-way never leaves a partial profiles/ that would stop a retry.
    /// Returns the log line when it copied, nil otherwise. Never throws.
    @discardableResult
    static func inheritPreviousVersion(instance: String) -> String? {
        let fm = FileManager.default
        let dest = instance + "/profiles"
        if exists(dest) { return nil }
        let leaf = (instance as NSString).lastPathComponent
        guard leaf.hasPrefix("inst-") else { return nil }
        let mine = versionKey(String(leaf.dropFirst(5)))
        let parent = (instance as NSString).deletingLastPathComponent
        var best: (key: [Int], dir: String)?
        for kid in (try? fm.contentsOfDirectory(atPath: parent)) ?? [] where kid.hasPrefix("inst-") && kid != leaf {
            let k = versionKey(String(kid.dropFirst(5)))
            var isDir: ObjCBool = false
            guard k.lexicographicallyPrecedes(mine),
                  fm.fileExists(atPath: parent + "/" + kid + "/profiles", isDirectory: &isDir), isDir.boolValue
            else { continue }
            if best == nil || best!.key.lexicographicallyPrecedes(k) { best = (k, parent + "/" + kid + "/profiles") }
        }
        guard let src = best?.dir else { return nil }
        let tmp = instance + "/.profiles-inherit-" + UUID().uuidString
        do {
            try fm.createDirectory(atPath: instance, withIntermediateDirectories: true)
            if clonefile(src, tmp, UInt32(CLONE_NOFOLLOW)) != 0 {
                let why = String(cString: strerror(errno))
                try? fm.removeItem(atPath: tmp)
                try fm.copyItem(atPath: src, toPath: tmp)   // not APFS / cross-volume: a real copy
                NSLog("[profiles] clonefile failed (%@); copied instead", why)
            }
            if rename(tmp, dest) != 0 {
                // Someone else created profiles/ meanwhile: theirs stands.
                try? fm.removeItem(atPath: tmp)
                return nil
            }
        } catch {
            try? fm.removeItem(atPath: tmp)
            NSLog("[profiles] copying %@ -> %@ failed: %@", src, dest, error.localizedDescription)
            return nil
        }
        let n = ((try? fm.contentsOfDirectory(atPath: dest)) ?? []).filter { !$0.hasPrefix(".") }.count
        let line = "copied \(n) profile(s) from \(src) to \(dest) (the old version's copy is untouched)"
        NSLog("[profiles] %@", line)
        inheritedNote = line
        return line
    }

    /// "26.2" -> [26, 2], "1.21.10" -> [1, 21, 10]; a non-number part is 0.
    static func versionKey(_ v: String) -> [Int] {
        v.split(separator: ".").map { Int($0) ?? 0 }
    }

    /// Create (or repair) <instance>/profiles/<key> from <gameDir> (= fgamedir).
    /// `mayInherit`: an account that owns the game. Never deletes anything.
    static func prepare(instance: String, seedDir gd: String, key: String, displayName: String,
                        mayInherit: Bool) throws -> Result {
        let fm = FileManager.default
        let profiles = instance + "/profiles"
        let dir = profiles + "/" + key
        let marker = profiles + "/.migrated"
        try fm.createDirectory(atPath: profiles, withIntermediateDirectories: true)
        let created = !fm.fileExists(atPath: dir)
        var notes: [String] = []
        if created { try fm.createDirectory(atPath: dir, withIntermediateDirectories: true) }

        // Seed: copy what is missing (a profile's own edits are never replaced).
        var seeded: [String] = []
        for e in seedCopy where fm.fileExists(atPath: gd + "/" + e) && !exists(dir + "/" + e) {
            try fm.copyItem(atPath: gd + "/" + e, toPath: dir + "/" + e)
            seeded.append(e)
        }
        if !seeded.isEmpty { notes.append("seeded " + seeded.joined(separator: ",")) }
        // A new player's start values (MCOptionDefaults), in the options.txt
        // that was copied just now and in no other: a profile that had its
        // options before this launch is not opened.
        if seeded.contains("options.txt") {
            let started = MCOptionDefaults.apply(optionsPath: dir + "/options.txt")
            if !started.isEmpty { notes.append("start values " + started.joined(separator: ",")) }
        }

        // Shared read-only trees: relative links into fgamedir, repaired if wrong.
        let rel = relativePath(from: dir, to: gd)
        var linked: [String] = []
        for e in shared where fm.fileExists(atPath: gd + "/" + e) {
            let p = dir + "/" + e, want = rel + "/" + e
            if let cur = try? fm.destinationOfSymbolicLink(atPath: p) {
                if cur == want { continue }
                try fm.removeItem(atPath: p)       // a link (only the link) pointing elsewhere
            } else if exists(p) {
                notes.append("\(e) is a real dir (left alone)")
                continue
            }
            try fm.createSymbolicLink(atPath: p, withDestinationPath: want)
            linked.append(e)
        }
        if !linked.isEmpty { notes.append("linked " + linked.joined(separator: ",")) }

        // Migration of the old shared data: once, to the first owning account.
        var migrated = false
        if mayInherit, !fm.fileExists(atPath: marker), created || !hasSaves(dir) {
            try Data(key.utf8).write(to: URL(fileURLWithPath: marker), options: .atomic)
            var moved: [String] = []
            for sub in inheritMoveChildren {
                let src = gd + "/" + sub
                guard let kids = try? fm.contentsOfDirectory(atPath: src), !kids.isEmpty else { continue }
                try fm.createDirectory(atPath: dir + "/" + sub, withIntermediateDirectories: true)
                var n = 0
                for k in kids where k != ".DS_Store" {
                    var dst = dir + "/" + sub + "/" + k, i = 2
                    while exists(dst) { dst = dir + "/" + sub + "/" + k + "-\(i)"; i += 1 }
                    try fm.moveItem(atPath: src + "/" + k, toPath: dst)
                    n += 1
                }
                moved.append("\(sub)/*(\(n))")
            }
            for f in inheritMoveFiles where fm.fileExists(atPath: gd + "/" + f) && !exists(dir + "/" + f) {
                try fm.moveItem(atPath: gd + "/" + f, toPath: dir + "/" + f)
                moved.append(f)
            }
            migrated = true
            notes.append("INHERITED the shared data (" + (moved.isEmpty ? "nothing to move" : moved.joined(separator: ", ")) + ")")
        }

        // A label for whoever browses the container (the dir names are opaque).
        try? Data((displayName + "\n").utf8).write(to: URL(fileURLWithPath: dir + "/.account"), options: .atomic)
        let head = created ? "new profile \(key)" : "profile \(key)"
        return Result(path: dir, note: ([head] + notes).joined(separator: "; "), created: created, migrated: migrated)
    }

    /// The saves dir the launcher lists for `key`: the profile's, or — for an
    /// owning account whose profile does not exist yet while nobody has
    /// inherited — the shared fgamedir's (the worlds it is about to inherit).
    static func savesDir(instance: String, seedDir gd: String, key: String, mayInherit: Bool) -> String? {
        let fm = FileManager.default
        let dir = instance + "/profiles/" + key
        if mayInherit, !fm.fileExists(atPath: instance + "/profiles/.migrated"), !hasSaves(dir) { return gd + "/saves" }
        return fm.fileExists(atPath: dir) ? dir + "/saves" : nil
    }

    static func hasSaves(_ dir: String) -> Bool {
        let kids = (try? FileManager.default.contentsOfDirectory(atPath: dir + "/saves")) ?? []
        return kids.contains { !$0.hasPrefix(".") }
    }

    /// exists, without following a final symlink (a dangling link is "there").
    static func exists(_ p: String) -> Bool {
        (try? FileManager.default.attributesOfItem(atPath: p)) != nil
    }

    static func relativePath(from dir: String, to target: String) -> String {
        let a = URL(fileURLWithPath: dir).standardizedFileURL.pathComponents
        let b = URL(fileURLWithPath: target).standardizedFileURL.pathComponents
        var i = 0
        while i < a.count, i < b.count, a[i] == b[i] { i += 1 }
        let up = Array(repeating: "..", count: a.count - i)
        let parts = up + b[i...]
        return parts.isEmpty ? "." : parts.joined(separator: "/")
    }
}

/// What a new player's options.txt starts with here, whatever the game's archive
/// ships. The payload goes into the app as it came (its payload-version is its
/// identity, visionos/bundle_payload.sh), so the values are put in where a
/// player's options.txt is made: when the app lays its game dir out of the
/// bundle (KleptonPayload) and when a profile is seeded from that game dir
/// (MCProfileFS.prepare) — the second also for a container whose game dir was
/// laid out by an earlier build, or staged by hand.
///
///   renderDistance      5    the 1.0.3 release ships 12 and 12, which cost the
///   simulationDistance  8    game's render thread 46 ms a frame on an M5 (a
///                            build tree's bake leaves 16 and 12); these are
///                            what the owner of this tree plays with
///
///   graphicsPreset      "custom", where the file names another preset: with
///                            "fast", "fancy" or "fabulous" the game puts the
///                            preset's own distances back at its first start
///                            (seen in the simulator: 16 and 12 for "fancy"),
///                            as it does itself when a player moves one of the
///                            sliders. The 1.0.3 release ships "custom".
///
/// Only these lines are written; a distance that is missing is added, a preset
/// line that is missing is not. No other line is read for its meaning or
/// changed, the language least of all. An options.txt that was there before is
/// never passed in: both callers hand over the file they have just copied, so a
/// value a player chose in the game stays.
enum MCOptionDefaults {
    /// `add`: write the line when the file has none.
    static let start: [(key: String, value: String, add: Bool)] = [
        ("renderDistance", "5", true),
        ("simulationDistance", "8", true),
        ("graphicsPreset", "\"custom\"", false),
    ]

    /// The text with the start values in, and the keys that were written.
    /// Line-wise (`key:value`, as the game writes it); line endings stay.
    static func rewrite(_ text: String) -> (text: String, changed: [String]) {
        var changed: [String] = []
        var seen = Set<String>()
        var crlf = false
        var lines = text.components(separatedBy: "\n").map { raw -> String in
            let cr = raw.hasSuffix("\r")
            if cr { crlf = true }
            let line = cr ? String(raw.dropLast()) : raw
            for (key, value, _) in start where line.hasPrefix(key + ":") {
                seen.insert(key)
                if line == key + ":" + value { return raw }
                if !changed.contains(key) { changed.append(key) }
                return key + ":" + value + (cr ? "\r" : "")
            }
            return raw
        }
        // A file without the line gets it, before the empty piece a final
        // newline leaves at the end.
        let trailing = lines.last == ""
        if trailing { lines.removeLast() }
        for (key, value, add) in start where add && !seen.contains(key) {
            lines.append(key + ":" + value + (crlf ? "\r" : ""))
            changed.append(key)
        }
        if trailing { lines.append("") }
        return (lines.joined(separator: "\n"), changed)
    }

    /// Apply to a file that was copied just now. Returns the keys that moved;
    /// never throws — a file that cannot be read or written stays as it is.
    @discardableResult
    static func apply(optionsPath: String) -> [String] {
        let url = URL(fileURLWithPath: optionsPath)
        guard let data = try? Data(contentsOf: url), let text = String(data: data, encoding: .utf8) else {
            NSLog("[options] %@ not readable: the shipped values stay", optionsPath)
            return []
        }
        let (out, changed) = rewrite(text)
        if changed.isEmpty { return [] }
        do {
            try Data(out.utf8).write(to: url, options: .atomic)
        } catch {
            NSLog("[options] %@ not written (%@): the shipped values stay", optionsPath, error.localizedDescription)
            return []
        }
        NSLog("[options] start values in %@: %@", optionsPath, changed.joined(separator: ","))
        return changed
    }
}

#if !KL_PROFILES_HARNESS
/// Which profile a Launch uses, for the account the boot will pick.
@MainActor
enum MCProfiles {
    static var enabled: Bool {
        MSAAccounts.isMinecraft && MCEnv.on("KL_MC_PROFILES", default: true) && MCEnv.on("KL_MSA", default: true)
    }

    /// (instance dir, fgamedir) from launch.txt.
    static func dirs() -> (String, String)? {
        guard let gd = MCInstance.read().gameDir else { return nil }
        return ((gd as NSString).deletingLastPathComponent, gd)
    }

    /// The account the boot will run as — the same pick kl_msa_session makes
    /// (KL_MSA_ACCOUNT > the card > the default): (key, owns, display name).
    static func bootTarget() -> (String, Bool, String) {
        let store = MSAStore.forTree(MSAAccounts.shared.tree)
        let (id, _, _) = store.pickForBoot(override: MCEnv.value("KL_MSA_ACCOUNT"))
        guard let id, let key = store.profileKey(for: id) else { return ("offline", false, "Offline (Player)") }
        let a = store.snapshot().accounts.first { $0.id == id }
        return (key, (a?.resolved ?? false) && (a?.owns ?? false), a?.name ?? key)
    }

    /// Launch: prepare the dir and export it. Failure = the shared dir (logged).
    static func prepareForLaunch() {
        unsetenv("KL_MC_GAMEDIR"); unsetenv("KL_MC_PROFILE_NOTE")
        guard enabled else {
            NSLog("[profiles] off (KL_MC_PROFILES=0 or KL_MSA=0): the shared game dir")
            var note = "profiles off"
            if let (_, gd) = dirs(), standUp(gameDir: gd, who: "shared game dir") { note += "; play mode put back to standing" }
            setenv("KL_MC_PROFILE_NOTE", note, 1)
            return
        }
        guard let (inst, gd) = dirs() else { NSLog("[profiles] no launch.txt game dir"); return }
        MCProfileFS.inheritPreviousVersion(instance: inst)
        let (key, owns, name) = bootTarget()
        // A bundled game dir has no personal save. See sharesSeedWorld.
        let inherit = owns || KleptonPayload.sharesSeedWorld
        do {
            let r = try MCProfileFS.prepare(instance: inst, seedDir: gd, key: key, displayName: name, mayInherit: inherit)
            // One-time UI comfort defaults for THIS profile, before the game
            // reads its config (kl_app.c's vrEnabled edit comes later and is
            // line-wise too). Never throws; a failure is logged and the launch
            // goes on with the config as it is.
            let comfort = MCUIComfort.apply(profileDir: r.path)
            NSLog("[comfort] %@: %@", name, comfort.summary)
            // One-time HUD pitch move (MCHUDPitch.swift, its own marker): after
            // comfort, which edits the same file, and before the game reads it.
            // Never throws either.
            let pitch = MCHUDPitch.apply(profileDir: r.path)
            NSLog("[hud-pitch] %@: %@", name, pitch.summary)
            var note = r.note
            if MCProfileFS.inheritedNote != nil { note += "; profiles copied from the previous Minecraft version" }
            if case .applied = comfort { note += "; hud comfort defaults applied" }
            if case .applied = pitch { note += "; hud pitch moved" }
            // Turning (MCTurningPreferences.swift): the launcher's
            // Smooth / 45° Snap choice -> this profile's vrWorldRotationIncrement,
            // re-applied every launch. After comfort so a brand-new profile's
            // missing config is reported once, not seeded. Never throws.
            let turning = MCTurning.stored
            let turned = MCTurning.apply(turning, profileDir: r.path)
            NSLog("[turning] %@: %@ -> %@", name, turning.rawValue, turned.summary)
            if case .applied = turned { note += "; turning \(turning.rawValue)" }
            // Play mode (MCSeatedGuard.swift): never start in Vivecraft's
            // seated mode. Every launch, after the other edits of this file.
            if standUp(gameDir: r.path, who: name) { note += "; play mode put back to standing" }
            setenv("KL_MC_GAMEDIR", r.path, 1)
            setenv("KL_MC_PROFILE_NOTE", "\(name): \(note)", 1)
            NSLog("[profiles] %@ -> %@ (%@)", name, r.path, note)
        } catch {
            setenv("KL_MC_PROFILE_NOTE", "profile \(key) FAILED (\(error.localizedDescription)) — shared dir", 1)
            NSLog("[profiles] %@: preparing %@ failed: %@ — the shared game dir", name, key, error.localizedDescription)
            // The game then runs in the shared dir: its config gets the guard.
            _ = standUp(gameDir: gd, who: "shared game dir")
        }
    }

    /// MCSeatedGuard on the game dir this start uses; true when it had to act.
    /// Never throws: a failure is one log line and the launch goes on.
    private static func standUp(gameDir: String, who: String) -> Bool {
        let outcome = MCSeatedGuard.apply(gameDir: gameDir)
        NSLog("[seated-guard] %@: %@", who, outcome.summary)
        return outcome == .applied
    }

    /// The launcher's world list for the selected card (no dir is created).
    nonisolated static func savesDir(selection: String, tree: String) -> String? {
        let inst = MCInstance.read()
        guard let gd = inst.gameDir else { return nil }
        let instance = (gd as NSString).deletingLastPathComponent
        let on = MCEnv.on("KL_MC_PROFILES", default: true) && MCEnv.on("KL_MSA", default: true)
        guard on else { return gd + "/saves" }
        // Before the first list, so the worlds of the previous Minecraft
        // version are already there when the launcher shows them.
        MCProfileFS.inheritPreviousVersion(instance: instance)
        let store = MSAStore.forTree(tree)
        let seed = KleptonPayload.sharesSeedWorld
        if selection == MSAStore.offline {
            return MCProfileFS.savesDir(instance: instance, seedDir: gd, key: "offline", mayInherit: seed)
        }
        guard let a = store.snapshot().accounts.first(where: { $0.id == selection }) else { return nil }
        // A key not pinned yet is what profileKey would pin (read-only here).
        let key = a.profileKey ?? (a.xuid.isEmpty ? "acct-" + a.id.lowercased() : "xuid-" + a.xuid)
        return MCProfileFS.savesDir(instance: instance, seedDir: gd, key: key, mayInherit: (a.resolved && a.owns) || seed)
    }
}
#endif
