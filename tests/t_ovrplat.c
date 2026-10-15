// The Oculus Platform completion-queue gate. `make ovrplat`, in `make check`.
//
// The check lives in kl_ovrplat.c (kl_ovrplat_queue_selftest) because what it
// asserts is that file's own queue and call-surface table, driven through the
// functions the guest's ovr_* calls land in. Testing a copy would test the copy.
//
// No guest, no game files: the requests it issues are the self-test's own
// message types, and a pool's worth of them to reach its limit. stderr
// carries one "[plat] ... completion queued" line per request — over three
// thousand — so it goes to /dev/null; the verdict and any failure are on
// stdout.
#include <stdio.h>
#include "../runtime/xr/kl_ovrplat.h"

int main(void) {
    if (!freopen("/dev/null", "w", stderr)) return 2;
    printf("=== Oculus Platform completion queue ===\n");
    int ok = kl_ovrplat_queue_selftest(stdout);
    printf("%s: no completion lost, no held message rewritten, no slot torn, pool bounded\n",
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
