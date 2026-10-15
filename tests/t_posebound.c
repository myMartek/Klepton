// The pose-boundary gate for KL_OVRP_ONE_POSE. `make posebound`, in `make check`.
//
// From rebelancap's fork (tests/t_posebound.c, his commit a0e966f). Paint the
// Town Red VR measured the one-pose invariant BROKEN in gameplay and held in
// the menu — `poses/frame` 1.70 against 1.00 — which is the same split as the
// head-turn judder seen in the headset. The check lives in kl_ovrp.c
// (kl_ovrp_pose_boundary_selftest) because what it asserts is that file's frame
// boundary, driven through the entry points the guest actually calls.
//
// What differs here: in his tree the feature is always on, in this one it is a
// switch — ON by default since 2026-10-15 (the OVRPlugin half of "no XR
// prediction"), off with KL_OVRP_ONE_POSE=0. So the gate has more than one
// side, and each side is a PROCESS, because the switch and its boundary are
// read once into statics — as they are in the shipping build. Every case is a
// child forked before anything has touched kl_ovrp, with the environment set
// for it:
//
//   (a) KL_OVRP_ONE_POSE=1 — the switch set explicitly. One head pose per
//       guest frame cycle; the frame record carries it.
//   (a) default — the switch unset, the shipping default: the same as (a).
//   (b) KL_OVRP_ONE_POSE=1 KL_OVRP_POSE_BOUNDARY=index — his finding. The same
//       call pattern splits every cycle across two poses. This case passes
//       when it SEES the split: it is the proof that (a) can fail.
//   (c) KL_OVRP_ONE_POSE=0 — what the app's Test switch "XR prediction" sets.
//       Nothing frozen, no state of the feature valid, the old per-step
//       answers.
//   (d) KL_OVRP_ONE_POSE=1, a guest that never speaks OVRPlugin (OpenXR): the
//       switch is in force only for a guest that calls ovrp_WaitToBeginFrame,
//       so here it must do nothing — the answers of (c), nothing frozen.
//   (e) KL_OVRP_ONE_POSE=1, Walkabout's shape: an OpenXR frame loop whose Meta
//       SDK calls ovrp_Update2(step 0, frameIdx 0) in mid-frame. Before the
//       switch was gated each such call re-froze the head the OpenXR queries
//       read. The head before and after it must be one pose — and the same
//       frames run once more with KL_OVRP_ONE_POSE=0, as the reference.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include "../runtime/xr/kl_ovrp.h"

static const struct {
    const char *tag, *one_pose, *boundary;
    int which;
    const char *title, *verdict;
} CASES[] = {
    { "(a)", "1", NULL, KL_OVRP_POSEBOUND_CYCLE,
      "KL_OVRP_ONE_POSE=1, boundary = the guest's frame cycle",
      "one guest frame cycle, one head pose, one filed anchor" },
    { "(b)", "1", "index", KL_OVRP_POSEBOUND_INDEX,
      "KL_OVRP_ONE_POSE=1 KL_OVRP_POSE_BOUNDARY=index — must SPLIT",
      "keyed on the frame index the same pattern splits into two poses a "
      "cycle (the gate can fail)" },
    { "(a) default", NULL, NULL, KL_OVRP_POSEBOUND_CYCLE,
      "KL_OVRP_ONE_POSE unset, the shipping default (on)",
      "one guest frame cycle, one head pose, one filed anchor" },
    { "(c)", "0", NULL, KL_OVRP_POSEBOUND_OFF,
      "KL_OVRP_ONE_POSE=0 (the app's XR prediction switch)",
      "switch off: nothing frozen, no one-pose state valid, per-step answers "
      "as before" },
    { "(d)", "1", NULL, KL_OVRP_POSEBOUND_XR,
      "KL_OVRP_ONE_POSE=1, a guest that never speaks OVRPlugin (OpenXR)",
      "the switch does nothing there: latch pose, live eye transform, nothing "
      "frozen" },
    { "(e)", "1", NULL, KL_OVRP_POSEBOUND_MIXED,
      "KL_OVRP_ONE_POSE=1, OpenXR frames with ovrp_Update2(0, 0) in mid-frame "
      "(Walkabout)",
      "the Update2 does not change the head inside the frame, nothing frozen" },
    { "(e) reference", "0", NULL, KL_OVRP_POSEBOUND_MIXED_OFF,
      "the same frames, KL_OVRP_ONE_POSE=0",
      "switch off gives the same answers" },
};

// Knobs that would change what the pattern reads, were one left over in the
// caller's environment: a forced IPD replaces the eye offsets the gate pushes,
// the latch and the record switches select other paths, a scripted head or a
// test yaw moves the pose.
static const char *const CLEARED[] = {
    "KL_OVRP_ONE_POSE", "KL_OVRP_POSE_BOUNDARY", "KL_OVRP_LATCH", "KL_OVRP_IPD",
    "KL_OVRP_EYE_CANT", "KL_OVRP_HEAD", "KL_OVRP_RECORD_INDEXED",
    "KL_OVRP_RECORD_LATE", "KL_OVRP_TEST_BR_YAW", "KL_OVRP_POSE_TRACE",
};

static int run(unsigned i) {
    for (unsigned k = 0; k < sizeof CLEARED / sizeof CLEARED[0]; k++)
        unsetenv(CLEARED[k]);
    if (CASES[i].one_pose) setenv("KL_OVRP_ONE_POSE", CASES[i].one_pose, 1);
    if (CASES[i].boundary) setenv("KL_OVRP_POSE_BOUNDARY", CASES[i].boundary, 1);
    printf("--- %s %s ---\n", CASES[i].tag, CASES[i].title);
    int ok = kl_ovrp_pose_boundary_selftest(stdout, CASES[i].which);
    printf("%s %s: %s\n", ok ? "PASS" : "FAIL", CASES[i].tag, CASES[i].verdict);
    fflush(stdout);
    return ok;
}

int main(void) {
    printf("=== one head pose per guest frame (KL_OVRP_ONE_POSE) ===\n");
    int all = 1;
    for (unsigned i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        fflush(stdout);
        fflush(stderr);
        pid_t pid = fork();
        if (pid == 0) _exit(run(i) ? 0 : 1);
        if (pid < 0) {
            printf("FAIL: fork() failed — this gate needs a process per case\n");
            return 1;
        }
        int st = 0;
        if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            // A child that died says nothing itself.
            if (!WIFEXITED(st))
                printf("FAIL %s: the case did not run to its end\n", CASES[i].tag);
            all = 0;
        }
    }
    printf("%s: the pose boundary — switch on, its index mode, switch off, and "
           "inert for a guest that does not delimit its frames\n",
           all ? "PASS" : "FAIL");
    return all ? 0 : 1;
}
