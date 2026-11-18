// The game's console for the app (runtime/linux/kl_lx_netcon.c), on this Mac:
// a guest's side made of the same calls the syscall table makes for it —
// socket, bind, listen, a non-blocking accept that finds nothing, poll, accept,
// recv — against the app's kl_lx_netcon_send and its output callback.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../runtime/linux/kl_lx.h"

int kl_errno_to_linux(int e) { return e == EAGAIN ? 11 : e; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                                         fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static _Atomic int g_heard;
static void heard(const char *line) { if (!strcmp(line, "godmode ON")) g_heard = 1; }

int main(void) {
    CHECK(kl_lx_netcon_send("god") < 0, "a line was sent with nobody listening");
    setenv("KL_LX_NETCON", "29123", 1);
    CHECK(kl_lx_netcon_enabled(), "KL_LX_NETCON did not enable it");
    CHECK(kl_lx_netcon_socket(2, 2, 0) < 0, "a UDP socket was served");
    int64_t s = kl_lx_netcon_socket(2, 1 | 04000, 6);
    CHECK(s >= 0, "no TCP socket: %lld", (long long)s);
    CHECK(kl_lx_netcon_sockopt((int)s, 0, NULL, NULL) == 0, "setsockopt refused");
    uint8_t other[16] = { 2, 0, 0x1f, 0x90 };          // port 8080
    CHECK(kl_lx_netcon_bind((int)s, other, 16) < 0, "a bind to another port was taken");
    uint8_t mine[16] = { 2, 0, 29123 >> 8, 29123 & 0xff };
    CHECK(kl_lx_netcon_bind((int)s, mine, 16) == 0, "the console's port was refused");
    CHECK(kl_lx_netcon_listen((int)s, 5) == 0, "listen refused");
    CHECK(kl_lx_netcon_state() == 1, "state %d after listen", kl_lx_netcon_state());
    CHECK(kl_lx_netcon_accept((int)s, NULL, NULL, 0) == -11, "a connection out of nowhere");

    kl_lx_netcon_set_output(heard);
    CHECK(kl_lx_netcon_send("sv_cheats 1; god") == 0, "the first line was not written");
    CHECK(kl_lx_netcon_send("impulse 101") == 0, "the second line was not written");
    struct pollfd p = { (int)s, POLLIN, 0 };
    CHECK(poll(&p, 1, 1000) == 1, "the listener did not turn readable");
    uint8_t addr[16]; uint32_t alen = sizeof addr;
    int64_t c = kl_lx_netcon_accept((int)s, addr, &alen, 04000);
    CHECK(c >= 0, "accept failed: %lld", (long long)c);
    CHECK(alen == 16 && addr[0] == 2 && addr[4] == 127, "the peer's address is not loopback");
    CHECK(kl_lx_netcon_state() == 2, "state %d after accept", kl_lx_netcon_state());
    CHECK(kl_lx_netcon_is((int)c), "the connection is not known as the console's");

    char buf[256] = {0};
    size_t have = 0;
    for (int i = 0; i < 100 && have < 30; i++) {
        int64_t n = kl_lx_netcon_recvfrom((int)c, buf + have, sizeof buf - 1 - have, 0x40, NULL, NULL);
        if (n > 0) have += (size_t)n; else usleep(10000);
    }
    CHECK(!strcmp(buf, "sv_cheats 1; god\nimpulse 101\n"), "the game read '%s'", buf);
    const char *reply = "godmode ON\n";
    CHECK(kl_lx_netcon_sendto((int)c, reply, strlen(reply), 0x4000) == (int64_t)strlen(reply), "reply not sent");
    for (int i = 0; i < 100 && !g_heard; i++) usleep(10000);
    CHECK(g_heard, "the app did not hear the console's reply");

    // The game drops the connection: the next line opens a new one.
    kl_lx_netcon_close((int)c); close((int)c);
    for (int i = 0; i < 100 && kl_lx_netcon_send("noclip") != 0; i++) usleep(10000);
    p.revents = 0;
    CHECK(poll(&p, 1, 1000) == 1, "no second connection was offered");
    int64_t c2 = kl_lx_netcon_accept((int)s, NULL, NULL, 0);
    CHECK(c2 >= 0, "the second connection was not accepted");
    memset(buf, 0, sizeof buf);
    usleep(50000);
    kl_lx_netcon_recvfrom((int)c2, buf, sizeof buf - 1, 0x40, NULL, NULL);
    CHECK(!strcmp(buf, "noclip\n"), "the second connection read '%s'", buf);

    printf(fails ? "t_netcon: %d FAILED\n" : "t_netcon: all passed\n", fails);
    return fails != 0;
}
