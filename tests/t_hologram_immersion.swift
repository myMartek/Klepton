// The real KleptonImmersion.swift imports SwiftUI and is visionOS-only, so the
// hologram test — which compiles KleptonHologram.swift on its own for the Mac —
// cannot include it. KleptonHologram only reads the pipeline's stencil format,
// and without the crown that format is .invalid.
import Metal

enum KleptonImmersion {
    static var stencilFormat: MTLPixelFormat { .invalid }
}
