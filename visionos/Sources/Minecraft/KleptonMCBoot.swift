import SwiftUI
import Foundation

// The Minecraft app's window and the life of its process: what KleptonApp.swift's
// BootView and Lifecycle are for every other target, for the Mach-O guest.
//
// Ported from KleptonApp.swift of rebelancap's vivecraft-visionos 1.0.3, where
// both lived in the shared file. They are apart here because this tree's
// BootView and Lifecycle answer a different question: an installed game there
// starts by itself, shows a start window that goes away once the picture is up,
// boots ONCE per process and leaves the moment its picture is gone. Minecraft's
// window is a launcher (accounts, world, play mode), and its game can end and
// hand the player back to it. So this target gets its own window (MCBootView)
// and its own rule for leaving (MCLifecycle), and KleptonApp.swift hands both
// over behind `#if KL_MACHO_GUEST`.
//
// What it shares with the other games, on the owner's word ("Das Fenster soll
// auch geschlossen werden, wenn man das Spiel dann startet"): once the
// immersive game's picture is up the launcher window CLOSES, as a start window
// does, where the reference build left it open in front of the game saying
// "Playing". Three things follow from that, and they are BootSession's:
//
//   what closes it       the compositor's first frame that holds the game's
//                        picture (KleptonCompositor.swift tells pictureIsUp),
//                        with the same dismissWindow() as BootView's
//                        (KL_KEEP_START_WINDOW=1 keeps it, as there). Later
//                        than BootView, which goes at `.opened`: this game
//                        starts its Java side and loads its resources AFTER
//                        the space is open (12 s in the simulator), the space
//                        is empty for that long, and the launcher's progress
//                        line is all there is to look at. Never in windowed
//                        play: there the window is the game.
//   what keeps the sound nothing special, as for every game here: the immersive
//                        space is a scene of the app, so closing the window by
//                        hand of the app leaves the app active and its audio
//                        session with it. Should the system move the phase all
//                        the same (rebelancap saw `.inactive`, then
//                        `.background`, when a PERSON closed his window), the
//                        layer's state outranks the phase for this game
//                        (KleptonScenePolicy): a running layer is heard and is
//                        not exited under.
//   what brings it back  the game's end. With no window there is no view to
//                        notice it, so the watch is the session's, not a
//                        view's: it dismisses the space and opens the launcher
//                        window again (the window actions are kept from the
//                        last live view, as KLRun keeps one for the keyboard).
//                        A Digital Crown press or Home while the game runs is
//                        NOT that: it takes the picture away for good, and then
//                        the app leaves, as every game of this tree does.
//
// What it takes from this tree, so that Minecraft gets the same picture settings
// as every other game: the start window's settings are applied on the way into a
// boot exactly where BootView applies them (KleptonRendering, the Test switches,
// the hand settings), the kind of immersive space follows "Frame rate"
// (Immersive.mixed), and the performance panel is the tree's own.

/// What backgrounding means for this process. The decision is
/// KleptonScenePolicy.lifetime (MCAudioSession.swift); this carries it out.
///
/// Every other target exits on `.background` (and on a crown press) at once.
/// **Not Minecraft's world while its layer is still ours.** Closing the launcher
/// window while the world plays takes the app's phase to `.background` with the
/// space still presenting (seen on the headset with the reference build), and
/// exiting there killed a game the wearer was in. So for this guest the exit
/// waits while the immersive layer is running or resumably paused, and is taken,
/// on the original terms, the moment the layer ends or the game's run does.
enum MCLifecycle {
    static func scenePhaseChanged(to phase: ScenePhase) {
        NSLog("[app] scene phase -> \(phase)")
        // The phase is one of the audio's two inputs, not its decision: while an
        // immersive layer exists its state decides (a crown press or Home pauses
        // or ends it, and that silences); with none, the phase does. See
        // KleptonScenePolicy. `.active` is also the audio's cue to rebuild.
        switch phase {
        case .active:     MCAudioSession.scenePhaseChanged(to: .active)
        case .background: MCAudioSession.scenePhaseChanged(to: .background)
        default:          MCAudioSession.scenePhaseChanged(to: .inactive)
        }
        if phase == .background, !exitOnBackground {
            NSLog("[app] backgrounded; KL_EXIT_ON_BACKGROUND=0, staying alive")
        }
        reconsider("scene phase \(phase)", grace: false)
    }

