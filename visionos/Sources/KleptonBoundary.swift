import Foundation
import Metal
import ARKit
import QuartzCore
import simd

// The play area in the game: how near the player is to its edge, and what is
// shown for it (KLRoomGuard.mode, a setting of each game).
//
//  - wall: a see-through wall stands on the edge of the area, as a Quest's
//    boundary does — a grid that is barely there when the head comes within
//    35 cm of the edge (a hand 25 cm), clearer the nearer it gets, and lit only round the
//    part that is near. Never more than about half opaque, so the game can be
//    played on through it.
//  - room: the game itself turns see-through, so the real room shows (what
//    Klepton PR #41 did, by degrees here rather than at once). Only in a mixed
//    space; in the full one (frame rate up to 120) visionOS shows the room at its
//    own boundary, and the wall is drawn instead.
//
// Neither while the game is in its own mixed reality (a passthrough layer in its
// frames, kl_ovrp_passthrough_live): the room is already in view there, so a wall
// or a fade guards against nothing, and the wall was drawn wrongly over the
// game's cut-out picture (seen in Angry Birds VR, 2026-10-18; the owner's rule:
// never a wall or a fade in mixed reality).
//
// Only the distance across the floor counts — the area is a shape on the floor —
// so a controller held low, put down or swung at the floor never sets it off,
// and neither does a table inside the area: nothing about the floor or what
// stands in the area is part of it.
//
// The room is found through this game's own world anchor for it (KLRoomStore.
// anchors), which visionOS hands back once the headset knows where it is; the
// start window's check (KLRoomFlow, purpose .check) has made sure there is one.
// Until it arrives nothing is shown.

final class KLBoundary {
    let mode: KLRoomGuard.Mode
    private let device: MTLDevice
    private let lock = NSLock()
    private struct Active {
        let room: KLRoomRecord
        var rawFromRoom: simd_float4x4
        let sdf: [Float]
        let wallVerts: MTLBuffer?
        let wallCount: Int
    }
    private var active: Active?          // under lock
    private let rooms: [KLRoomRecord]
    private let anchorRoom: [UUID: UUID]   // this game's anchor -> room
    private var wallPipe: MTLRenderPipelineState?
    private var fadePipe: MTLRenderPipelineState?
    private var said = Set<String>()
    private var watching = false
    private var lastNear: Float = 0
    private var lastShowsRoom = false

    /// nil where there is nothing to show: the switch is off, the setting is
    /// off, or no room has been measured.
    static func make(device: MTLDevice, color: MTLPixelFormat, amplification: Int) -> KLBoundary? {
        guard KLRoomGuard.enabled else { return nil }
        let mode = KLRoomGuard.mode
        guard mode != .off else { NSLog("[room] play area: shown as nothing (the game's setting)"); return nil }
        let stationary = KLRoomGuard.style == .stationary
        let rooms = stationary ? [] : KLRoomStore.load()
        guard stationary || !rooms.isEmpty else { NSLog("[room] play area: no room measured — nothing to show"); return nil }
        return KLBoundary(device: device, color: color, amplification: amplification, mode: mode, rooms: rooms,
                          stationary: stationary)
    }

    /// Stationary: no room — a circle round the head, put there at the first
    /// frame and again at each recentring (`centre`).
    let stationary: Bool
    private var circle: (room: KLRoomRecord, sdf: [Float], verts: MTLBuffer?, count: Int)?

