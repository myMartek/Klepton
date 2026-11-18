import CoreGraphics
import CoreText
import Foundation

// The menu in the headset: a panel of the app's own over the game, in every game, opened and closed with
// Create + Options held together for a second. The owner's spec (2026-11-17/18): the game takes no input while
// it is open; either stick moves through it, Cross or a trigger chooses, Circle goes back. The main page has the
// performance panel, Graphics — every picture setting that can change while the game runs —, the
// play area's edge (Edge / Fade out / Off, only where a play area is in force: KLBoundary), Half-Life: Alyx's
// quick save and quick load (`save quick` / `load quick` through its console), its Cheats where they are allowed
// (Settings › Play › Cheats), and Close. German where the game speaks German
// (Alyx: its own language setting; any other game: the device's), English otherwise.
//
// Graphics, and what each does while the game runs:
//   MetalFX            the compositor reads the switch every frame (KleptonMetalFX.isOn). Only where the game
//                      draws below its eye size; at 100 % there is nothing to upscale.
//   Frame generation   the compositor's mode (KleptonFrameGen.mode), every frame.
//   Frame cadence      every refresh or every second one (kl_app_set_pace_div).
//   Foveated rendering Alyx: kl_mtl_foveate_set (its hooks are always put in for Alyx: KL_VK_FOVEATE_LIVE).
//                      Other Vulkan games: a new curve for kl_fove (kl_fove_set_curve) — only where it was on
//                      from the start, because its shaders are made for it then. GL games: not offered.
// A change lasts until the game ends and is not stored (the owner's word, 2026-11-18): the next start begins with
// the game's settings as they are in the start window.
// Not here, and why: the resolution, edge smoothing and the display output are sizes the game or the system
// fixes at the start; trimming the view while it runs only cuts off the edge without saving any work.
// Recenter view was in it for a day and is out (2026-11-18): in Half-Life: Alyx the game kept drawing for the
// head as it was, the compositor placed the picture for the recentred one, and half of the view was black. An
// OpenXR game is told of a recentre by an event of its own (XrEventDataReferenceSpaceChangePending), which the
// runtime does not send yet.
//
// The cheats reach the game through its net console (runtime/linux/kl_lx_netcon.c): the engine is started
// with -netconport and +sv_cheats 1, and every line is sent with "sv_cheats 1;" in front of it, so a level
// loaded since — which may have put the variable back — does not refuse it. Their state is read back from the
// game whenever the menu opens (`query`).

let klAlyxCheatsKey = "alyx.cheats"
/// The net console's port. Nothing listens on it outside the game's process.
let klAlyxNetconPort = 29015

/// Whether this run has the cheats: Half-Life: Alyx with the Cheats setting on (KL_ALYX_CHEATS=0|1 overrides).
/// Read once — the engine's arguments are fixed at its start.
let klAlyxCheatsOn: Bool = {
    guard klIsAlyx() else { return false }
    if let e = getenv("KL_ALYX_CHEATS") { return atoi(e) != 0 }
    return UserDefaults.standard.bool(forKey: klAlyxCheatsKey)
}()

final class KLHeadsetMenu {
    static let shared = KLHeadsetMenu()

    enum Page { case main, graphics, cheats }
    enum Action {
        case overlay, open(Page), close, back
        case cheat(String, (Bool) -> String)       // state key, the command for a state
        case give(String)                          // the command
        case metalFX, frameGen, cadence, foveation
        case playArea, quickSave, quickLoad
    }
    /// One line of a page, as drawn.
    struct Row {
        var title: String
        var section: String? = nil
        var value: String? = nil                   // the chip on the right
        var lit = false                            // the chip in the "on" colour
        var arrow = false
        var enabled = true
        var why: String? = nil                     // said in the footer when chosen while not enabled
    }

    let german: Bool = klIsAlyx() ? AlyxLanguage.stored == .german : KLText.german
    func t(_ en: String, _ de: String) -> String { german ? de : en }
    let title: String = {
        let b = Bundle.main
        return (b.object(forInfoDictionaryKey: "CFBundleDisplayName") as? String)
            ?? (b.object(forInfoDictionaryKey: "CFBundleName") as? String) ?? "Klepton"
    }()

