// The game's console, reachable from the app: Source 2's net console, served
// without a network.
//
// Half-Life: Alyx has no keyboard here (SDL's offscreen video driver: no
// window, no key ever reaches the engine), so its console cannot be typed into.
// What the engine does have is a net console: started with `-netconport <n>`
// it listens on that TCP port and runs every line a client sends as a console
// command, and sends its console output back. The app's cheat menu
// (KleptonCheatMenu.swift) is that client.
//
// No socket of the system is involved. A Linux guest has had no sockets at all
// (the syscalls answered ENOSYS), and Alyx runs fine without them; opening real
// ones would hand the engine's other networking a path it has never taken here,
// and a listening socket is the kind of thing a visionOS app is asked about.
// So with KL_LX_NETCON=<port> set, and only then, a TCP socket the guest makes
// is a stand-in: one end of a local socket pair. Bound to the console's port
// and listening, its other end is a doorbell — the app rings it (one byte) when
// it has a connection for the game, the descriptor turns readable as a real
// listener would, and accept() hands the guest one end of a second socket pair
// whose other end the app holds. Everything after that — recv, send, poll,
// non-blocking reads — is the host's own socket pair doing what it does.
//
// Without KL_LX_NETCON every socket call answers as before.
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include "kl_lx.h"
#include "../klepton.h"

#define LX_AF_INET      2
#define LX_SOCK_STREAM  1
#define LX_SOCK_NONBLOCK 04000
#define LX_SOCK_CLOEXEC  02000000
#define LX_MSG_PEEK     0x2
#define LX_MSG_DONTWAIT 0x40
#define LX_MSG_WAITALL  0x100

enum { NC_NONE, NC_STUB, NC_LISTENER, NC_CONN };
#define NC_FDS 4096
static uint8_t g_kind[NC_FDS];
static int g_port = -1;                 // KL_LX_NETCON, or 0 when unset
static int g_bellof[NC_FDS];            // each stand-in's other end (+1; 0 = none)
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_listener = -1;             // the guest's listening descriptor
static int g_bell = -1;                 // the other end of it: one byte = one connection waiting
static int g_pending = -1;              // the guest's end of the next connection, until accepted
static int g_app = -1;                  // the app's end of the connection
static _Atomic int g_state;             // kl_lx_netcon_state()
static void (*g_output)(const char *line);

static int port(void) {
    if (g_port < 0) {
        const char *e = getenv("KL_LX_NETCON");
        g_port = e && *e ? atoi(e) : 0;
        if (g_port > 0) fprintf(stderr, "[netcon] the game's console is to listen on port %d (KL_LX_NETCON)\n", g_port);
    }
    return g_port;
}

int kl_lx_netcon_enabled(void) { return port() > 0; }
int kl_lx_netcon_is(int fd) { return fd >= 0 && fd < NC_FDS && g_kind[fd] != NC_NONE; }

static int64_t lx_err(void) { return -(int64_t)kl_errno_to_linux(errno); }
static int64_t lx_errno(int e) { return -(int64_t)kl_errno_to_linux(e); }

static void nosigpipe(int fd) { int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }

static void set_flags(int fd, int lxflags) {
    if (lxflags & LX_SOCK_NONBLOCK) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    if (lxflags & LX_SOCK_CLOEXEC) fcntl(fd, F_SETFD, FD_CLOEXEC);
}

// Linux's sockaddr_in: family (16 bits, host order), port (network order), address.
static void fill_addr(void *addr, uint32_t *len) {
    if (!addr || !len) return;
    uint8_t a[16] = { LX_AF_INET, 0, (uint8_t)(port() >> 8), (uint8_t)port(), 127, 0, 0, 1 };
    uint32_t n = *len < sizeof a ? *len : (uint32_t)sizeof a;
    memcpy(addr, a, n);
    *len = sizeof a;
}

