// KL_OVRP_NO_TRACKERS (Angry Birds VR: Isle of Pigs): the four tracker nodes
// answer as on a Quest — not present, not tracked, identity at the origin —
// while the head and eyes keep the head pose. Milliseconds; in `make check`.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../runtime/xr/kl_ovrp.h"

typedef uint64_t (*pred_fn)(int, int *);
typedef uint64_t (*pose3_fn)(int, int, int, void *);

int main(void) {
    setenv("KL_OVRP_NO_TRACKERS", "1", 1);
    printf("=== tracker nodes under KL_OVRP_NO_TRACKERS ===\n");
    pred_fn present = (pred_fn)kl_ovrp_sym("ovrp_GetNodePresent2");
    pred_fn ptracked = (pred_fn)kl_ovrp_sym("ovrp_GetNodePositionTracked2");
    pred_fn ovalid = (pred_fn)kl_ovrp_sym("ovrp_GetNodeOrientationValid");
    pose3_fn pose = (pose3_fn)kl_ovrp_sym("ovrp_GetNodePoseState3");
    if (!present || !ptracked || !ovalid || !pose) { printf("FAIL: entry points missing\n"); return 1; }
    kl_ovrp_set_head_pose(0.7f, 1.6f, -0.4f, 0, 0, 0, 1);
    int bad = 0;
    for (int n = 0; n <= 9; n++) {
        int a = -1, b = -1, c = -1;
        present(n, &a); ptracked(n, &b); ovalid(n, &c);
        float f[22] = {0};
        pose(-1, 0, n, f);
        int tracker = n >= 5 && n <= 8;
        int want = !tracker;
        if (a != want || b != want || c != want) {
            printf("FAIL node %d: present %d tracked %d valid %d, want %d\n", n, a, b, c, want); bad++;
        }
        if (tracker && (f[3] != 1.0f || f[4] != 0.0f || f[5] != 0.0f || f[6] != 0.0f)) {
            printf("FAIL node %d: pose (%.2f %.2f %.2f) w=%.2f, want the origin\n", n, f[4], f[5], f[6], f[3]); bad++;
        }
        if (n == 9 && (f[4] < 0.69f || f[4] > 0.71f || f[6] > -0.39f || f[6] < -0.41f)) {
            printf("FAIL head: x=%.3f z=%.3f, want 0.7 / -0.4\n", f[4], f[6]); bad++;
        }
    }
    printf("%s\n", bad ? "FAIL tracker nodes" : "PASS: trackers absent at the origin, the head keeps its position");
    return bad ? 1 : 0;
}
