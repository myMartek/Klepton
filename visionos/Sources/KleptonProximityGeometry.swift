import Foundation
import simd

// Pure geometry/state, also compiled by the macOS synthetic tests.
struct KLProximityTriangle {
    let a: SIMD3<Float>, b: SIMD3<Float>, c: SIMD3<Float>
    func distance(to p: SIMD3<Float>) -> Float { simd_length(p - closestPoint(to: p)) }
    func closestPoint(to p: SIMD3<Float>) -> SIMD3<Float> {
        // Closest point on a triangle (including degenerate reconstruction faces).
        let ab = b-a, ac = c-a, ap = p-a
        let d1 = simd_dot(ab, ap), d2 = simd_dot(ac, ap)
        if d1 <= 0 && d2 <= 0 { return a }
        let bp = p-b, d3 = simd_dot(ab, bp), d4 = simd_dot(ac, bp)
        if d3 >= 0 && d4 <= d3 { return b }
        let cp = p-c, d5 = simd_dot(ab, cp), d6 = simd_dot(ac, cp)
        if d6 >= 0 && d5 <= d6 { return c }
        func segment(_ x: SIMD3<Float>, _ y: SIMD3<Float>) -> SIMD3<Float> {
            let v = y-x, n = simd_length_squared(v)
            return x + v * (n > 0 ? min(1, max(0, simd_dot(p-x,v)/n)) : 0)
        }
        let vc = d1*d4-d3*d2, vb = d5*d2-d1*d6, va = d3*d6-d5*d4
        if simd_length_squared(simd_cross(ab,ac)) < 1e-12 {
            return [segment(a,b), segment(b,c), segment(c,a)].min {
                simd_length_squared(p-$0) < simd_length_squared(p-$1)
            }!
        }
        if vc <= 0 && d1 >= 0 && d3 <= 0 { return segment(a,b) }
        if vb <= 0 && d2 >= 0 && d6 <= 0 { return segment(a,c) }
        if va <= 0 && d4-d3 >= 0 && d5-d6 >= 0 { return segment(b,c) }
        let denom = 1 / (va+vb+vc)
        return a + ab*(vb*denom) + ac*(vc*denom)
    }
}

struct KLProximityIndex {
    private var cells: [SIMD3<Int32>: [Int]] = [:]
    private var large: [Int] = []
    private var triangles: [KLProximityTriangle] = []
    private(set) var count = 0
    private func cell(_ p: SIMD3<Float>) -> SIMD3<Int32> {
        SIMD3(Int32(floor(p.x)), Int32(floor(p.y)), Int32(floor(p.z)))
    }
    init(_ triangles: [KLProximityTriangle]) {
        for t in triangles {
            guard [t.a,t.b,t.c].allSatisfy({ $0.x.isFinite && $0.y.isFinite && $0.z.isFinite && simd_length($0) < 10000 }) else { continue }
            let id = count
            self.triangles.append(t)
            count += 1
            let lo = cell(simd_min(t.a,simd_min(t.b,t.c)))
            let hi = cell(simd_max(t.a,simd_max(t.b,t.c)))
            if (Int(hi.x-lo.x)+1)*(Int(hi.y-lo.y)+1)*(Int(hi.z-lo.z)+1) > 512 {
                large.append(id); continue
            }
            for x in lo.x...hi.x { for y in lo.y...hi.y { for z in lo.z...hi.z {
                cells[SIMD3(x,y,z), default: []].append(id)
            } } }
        }
    }
    // Exact nearest point for throttled diagnostics, not merely a centroid or
    // the threshold-limited candidate distance used by the fast safety query.
    func nearestPoint(to p: SIMD3<Float>) -> SIMD3<Float>? {
        guard p.x.isFinite && p.y.isFinite && p.z.isFinite else { return nil }
        var best: SIMD3<Float>?
        var distanceSquared = Float.infinity
        for t in triangles {
            let point = t.closestPoint(to: p)
            let d = simd_length_squared(point-p)
            if d < distanceSquared { best = point; distanceSquared = d }
        }
        return best
    }
    func clearance(_ p: SIMD3<Float>, radius: Float, searchDistance: Float = 0.4) -> Float {
        guard p.x.isFinite && p.y.isFinite && p.z.isFinite && simd_length(p) < 10000 else { return -.infinity }
        // Only distances <= release threshold matter. Triangle AABBs occupy every
        // intersected cell, so long triangles cannot escape the neighbourhood.
        let reach = radius + searchDistance
        let lo = cell(p-SIMD3(repeating: reach)), hi = cell(p+SIMD3(repeating: reach))
        var d = Float.infinity
        for t in large { d = min(d,triangles[t].distance(to:p)) }
        for x in lo.x...hi.x { for y in lo.y...hi.y { for z in lo.z...hi.z {
            for t in cells[SIMD3(x,y,z)] ?? [] { d = min(d,triangles[t].distance(to:p)) }
        } } }
        return d-radius
    }
}

enum KLProximityFreshness {
    // Explicit policy bound, not a claim about ARKit stream frequency.
    static func isFresh(timestamp: Double, now: Double) -> Bool {
        timestamp.isFinite && now.isFinite && now-timestamp >= -0.05 && now-timestamp < 0.20
    }
}

struct KLProximityLatch {
    private(set) var reveal = true
    private var clearSince: TimeInterval?
    private var accepted = false
    mutating func update(clearance: Float?, valid: Bool, now: TimeInterval = ProcessInfo.processInfo.systemUptime) {
        // Before the first measured clearance, an unknown sample still shows
        // the room. After that, a brief tracking loss keeps the last decision:
        // looking at a blank ceiling drops the headset out of full tracking
        // without moving any origin onto a surface.
        guard valid, let clearance, clearance.isFinite else {
            if !accepted { reveal = true; clearSince = nil }
            return
        }
        accepted = true
        if clearance <= 0.30 { reveal = true; clearSince = nil }
        else if clearance > 0.40 {
            if clearSince == nil { clearSince = now }
            if now - clearSince! >= 0.3 { reveal = false }
        } else { clearSince = nil }
    }
}
