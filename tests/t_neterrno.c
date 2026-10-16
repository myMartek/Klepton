// What the guest reads in errno after a failed send/recv. `make neterrno`, in
// `make check`.
//
// klb_errno resolves "Darwin's errno did not move since the slot was filled" as
// a guest write. A client reading a non-blocking socket in a loop trips it: the
// first recv fails EAGAIN, the TLS layer clears errno (a guest write), and the
// second recv fails EAGAIN again -- Darwin's errno is still the EAGAIN of the
// first call, so the guest's 0 was kept and curl saw "no error" on a failed
// read, reset the stream and reported "Failed to receive data". The shim's send
// and recv now publish THIS call's errno through kl_errno_set, so the guest
// reads the failure it just had.
//
// Driven through the shim's own recv, send and __errno, the entries the guest
// calls, on local sockets only.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../runtime/klepton.h"

#define LX_EAGAIN 11          // Linux's value; Darwin's is 35
#define LX_EPIPE  32
#define LX_MSG_NOSIGNAL 0x4000

static int g_fail;
static void ck(int ok, const char *what) {
    if (!ok) g_fail++;
    printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

typedef ssize_t (*recv_fn)(int, void *, size_t, int);
typedef ssize_t (*send_fn)(int, const void *, size_t, int);
typedef int *(*errno_fn)(void);

int main(void) {
    printf("=== the guest's errno after a failed send/recv ===\n");
    signal(SIGPIPE, SIG_IGN);      // a send to a closed peer, the way a guest with MSG_NOSIGNAL sees it
    recv_fn g_recv = (recv_fn)kl_shim_lookup("recv");
    send_fn g_send = (send_fn)kl_shim_lookup("send");
    errno_fn g_errno = (errno_fn)kl_shim_lookup("__errno");
    if (!g_recv || !g_send || !g_errno) { printf("FAIL: the shim lacks recv, send or __errno\n"); return 1; }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); return 2; }
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    char buf[16];

    // The first failure is translated to Linux's number.
    ssize_t r = g_recv(sv[0], buf, sizeof buf, 0);
    ck(r < 0 && *g_errno() == LX_EAGAIN, "an empty non-blocking recv fails with EAGAIN (Linux 11)");

    // The TLS layer clears errno; the next recv fails the same way. The guest
    // must read EAGAIN again, not its own 0.
    for (int i = 0; i < 3; i++) {
        *g_errno() = 0;
        r = g_recv(sv[0], buf, sizeof buf, 0);
        int e = *g_errno();
        ck(r < 0 && e == LX_EAGAIN, "...cleared by the guest, the repeated EAGAIN is EAGAIN again");
        if (e != LX_EAGAIN) printf("       (read %d)\n", e);
    }

    // The same for a second, different value after a success in between.
    ck(g_send(sv[1], "x", 1, 0) == 1, "a send on the pair succeeds");
    ck(g_recv(sv[0], buf, sizeof buf, 0) == 1, "...and the recv gets the byte");
    *g_errno() = 0;
    r = g_recv(sv[0], buf, sizeof buf, 0);
    ck(r < 0 && *g_errno() == LX_EAGAIN, "EAGAIN after a success is EAGAIN");

    // send: the peer is gone, so it fails EPIPE, twice, with a clear between.
    close(sv[0]);
    r = g_send(sv[1], "x", 1, LX_MSG_NOSIGNAL);
    ck(r < 0 && *g_errno() == LX_EPIPE, "send to a closed peer fails with EPIPE");
    *g_errno() = 0;
    r = g_send(sv[1], "x", 1, LX_MSG_NOSIGNAL);
    ck(r < 0 && *g_errno() == LX_EPIPE, "...cleared by the guest, the repeated EPIPE is EPIPE again");
    close(sv[1]);

    // A success leaves the guest's own errno alone.
    int sv2[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv2);
    *g_errno() = 1234;
    ck(g_send(sv2[0], "y", 1, 0) == 1 && *g_errno() == 1234, "a successful send does not touch the guest's errno");
    ck(g_recv(sv2[1], buf, sizeof buf, 0) == 1 && *g_errno() == 1234, "a successful recv does not touch it either");
    close(sv2[0]); close(sv2[1]);

    printf("neterrno: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
