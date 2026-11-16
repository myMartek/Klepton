// Makes the settings window's explanation pictures from one base scene.
// usage: swift makehelp.swift <base.jpg> <outdir>
import Foundation
import CoreImage
import CoreImage.CIFilterBuiltins
import ImageIO
import UniformTypeIdentifiers
import CoreGraphics

let args = CommandLine.arguments
let base = CIImage(contentsOf: URL(fileURLWithPath: args[1]))!
let outDir = URL(fileURLWithPath: args[2])
let W: CGFloat = 960, H: CGFloat = 600
let ctx = CIContext(options: [.workingColorSpace: CGColorSpace(name: CGColorSpace.sRGB)!])

func fit(_ img: CIImage, w: CGFloat, h: CGFloat) -> CIImage {
    let s = max(w / img.extent.width, h / img.extent.height)
    let f = CIFilter.lanczosScaleTransform()
    f.inputImage = img; f.scale = Float(s); f.aspectRatio = 1
    var o = f.outputImage!
    o = o.transformed(by: CGAffineTransform(translationX: -o.extent.minX, y: -o.extent.minY))
    let x = (o.extent.width - w) / 2, y = (o.extent.height - h) / 2
    return o.cropped(to: CGRect(x: x, y: y, width: w, height: h))
        .transformed(by: CGAffineTransform(translationX: -x, y: -y))
}
let scene = fit(base, w: W, h: H)

/// Down to `f` of the size and back up, linearly (a plain stretch) or nearest (stairs).
func resample(_ img: CIImage, _ f: CGFloat, nearest: Bool = false, lanczos: Bool = false) -> CIImage {
    let e = img.extent
    var small: CIImage
    if lanczos || !nearest {
        let d = CIFilter.lanczosScaleTransform(); d.inputImage = img; d.scale = Float(f); d.aspectRatio = 1
        small = d.outputImage!
    } else {
        small = img.samplingNearest().transformed(by: CGAffineTransform(scaleX: f, y: f))
    }
    let back = CGAffineTransform(scaleX: e.width / small.extent.width, y: e.height / small.extent.height)
    let up = (nearest ? small.samplingNearest() : small.samplingLinear()).transformed(by: back)
    return up.cropped(to: e)
}
func sharpen(_ img: CIImage, _ amount: Float) -> CIImage {
    let s = CIFilter.unsharpMask(); s.inputImage = img; s.radius = 1.6; s.intensity = amount
    return s.outputImage!.cropped(to: img.extent)
}
/// A crop of `r` (in scene coordinates, origin bottom-left) enlarged to the full picture.
func zoom(_ img: CIImage, _ r: CGRect, nearest: Bool = false) -> CIImage {
    let c = img.cropped(to: r).transformed(by: CGAffineTransform(translationX: -r.minX, y: -r.minY))
    let s = CGAffineTransform(scaleX: W / r.width, y: H / r.height)
    return (nearest ? c.samplingNearest() : c.samplingLinear()).transformed(by: s).cropped(to: CGRect(x: 0, y: 0, width: W, height: H))
}
/// `inner` inside, `outer` outside an ellipse that fades over `soft`.
func radialMix(inner: CIImage, outer: CIImage, r0: CGFloat, r1: CGFloat) -> CIImage {
    let g = CIFilter.radialGradient()
    g.center = CGPoint(x: W / 2, y: H / 2); g.radius0 = Float(r0); g.radius1 = Float(r1)
    g.color0 = CIColor(red: 1, green: 1, blue: 1); g.color1 = CIColor(red: 0, green: 0, blue: 0)
    let mask = g.outputImage!.cropped(to: scene.extent)
    let b = CIFilter.blendWithMask(); b.inputImage = inner; b.backgroundImage = outer; b.maskImage = mask
    return b.outputImage!.cropped(to: scene.extent)
}
func darken(_ img: CIImage, _ by: Float) -> CIImage {
    let c = CIFilter.colorControls(); c.inputImage = img; c.brightness = -by; c.saturation = 0.6
    return c.outputImage!
}