    private static var exitOnBackground: Bool { klEnvOn("KL_EXIT_ON_BACKGROUND", default: true) }

    // The game's run count when the keep-alive began (nil = not keeping alive),
    // the poll that watches for the game ending with no window left to see it,
    // and a deferred exit already in flight. Main thread only.
    nonisolated(unsafe) private static var keptAtRuns: Int32?
    nonisolated(unsafe) private static var keepAlivePoll: Timer?
    nonisolated(unsafe) private static var exitPending = false
    /// The immersive space this process opened and has not given back itself
    /// (BootSession keeps it in step). While it is set, a layer that ENDS was
    /// ended by the system — a Digital Crown press, Home — and not by the game
    /// quitting, which gives the space back before it dismisses it.
    nonisolated(unsafe) static var holdsSpace = false

    /// Re-decide the process's lifetime. Called on every scene phase, on every
    /// immersive layer change (MCAudioSession.layerChanged — the half that still
    /// fires once the launcher window, and the phase handler under it, are
    /// gone), and once a second while kept alive. Main thread.
    ///
    /// `grace` is for a layer that has just ended: its render thread is still
    /// stopping and joining the guest, so the exit waits a bounded 2 s and is
    /// then re-decided rather than taken blind.
    static func reconsider(_ why: String, grace: Bool) {
        // The game's run has ended since we started keeping it alive: Quit Game
        // with the launcher window closed. The window's poll would normally
        // answer that, and it went with the window. Its own Quitting… time is
        // bounded in C (KL_MC_QUIT_GRACE_MS), so waiting for the count is too.
        let gameOver = keptAtRuns.map { kl_app_macho_runs() > $0 } == true
        var decision = KleptonScenePolicy.lifetime(
            phase: MCAudioSession.phase, layer: MCAudioSession.layer, nativeGuest: true,
            exitOnBackground: exitOnBackground, gameOver: gameOver)
        // The picture was taken away while the game runs (crown press, Home):
        // the layer is invalid, the launcher window was closed when the game
        // started, and what is left is a game nobody can see or reach. Every
        // game of this tree leaves at that point, whatever the phase says and
        // however long the system takes to say it (KleptonApp.swift, Lifecycle);
        // so does this one, after the same short grace a layer end gets here.
        if MCAudioSession.layer == .ended, holdsSpace, exitOnBackground { decision = .exit }
        switch decision {
        case .carryOn:
            if keptAtRuns != nil { NSLog("[app] keep-alive over (\(why)) — \(MCAudioSession.phase.rawValue)") }
            keepAlivePoll?.invalidate(); keepAlivePoll = nil
            keptAtRuns = nil
        case .keepForImmersive:
            guard keptAtRuns == nil else { return }
            keptAtRuns = kl_app_macho_runs()
            NSLog("[app] backgrounded (\(why)) but the immersive layer is "
                  + "\(MCAudioSession.layer.rawValue) — NOT exiting; the exit follows the layer or the game ending")
            // With grace: whatever it finds (the game's run over, the layer
            // ended) takes the same bounded 2 s as a layer end, and cannot cut
            // short one already running.
            keepAlivePoll = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { _ in
                reconsider("keep-alive poll", grace: true)
            }
        case .exit:
            if grace {
                guard !exitPending else { return }
                exitPending = true
                NSLog("[app] \(why) — leaving in 2 s unless the space comes back")
                DispatchQueue.main.asyncAfter(deadline: .now() + 2) {
                    exitPending = false
                    reconsider("\(why), after grace", grace: false)
                }
                return
            }
            NSLog("[app] leaving (KL_EXIT_ON_BACKGROUND; \(why); phase \(MCAudioSession.phase.rawValue), "
                  + "layer \(MCAudioSession.layer.rawValue)\(holdsSpace ? ", the picture was taken away" : ""))")
            // The last word on whether the run made a sound, then the tree's own
            // leaving (what a run leaves behind for the next start window, the
            // flush, the exit).
            kl_audio_heartbeat(stderr)
            Lifecycle.leave()
        }
    }
}

