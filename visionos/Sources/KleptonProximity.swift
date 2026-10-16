import ARKit
import SwiftUI
import QuartzCore
import simd

// Best effort only: reconstruction can miss glass, moving objects and unmapped
// surfaces. Nothing here represents a collision guarantee or a certified boundary.
final class KleptonProximity: @unchecked Sendable {
    static let shared = KleptonProximity()
    private let lock = NSLock()
    private var optedOut = false
    private var reveal = true
    private var evaluatedAt = 0.0
    private var tooCloseFlag = false
    private var closeEpisode = false
    private var generation = 0
    private var meshTask: Task<Void, Never>?
    private var sampleTask: Task<Void, Never>?
    // The serial worker owns mesh decoding, spatial indexing and distance queries.
    private let worker = DispatchQueue(label: "klepton.proximity", qos: .userInitiated)
    private var indexes: [UUID: KLProximityIndex] = [:]
    private var latch = KLProximityLatch()
    private var pose: (head: SIMD3<Float>?, hands: (SIMD3<Float>, SIMD3<Float>)?, time: Double, diagnostic: String)?
    private var nextLog = 0.0
    private var lastReason = ""
    private var nearDevice = ""
    private var holding = false
    // Snapshot only: ARKit queries stay on the render thread; SDK marks device
    // query and accessory latest collection thread-unsafe. Geometry stays here.
    func capture(world: WorldTrackingProvider, controllers: KleptonControllers) {
        guard applies else { return }
        let now = CACurrentMediaTime()
        let head = world.queryDeviceAnchor(atTimestamp: now)
        let hands = controllers.proximitySnapshot(at: now)
        let valid = world.state == .running && head?.trackingState == .tracked
            && head.map { KLProximityFreshness.isFresh(timestamp: $0.timestamp, now: now) } == true
        let column = head?.originFromAnchorTransform.columns.3
        let center = valid ? column.map { SIMD3($0.x,$0.y,$0.z) } : nil
        let diagnostic = "world=\(world.state) head=\(head.map { String(describing: $0.trackingState) } ?? "missing") age=\(head.map { String(format: "%.3f", now-$0.timestamp) } ?? "n/a") \(hands.diagnostic)"
        lock.withLock { pose = (center, hands.centers, now, diagnostic) }
    }
    var applies: Bool { klTargetName() == "x9531494193591052" }
    var enabled: Bool { lock.withLock { !optedOut } && applies }
    func setEnabled(_ value: Bool) {
        lock.withLock { optedOut = !value; reveal = true; evaluatedAt = 0; tooCloseFlag = false; closeEpisode = false }
    }
    var shouldReveal: Bool {
        guard enabled else { return false }
        return lock.withLock { reveal || CACurrentMediaTime()-evaluatedAt > 0.25 }
    }
    /// True only after a real approach. The room can also be visible while
    /// sensing is still starting, and that is not this notice.
    var tooClose: Bool { enabled && lock.withLock { tooCloseFlag } }
    func invalidate(_ reason: String) {
        lock.withLock { reveal = true; evaluatedAt = 0; pose = nil; tooCloseFlag = false; closeEpisode = false }
        NSLog("[proximity] fault: \(reason)")
    }
    func stop() {
        let tasks = lock.withLock { () -> (Task<Void, Never>?, Task<Void, Never>?) in
            generation += 1
            reveal = true; evaluatedAt = 0; tooCloseFlag = false; closeEpisode = false
            let old = (meshTask, sampleTask)
            meshTask = nil; sampleTask = nil
            return old
        }
        tasks.0?.cancel(); tasks.1?.cancel()
    }
    func start(_ scene: SceneReconstructionProvider) {
        stop()
        let token = lock.withLock { generation }
        worker.sync { indexes.removeAll(); latch = KLProximityLatch() }
        let newMeshTask = Task.detached { [weak self] in
            for await update in scene.anchorUpdates {
                guard let self, !Task.isCancelled else { return }
                // Backpressure: at most one retained mesh update waiting on work.
                await withCheckedContinuation { (done: CheckedContinuation<Void, Never>) in
                    self.worker.async {
                        defer { done.resume() }
                        guard self.lock.withLock({ self.generation == token }) else { return }
                        if update.event == .removed { self.indexes.removeValue(forKey: update.anchor.id) }
                        else { self.indexes[update.anchor.id] = KLProximityIndex(Self.triangles(update.anchor)) }
                    }
                }
            }
        }
        let newSampleTask = Task.detached { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                // Await worker completion: no unbounded queue of stale pose samples.
                await withCheckedContinuation { (done: CheckedContinuation<Void, Never>) in
                    self.worker.async {
                        defer { done.resume() }
                        guard self.lock.withLock({ self.generation == token }) else { return }
                        let now = CACurrentMediaTime()
                        let pose = self.lock.withLock { self.pose }
                        let count = self.indexes.values.reduce(0) { $0 + $1.count }
                        let fresh = pose.map { now-$0.time < 0.20 } ?? false
                        // A headset that has slipped to orientation-only tracking
                        // is simply absent. Controllers that are still tracked
                        // remain real measurements, so looking up does not throw
                        // away a close controller or invent a close ceiling.
                        var samples: [(String, SIMD3<Float>)] = []
                        if let head = pose?.head { samples.append(("head", head)) }
                        if let hands = pose?.hands {
                            samples.append(("left", hands.0))
                            samples.append(("right", hands.1))
                        }
                        let valid = scene.state == .running && fresh && count > 0 && !samples.isEmpty
                        let distances: [(String, SIMD3<Float>, Float)] = valid ? samples.map { name, p in
                            (name, p, self.indexes.values.reduce(Float.infinity) {
                                min($0, $1.clearance(p, radius: 0))
                            })
                        } : []
                        self.latch.update(clearance: distances.map(\.2).min(), valid: valid, now: now)
                        if let close = distances.filter({ $0.2 <= 0.30 }).min(by: { $0.2 < $1.2 }),
                           self.nearDevice != close.0 {
                            var point = SIMD3<Float>(repeating: .nan)
                            var anchor = UUID()
                            var best = Float.infinity
                            for (id, index) in self.indexes {
                                guard let q = index.nearestPoint(to: close.1) else { continue }
                                let d = simd_length(q-close.1)
                                if d < best { best = d; point = q; anchor = id }
                            }
                            NSLog("[proximity] threshold crossed by \(close.0) originDistance=\(close.2)m closestPoint=\(point) meshAnchor=\(anchor)")
                            self.nearDevice = close.0
                        } else if !self.latch.reveal {
                            self.nearDevice = ""
                        }
                        if !valid && !self.latch.reveal {
                            if !self.holding {
                                NSLog("[proximity] holding last clearance through tracking loss")
                                self.holding = true
                            }
                        } else if valid {
                            self.holding = false
                        }
                        let reason: String
                        if scene.state != .running { reason = "scene reconstruction \(scene.state)" }
                        else if !fresh { reason = "pose snapshot stale" }
                        else if samples.isEmpty { reason = "no tracked origin" }
                        else if count == 0 { reason = "mesh empty" }
                        else if self.latch.reveal { reason = "within 30 cm" }
                        else { reason = "clear" }
                        if now >= self.nextLog || (reason != self.lastReason && now >= self.nextLog-1.5) {
                            var nearest = [String]()
                            for (name, p, _) in distances {
                                var best: (UUID, SIMD3<Float>, Float)?
                                for (id, index) in self.indexes {
                                    guard let q = index.nearestPoint(to: p) else { continue }
                                    let d = simd_length(q-p)
                                    if d < (best?.2 ?? .infinity) { best = (id, q, d) }
                                }
                                if let (id, q, d) = best {
                                    nearest.append("\(name){origin=\(p) closestPoint=\(q) meshAnchor=\(id) originDistance=\(d)m}")
                                }
                            }
                            NSLog("[proximity] generation=\(token) reveal=\(self.latch.reveal) scene=\(scene.state) meshes=\(self.indexes.count) triangles=\(count) snapshotAge=\(pose.map { now-$0.time } ?? -1) \(pose?.diagnostic ?? "no pose snapshot") originToMesh=\(distances.map { "\($0.0)=\($0.2)" }) radius=0 trigger=0.30 release=0.40 stable=0.3 nearestWorld=\(nearest.joined(separator: " ")) reason=\(reason)")
                            self.nextLog = now+2; self.lastReason = reason
                        }
                        self.lock.withLock {
                            guard self.generation == token else { return }
                            if distances.contains(where: { $0.2 <= 0.30 }) { self.closeEpisode = true }
                            if !self.latch.reveal { self.closeEpisode = false }
                            self.reveal = self.latch.reveal
                            self.evaluatedAt = now
                            self.tooCloseFlag = self.latch.reveal && self.closeEpisode
                        }
                    }
                }
                try? await Task.sleep(nanoseconds: 33_000_000)
            }
        }
        lock.withLock {
            if generation == token { meshTask = newMeshTask; sampleTask = newSampleTask }
            else { newMeshTask.cancel(); newSampleTask.cancel() }
        }
    }
    private static func triangles(_ anchor: MeshAnchor) -> [KLProximityTriangle] {
        let vertices = anchor.geometry.vertices, faces = anchor.geometry.faces
        func vertex(_ i: Int) -> SIMD3<Float> {
            let p = vertices.buffer.contents().advanced(by: vertices.offset + i*vertices.stride)
            let v = SIMD3(p.loadUnaligned(as: Float.self),
                          p.advanced(by: 4).loadUnaligned(as: Float.self),
                          p.advanced(by: 8).loadUnaligned(as: Float.self))
            let w = anchor.originFromAnchorTransform * SIMD4(v,1)
            return SIMD3(w.x,w.y,w.z)
        }
        func index(_ i: Int) -> Int {
            let p = faces.buffer.contents().advanced(by: i*faces.bytesPerIndex)
            return faces.bytesPerIndex == 2 ? Int(p.load(as: UInt16.self)) : Int(p.load(as: UInt32.self))
        }
        guard faces.primitive.indexCount == 3 else { return [] }
        return (0..<faces.count).compactMap { face in
            let a = index(face*3), b = index(face*3+1), c = index(face*3+2)
            guard a < vertices.count, b < vertices.count, c < vertices.count else { return nil }
            return KLProximityTriangle(a: vertex(a), b: vertex(b), c: vertex(c))
        }
    }
}

struct KLProximityControls: View {
    @State private var enabled = KleptonProximity.shared.enabled
    var body: some View {
        if KleptonProximity.shared.applies {
            Toggle(isOn: $enabled) {
                HStack(spacing: 16) {
                    Image(systemName: "person.wave.2.fill")
                        .font(.system(size: 34, weight: .medium))
                        .frame(width: 64, height: 64)
                        .background(.white.opacity(0.14), in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Proximity Sense")
                            .font(.title.weight(.semibold))
                        Text(klT("Switches to passthrough when something comes close.",
                                 "Wechselt zur Durchsicht, sobald etwas nahe kommt."))
                            .font(.title3)
                            .foregroundStyle(.secondary)
                    }
                }
                .padding(.leading, 28)
            }
            .onChange(of: enabled) { _, value in KleptonProximity.shared.setEnabled(value) }
            .padding(.trailing, 28)
            .padding(.vertical, 12)
        }
    }
}
