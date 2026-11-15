import Metal

// MetalFX, for the visionOS SIMULATOR, where the framework does not exist.
//
// The compositor's upscaling (KleptonCompositor.swift, KleptonTuning.swift)
// imports MetalFX, and the simulator's SDK has no such module, so this tree's
// app has not built for the simulator since that was added. Every other target
// does not mind: none of them is run there any more. Minecraft is the one game
// whose libraries exist for the simulator (a build of vivecraft-visionos makes
// both slices), and running it there is the only way to look at this target
// without a headset — so, for a simulator build of THIS target only, the few
// names the compositor uses are declared here, as a device without a scaler:
// `supportsDevice` says no, no scaler is ever made, and the compositor takes
// its ordinary linear path ("MetalFX: off").
//
// Nothing of this is in a device build (the whole file is inside
// `#if targetEnvironment(simulator)`), where the two files import the real
// framework (`#if canImport(MetalFX)`), and it is in no other target's project.
#if targetEnvironment(simulator)
enum MTLFXSpatialScalerColorProcessingMode { case perceptual, linear, hdr }

protocol MTLFXSpatialScaler: AnyObject {
    var colorTextureUsage: MTLTextureUsage { get }
    var outputTextureUsage: MTLTextureUsage { get }
    var colorTexture: MTLTexture? { get set }
    var outputTexture: MTLTexture? { get set }
    var inputContentWidth: Int { get set }
    var inputContentHeight: Int { get set }
    func encode(commandBuffer: MTLCommandBuffer)
}

final class MTLFXSpatialScalerDescriptor {
    var colorTextureFormat: MTLPixelFormat = .invalid
    var outputTextureFormat: MTLPixelFormat = .invalid
    var inputWidth = 0, inputHeight = 0, outputWidth = 0, outputHeight = 0
    var colorProcessingMode: MTLFXSpatialScalerColorProcessingMode = .perceptual
    static func supportsDevice(_ device: MTLDevice) -> Bool { false }
    func makeSpatialScaler(device: MTLDevice) -> (any MTLFXSpatialScaler)? { nil }
}
#endif