    private static let cheatEntries: [(en: String, de: String, section: (String, String)?, action: Action)] = [
        // Fly mode (vrfly_enabled) was in the first build and is out on the owner's word (2026-11-17): it takes the
        // head tracking away, and the keyboard it is steered with does not exist here.
        ("God mode", "Godmode (unverwundbar)", ("Switches", "Ein/Aus"), .cheat("god", { _ in "god" })),
        ("Unlimited ammo from the backpack", "Unbegrenzte Munition im Rucksack", nil,
         .cheat("sv_infinite_ammo", { "sv_infinite_ammo \($0 ? 1 : 0)" })),
        ("Bottomless magazine", "Magazin wird nie leer", nil,
         .cheat("sv_infinite_clips", { "sv_infinite_clips \($0 ? 1 : 0)" })),
        ("All weapons + 20 resin", "Alle Waffen + 20 Harz", ("Give", "Gegenstände"), .give("impulse 101")),
        ("All weapon upgrades", "Alle Waffenverbesserungen", nil, .give("impulse 102")),
        ("Pistol", "Pistole", nil, .give("give item_hlvr_weapon_energygun")),
        ("SMG", "Maschinenpistole", nil, .give("give item_hlvr_weapon_rapidfire")),
        ("Shotgun", "Schrotflinte", nil, .give("give item_hlvr_weapon_shotgun")),
        ("Health syringe", "Heilspritze", nil, .give("give item_healthvial")),
        ("5× resin", "5× Harz", nil, .give("give item_hlvr_crafting_currency_large")),
    ]

    // ---- state (under lock) ----
    private let lock = NSLock()
    private(set) var isOpen = false
    private var page = Page.main
    private var cursor = 0
    private var on: [String: Bool] = [:]
    private var flashRow = -1, flashText = "", flashUntil = 0.0, flashPage = Page.cheats
    private var note = ""                            // the footer's line, if any
    /// Bumped on every change the panel shows; the compositor redraws when it differs.
    private(set) var generation = 0

    // ---- graphics, as this run has them ----
    /// Foveation: Alyx's four steps (AlyxFoveation) or the other Vulkan games' five (KleptonRendering.vkFoveation).
    private enum Fove { case alyx, vulkan, none }
    /// Asked each time: the modules are in place only once the game has made its Vulkan device.
    private var fove: Fove { klIsAlyx() ? (kl_mtl_foveate_live() != 0 ? .alyx : .none)
                                        : (kl_fove_live() != 0 ? .vulkan : .none) }
    private lazy var foveIndex: Int = {
        switch fove {
        case .alyx: return Self.alyxSteps.firstIndex(of: AlyxFoveation.stored) ?? 0
        case .vulkan:
            let v = UserDefaults.standard.string(forKey: KleptonRendering.vkFoveationKey) ?? KleptonRendering.targetVkFoveation
            return KleptonRendering.vkFoveationLevels.firstIndex(of: v) ?? 0
        case .none: return 0
        }
    }()
    private static let alyxSteps: [AlyxFoveation] = [.off, .on, .strong, .strongest]

    private func foveName(_ i: Int) -> String {
        switch fove {
        case .alyx: return Self.alyxSteps[i].name
        case .vulkan:
            return [t("Off", "Aus"), t("Low", "Niedrig"), t("Medium", "Mittel"), t("High", "Hoch"), "Ultra"][i]
        case .none: return ""
        }
    }