int64_t kl_lx_netcon_socket(int domain, int type, int protocol) {
    (void)protocol;
    if (port() <= 0 || domain != LX_AF_INET || (type & 0xf) != LX_SOCK_STREAM) {
        static int told;
        if (!told++) fprintf(stderr, "[netcon] socket(%d, %#x) asked for and refused, as always\n", domain, type);
        return lx_errno(ENOSYS);
    }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return lx_err();
    if (sv[0] >= NC_FDS) { close(sv[0]); close(sv[1]); return lx_errno(EMFILE); }
    nosigpipe(sv[0]); nosigpipe(sv[1]);
    set_flags(sv[0], type);
    fcntl(sv[1], F_SETFD, FD_CLOEXEC);
    pthread_mutex_lock(&g_lock);
    g_kind[sv[0]] = NC_STUB;
    g_bellof[sv[0]] = sv[1] + 1;            // closed with it (kl_lx_netcon_close)
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "[netcon] the game made a TCP socket (fd %d, type %#x)\n", sv[0], type);
    return sv[0];
}

int64_t kl_lx_netcon_bind(int fd, const void *addr, uint32_t len) {
    if (!kl_lx_netcon_is(fd)) return lx_errno(ENOTSOCK);
    const uint8_t *a = addr;
    int p = a && len >= 4 ? (a[2] << 8 | a[3]) : -1;
    fprintf(stderr, "[netcon] bind fd %d to port %d%s\n", fd, p, p == port() ? " (the console's)" : "");
    if (p != port()) return lx_errno(EADDRINUSE);
    return 0;
}

int64_t kl_lx_netcon_listen(int fd, int backlog) {
    (void)backlog;
    if (!kl_lx_netcon_is(fd)) return lx_errno(ENOTSOCK);
    pthread_mutex_lock(&g_lock);
    int ok = g_listener < 0 && g_bellof[fd] > 0;
    if (ok) { g_listener = fd; g_bell = g_bellof[fd] - 1; g_kind[fd] = NC_LISTENER; g_state = 1; }
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, ok ? "[netcon] the game's console listens (fd %d)\n"
                       : "[netcon] listen on fd %d refused: a console already listens\n", fd);
    return ok ? 0 : lx_errno(EADDRINUSE);
}

int64_t kl_lx_netcon_accept(int fd, void *addr, uint32_t *len, int flags) {
    if (!kl_lx_netcon_is(fd) || g_kind[fd] != NC_LISTENER) return lx_errno(EINVAL);
    char b;
    ssize_t n = read(fd, &b, 1);           // blocks, or EAGAIN, as the listener was set
    if (n <= 0) return n == 0 ? lx_errno(EAGAIN) : lx_err();
    pthread_mutex_lock(&g_lock);
    int c = g_pending;
    g_pending = -1;
    if (c >= 0 && c < NC_FDS) { g_kind[c] = NC_CONN; g_state = 2; }
    pthread_mutex_unlock(&g_lock);
    if (c < 0) return lx_errno(EAGAIN);
    set_flags(c, flags);
    fill_addr(addr, len);
    fprintf(stderr, "[netcon] the game accepted the app's connection (fd %d)\n", c);
    return c;
}

static int darwin_msg_flags(int f) {
    return (f & LX_MSG_PEEK ? MSG_PEEK : 0) | (f & LX_MSG_DONTWAIT ? MSG_DONTWAIT : 0)
         | (f & LX_MSG_WAITALL ? MSG_WAITALL : 0);
}

int64_t kl_lx_netcon_recvfrom(int fd, void *buf, size_t n, int flags, void *addr, uint32_t *len) {
    ssize_t r = recv(fd, buf, n, darwin_msg_flags(flags));
    if (r < 0) return lx_err();
    fill_addr(addr, len);
    return r;
}

int64_t kl_lx_netcon_sendto(int fd, const void *buf, size_t n, int flags) {
    ssize_t r = send(fd, buf, n, darwin_msg_flags(flags));
    return r < 0 ? lx_err() : r;
}

// Linux's msghdr: name, namelen (32 bits, padded), iov, iovlen (64 bits),
// control, controllen (64 bits), flags.
typedef struct { void *name; uint32_t namelen; uint32_t pad; struct iovec *iov; uint64_t iovlen;
                 void *control; uint64_t controllen; int32_t flags; } lx_msghdr;

int64_t kl_lx_netcon_msg(int fd, void *m, int flags, int sending) {
    lx_msghdr *h = m;
    if (!h) return lx_errno(EFAULT);
    ssize_t r = sending ? writev(fd, h->iov, (int)h->iovlen) : readv(fd, h->iov, (int)h->iovlen);
    (void)flags;
    if (r < 0) return lx_err();
    if (!sending) { h->controllen = 0; h->flags = 0; if (h->name) fill_addr(h->name, &h->namelen); }
    return r;
}

