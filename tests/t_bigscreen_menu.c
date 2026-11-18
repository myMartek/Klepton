// Menu-yields-film (Bigscreen x2497738113633933): while a room-menu
// BigUIPage IsShowing, the film quad overlay is skipped host-side and the
// in-eye mesh carries the picture. `make bigscreen_menu`, in `make check`.
#include <stdio.h>
#include "../runtime/xr/kl_ovrp.h"

int main(void) {
    printf("=== bigscreen menu-yields-film ===\n");
    int ok = kl_bigscreen_menu_selftest();
    printf("%s\n", ok ? "PASS bigscreen menu-yields-film" : "FAIL bigscreen menu-yields-film");
    return ok ? 0 : 1;
}
