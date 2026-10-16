// Run from repository root:
// swiftc visionos/Sources/KleptonProximityGeometry.swift tests/t_proximity.swift -o build/t_proximity && build/t_proximity
import Foundation
import simd

@main struct ProximityTests {
    static func main() {
        var checks = 0
        // Freshness applies to latest pose timestamps, not low-frequency stream
        // arrival times. Unknown, stale, and excessively future poses fail closed.
        precondition(KLProximityFreshness.isFresh(timestamp: 99.9, now: 100))
        precondition(!KLProximityFreshness.isFresh(timestamp: 99.7, now: 100))
        precondition(!KLProximityFreshness.isFresh(timestamp: 100.1, now: 100))
        precondition(!KLProximityFreshness.isFresh(timestamp: .nan, now: 100))
        func near(_ a: Float, _ b: Float) {
            precondition(abs(a-b) < 0.0001, "\(a) != \(b)"); checks += 1
        }
        let t = KLProximityTriangle(a: SIMD3(0,0,0), b: SIMD3(2,0,0), c: SIMD3(0,2,0))
        near(t.distance(to: SIMD3(0.5,0.5,0.3)),0.3) // face
        near(t.distance(to: SIMD3(1,-1,0)),1) // edge
        near(t.distance(to: SIMD3(-1,-1,0)),sqrt(2)) // vertex
        let degenerate = KLProximityTriangle(a: .zero,b: SIMD3(1,0,0),c: SIMD3(2,0,0))
        near(degenerate.distance(to: SIMD3(1,1,0)),1)
        let point = KLProximityTriangle(a: .zero,b: .zero,c: .zero)
        near(point.distance(to: SIMD3(0,0,1)),1)
        let index = KLProximityIndex([t])
        near(index.clearance(SIMD3(0.5,0.5,0.5),radius:0.2),0.3)
        near(index.clearance(SIMD3(0.5,0.5,0.5),radius:0),0.5)
        for (triangle, query, expected) in [
            (t, SIMD3<Float>(0.5,0.5,0.3), SIMD3<Float>(0.5,0.5,0)),
            (t, SIMD3<Float>(1,-1,0), SIMD3<Float>(1,0,0)),
            (t, SIMD3<Float>(-1,-1,0), SIMD3<Float>.zero),
            (degenerate, SIMD3<Float>(1,1,0), SIMD3<Float>(1,0,0)),
            (point, SIMD3<Float>(0,0,1), SIMD3<Float>.zero)
        ] {
            let q = triangle.closestPoint(to: query)
            near(simd_length(q-expected),0)
            near(simd_length(query-q),triangle.distance(to:query))
            near(simd_length(KLProximityIndex([triangle]).nearestPoint(to:query)!-expected),0)
        }
        precondition(KLProximityIndex([]).nearestPoint(to: .zero) == nil)
        // Long faces cross cells even when all vertices are far from the query.
        let long = KLProximityTriangle(a: SIMD3(-100,-100,0), b: SIMD3(100,-100,0), c: SIMD3(0,100,0))
        near(KLProximityIndex([long]).clearance(SIMD3(0,0,0.4),radius:0.18),0.22)
        precondition(index.clearance(SIMD3(.nan,0,0),radius:0.2) == -.infinity)
        var latch = KLProximityLatch()
        precondition(latch.reveal)
        latch.update(clearance:0.41,valid:true,now:0); precondition(latch.reveal)
        latch.update(clearance:0.41,valid:true,now:0.31); precondition(!latch.reveal)
        latch.update(clearance:0.35,valid:true,now:0.4); precondition(!latch.reveal)
        latch.update(clearance:0.30,valid:true,now:0.5); precondition(latch.reveal)
        latch.update(clearance:0.40,valid:true,now:0.6); precondition(latch.reveal)
        latch.update(clearance:0.41,valid:true,now:0.7); precondition(latch.reveal)
        latch.update(clearance:0.41,valid:true,now:1.01); precondition(!latch.reveal)
        // A dropout after a real clearance holds that clearance. It does not
        // reveal the room, and a later measured 30 cm still does, immediately.
        latch.update(clearance:1,valid:false,now:1.1); precondition(!latch.reveal)
        latch.update(clearance:nil,valid:true,now:1.2); precondition(!latch.reveal)
        latch.update(clearance:.nan,valid:true,now:1.3); precondition(!latch.reveal)
        latch.update(clearance:0.30,valid:true,now:1.4); precondition(latch.reveal)
        latch.update(clearance:nil,valid:false,now:1.5); precondition(latch.reveal)
        var unknown = KLProximityLatch()
        unknown.update(clearance:nil,valid:false,now:0); precondition(unknown.reveal)
        unknown.update(clearance:.nan,valid:true,now:0); precondition(unknown.reveal)
        // Randomized grid-vs-brute-force checks throughout the active band.
        var seed: UInt64 = 42
        func random() -> Float {
            seed = seed &* 6364136223846793005 &+ 1
            return Float(seed >> 40) / Float(1 << 24) * 6 - 3
        }
        func vector() -> SIMD3<Float> { SIMD3(random(),random(),random()) }
        let triangles = (0..<200).map { _ in KLProximityTriangle(a:vector(),b:vector(),c:vector()) }
        let grid = KLProximityIndex(triangles)
        for _ in 0..<1000 {
            let p = vector(), radius: Float = 0.2
            let brute = triangles.map { $0.distance(to:p)-radius }.min()!
            let found = grid.clearance(p,radius:radius)
            near(simd_length(grid.nearestPoint(to: p)!-p)-radius, brute)
            let originDistance = grid.clearance(p, radius: 0)
            if brute+radius <= 0.4 { near(originDistance,brute+radius) }
            else { precondition(originDistance > 0.4) }
            if brute <= 0.4 { near(found,brute) }
            else { precondition(found > 0.4) }
        }
        print("proximity: \(checks) geometry comparisons plus hysteresis/unknown-state checks passed")
    }
}