    /// The lines of a page with what each does, from the state as it is now. Under `lock`.
    private func entries(_ p: Page) -> [(Row, Action)] {
        switch p {
        case .main:
            var e: [(Row, Action)] = [
                (Row(title: t("Performance overlay", "Leistungsanzeige"), value: kl_metrics_visible() != 0 ? t("ON", "AN") : t("OFF", "AUS"),
                     lit: kl_metrics_visible() != 0), .overlay),
                (Row(title: t("Graphics", "Grafik"), arrow: true), .open(.graphics)),
            ]
            // Only where this run has a play area (KLBoundary.available).
            if KLBoundary.available {
                let m = KLBoundary.mode
                e.append((Row(title: t("Play area", "Spielbereich"),
                              value: m == .wall ? t("Edge", "Rand") : m == .room ? t("Fade out", "Ausblenden") : t("OFF", "AUS"),
                              lit: m != .off), .playArea))
            }
            if klIsAlyx() {
                e.append((Row(title: t("Quick save", "Schnell speichern")), .quickSave))
                e.append((Row(title: t("Quick load", "Schnell laden")), .quickLoad))
            }
            if klAlyxCheatsOn { e.append((Row(title: "Cheats", arrow: true), .open(.cheats))) }
            e.append((Row(title: t("Close", "Schließen")), .close))
            return e
        case .graphics:
            var e: [(Row, Action)] = []
            if KleptonMetalFX.deviceSupported {
                let can = KleptonMetalFX.canUpscale, isOn = KleptonMetalFX.isOn
                e.append((Row(title: "MetalFX", value: can ? (isOn ? t("ON", "AN") : t("OFF", "AUS")) : t("at 100 %", "bei 100 %"),
                              lit: can && isOn, enabled: can,
                              why: t("The game was started at 100 % resolution: there is nothing to upscale. Lower it in the settings first.",
                                     "Das Spiel läuft mit 100 % Auflösung – nichts hochzuskalieren. Erst in den Einstellungen senken.")),
                          .metalFX))
            }
            let fg = KleptonFrameGen.mode
            e.append((Row(title: t("Frame generation", "Zwischenbilder"),
                          value: [t("OFF", "AUS"), t("Ahead", "Voraus"), t("Between", "Dazwischen")][fg.rawValue],
                          lit: fg != .off), .frameGen))
            let div = Int(kl_app_pace_div())
            e.append((Row(title: t("Frame cadence", "Bildtakt"),
                          value: div > 1 ? t("Every 2nd", "Jedes 2.") : t("Every", "Jedes Bild"), lit: div > 1), .cadence))
            if fove != .none {
                e.append((Row(title: "Foveated Rendering", value: foveName(foveIndex), lit: foveIndex > 0), .foveation))
            }
            e.append((Row(title: t("Back", "Zurück")), .back))
            return e
        case .cheats:
            var e: [(Row, Action)] = Self.cheatEntries.map { c in
                var r = Row(title: german ? c.de : c.en, section: c.section.map { german ? $0.1 : $0.0 })
                if case .cheat(let key, _) = c.action {
                    let v = on[key] ?? false
                    r.value = v ? t("ON", "AN") : t("OFF", "AUS"); r.lit = v
                }
                return (r, c.action)
            }
            e.append((Row(title: t("Back", "Zurück")), .back))
            return e
        }
    }

    struct Snapshot {
        var page: Page; var cursor: Int; var rows: [Row]
        var flashRow: Int; var flashText: String; var flashUntil: Double; var flashPage: Page; var note: String
        var console: Int32
    }
    func snapshot() -> (Int, Snapshot) {
        lock.lock(); defer { lock.unlock() }
        return (generation, Snapshot(page: page, cursor: cursor, rows: entries(page).map(\.0), flashRow: flashRow,
                                     flashText: flashText, flashUntil: flashUntil, flashPage: flashPage, note: note,
                                     console: klAlyxCheatsOn ? kl_lx_netcon_state() : -1))
    }

    /// Once a run, before the first hold: the hold is the menu's (kl_metrics_set_chord_menu), and Alyx's console
    /// replies come here. Called from the controllers' poll.
    static let prepared: Void = {
        kl_metrics_set_chord_menu(1)
        if klIsAlyx() {
            kl_lx_netcon_set_output { line in
                guard let line else { return }
                KLHeadsetMenu.shared.heard(String(cString: line))
            }
        }
        NSLog("[menu] Create + Options held for a second open the menu in the headset%@",
              klIsAlyx() ? "; the game's console on port \(klAlyxNetconPort)\(klAlyxCheatsOn ? ", cheats on" : "")" : "")
    }()

    /// The console variables the cheats switch, read back from the game (`query`).
    static let readVariables = ["sv_infinite_ammo", "sv_infinite_clips"]