/// The launcher's boot state, one per PROCESS rather than one per window.
///
/// The window can be closed and opened again while a game runs (MCLifecycle
/// keeps the process for the immersive layer), and a fresh view would read
/// running=false, finished=false, no space opened, no game answered: Play would
/// be enabled again over a game being played, Quit Game could not dismiss a
/// space "this window" never opened, and an OLD game's end would be answered by
/// dismissing the CURRENT game's space.
///
/// Main actor, like MCLauncherModel and MSAAccounts beside it. Every write is
/// MCBootView's, on the main thread — the boot thread's results arrive through
/// DispatchQueue.main.async.
@MainActor
final class BootSession: ObservableObject {
    static let shared = BootSession()
    /// Internal rather than private so a test can start from a fresh one.
    init() {}

    @Published var log = ""
    @Published var status = "idle"
    @Published var running = false
    @Published var finished = false
    @Published var succeeded = false

    /// The immersive space this process asked for and has not given back.
    private(set) var openedSpace = false
    /// How many of the game's returns have been answered
    /// (kl_app_macho_runs counts them in C).
    private(set) var gamesAnswered: Int32 = 0

    /// No launch while one is starting (`running`) or has happened and not ended
    /// (`finished`). gameEnded() clears both, which is what lets a quit to the
    /// launcher run again; a failed boot leaves `finished`, which was already
    /// final.
    var mayLaunch: Bool { !running && !finished }

    /// Claim the space before asking for it, so two paths (or two windows)
    /// cannot both open it.
    func claimSpace() -> Bool {
        guard !openedSpace else { return false }
        openedSpace = true
        return true
    }

    /// The window and space actions of the last live launcher view. Kept here
    /// because the launcher window is closed while the immersive game plays,
    /// and what has to happen at the game's end — the space dismissed, the
    /// launcher back — needs them when no view is alive to lend its own.
    struct SceneActions {
        let openWindow: OpenWindowAction
        let dismissWindow: DismissWindowAction
        let dismissImmersiveSpace: DismissImmersiveSpaceAction
    }
    private var actions: SceneActions?
    /// The launcher window was closed by the app when the game's picture came
    /// up, and has not been opened again since.
    private(set) var launcherClosed = false
    /// The space is open and the game has not shown a picture in it yet: it is
    /// starting its Java side and loading its resources. The launcher shows
    /// the load until this clears (KleptonMCLauncher.swift, progressStrip).
    @Published private(set) var awaitingPicture = false
    private var watch: Task<Void, Never>?

    /// A launcher view is on screen: its actions are the ones to use from now
    /// on, and the watch for the game's end runs for the rest of the process.
    func attach(_ a: SceneActions) {
        actions = a
        launcherClosed = false
        guard watch == nil else { return }
        watch = Task { @MainActor in
            while !Task.isCancelled {
                await self.tick()
                try? await Task.sleep(for: .milliseconds(200))
            }
        }
    }

    /// openImmersiveSpace answered `.opened`: from here a layer that ends
    /// without the game having quit was taken away by the system.
    func spaceIsUp() {
        MCLifecycle.holdsSpace = openedSpace
        KLRun.shared.up = true
        awaitingPicture = openedSpace
    }

    /// The game's picture is up — the compositor has put the first frame of
    /// this run's game into a drawable (it says so once per layer): the
    /// launcher has done its job, as a start window has. Not when asked to
    /// stay (KL_KEEP_START_WINDOW=1, the tree's own switch for that), and not
    /// for a game that is already on its way out.
    func pictureIsUp() {
        awaitingPicture = false
        guard openedSpace, !launcherClosed, !MCLauncherModel.shared.quitting,
              !klEnvOn("KL_KEEP_START_WINDOW", default: false), let a = actions else { return }
        launcherClosed = true
        NSLog("[launcher] the game's picture is up — closing the launcher window (it comes back when the game ends)")
        fputs("[launcher] the game's picture is up — launcher window closed\n", stderr)
        a.dismissWindow()
    }