    private init(device: MTLDevice, color: MTLPixelFormat, amplification: Int, mode: KLRoomGuard.Mode,
                 rooms: [KLRoomRecord], stationary: Bool) {
        self.device = device
        self.mode = mode
        self.rooms = rooms
        self.stationary = stationary
        var map: [UUID: UUID] = [:]
        for (room, anchor) in KLRoomStore.anchors { map[anchor] = room }
        anchorRoom = map
        do {
            let lib = try device.makeLibrary(source: Self.msl, options: nil)
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = lib.makeFunction(name: "klb_wall_v")
            d.fragmentFunction = lib.makeFunction(name: "klb_wall_f")
            d.colorAttachments[0].pixelFormat = color
            d.depthAttachmentPixelFormat = .depth32Float
            d.maxVertexAmplificationCount = amplification
            let c = d.colorAttachments[0]!
            c.isBlendingEnabled = true
            c.rgbBlendOperation = .add; c.alphaBlendOperation = .add
            c.sourceRGBBlendFactor = .one; c.sourceAlphaBlendFactor = .one
            c.destinationRGBBlendFactor = .oneMinusSourceAlpha; c.destinationAlphaBlendFactor = .oneMinusSourceAlpha
            wallPipe = try device.makeRenderPipelineState(descriptor: d)
            let f = MTLRenderPipelineDescriptor()
            f.vertexFunction = lib.makeFunction(name: "klb_full_v")
            f.fragmentFunction = lib.makeFunction(name: "klb_full_f")
            f.colorAttachments[0].pixelFormat = color
            f.depthAttachmentPixelFormat = .depth32Float
            f.maxVertexAmplificationCount = amplification
            let g = f.colorAttachments[0]!
            g.isBlendingEnabled = true
            g.rgbBlendOperation = .add; g.alphaBlendOperation = .add
            // what is there, times the fragment's alpha: the picture fades towards nothing
            g.sourceRGBBlendFactor = .zero; g.sourceAlphaBlendFactor = .zero
            g.destinationRGBBlendFactor = .sourceAlpha; g.destinationAlphaBlendFactor = .sourceAlpha
            fadePipe = try device.makeRenderPipelineState(descriptor: f)
        } catch {
            NSLog("[room] play area: the pipelines failed to build: \(error) — nothing shown")
        }
        if stationary {
            NSLog("[room] play area: stationary, a circle of %.2f m round the player, shown as %@",
                  KLRoomGuard.stationaryRadius, mode == .wall ? "a wall" : "the room through the game")
        } else {
            NSLog("[room] play area: shown as %@, %d measured room(s), this game knows %d of them",
                  mode == .wall ? "a wall" : "the room through the game", rooms.count, map.count)
        }
    }

    /// The anchors visionOS hands back: one of ours means we are in that room.
    func watch(_ world: WorldTrackingProvider) {
        guard !watching, !stationary else { return }
        watching = true
        Task.detached { [weak self] in
            for await u in world.anchorUpdates {
                guard let self else { return }
                self.anchorUpdate(u.anchor, removed: u.event == .removed)
            }
        }
    }

    private func anchorUpdate(_ a: WorldAnchor, removed: Bool) {
        guard let roomID = anchorRoom[a.id], let room = rooms.first(where: { $0.id == roomID }) else { return }
        lock.lock()
        let current = active
        lock.unlock()
        if removed || !a.isTracked {
            if current?.room.id == roomID {
                NSLog("[room] play area: the room's anchor is not tracked now — the edge holds where it was")
            }
            return
        }
        // On the floor of this space, whose origin is on the floor: the anchor's own
        // height was 2 m off once (2026-10-17), and the edge is only ever measured
        // across the floor anyway.
        var m = a.originFromAnchorTransform
        if abs(m.columns.3.y) > 0.05 {
            NSLog("[room] play area: the room's anchor is %.2f m above this space's floor — put on the floor", m.columns.3.y)
        }
        m.columns.3.y = 0
        if var cur = current, cur.room.id == roomID {
            cur.rawFromRoom = m
            lock.lock(); active = cur; lock.unlock()
            return
        }
        let mask = room.playMask()
        var sdf = [Float](repeating: 0, count: mask.count)
        mask.withUnsafeBufferPointer { m in sdf.withUnsafeMutableBufferPointer { s in kl_room_sdf(room.grid, m.baseAddress, s.baseAddress) } }
        let (buf, count) = makeWalls(room, mask: mask)
        lock.lock()
        active = Active(room: room, rawFromRoom: m, sdf: sdf, wallVerts: buf, wallCount: count)
        lock.unlock()
        NSLog("[room] play area: in room %@ (%.1f m², %d wall vertices)", roomID.uuidString, room.areaM2, count)
    }

    /// The edge of the area as walls 2.6 m high, in the room's frame: x, y, z and
    /// the distance along the edge (for the grid).
    private func makeWalls(_ room: KLRoomRecord, mask: [UInt8]) -> (MTLBuffer?, Int) {
        var pts = [Float](repeating: 0, count: 2 * 40000)
        var lens = [Int32](repeating: 0, count: 256)
        let g = room.grid
        let loops = mask.withUnsafeBufferPointer { m in
            pts.withUnsafeMutableBufferPointer { p in
                lens.withUnsafeMutableBufferPointer { l in kl_room_contours(g, m.baseAddress, p.baseAddress, 40000, l.baseAddress, 256) }
            }
        }
        guard loops > 0 else { return (nil, 0) }
        var v: [SIMD4<Float>] = []
        var start = 0
        for k in 0..<Int(loops) {
            let n = Int(lens[k])
            var loop: [SIMD2<Float>] = []
            for i in 0..<n {
                let p = SIMD2<Float>(pts[2 * (start + i)], pts[2 * (start + i) + 1])
                if let last = loop.last, simd_distance(last, p) < 0.08 { continue }
                loop.append(p)
            }
            start += n
            guard loop.count >= 3 else { continue }
            var s: Float = 0
            for i in 0..<loop.count {
                let a = loop[i], b = loop[(i + 1) % loop.count]
                let l = simd_distance(a, b)
                let lo: Float = 0, hi: Float = 2.6
                let a0 = SIMD4<Float>(a.x, lo, a.y, s), a1 = SIMD4<Float>(a.x, hi, a.y, s)
                let b0 = SIMD4<Float>(b.x, lo, b.y, s + l), b1 = SIMD4<Float>(b.x, hi, b.y, s + l)
                v += [a0, b0, b1, a0, b1, a1]
                s += l
            }
        }
        guard !v.isEmpty else { return (nil, 0) }
        let buf = device.makeBuffer(bytes: v, length: v.count * MemoryLayout<SIMD4<Float>>.stride, options: .storageModeShared)
        return (buf, v.count)
    }