    /// What the game says about the switches, as it prints it: "godmode ON" / "godmode OFF" after each `god`,
    /// and a variable's value when its name alone is sent ("sv_infinite_ammo = 1", or with the flags after it).
    private func heard(_ line: String) {
        let l = line.lowercased().trimmingCharacters(in: .whitespaces)
        var key: String?, value: Bool?
        if l.hasPrefix("godmode") {
            key = "god"
            value = l.hasSuffix("on") ? true : l.hasSuffix("off") ? false : nil
        } else if let name = Self.readVariables.first(where: { l.hasPrefix($0) || l.hasPrefix("\"" + $0) }) {
            // The first word after the name and its separator ("=" or ":").
            let rest = l.dropFirst(l.hasPrefix("\"") ? name.count + 2 : name.count)
            let word = rest.drop(while: { " \t=:\"".contains($0) }).prefix(while: { !" \t\",".contains($0) })
            key = name
            value = ["1", "true"].contains(String(word)) ? true : ["0", "false"].contains(String(word)) ? false : nil
        }
        guard let key, let value else { return }
        lock.lock()
        on[key] = value
        generation += 1
        lock.unlock()
    }

    /// Asks the game how the cheats stand: a saved game brings god mode back with it, and a new start puts the
    /// variables back, so what the app last sent is no guide. `god` has no question form; twice is no change, and
    /// the second answer is the state. The answers come back through `heard`.
    private func query() {
        guard klAlyxCheatsOn else { return }
        sendConsole("god; god; " + Self.readVariables.joined(separator: "; "))
    }

    func toggleOpen() {
        lock.lock()
        isOpen.toggle()
        if isOpen { page = .main; cursor = 0; note = "" }
        generation += 1
        let open = isOpen
        lock.unlock()
        NSLog("[menu] \(open ? "opened" : "closed")")
        if open { resetInput(); query() }
    }

    // ---- input: called with each poll's buttons while the menu is open ----
    private var stickDir = 0, stickRepeat = 0.0
    private var selectWas = true, backWas = true      // a press that opened the menu is not a choice

    private func resetInput() { stickDir = 0; selectWas = true; backWas = true }

    /// `stickY` the stronger of the two sticks' vertical travel (up positive); `select` Cross or a
    /// trigger; `back` Circle. Edges only; a held stick repeats.
    func drive(stickY: Float, select: Bool, back: Bool, now: Double) {
        let dir = stickY > 0.6 ? -1 : stickY < -0.6 ? 1 : (abs(stickY) < 0.3 ? 0 : stickDir)
        var move = 0
        if dir != stickDir {
            stickDir = dir
            if dir != 0 { move = dir; stickRepeat = now + 0.45 }
        } else if dir != 0, now >= stickRepeat {
            move = dir; stickRepeat = now + 0.16
        }
        let pick = select && !selectWas, leave = back && !backWas
        selectWas = select; backWas = back
        guard move != 0 || pick || leave else { return }

        lock.lock()
        let items = entries(page)
        if move != 0 { cursor = (cursor + move + items.count) % items.count; note = "" }
        var console: String?, ask = false, closeMenu = false, cheatPrefix = true
        func goBack() {
            let from = page
            page = .main
            cursor = entries(.main).firstIndex { if case .open(let p) = $0.1 { return p == from }; return false } ?? 0
        }
        if leave {
            if page == .main { closeMenu = true } else { goBack() }
        } else if pick {
            let (row, action) = items[min(cursor, items.count - 1)]
            if !row.enabled {
                note = row.why ?? ""
            } else {
                switch action {
                case .overlay: kl_metrics_set_visible(kl_metrics_visible() != 0 ? 0 : 1)
                case .open(let p):
                    page = p; cursor = 0; note = ""
                    if p == .cheats { ask = true }
                case .close: closeMenu = true
                case .back: goBack()
                case .cheat(let key, let command):
                    let wanted = !(on[key] ?? false)
                    on[key] = wanted
                    console = command(wanted)
                case .give(let command):
                    console = command
                    flashRow = cursor; flashText = t("Received", "Erhalten"); flashUntil = now + 1.5; flashPage = page
                case .metalFX:
                    let to = !KleptonMetalFX.isOn
                    KleptonMetalFX.liveSwitch = to
                    if to && KleptonFrameGen.mode != .off {
                        note = t("While MetalFX is on, no in-between pictures are made.",
                                 "Solange MetalFX an ist, entstehen keine Zwischenbilder.")
                    }
                case .frameGen:
                    let m = KleptonFrameGen.mode.next
                    KleptonFrameGen.mode = m
                    if m != .off && KleptonMetalFX.isOn && KleptonMetalFX.canUpscale {
                        note = t("While MetalFX is on, no in-between pictures are made.",
                                 "Solange MetalFX an ist, entstehen keine Zwischenbilder.")
                    }
                case .cadence:
                    let div: Int32 = kl_app_pace_div() > 1 ? 1 : 2
                    kl_app_set_pace_div(div)
                    if div > 1 && kl_app_shown_at_half_rate() != 0 {
                        note = t("visionOS shows the game at half rate already (50 Hz): every 2nd changes nothing now.",
                                 "visionOS zeigt das Spiel schon mit halber Rate (50 Hz): „Jedes 2.“ ändert gerade nichts.")
                    }
                case .playArea:
                    // Edge, fade out (only in a mixed space, where the room can show), off.
                    let order: [KLRoomGuard.Mode] = Immersive.mixedAtBoot ? [.wall, .room, .off] : [.wall, .off]
                    let now = KLBoundary.mode
                    KLBoundary.mode = order[((order.firstIndex(of: now) ?? -1) + 1) % order.count]
                case .quickSave:
                    console = "save quick"; cheatPrefix = false
                    flashRow = cursor; flashText = t("Saved", "Gespeichert"); flashUntil = now + 1.5; flashPage = page
                case .quickLoad:
                    console = "load quick"; cheatPrefix = false
                    closeMenu = true
                case .foveation:
                    foveIndex = (foveIndex + 1) % (fove == .alyx ? Self.alyxSteps.count : KleptonRendering.vkFoveationLevels.count)
                    applyFoveation(foveIndex)
                }
            }
        }
        if closeMenu { isOpen = false }
        generation += 1
        lock.unlock()
        if closeMenu { NSLog("[menu] closed") }
        if let console { sendConsole(console, cheat: cheatPrefix) }
        if ask { query() }
    }