    /// Five times a second, for the whole process: the two things a game does
    /// to its launcher. Polled rather than pushed — both are C state written by
    /// a guest thread.
    private func tick() async {
        let launcher = MCLauncherModel.shared
        // The game closed its window (Quit Game): its picture is gone, so the
        // space goes NOW rather than when its main returns (which can take
        // until the 15 s watchdog). The launcher reads Quitting… until the run
        // ends (KL_MC_QUIT_GRACE_MS in the door).
        if !launcher.quitting, kl_app_macho_closing() != 0 {
            launcher.quitting = true
            NSLog("[launcher] the game closed its window — dismissing the space, waiting for the run to end")
            fputs("[launcher] the game closed its window — dismissing the space\n", stderr)
            await showLauncher()
            await giveSpaceBack()
        }
        if claimGameEnd(runs: kl_app_macho_runs()) {
            await gameEnded()
        }
    }

    /// The launcher window again, if it was closed for the game. BEFORE the
    /// space is dismissed, never after: an app whose last scene has gone is
    /// sent to the background, and this one would then be taken for one whose
    /// picture was taken away (the same reason the tree shows a small window
    /// while a picture moves between spaces, KleptonRealityPresent.swift).
    private func showLauncher() async {
        guard launcherClosed, let a = actions else { return }
        launcherClosed = false
        NSLog("[launcher] opening the launcher window again")
        fputs("[launcher] launcher window opened again\n", stderr)
        a.openWindow(id: SurrealAimGuide.launcherID)
        // A moment for it to be a scene of the app before the space goes.
        try? await Task.sleep(for: .milliseconds(300))
    }

    /// Give the space back and dismiss it; the flag first, so that the layer
    /// ending under this is the game's own end and not a picture taken away.
    private func giveSpaceBack() async {
        awaitingPicture = false
        guard releaseSpace() else { return }
        MCLifecycle.holdsSpace = false
        await actions?.dismissImmersiveSpace()
    }

    /// The game returned from its main: Quit Game. The immersive space goes
    /// away, the launcher is back to idle — accounts, play target and settings
    /// editable, its Play runs the game again in this process
    /// (kl_app_macho_rerun) — and, if it was closed for the game, its window
    /// has been opened again (showLauncher). Where a second run cannot share the process
    /// (kl_app_macho_rerun_ok: the isolate could not be torn down, too little
    /// memory, KL_MC_RERUN=0) the app leaves instead, so the next tap on the
    /// icon lands on the launcher rather than on a window that cannot play.
    private func gameEnded() async {
        let launcher = MCLauncherModel.shared
        let rc = kl_app_macho_rc()
        let rerun = kl_app_macho_rerun_ok() != 0
        if rerun { await showLauncher() }
        await giveSpaceBack()
        if !rerun {
            NSLog("[launcher] game ended (rc=\(rc)) — no re-run in this process; exiting so the next launch starts at the launcher")
            fputs("[launcher] game ended (rc=\(rc)) — no re-run in this process; exiting\n", stderr)
            Lifecycle.leave()
        }
        NSLog("[launcher] game ended (rc=\(rc)) — back to the launcher")
        fputs("[launcher] game ended (rc=\(rc)) — back to the launcher\n", stderr)
        fflush(nil)
        status = "game ended (rc=\(rc))"
        running = false; finished = false; succeeded = false
        launcher.quitting = false
        launcher.details = false
        launcher.gameEnded()
        KLRun.shared.up = false
    }

    /// What openImmersiveSpace answered. Anything but `.opened` leaves no space
    /// to dismiss, so the claim is released — the next Play may claim again.
    func spaceOpened(_ opened: Bool) {
        if !opened { openedSpace = false; MCLifecycle.holdsSpace = false; awaitingPicture = false }
    }

    /// Give the space back; true if there was one to dismiss.
    func releaseSpace() -> Bool {
        guard openedSpace else { return false }
        openedSpace = false
        return true
    }

    /// True once per finished run. Taken before the await that answers it, so a
    /// second window's poll cannot answer the same end.
    func claimGameEnd(runs: Int32) -> Bool {
        guard runs > gamesAnswered else { return false }
        gamesAnswered = runs
        return true
    }
}