func save(_ img: CIImage, _ name: String) {
    let url = outDir.appendingPathComponent(name + ".jpg")
    let cg = ctx.createCGImage(img, from: CGRect(x: 0, y: 0, width: W, height: H))!
    let d = CGImageDestinationCreateWithURL(url as CFURL, UTType.jpeg.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(d, cg, [kCGImageDestinationLossyCompressionQuality: 0.82] as CFDictionary)
    CGImageDestinationFinalize(d)
    print(name)
}

// The scene itself.
save(scene, "KLHelp-scene")
// Resolution: a coarse picture against the fine one (crop of the sign and the towers).
let detail = CGRect(x: W * 0.215, y: H * 0.47, width: W * 0.40, height: H * 0.40)
save(zoom(resample(scene, 0.30), detail), "KLHelp-resolution-a")
save(zoom(scene, detail), "KLHelp-resolution-b")
// MetalFX: 75 % stretched against 75 % enlarged by a scaler (Lanczos + sharpening).
let small75 = resample(scene, 0.38)
save(zoom(small75, detail), "KLHelp-metalfx-a")
save(zoom(sharpen(resample(scene, 0.38, lanczos: true), 1.6), detail), "KLHelp-metalfx-b")
// Foveated rendering: the whole picture fine against a fine middle and a coarse edge.
let coarse = resample(scene, 0.10, nearest: false)
save(scene, "KLHelp-foveation-a")
save(radialMix(inner: scene, outer: coarse, r0: H * 0.22, r1: H * 0.50), "KLHelp-foveation-b")
// Edge smoothing: the railing's wires as stairs against smooth.
let rail = CGRect(x: W * 0.28, y: H * 0.16, width: W * 0.24, height: H * 0.24)
let stairsUp = resample(scene, 0.5, nearest: true)
save(zoom(stairsUp, rail, nearest: true), "KLHelp-msaa-a")
save(zoom(resample(scene, 0.5), rail), "KLHelp-msaa-b")
// Display output: the sign put on the display coarsely against finely.
let sign = CGRect(x: W * 0.20, y: H * 0.60, width: W * 0.42, height: H * 0.26)
save(zoom(resample(scene, 0.40), sign), "KLHelp-output-a")
save(zoom(scene, sign), "KLHelp-output-b")
// Field of view: the outermost edge dark against the whole display.
let black = CIImage(color: CIColor(red: 0, green: 0, blue: 0)).cropped(to: scene.extent)
save(radialMix(inner: scene, outer: black, r0: W * 0.40, r1: W * 0.47), "KLHelp-view-a")
save(scene, "KLHelp-view-b")
// Progressive immersion: the whole view against a portal over a dim, plain room.
let room: CIImage = { let b = CIFilter.gaussianBlur(); b.inputImage = scene.clampedToExtent(); b.radius = 30; let c = CIFilter.colorControls(); c.inputImage = b.outputImage!.cropped(to: scene.extent); c.saturation = 0.15; c.brightness = -0.12; return c.outputImage!.cropped(to: scene.extent) }()
let portalRect = CGRect(x: W * 0.17, y: H * 0.16, width: W * 0.66, height: H * 0.68)
let portalMask = CIImage(color: CIColor(red: 1, green: 1, blue: 1)).cropped(to: portalRect)
let blurMask = { () -> CIImage in
    let b = CIFilter.gaussianBlur(); b.inputImage = portalMask.clampedToExtent(); b.radius = 6
    return b.outputImage!.cropped(to: scene.extent)
}()
let roundMask: CIImage = {
    let m = CIFilter.roundedRectangleGenerator()
    m.extent = portalRect; m.radius = 40; m.color = CIColor(red: 1, green: 1, blue: 1)
    return m.outputImage!.composited(over: black)
}()
_ = blurMask
let pm = CIFilter.blendWithMask(); pm.inputImage = scene; pm.backgroundImage = room; pm.maskImage = roundMask
save(scene, "KLHelp-progressive-a")
save(pm.outputImage!.cropped(to: scene.extent), "KLHelp-progressive-b")

// Frame rate and frame generation: drawn strips of pictures over the scene.
func drawn(_ draw: (CGContext) -> Void) -> CIImage {
    let cs = CGColorSpace(name: CGColorSpace.sRGB)!
    let c = CGContext(data: nil, width: Int(W), height: Int(H), bitsPerComponent: 8, bytesPerRow: 0, space: cs,
                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    let bg = ctx.createCGImage(darken(scene, 0.30), from: scene.extent)!
    c.draw(bg, in: CGRect(x: 0, y: 0, width: W, height: H))
    draw(c)
    return CIImage(cgImage: c.makeImage()!)
}
let thumbCG = ctx.createCGImage(scene, from: scene.extent)!
func thumb(_ c: CGContext, _ r: CGRect, dash: Bool = false, alpha: CGFloat = 1, shift: CGFloat = 0) {
    c.saveGState()
    let path = CGPath(roundedRect: r, cornerWidth: 10, cornerHeight: 10, transform: nil)
    c.addPath(path); c.clip()
    c.setAlpha(alpha)
    c.draw(thumbCG, in: r.offsetBy(dx: shift, dy: 0).insetBy(dx: -r.width * 0.15, dy: -r.height * 0.15))
    c.restoreGState()
    c.saveGState()
    c.addPath(path)
    c.setStrokeColor(CGColor(srgbRed: 1, green: 1, blue: 1, alpha: dash ? 0.95 : 0.8))
    c.setLineWidth(dash ? 4 : 3)
    if dash { c.setLineDash(phase: 0, lengths: [12, 8]) }
    c.strokePath()
    c.restoreGState()
}
// Frame rate: a row of pictures, twice as many above as below.
save(drawn { c in
    let tw: CGFloat = 76, th: CGFloat = 48
    for i in 0..<10 { thumb(c, CGRect(x: 40 + CGFloat(i) * 89, y: 360, width: tw, height: th), shift: CGFloat(i) * -6) }
    for i in 0..<5 { thumb(c, CGRect(x: 40 + CGFloat(i) * 178, y: 190, width: tw, height: th), shift: CGFloat(i) * -12) }
    c.setFillColor(CGColor(srgbRed: 1, green: 1, blue: 1, alpha: 0.85))
    c.fill(CGRect(x: 40, y: 330, width: 880, height: 4))
    c.fill(CGRect(x: 40, y: 160, width: 880, height: 4))
}, "KLHelp-framerate")
// Frame generation: two pictures of the game and a made one between them.
save(drawn { c in
    let tw: CGFloat = 250, th: CGFloat = 160
    thumb(c, CGRect(x: 50, y: 220, width: tw, height: th), shift: 0)
    thumb(c, CGRect(x: 355, y: 220, width: tw, height: th), dash: true, alpha: 0.9, shift: -14)
    thumb(c, CGRect(x: 660, y: 220, width: tw, height: th), shift: -28)
    c.setStrokeColor(CGColor(srgbRed: 1, green: 1, blue: 1, alpha: 0.9)); c.setLineWidth(4)
    for x in [310.0, 615.0] {
        c.move(to: CGPoint(x: x, y: 300)); c.addLine(to: CGPoint(x: x + 36, y: 300)); c.strokePath()
        c.move(to: CGPoint(x: x + 26, y: 310)); c.addLine(to: CGPoint(x: x + 36, y: 300)); c.addLine(to: CGPoint(x: x + 26, y: 290)); c.strokePath()
    }
}, "KLHelp-framegen")
