//
//  KleptonImmersion.swift — crown-driven progressive immersion (visionOS 26).
//
//  Ported from the local crown patch: the system puts the game inside a portal
//  whose size the Digital Crown controls. 0 closes the portal completely, 1.0
//  is full immersion. The setting is read once at boot; the scene and the
//  compositor share it.
//

import SwiftUI

enum KleptonImmersion {
    /// Set at boot: the switch was on and the title is not mixed reality.
    nonisolated(unsafe) static var enabled = false

    /// Whether the crown's 0 % edge has closed the sound. Kept here so the fade
    /// is asked for once per crossing, not on every crown tick.
    nonisolated(unsafe) static var soundClosed = false

    /// The amount the system last reported, and whether the portal has ever
    /// been open far enough to count as "playing".
    nonisolated(unsafe) static var amount: Double = 1
    nonisolated(unsafe) static var wasImmersed = false

    static func noteAmount(_ value: Double?) {
        let v = value ?? 0
        amount = v
        if v > 0.9 { wasImmersed = true }
    }

    static let minimum = 0.0
    static let maximum = 1.0
    static let initial = 1.0

    static var style: any ImmersionStyle {
        .progressive(minimum...maximum, initialAmount: initial)
    }

    static var stencilFormat: MTLPixelFormat {
        enabled ? .stencil8 : .invalid
    }

    /// Present priming frames through a Compositor render context even when the
    /// crown feature is off. The plain `drawable.encodePresent` path aborts with
    /// "BUG IN CLIENT" in `cp_drawable_encode_present` on visionOS 26 and 27 as
    /// soon as the scene offers the progressive style, so an app with the
    /// switch off cannot start at all without this (measured 2026-11-06, first
    /// launch with the switch off).
    static let primingThroughContext = true
}