/// The Minecraft app's window: the launcher, or the game's own picture while it
/// plays in the window.
struct MCBootView: View {
    // Process state lives in BootSession (see there); these forward to it so
    // every read and write below keeps its name and its shape.
    @ObservedObject private var session = BootSession.shared
    private var log: String {
        get { session.log } nonmutating set { session.log = newValue } }
    private var status: String {
        get { session.status } nonmutating set { session.status = newValue } }
    private var running: Bool {
        get { session.running } nonmutating set { session.running = newValue } }
    private var finished: Bool {
        get { session.finished } nonmutating set { session.finished = newValue } }
    private var succeeded: Bool {
        get { session.succeeded } nonmutating set { session.succeeded = newValue } }
    // The game's flat picture. Polled rather than pushed: it is C state written
    // by a guest thread, and a callback out of one into SwiftUI would be a
    // main-actor hop from a thread that is mid-frame. showShell stays per
    // window: it is re-derived from C on every poll.
    @State private var showShell = false
    @ObservedObject private var launcher = MCLauncherModel.shared
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace
    @Environment(\.openWindow) private var openWindow
    @Environment(\.dismissWindow) private var dismissWindow

    var body: some View {
        Group {
            if showShell { ShellWindow() }
            // Minecraft always opens its own launcher. The runtime's report is
            // in the log the launcher exports.
            else {
                MCLauncherView(running: running, finished: finished, succeeded: succeeded, status: status,
                               onLaunch: { launch() },
                               onDetails: { NSLog("[launcher] diagnostics are available as an export") })
            }
        }
        .onAppear {
            session.attach(.init(openWindow: openWindow, dismissWindow: dismissWindow,
                                 dismissImmersiveSpace: dismissImmersiveSpace))
        }
        .task { await watchPresentation() }
        .task { await loadAccounts() }
    }

    /// **Minecraft never starts by itself.** Its play mode is a choice the
    /// player makes with the Play button, every session; a remembered Immersive
    /// choice must not start a game on its own, and neither KL_AUTOBOOT=1 nor
    /// KL_LAUNCHER=0 may answer for the player. A deliberate scripted test still
    /// exists: KL_LAUNCHER_SCRIPT's explicit `mode=…;launch` steps go through
    /// the same Play action.
    ///
    /// The ownership gate reads the account store, so the list (and whether the
    /// device is unlocked) is loaded here, before the button can decide.
    private func loadAccounts() async {
        await MSAAccounts.shared.reload()
        if klEnvOn("KL_AUTOBOOT", default: false) || MCLauncher.autoboot {
            NSLog("[launcher] KL_AUTOBOOT is ignored for Minecraft: the game starts only from the Play action")
        }
    }

    /// The idempotence guard, and it sits BEFORE prepareForLaunch and the
    /// profile seeding because those are the mutations (launch arguments, game
    /// dir, env) that must not be redone under a game that is running. Ahead of
    /// the ownership gate, too, so an ignored press records no refusal.
    private func launchMayProceed(_ who: String) -> Bool {
        guard session.mayLaunch else {
            NSLog("[launcher] \(who) ignored — a game is already launching or running in this process "
                  + "(running=\(running) finished=\(finished))")
            return false
        }
        return true
    }

    /// A person pressed Play: the launcher's choices first.
    private func launch() {
        guard launchMayProceed("Play") else { return }
        guard OwnershipGate.allowsLaunch() else {
            OwnershipGate.recordRefusal()
            return
        }
        if MCLauncher.enabled { MCLauncherModel.shared.prepareForLaunch(applyPlay: true) }
        else {                                                                // KL_LAUNCHER=0: same game dirs,
            MCProfiles.prepareForLaunch()                                     // same play-mode rule
            MCPlayMode.apply(MCLauncherModel.shared.playMode)
        }
        boot()
    }

    /// Which of the two things the window shows. (The game's end is the
    /// session's to notice: BootSession.tick, which runs with or without a
    /// window.)
    private func watchPresentation() async {
        while !Task.isCancelled {
            let mono = kl_present_mode_now() == KL_PRESENT_MONO
            // The game's flat picture (windowed play) is shown only while ITS
            // run is on: from Play until the game returns, and not while it is
            // closing (Quitting… belongs to the launcher). kl_present is not
            // read back for whether the mode resets after the game's window
            // closes, so this must not depend on that; a window opened again
            // mid-game shows the game, after the game it shows the launcher.
            let shell = mono && session.running && !launcher.quitting
            if shell != showShell { showShell = shell }

            try? await Task.sleep(for: .milliseconds(200))
        }
    }

