import Foundation

/// The VRR rate-floor default for a target (see KleptonCompositor's
/// `rateFloor`): which guests get every foveation zone floored, and where.
/// Bigscreen renders only 37.6% of the fragments and the upsample shimmers on
/// room textures and film reflections — 0.50, the Test-switch value, bounds
/// the worst mip bias to +1.0 at ~49% of the fragments. Everywhere else 0
/// (today's behaviour). `KL_VRR_FLOOR` in the environment still wins where
/// set; this is only the default it falls back to.
func klVRRFloorDefault(_ target: String) -> Float {
    if target == "x2497738113633933" { return 0.5 }
    return 0
}
