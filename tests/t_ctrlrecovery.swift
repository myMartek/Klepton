// Exercise the bar the take-back measures against: what counts as "the stream
// came back".
@main
struct ControllerRecoveryTests {
    static func expect(_ condition: @autoclosure () -> Bool, _ description: String) {
        precondition(condition(), description)
    }

    static func main() {
        let p = ControllerRecoveryPolicy()

        // The measured cases, 2026-11-06: a double-click peek returning to the
        // game delivered 46 states within 0.8 s; Control Center closing left
        // the stream at 0 for the next nine seconds.
        expect(p.inputReturned(before: 1000, after: 1046),
               "A return that delivers 46 states is alive")
        expect(!p.inputReturned(before: 1000, after: 1000),
               "A return that delivers nothing is dead")

        // The bar: ten states. One is not proof — a half-closed stream
        // delivered exactly one — and ten is far below the ~85/s a working
        // pair produces.
        expect(!p.inputReturned(before: 1000, after: 1009),
               "Nine states are not proof of a working stream")
        expect(p.inputReturned(before: 1000, after: 1010),
               "Ten states are")

        // A dead stream that produced a few stragglers is still dead.
        expect(!p.inputReturned(before: 1000, after: 1001),
               "One straggler must not count")

        print("t_ctrlrecovery: ok")
    }
}