    /// The stationary circle round `head` (ARKit's frame), on the floor below it.
    /// The circle is a play area like a measured one — a mask on a grid, its
    /// distance field and its walls — in a frame of its own centred on the head.
    func centre(at head: SIMD3<Float>) {
        guard stationary else { return }
        if circle == nil {
            let r = KLRoomGuard.stationaryRadius
            let g = kl_room_grid_make(0, 0, r + 0.6, 0.05)
            var mask = [UInt8](repeating: 0, count: Int(kl_room_cells(g)))
            var ring: [Float] = []
            for k in 0..<96 {
                let a = Float(k) / 96 * 2 * .pi
                ring += [r * cos(a), r * sin(a)]
            }
            mask.withUnsafeMutableBufferPointer { m in
                ring.withUnsafeMutableBufferPointer { p in kl_room_fill_polygon(g, m.baseAddress, p.baseAddress, 96, 1) }
            }
            let room = KLRoomRecord(id: UUID(), created: Date(), updated: Date(), cell: g.cell, x0: g.x0, z0: g.z0,
                                    w: g.w, h: g.h, play: KLRoomRecord.pack(mask, bit: 1), obstacles: Data(), walls: [])
            var sdf = [Float](repeating: 0, count: mask.count)
            mask.withUnsafeBufferPointer { m in sdf.withUnsafeMutableBufferPointer { s in kl_room_sdf(g, m.baseAddress, s.baseAddress) } }
            let (buf, count) = makeWalls(room, mask: mask)
            circle = (room, sdf, buf, count)
        }
        guard let c = circle else { return }
        var m = matrix_identity_float4x4
        // the floor: the space's origin is on it
        m.columns.3 = SIMD4<Float>(head.x, 0, head.z, 1)
        lock.lock()
        active = Active(room: c.room, rawFromRoom: m, sdf: c.sdf, wallVerts: c.verts, wallCount: c.count)
        lock.unlock()
        NSLog("[room] play area: stationary circle centred at (%.2f, %.2f)", head.x, head.z)
    }

    private func sayOnce(_ key: String, _ s: String) {
        if said.insert(key).inserted { NSLog("[room] play area: %@", s) }
    }