int64_t kl_lx_netcon_name(int fd, void *addr, uint32_t *len) {
    if (!kl_lx_netcon_is(fd)) return lx_errno(ENOTSOCK);
    fill_addr(addr, len);
    return 0;
}

int64_t kl_lx_netcon_sockopt(int fd, int get, void *val, uint32_t *len) {
    if (!kl_lx_netcon_is(fd)) return lx_errno(ENOTSOCK);
    if (get && val && len && *len >= 4) { *(int32_t *)val = 0; *len = 4; }
    return 0;
}

int64_t kl_lx_netcon_shutdown(int fd, int how) {
    return shutdown(fd, how) == 0 ? 0 : lx_err();
}

void kl_lx_netcon_close(int fd) {
    if (!kl_lx_netcon_is(fd)) return;
    pthread_mutex_lock(&g_lock);
    if (g_kind[fd] == NC_LISTENER) { g_listener = -1; g_bell = -1; g_state = 0; }
    if (g_bellof[fd] > 0) { close(g_bellof[fd] - 1); g_bellof[fd] = 0; }
    if (g_kind[fd] == NC_CONN) {
        g_state = g_listener >= 0 ? 1 : 0;
        if (g_app >= 0) { shutdown(g_app, SHUT_RDWR); }
    }
    g_kind[fd] = NC_NONE;
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "[netcon] the game closed fd %d\n", fd);
}

// ---- the app's side ----

static void *drain(void *arg) {
    int fd = (int)(intptr_t)arg;
    char line[1024];
    size_t have = 0;
    for (;;) {
        char buf[512];
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\n' || ch == '\r' || have == sizeof line - 1) {
                line[have] = 0;
                if (have) {
                    fprintf(stderr, "[netcon] < %s\n", line);
                    if (g_output) g_output(line);
                }
                have = 0;
            } else if (ch) {
                line[have++] = ch;
            }
        }
    }
    pthread_mutex_lock(&g_lock);
    if (g_app == fd) { close(g_app); g_app = -1; }
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "[netcon] the console's connection has ended\n");
    return NULL;
}

void kl_lx_netcon_set_output(void (*fn)(const char *line)) { g_output = fn; }

int kl_lx_netcon_state(void) { return g_state; }

int kl_lx_netcon_send(const char *command) {
    pthread_mutex_lock(&g_lock);
    if (g_listener < 0 || g_bell < 0) {
        pthread_mutex_unlock(&g_lock);
        fprintf(stderr, "[netcon] not sent, the game's console is not listening: %s\n", command);
        return -1;
    }
    if (g_app < 0) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { pthread_mutex_unlock(&g_lock); return -1; }
        nosigpipe(sv[0]); nosigpipe(sv[1]);
        if (g_pending >= 0) close(g_pending);
        g_pending = sv[0];
        g_app = sv[1];
        char one = 1;
        if (write(g_bell, &one, 1) != 1) fprintf(stderr, "[netcon] the doorbell could not be rung: %s\n", strerror(errno));
        pthread_t th;
        if (pthread_create(&th, NULL, drain, (void *)(intptr_t)g_app) == 0) pthread_detach(th);
    }
    int fd = g_app;
    pthread_mutex_unlock(&g_lock);
    size_t n = strlen(command);
    struct iovec iov[2] = { { (void *)command, n }, { "\n", 1 } };
    ssize_t w = writev(fd, iov, 2);
    fprintf(stderr, "[netcon] > %s%s\n", command, w == (ssize_t)(n + 1) ? "" : " (NOT written)");
    return w == (ssize_t)(n + 1) ? 0 : -1;
}

// pselect6: Linux's fd_set is Darwin's (1024 bits, the same bit order), and so
// is the timespec. The signal mask is not applied — no guest here has needed it.
int64_t kl_lx_pselect6(int n, void *r, void *w, void *e, const struct timespec *ts) {
    struct timeval tv, *tvp = NULL;
    if (ts) { tv.tv_sec = ts->tv_sec; tv.tv_usec = (int)(ts->tv_nsec / 1000); tvp = &tv; }
    int got = select(n > FD_SETSIZE ? FD_SETSIZE : n, r, w, e, tvp);
    return got < 0 ? lx_err() : got;
}
