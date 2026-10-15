// The MetalFX switch on the settings page (KleptonMetalFXPreset.swift): turned
// on at 100 % it sets the resolution to 75 %; any other value, and turning it
// off, leaves the resolution alone. Run on the Mac. `make mfxpreset`, in `make check`.
import Foundation

@main
struct MetalFXPresetCheck {
    static var failed = 0
    static func expect(on: Bool, from: Double, _ want: Double?) {
        let got = KleptonMetalFXPreset.renderScale(switchedOn: on, from: from)
        let show = { (v: Double?) in v.map { String(format: "%.0f%%", $0 * 100) } ?? "unchanged" }
        print("  \(got == want ? "ok  " : "FAIL") \(on ? "on " : "off") at \(String(format: "%.0f%%", from * 100)) -> \(show(got))")
        if got != want { failed += 1 }
    }

    static func main() {
        print("=== MetalFX switch: resolution preset ===")
        expect(on: true, from: 1.0, 0.75)
        // A slider in steps of 0.05 can stop a hair below 1.0.
        expect(on: true, from: 1.0 - 1e-9, 0.75)
        expect(on: true, from: 0.95, nil)
        expect(on: true, from: 0.75, nil)
        expect(on: true, from: 0.5, nil)
        expect(on: true, from: 0.25, nil)
        expect(on: false, from: 1.0, nil)
        expect(on: false, from: 0.75, nil)
        if failed > 0 { print("mfxpreset: FAIL (\(failed))"); exit(1) }
        print("mfxpreset: PASS")
    }
}