    /// Each frame, inside the composite pass, after the game's picture and its panels.
    /// `views`: each view's projection and its view-from-world (world = the space
    /// the composite places things in, `worldFromRaw` from ARKit's own frame).
    /// `points`: head and hands in ARKit's frame.
    func encode(_ enc: MTLRenderCommandEncoder, views: [(projection: simd_float4x4, viewFromWorld: simd_float4x4)],
                worldFromRaw: simd_float4x4, points: [SIMD3<Float>], mixedSpace: Bool,
                gameShowsRoom: Bool, depthState: MTLDepthStencilState?) {
        lock.lock(); let a = active; lock.unlock()
        guard let a, !views.isEmpty else { return }
        if gameShowsRoom {
            if !lastShowsRoom { NSLog("[room] play area: the game shows the room (passthrough) — no wall, no fade") }
            lastShowsRoom = true
            lastNear = 0
            return
        }
        if lastShowsRoom { NSLog("[room] play area: passthrough ended — guarding the edge again") }
        lastShowsRoom = false
        let roomFromRaw = a.rawFromRoom.inverse
        // How near each point is, 0 (far enough inside) to 1 (on the edge or beyond).
        var near: [SIMD4<Float>] = []
        var most: Float = 0
        // The whole picture fading out reacts later than the wall: the owner, after
        // playing with it (2026-10-17), "reagiert zu schnell … erst später anfangen
        // auszublenden, wenn man näher zur Wand kommt". The head from 20 cm, a hand
        // only from 10 cm (a swing in Beat Saber should not dim the game), and
        // easing in: barely anything over the first half of that.
        var fade: Float = 0
        for (k, p) in points.prefix(3).enumerated() {
            let r = roomFromRaw * SIMD4<Float>(p, 1)
            let d = a.sdf.withUnsafeBufferPointer { s in kl_room_sdf_at(a.room.grid, s.baseAddress, r.x, r.z) }
            // Late, so that it does not get in the way (the owner, after the first
            // run: 70 cm was "viel zu früh"): the head from 35 cm, a hand — which
            // swings out further and more often — from 25 cm.
            let start: Float = k == 0 ? 0.35 : 0.25
            var t = max(0, min(1, (start - d) / start))
            t = t * t * (3 - 2 * t)
            most = max(most, t)
            near.append(SIMD4<Float>(r.x, r.y, r.z, t))
            let fadeStart: Float = k == 0 ? 0.20 : 0.10
            let f = max(0, min(1, (fadeStart - d) / fadeStart))
            fade = max(fade, f * f)
        }
        while near.count < 3 { near.append(.zero) }
        if most > 0.6 && lastNear <= 0.6 { NSLog("[room] play area: near the edge (%.2f)", most) }
        lastNear = most
        guard most > 0.002 else { return }
        if let depthState { enc.setDepthStencilState(depthState) }

        if mode == .room && mixedSpace, let fadePipe {
            sayOnce("room", "the game turns see-through near the edge")
            guard fade > 0.002 else { return }
            var u = SIMD4<Float>(0.9 * fade, 0, 0, 0)
            enc.setRenderPipelineState(fadePipe)
            enc.setFragmentBytes(&u, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            return
        }
        if mode == .room { sayOnce("full", "a full space shows no room — the wall is drawn instead") }
        guard let wallPipe, let buf = a.wallVerts, a.wallCount > 0 else { return }
        sayOnce("wall", "a wall at the edge")
        let worldFromRoom = worldFromRaw * a.rawFromRoom
        var mvp = views.map { $0.projection * $0.viewFromWorld * worldFromRoom }
        var frag = near + [SIMD4<Float>(0.45, 0.72, 1.0, 1), SIMD4<Float>(0.55, 0, 0, 0)]
        enc.setRenderPipelineState(wallPipe)
        enc.setVertexBuffer(buf, offset: 0, index: 0)
        enc.setVertexBytes(&mvp, length: mvp.count * MemoryLayout<simd_float4x4>.stride, index: 1)
        enc.setFragmentBytes(&frag, length: frag.count * MemoryLayout<SIMD4<Float>>.stride, index: 0)
        enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: a.wallCount)
    }

    private static let msl = """
    #include <metal_stdlib>
    using namespace metal;
    struct WOut { float4 pos [[position]]; float3 room; float s; };
    vertex WOut klb_wall_v(uint vid [[vertex_id]],
                           const device float4 *v [[buffer(0)]],
                           constant float4x4 *mvp [[buffer(1)]],
                           ushort amp [[amplification_id]]) {
        float4 p = v[vid];
        WOut o;
        o.pos = mvp[amp] * float4(p.xyz, 1.0);
        o.room = p.xyz;
        o.s = p.w;
        return o;
    }
    // u[0..2]: head and hands (room frame, w = how near), u[3]: colour, u[4].x: the most opaque it gets
    fragment float4 klb_wall_f(WOut in [[stage_in]], constant float4 *u [[buffer(0)]]) {
        float a = 0.0;
        for (int k = 0; k < 3; k++) {
            float t = u[k].w;
            if (t <= 0.0) continue;
            float d = distance(in.room, u[k].xyz);
            float reach = 0.35 + 0.9 * t;
            a = max(a, t * (1.0 - smoothstep(0.15, reach, d)));
        }
        // a 25 cm grid, its lines 1.5 cm wide
        float2 g = abs(fract(float2(in.s, in.room.y) / 0.25 + 0.5) - 0.5) * 0.25;
        float line = 1.0 - smoothstep(0.006, 0.014, min(g.x, g.y));
        float top = 1.0 - smoothstep(2.0, 2.6, in.room.y);
        float alpha = clamp(a * (0.22 + 0.78 * line) * top * u[4].x, 0.0, 1.0);
        return float4(u[3].rgb * alpha, alpha);
    }
    struct FOut { float4 pos [[position]]; };
    vertex FOut klb_full_v(uint vid [[vertex_id]]) {
        float2 p = float2(float((vid << 1) & 2u), float(vid & 2u));
        FOut o;
        o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
        return o;
    }
    fragment float4 klb_full_f(constant float4 *u [[buffer(0)]]) {
        return float4(0.0, 0.0, 0.0, 1.0 - u[0].x);
    }
    """
}