    /// Under `lock`. Alyx: its step's three numbers (AlyxFoveation.knobs). Other Vulkan games: the level's curve as
    /// the start applies it (KleptonRendering.applyEnvironment). For this run only, like every change in the menu.
    private func applyFoveation(_ i: Int) {
        switch fove {
        case .alyx:
            let step = Self.alyxSteps[i]
            if let k = step.knobs {
                kl_mtl_foveate_set(Float(k.edge) ?? 0, Float(k.inner) ?? 0.3, k.outer.flatMap(Float.init) ?? 0)
            } else {
                kl_mtl_foveate_set(0, 0, 0)
            }
            NSLog("[menu] foveation: \(step.title)")
        case .vulkan:
            let level = KleptonRendering.vkFoveationLevels[i]
            // kl_fove.m's levels, and the two the app sharpens (applyEnvironment): box and ramp as parts of the
            // target's height, the floor the lowest rate. Off is full rate everywhere — the shaders stay as made.
            let curve: (Float, Float, Float) = {
                switch level {
                case "low":    return (1216.0 / 2016, 384.0 / 2016, 1.0 / 3)
                case "medium": return (1024.0 / 2016, 384.0 / 2016, 0.22)
                case "high":   return (0.18, 384.0 / 2016, 0.16)
                case "ultra":  return (0.20, 0.15, 0.06)
                default:       return (1, 0, 1)
                }
            }()
            kl_fove_set_curve(curve.0, curve.1, curve.2)
            NSLog("[menu] foveation: \(level)")
        case .none: break
        }
    }

    private let queue = DispatchQueue(label: "klepton.menu", qos: .userInitiated)
    /// A cheat with cheats allowed in front of it: a level loaded since may have put sv_cheats back. Saving and
    /// loading go as they are, so a game without cheats stays one.
    private func sendConsole(_ command: String, cheat: Bool = true) {
        queue.async {
            let ok = kl_lx_netcon_send((cheat && klAlyxCheatsOn ? "sv_cheats 1; " : "") + command) == 0
            self.lock.lock()
            if !ok { self.note = self.t("The game's console is not ready yet – try again in a moment.",
                                        "Die Spielkonsole ist noch nicht bereit – bitte gleich noch einmal versuchen.") }
            self.generation += 1
            self.lock.unlock()
        }
    }
}

// MARK: - The picture

