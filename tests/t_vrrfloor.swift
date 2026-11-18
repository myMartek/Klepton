// VRR rate floor default (Bigscreen x2497738113633933, 2026-10-24): the
// guest foveates to 37.6% of the fragments and the upsample shimmers on
// textures/reflections; 0.50 floors every zone at half rate (same value as
// the Test switch KL_VRR_FLOOR). Pins the per-target default; KL_VRR_FLOOR
// in the environment still wins. `make vrrfloor`.
import Foundation

@main
struct VRRFloorTests {
    static func check(_ cond: Bool, _ what: String, _ fail: inout Bool) {
        print("  \(what) \(cond ? "ok" : "FAIL")")
        if !cond { fail = true }
    }

    static func main() {
        var fail = false
        print("=== bigscreen vrr floor ===")
        check(klVRRFloorDefault("x2497738113633933") == 0.5, "bigscreen floors at 0.50", &fail)
        check(klVRRFloorDefault("x24882487241367228") == 0, "other title keeps 0", &fail)
        check(klVRRFloorDefault("") == 0, "empty keeps 0", &fail)
        print(fail ? "FAIL bigscreen vrr floor" : "PASS bigscreen vrr floor")
        if fail { fatalError("vrr floor default wrong") }
    }
}