    /// Whether openImmersiveSpace actually gave us the space. `.userCancelled`,
    /// `.error` and anything a later SDK adds all mean there is none to hold.
    private static func opened(_ r: OpenImmersiveSpaceAction.Result) -> Bool {
        if case .opened = r { return true }
        return false
    }

    /// What this tree's start window does on the way into a boot, for the same
    /// settings: the Test switches first (none of these overwrites a value that
    /// is set, so a switch wins over the picture setting it retunes), then the
    /// picture settings the runtime reads from its environment, then the two
    /// that belong to the hands. Before kl_app_configure, which is where the
    /// runtime starts reading.
    private func applyStartSettings() {
        KleptonTestSwitches.applyEnvironment()
        KleptonRendering.shared.applyEnvironment()
        KleptonDiagnostics.applyEnvironment()
        KleptonHandPinch.applyEnvironment()
    }

    /// ...and after it, once the target is resolved: the kind of space the game
    /// opens ("Frame rate": the mixed space with black behind the picture at up
    /// to 100 Hz, or the full one at up to 120), the crown switch, and the
    /// settings the runtime takes as defaults (foveated rendering, resolution,
    /// the eye size).
    private func publishStartSettings() {
        DispatchQueue.main.async {
            KleptonChroma.shared.immersionFull = !Immersive.mixed
            Immersive.mixedBlackAtBoot = Immersive.mixedBlack
            Immersive.mixedAtBoot = Immersive.mixed
            let crown = KleptonRendering.shared.crownImmersion && !KleptonRendering.targetIsMixedReality
            KleptonRendering.shared.crownImmersionActive = crown
            KleptonImmersion.enabled = crown
        }
        KleptonRendering.shared.apply()
    }

    private func boot() {
        // Guarded on this side too, not only in the door: the first boot maps
        // the image and brings ANGLE up once per process, and the obvious next
        // thing to do with a finished run is press Play again.
        guard !running, !finished else { return }
        // Before the image is mapped and before the immersive space opens. The
        // door checks again in macho_run, which is the call every path (this
        // one, a re-run, KL_LAUNCHER=0, a script) makes before the game's main.
        guard OwnershipGate.allowsLaunch() else {
            OwnershipGate.recordRefusal()
            return
        }
        // Play after a Quit Game: the process already booted, so this is a
        // re-run, not a boot — the env file and the command line are rebuilt
        // and the space opens again.
        if kl_app_macho_runs() > 0 {
            rerun()
            return
        }
        // The play mode this process will run: the launcher's choice, exported
        // to KL_IMMERSIVE before configure reads it and fixed from here on.
        MCPlayMode.commit(MCLauncherModel.shared.playMode)
        let windowed = !Immersive.wanted
        running = true; log = ""
        KLRun.shared.started = true
        applyStartSettings()

        // Off the main thread: the boot maps a 211 MB image and the windowed
        // game then runs on this very thread until it quits.
        Thread.detachNewThread {
            let rc = kl_app_configure(Paths.resources, Paths.container)
            if rc != 0 {
                // To the system log as well as the window: this is the one
                // failure that happens before there is a klepton-boot.log.
                NSLog("[app] configure failed: \(String(cString: kl_app_status()))")
                DispatchQueue.main.async {
                    status = String(cString: kl_app_status())
                    log = "configure failed: \(status)"
                    running = false; finished = true; succeeded = false
                }
                return
            }
            publishStartSettings()

            let logPath = String(cString: kl_app_log_path())
            let poll = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { _ in
                let t = KleptonLogTail.read(logPath)
                DispatchQueue.main.async { if log != t { log = t } }
            }
            RunLoop.current.add(poll, forMode: .common)

            var result = kl_app_boot()

            // The audio session. AFTER kl_app_boot, because boot is what opens
            // the log — anything printed earlier goes where nobody can read it
            // on a headset. BEFORE the game, because OpenAL opens its device at
            // the game's start and a device opened against an inactive session
            // starts successfully and produces nothing.
            MCAudioSession.start()

            // Windowed play: kl_app_lifecycle is the game's whole run (the
            // door's macho_run on this thread, blocking until Quit Game); its
            // picture arrives through kl_mono into ShellWindow, and there is no
            // immersive space. Immersive play is started by the compositor.
            if result == 0, windowed {
                result = kl_app_lifecycle(1)
            }
            poll.invalidate()

            let text = KleptonLogTail.read(logPath)
            DispatchQueue.main.async {
                log = text
                if windowed, result == 0 {
                    // The windowed game has ENDED: idle, not Playing.
                    // gameEnded() answers the run too (claimGameEnd), in either
                    // order; both leave the launcher idle with the same words.
                    status = "game ended (rc=\(kl_app_macho_rc()))"
                    running = false; finished = false; succeeded = false
                    return
                }
                status = String(cString: kl_app_status())
                succeeded = (result == 0)
                running = false; finished = true
                // The space opens only after boot has succeeded: the
                // compositor's first act is to start the game.
                if result == 0, !windowed { openSpace("") }
            }
        }
    }

