import Foundation

// What the MetalFX switch does to "Resolution" (KleptonMetalFX.setFromSettings,
// tapToggle). At 100 % there is nothing to upscale (KleptonMetalFX.canUpscale),
// so turning it on there sets the slider to 75 %, the start AVP Play presets for
// a MetalFX install (MetalFXResolution there), and remembers that the switch
// lowered it. Turning it off gives the 100 % back only then: the switch lowered
// it itself and the slider has not been moved by hand since (any move clears the
// mark). A resolution the player chose stays as it is. The mark is stored with
// the game's settings, so it holds across starts. Plain Foundation, so the rule
// runs on the Mac: tests/t_mfxpreset.swift, `make mfxpreset`.
struct KleptonMetalFXPreset {
    /// The resolution a game is set to when MetalFX is turned on at 100 %.
    static let renderScale = 0.75
    /// Set while the resolution is the one the switch lowered it to.
    static let loweredKey = "klepton.metalfxLoweredResolution"

    let defaults: UserDefaults
    init(defaults: UserDefaults = .standard) { self.defaults = defaults }

    /// Whether the switch lowered the resolution and nobody has moved it since.
    var lowered: Bool { defaults.bool(forKey: Self.loweredKey) }

    /// The resolution after the switch was turned, or nil to leave it as it is.
    /// `current` is the slider's value (KleptonRendering.renderScale, a fraction
    /// in steps of 0.05; 1.0 is 100 %). Keeps the mark up to date.
    func switched(on: Bool, from current: Double) -> Double? {
        if on {
            guard current >= 1.0 - 0.025 else { return nil }
            defaults.set(true, forKey: Self.loweredKey)
            return Self.renderScale
        }
        // The value is checked too: a stored resolution a target dropped since
        // (KleptonRendering's picture reset) is no longer the switch's.
        let restore = lowered && abs(current - Self.renderScale) < 0.025
        defaults.removeObject(forKey: Self.loweredKey)
        return restore ? 1.0 : nil
    }

    /// The player moved the slider: the resolution is theirs from now on.
    func movedByHand() {
        defaults.removeObject(forKey: Self.loweredKey)
    }
}
