// The XR_EXT_hand_tracking gate. `make xrhands`, in `make check`.
//
// As t_xrspace.c: the check lives in kl_openxr.c (kl_openxr_hands_selftest), so
// it exercises the entry points a guest calls and the joints the frontend
// publishes, not copies of them.
#include <stdio.h>
#include "../runtime/xr/kl_openxr.h"

int main(void) {
    printf("=== OpenXR hand tracking ===\n");
    int ok = kl_openxr_hands_selftest(stdout);
    printf("%s: hand joints reach a hand tracker\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