enum KLHeadsetMenuPanel {
    /// Laid out on 900 units across; the texture is as tall as the longest page, and a page uses its top.
    static let units = 900
    static let pixels = (w: 900, h: 1000)
    static let levels = 2
    /// Width in metres at 1.1 m (about 19 degrees).
    static let width: Float = 0.36

    private static let header: CGFloat = 104, row: CGFloat = 60, sectionGap: CGFloat = 44, footer: CGFloat = 92

    /// The height a page takes in the texture, in its pixels.
    static func height(_ s: KLHeadsetMenu.Snapshot) -> Int {
        let sections = s.rows.filter { $0.section != nil }.count
        return min(pixels.h, Int(header + CGFloat(s.rows.count) * row + CGFloat(sections) * sectionGap + footer + 16))
    }

    private static func rgb(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ a: CGFloat = 1) -> CGColor {
        CGColor(srgbRed: r, green: g, blue: b, alpha: a)
    }
    private static let back = rgb(0.04, 0.045, 0.05, 0.93)
    private static let edge = rgb(1, 1, 1, 0.10)
    private static let bright = rgb(0.96, 0.96, 0.96)
    private static let dim = rgb(0.62, 0.65, 0.70)
    private static let faint = rgb(0.45, 0.47, 0.50)
    private static let accent = rgb(0.98, 0.55, 0.16)
    private static let selected = rgb(0.98, 0.55, 0.16, 0.22)
    private static let green = rgb(0.36, 0.86, 0.47)
    private static let off = rgb(1, 1, 1, 0.14)
    private static let red = rgb(1.0, 0.45, 0.36)

    private static func font(_ size: CGFloat, bold: Bool = false) -> CTFont {
        CTFontCreateUIFontForLanguage(bold ? .emphasizedSystem : .system, size, nil)
            ?? CTFontCreateWithName("Helvetica" as CFString, size, nil)
    }
    private static let titleFont = font(40, bold: true), rowFont = font(32), smallFont = font(24),
                       sectionFont = font(22, bold: true), chipFont = font(22, bold: true)

    @discardableResult
    private static func text(_ ctx: CGContext, _ s: String, x: CGFloat, y: CGFloat, font: CTFont,
                             color: CGColor, right: Bool = false, maxWidth: CGFloat? = nil) -> CGFloat {
        let attrs = [kCTFontAttributeName: font, kCTForegroundColorAttributeName: color] as CFDictionary
        guard let str = CFAttributedStringCreate(nil, s as CFString, attrs) else { return 0 }
        var line = CTLineCreateWithAttributedString(str)
        var w = CGFloat(CTLineGetTypographicBounds(line, nil, nil, nil))
        if let maxWidth, w > maxWidth,
           let dots = CFAttributedStringCreate(nil, "…" as CFString, attrs),
           let cut = CTLineCreateTruncatedLine(line, Double(maxWidth), .end, CTLineCreateWithAttributedString(dots)) {
            line = cut; w = maxWidth
        }
        ctx.textPosition = CGPoint(x: right ? x - w : x, y: y)
        CTLineDraw(line, ctx)
        return w
    }

    private static func width(_ s: String, font: CTFont) -> CGFloat {
        guard let str = CFAttributedStringCreate(nil, s as CFString, [kCTFontAttributeName: font] as CFDictionary)
        else { return 0 }
        return CGFloat(CTLineGetTypographicBounds(CTLineCreateWithAttributedString(str), nil, nil, nil))
    }

    private static func rounded(_ ctx: CGContext, _ r: CGRect, _ radius: CGFloat, fill: CGColor) {
        ctx.addPath(CGPath(roundedRect: r, cornerWidth: radius, cornerHeight: radius, transform: nil))
        ctx.setFillColor(fill)
        ctx.fillPath()
    }

