//
//  KleptonControllerRecoveryPolicy.swift — the bar a take-back has to clear.
//
//  Split from the glue in KleptonControllerRecovery.swift on purpose: no
//  SwiftUI, no GameController, so it can be exercised in
//  tests/t_ctrlrecovery.swift without a headset.
//

/// One number and the decision that uses it.
struct ControllerRecoveryPolicy {
    /// States that count as "the stream came back". One is not proof — a
    /// half-closed stream delivered exactly one — and ten is far below the
    /// ~85/s a working pair produces. Same bar the local plugin used.
    var healthyStates = 10

    /// Did the stream show this much life since it was last sampled?
    func inputReturned(before: Int, after: Int) -> Bool {
        after - before >= healthyStates
    }
}