    /// Open the game's immersive space, once claimed. `.error` is retried as
    /// the tree's own start window retries it (it is not always final: a launch
    /// that follows a reinstall over a running instance got it once, and the
    /// next attempt opened); `.userCancelled` is an answer and is not asked
    /// again. Anything but `.opened` gives the claim back.
    private func openSpace(_ what: String) {
        guard session.claimSpace() else { return }
        Task { @MainActor in
            var got = false
            for attempt in 1...5 {
                let r = await openImmersiveSpace(id: Immersive.id)
                NSLog("[cp] openImmersiveSpace\(what) -> \(r)" + (attempt > 1 ? " (attempt \(attempt))" : ""))
                if case .opened = r {
                    got = true
                    // The controllers' take-back prompt needs a way to raise
                    // its window (KleptonControllerRecovery.swift).
                    KLControllerRecovery.capture(openWindow: openWindow, dismissWindow: dismissWindow)
                }
                guard case .error = r, attempt < 5 else { break }
                try? await Task.sleep(nanoseconds: 2_000_000_000)
            }
            session.spaceOpened(got)
            // The launcher window stays for now: the space is empty until the
            // game has loaded. It closes at the game's first picture
            // (BootSession.pictureIsUp, from the compositor).
            if got { session.spaceIsUp() }
        }
    }

    private func rerun() {
        running = true
        // The process's committed mode (MCPlayMode.applied) — a re-run cannot
        // change it: the stand-in's VR switch and the window capture were fixed
        // by the first configure/boot. prepareForLaunch exported the same value
        // again, so the door's env refresh and vrEnabled edit agree.
        let windowed = !Immersive.wanted
        applyStartSettings()
        Thread.detachNewThread {
            // Off the main thread: it reads files and asks for the Microsoft
            // session (a network round trip).
            let rc = kl_app_macho_rerun()
            DispatchQueue.main.async {
                status = String(cString: kl_app_status())
                if rc != 0 {
                    // Not finished=true: that would lock the launcher with no
                    // way to try again. Idle, with the reason in the footer.
                    NSLog("[launcher] re-run refused: \(status)")
                    running = false; finished = false; succeeded = false
                    return
                }
                // The render loop's end silenced the session; this run needs it.
                MCAudioSession.rearm()
                if windowed {
                    // No space to open; the game runs again on a thread of its
                    // own, exactly as the first run did on the boot thread.
                    Thread.detachNewThread {
                        let r = kl_app_lifecycle(1)
                        DispatchQueue.main.async {
                            if r == 0 {
                                status = "game ended (rc=\(kl_app_macho_rc()))"
                                running = false; finished = false; succeeded = false
                            } else {
                                status = String(cString: kl_app_status())
                                running = false; finished = true; succeeded = false
                            }
                        }
                    }
                    return
                }
                running = false; finished = true; succeeded = true
                openSpace(" (re-run)")
            }
        }
    }
}
