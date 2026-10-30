import simd

// Pinch release is a tap only if the same gesture never became a drag.
struct KLBrinkSelectionGesture {
    enum Action { case tap; case swipe(SIMD2<Float>) }
    private var last = SIMD2<Float>.zero
    private var dragged = false
    mutating func update(translation: SIMD2<Float>, ended: Bool, cancelled: Bool) -> Action? {
        guard !cancelled, translation.x.isFinite, translation.y.isFinite else { return nil }
        let step = translation - last
        if simd_length(step) >= 0.035 {
            last = translation; dragged = true
            return .swipe(step)
        }
        return ended && !dragged ? .tap : nil
    }
}
