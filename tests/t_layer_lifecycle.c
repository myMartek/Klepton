#include <stdio.h>
#include <stdlib.h>
#include "../runtime/xr/kl_ovrp.h"
#include "../runtime/guest/kl_driver.h"
#include <math.h>
#include <string.h>

int main(void) {
    setenv("KL_OVRP_ENQ_OWNS_OVERLAYS", "1", 1);
    setenv("KL_OVERLAY_PERSIST", "0", 1);
    setenv("KL_OVRP_OVERLAY_PERSIST", "0", 1);
    int ok = kl_ovrp_layer_lifecycle_selftest();
    printf("%s: enqueue expiration, destruction, reuse and hidden layers\n", ok ? "PASS" : "FAIL");
    unsetenv("KL_OVRP_EYE_MAX");
    setenv("KL_OVRP_EYE_SCALE", "1", 1);
    setenv("KL_OVRP_RENDER_SCALE", "1", 1);
    setenv("KL_OVRP_EYE_ALIGN", "1", 1);
    kl_target target = {.name = "unrelated", .kind = KL_GUEST_UNITY};
    kl_driver_init(&target, NULL, 0);
    kl_ovrp_set_eye_frustum(0, 1.7f, 1.0f, 1.0f, 1.2f);
    float before[4], after[4];
    kl_ovrp_eye_tangents(0, before);
    kl_ovrp_set_eye_texture_size(4633, 3717);
    int bw, bh, w, h;
    kl_ovrp_eye_texture_size(&bw, &bh);
    target.name = "x9531494193591052";
    kl_ovrp_set_eye_texture_size(4633, 3717);
    kl_ovrp_eye_texture_size(&w, &h);
    kl_ovrp_eye_tangents(0, after);
    int resolution_ok = bw == 3072 && bh == 2465 && w == bw && h == bh &&
                        !memcmp(before, after, sizeof before);
    setenv("KL_OVRP_EYE_MAX", "2800", 1);
    kl_ovrp_set_eye_texture_size(4633, 3717);
    kl_ovrp_eye_texture_size(&w, &h);
    resolution_ok &= w < bw && h < bh;
    printf("%s: Ghost Town keeps the 3072 baseline cap, FOV unchanged, env override\n",
           resolution_ok ? "PASS" : "FAIL");
    return !(ok && resolution_ok);
}