    static func draw(_ ctx: CGContext, _ s: KLHeadsetMenu.Snapshot, title: String, german: Bool, now: Double) {
        func t(_ en: String, _ de: String) -> String { german ? de : en }
        let H = CGFloat(height(s)), W = CGFloat(units)
        ctx.clear(CGRect(x: 0, y: 0, width: W, height: CGFloat(pixels.h)))
        let card = CGRect(x: 2, y: 2, width: W - 4, height: H - 4)
        rounded(ctx, card, 28, fill: back)
        ctx.addPath(CGPath(roundedRect: card, cornerWidth: 28, cornerHeight: 28, transform: nil))
        ctx.setStrokeColor(edge); ctx.setLineWidth(2); ctx.strokePath()

        let pad: CGFloat = 40
        let (head, sub): (String, String) = {
            switch s.page {
            case .main: return (title, t("Menu", "Menü"))
            case .graphics: return (t("Graphics", "Grafik"), t("Changes apply at once, until the game ends", "Änderungen wirken sofort, bis das Spiel endet"))
            case .cheats: return ("Cheats", t("Switch on and off, or take", "Cheats schalten oder Gegenstände holen"))
            }
        }()
        text(ctx, head, x: pad, y: 62, font: titleFont, color: bright, maxWidth: W - pad * 2 - 260)
        text(ctx, sub, x: pad, y: 92, font: smallFont, color: dim, maxWidth: W - pad * 2)
        // Alyx's console, top right on the cheats page: a cheat only works once the game has taken the connection.
        if s.page == .cheats, s.console >= 0 {
            let (consoleText, consoleColor): (String, CGColor) =
                s.console == 2 ? (t("Console connected", "Konsole verbunden"), green)
                : s.console == 1 ? (t("Console ready", "Konsole bereit"), dim)
                : (t("Console not ready", "Konsole nicht bereit"), red)
            text(ctx, consoleText, x: W - pad, y: 62, font: smallFont, color: consoleColor, right: true)
        }

        var y = header
        for (i, r) in s.rows.enumerated() {
            if let sec = r.section {
                text(ctx, sec.uppercased(), x: pad, y: y + 32, font: sectionFont, color: dim)
                y += sectionGap
            }
            let box = CGRect(x: 16, y: y + 3, width: W - 32, height: row - 6)
            if i == s.cursor {
                rounded(ctx, box, 14, fill: selected)
                rounded(ctx, CGRect(x: 16, y: y + 10, width: 6, height: row - 20), 3, fill: accent)
            }
            let ink = !r.enabled ? faint : i == s.cursor ? bright : rgb(0.88, 0.88, 0.9)
            text(ctx, r.title, x: pad, y: y + 41, font: rowFont, color: ink, maxWidth: W - pad * 2 - 230)
            if r.arrow { text(ctx, "›", x: W - pad, y: y + 42, font: titleFont, color: dim, right: true) }
            var chip: (String, CGColor, CGColor)?
            if let v = r.value {
                chip = !r.enabled ? (v, rgb(1, 1, 1, 0.06), faint) : r.lit ? (v, green, rgb(0, 0, 0)) : (v, off, bright)
            }
            if s.flashRow == i, now < s.flashUntil, s.flashPage == s.page { chip = ("✓ " + s.flashText, accent, rgb(0, 0, 0)) }
            if let (label, fill, fg) = chip {
                let w = max(84, width(label, font: chipFont) + 32)
                let c = CGRect(x: W - pad - w, y: y + 13, width: w, height: row - 26)
                rounded(ctx, c, (row - 26) / 2, fill: fill)
                text(ctx, label, x: c.midX - (w - 32) / 2, y: c.maxY - 10, font: chipFont, color: fg)
            }
            y += row
        }
        // The footer: how to use it, or what the last choice could not do.
        ctx.setFillColor(edge); ctx.fill(CGRect(x: pad, y: y + 10, width: W - pad * 2, height: 2))
        if !s.note.isEmpty {
            text(ctx, s.note, x: pad, y: y + 46, font: smallFont, color: amberOrRed(s.note), maxWidth: W - pad * 2)
        } else {
            text(ctx, t("Stick: choose · ✕ or trigger: OK · ○: back", "Stick: wählen · ✕ oder Trigger: OK · ○: zurück"),
                 x: pad, y: y + 46, font: smallFont, color: dim, maxWidth: W - pad * 2)
        }
        text(ctx, t("Hold Create + Options 1 s to close", "Zum Schließen Create + Options 1 s halten"),
             x: pad, y: y + 78, font: smallFont, color: dim, maxWidth: W - pad * 2)
    }
    private static func amberOrRed(_ s: String) -> CGColor { s.contains("Konsole") || s.contains("console") ? red : rgb(1.0, 0.76, 0.30) }
}
