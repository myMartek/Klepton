import Foundation

// What the settings page does to "Resolution" when the player turns MetalFX
// upscaling on (KleptonMetalFX.setFromSettings). At 100 % there is nothing to
// upscale (KleptonMetalFX.canUpscale), so the switch would do nothing; the
// slider goes to 75 % instead, the start AVP Play presets for a MetalFX
// install (MetalFXResolution there). Any other value is the player's own and
// stays, and switching off leaves the resolution alone. Plain Foundation, so
// the rule runs on the Mac: tests/t_mfxpreset.swift, `make mfxpreset`.
enum KleptonMetalFXPreset {
    /// The resolution a game is set to when MetalFX is turned on at 100 %.
    static let renderScale = 0.75

    /// The resolution after the switch was turned, or nil to leave it as it is.
    /// `current` is the slider's value (KleptonRendering.renderScale, a
    /// fraction in steps of 0.05; 1.0 is 100 %).
    static func renderScale(switchedOn on: Bool, from current: Double) -> Double? {
        on && current >= 1.0 - 0.025 ? renderScale : nil
    }
}
