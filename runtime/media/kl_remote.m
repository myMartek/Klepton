// See kl_remote.h.
#import <Foundation/Foundation.h>

#include "kl_remote.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// The shipping runtime compiles this file without ARC (build/xros/libklepton.a),
// the host tests with it: every object kept past its autorelease pool is
// retained and released by hand through these two.
#if __has_feature(objc_arc)
#define KL_RETAIN(x) (x)
#define KL_RELEASE(x) ((void)0)
#else
#define KL_RETAIN(x) [(x) retain]
#define KL_RELEASE(x) [(x) release]
#endif

// ---- what the app registered -------------------------------------------------

typedef struct rentry {
    char path[PATH_MAX];       // as registered
    char real[PATH_MAX];       // symlinks resolved (/var -> /private/var), or ""
    char kind[16];
    char a[512], b[160], c[64];
    long long size;
    pthread_mutex_t mu;        // guards url/url_t/a
    char url[4096];
    double url_t;              // when url was looked up (monotonic s)
    struct rentry *next;
} rentry;

// Lists that were replaced stay alive: an open handle and the cache keep
// pointing into them. A few entries per start, so never freed.
static rentry *g_list;
static pthread_mutex_t g_list_mu = PTHREAD_MUTEX_INITIALIZER;

static _Atomic unsigned long g_requests, g_resolves;
static _Atomic unsigned long long g_bytes;

static double mono_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void field(const char **p, char *out, size_t cap, char stop2) {
    size_t n = 0;
    while (**p && **p != '\t' && **p != stop2) {
        if (n + 1 < cap) out[n++] = **p;
        (*p)++;
    }
    out[n] = 0;
    if (**p == '\t') (*p)++;
}

void kl_remote_files_set(const char *spec) {
    rentry *head = NULL, **tail = &head;
    int count = 0;
    const char *p = spec ? spec : "";
    while (*p) {
        rentry *e = calloc(1, sizeof *e);
        if (!e) break;
        char size[32];
        field(&p, e->path, sizeof e->path, '\n');
        field(&p, e->kind, sizeof e->kind, '\n');
        field(&p, e->a, sizeof e->a, '\n');
        field(&p, e->b, sizeof e->b, '\n');
        field(&p, e->c, sizeof e->c, '\n');
        field(&p, size, sizeof size, '\n');
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        e->size = atoll(size);
        if (!e->path[0] || e->size <= 0 ||
            (strcmp(e->kind, "pcloud") != 0 && strcmp(e->kind, "url") != 0)) {
            free(e);
            continue;
        }
        if (!realpath(e->path, e->real)) e->real[0] = 0;
        pthread_mutex_init(&e->mu, NULL);
        *tail = e;
        tail = &e->next;
        count++;
    }
    pthread_mutex_lock(&g_list_mu);
    g_list = head;
    pthread_mutex_unlock(&g_list_mu);
    fprintf(stderr, "  [remote] %d film(s) on a server registered\n", count);
}

static rentry *lookup(const char *path) {
    if (!path || !*path) return NULL;
    pthread_mutex_lock(&g_list_mu);
    rentry *hit = NULL;
    for (rentry *e = g_list; e && !hit; e = e->next)
        if (strcmp(e->path, path) == 0 || (e->real[0] && strcmp(e->real, path) == 0)) hit = e;
    int any = g_list != NULL;
    pthread_mutex_unlock(&g_list_mu);
    if (hit || !any) return hit;
    char real[PATH_MAX];
    if (!realpath(path, real)) return NULL;
    pthread_mutex_lock(&g_list_mu);
    for (rentry *e = g_list; e && !hit; e = e->next)
        if (strcmp(e->path, real) == 0 || (e->real[0] && strcmp(e->real, real) == 0)) hit = e;
    pthread_mutex_unlock(&g_list_mu);
    return hit;
}

int kl_remote_is(const char *path) { return lookup(path) != NULL; }

// ---- HTTP ---------------------------------------------------------------------

static NSURLSession *session(void) {
    static NSURLSession *s;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSURLSessionConfiguration *c = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        c.HTTPMaximumConnectionsPerHost = 6;
        c.timeoutIntervalForRequest = 30;
        c.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
        c.URLCache = nil;
        s = KL_RETAIN([NSURLSession sessionWithConfiguration:c]);
    });
    return s;
}

// One request, waited for. *status gets the HTTP status (0 = no answer). The
// body is the caller's: KL_RELEASE it when done.
static NSData *http_sync(NSURLRequest *req, long *status) {
    __block NSData *body = nil;
    __block long st = 0;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    NSURLSessionDataTask *t = [session() dataTaskWithRequest:req
        completionHandler:^(NSData *d, NSURLResponse *r, NSError *err) {
            if (!err && [r isKindOfClass:[NSHTTPURLResponse class]]) {
                st = ((NSHTTPURLResponse *)r).statusCode;
                body = KL_RETAIN(d);
            }
            dispatch_semaphore_signal(sem);
        }];
    [t resume];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
#if !__has_feature(objc_arc)
    dispatch_release(sem);
#endif
    atomic_fetch_add(&g_requests, 1);
    if (status) *status = st;
    return body;
}

static NSString *form_escape(const char *s) {
    NSMutableCharacterSet *ok = [NSMutableCharacterSet alphanumericCharacterSet];
    [ok addCharactersInString:@"-._~"];
    return [@(s) stringByAddingPercentEncodingWithAllowedCharacters:ok] ?: @"";
}

// pCloud's getpublinkdownload: a fresh direct link to one file of a public
// folder (docs.pcloud.com; Explore360's PCloudClient.publicDownloadURL does
// the same). EU links answer only on eapi, US links only on api: the host
// that answered the listing comes first, the other one second.
static int resolve_pcloud(rentry *e, char *url, size_t cap) {
    const char *other = strcmp(e->a, "api.pcloud.com") == 0 ? "eapi.pcloud.com" : "api.pcloud.com";
    const char *hosts[2] = { e->a[0] ? e->a : "eapi.pcloud.com", other };
    for (int i = 0; i < 2; i++) @autoreleasepool {
        NSString *u = [NSString stringWithFormat:@"https://%s/getpublinkdownload", hosts[i]];
        NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:u]];
        req.HTTPMethod = @"POST";
        [req setValue:@"application/x-www-form-urlencoded" forHTTPHeaderField:@"Content-Type"];
        NSString *body = [NSString stringWithFormat:@"code=%@&fileid=%@&forcedownload=1",
                          form_escape(e->b), form_escape(e->c)];
        req.HTTPBody = [body dataUsingEncoding:NSUTF8StringEncoding];
        long st = 0;
        NSData *d = http_sync(req, &st);
        atomic_fetch_add(&g_resolves, 1);
        NSDictionary *j = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
        KL_RELEASE(d);
        if (![j isKindOfClass:[NSDictionary class]]) {
            fprintf(stderr, "  [remote] pCloud %s answered no link (HTTP %ld)\n", hosts[i], st);
            continue;
        }
        long result = [j[@"result"] longValue];
        if (result != 0) {
            NSString *err = [j[@"error"] isKindOfClass:[NSString class]] ? j[@"error"] : @"";
            fprintf(stderr, "  [remote] pCloud %s: %ld %s\n", hosts[i], result, err.UTF8String);
            // 7001/7002: unknown here, try the other data centre. Anything
            // else (traffic limit, deleted file) is the same on both.
            if (result == 7001 || result == 7002) continue;
            return -1;
        }
        NSArray *hs = j[@"hosts"];
        NSString *path = j[@"path"];
        if (![hs isKindOfClass:[NSArray class]] || !hs.count || ![path isKindOfClass:[NSString class]])
            return -1;
        snprintf(url, cap, "https://%s%s", [hs.firstObject description].UTF8String, path.UTF8String);
        if (i == 1) snprintf(e->a, sizeof e->a, "%s", hosts[1]);
        return 0;
    }
    return -1;
}

// The link to read from: the one looked up before while it is young, else a
// fresh one. `force` after the server refused the old one.
static int entry_url(rentry *e, int force, char *url, size_t cap) {
    pthread_mutex_lock(&e->mu);
    int rc = 0;
    if (strcmp(e->kind, "url") == 0) {
        snprintf(url, cap, "%s", e->a);
    } else if (!force && e->url[0] && mono_s() - e->url_t < 2 * 3600) {
        snprintf(url, cap, "%s", e->url);
    } else {
        rc = resolve_pcloud(e, e->url, sizeof e->url);
        if (rc == 0) {
            e->url_t = mono_s();
            snprintf(url, cap, "%s", e->url);
            fprintf(stderr, "  [remote] %s: link %s\n", strrchr(e->path, '/') ? strrchr(e->path, '/') + 1 : e->path,
                    force ? "renewed" : "looked up");
        } else {
            e->url[0] = 0;
        }
    }
    pthread_mutex_unlock(&e->mu);
    return rc;
}

// Bytes [off, off+len) of the file into out. 0 = all of them.
static int fetch_range(rentry *e, long long off, long len, unsigned char *out) {
    int force = 0;
    for (int attempt = 0; attempt < 4; attempt++) {
        if (attempt) usleep((useconds_t)(250000u << (attempt - 1)));
        @autoreleasepool {
        char url[4096];
        if (entry_url(e, force, url, sizeof url) != 0) { force = 1; continue; }
        NSURL *u = [NSURL URLWithString:@(url)];
        if (!u) { force = 1; continue; }
        NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:u];
        [req setValue:[NSString stringWithFormat:@"bytes=%lld-%lld", off, off + len - 1]
            forHTTPHeaderField:@"Range"];
        long st = 0;
        NSData *d = http_sync(req, &st);
        int done = 0;
        if (st == 206 && d.length == (NSUInteger)len) {
            memcpy(out, d.bytes, (size_t)len);
            atomic_fetch_add(&g_bytes, (unsigned long long)len);
            done = 1;
        } else if (st == 200 && d.length == (NSUInteger)e->size && off + len <= e->size) {
            // A server that ignores Range answers the whole file.
            memcpy(out, (const unsigned char *)d.bytes + off, (size_t)len);
            atomic_fetch_add(&g_bytes, (unsigned long long)d.length);
            done = 1;
        } else {
            fprintf(stderr, "  [remote] range %lld+%ld: HTTP %ld, %lu bytes (try %d)\n",
                    off, len, st, (unsigned long)d.length, attempt + 1);
        }
        KL_RELEASE(d);
        if (done) return 0;
        // Refused, expired or gone: a new link. No answer at all: the same one again.
        force = st != 0;
        }
    }
    return -1;
}

// ---- the block cache ----------------------------------------------------------
//
// 1 MiB blocks, shared by every handle (a film opens the picture and the sound
// separately, and the thumbnail before both), least recently used out first.

enum { BLK = 1 << 20, AHEAD = 3 };

typedef struct blk {
    rentry *e;
    long long idx;
    unsigned char *data;
    int len;
    int state;                 // 0 loading, 1 ready, 2 failed
    int pins;                  // readers between finding it and copying out
    unsigned long long used;
    struct blk *next;
} blk;

static blk *g_blocks;
static int g_nblocks;
static unsigned long long g_tick;
static pthread_mutex_t g_cmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ccv = PTHREAD_COND_INITIALIZER;

static int cache_cap(void) {
    static int cap;
    if (!cap) {
        const char *v = getenv("KL_REMOTE_CACHE_MB");
        int mb = v && *v ? atoi(v) : 64;
        cap = mb < 8 ? 8 : mb;
    }
    return cap;
}

static blk *find_locked(rentry *e, long long idx) {
    for (blk *b = g_blocks; b; b = b->next)
        if (b->e == e && b->idx == idx) return b;
    return NULL;
}

static void unlink_locked(blk *x) {
    for (blk **pp = &g_blocks; *pp; pp = &(*pp)->next)
        if (*pp == x) { *pp = x->next; g_nblocks--; return; }
}

static void evict_locked(void) {
    while (g_nblocks >= cache_cap()) {
        blk *old = NULL;
        for (blk *b = g_blocks; b; b = b->next)
            if (b->state == 1 && !b->pins && (!old || b->used < old->used)) old = b;
        if (!old) return;      // all loading: let it grow for a moment
        unlink_locked(old);
        free(old->data);
        free(old);
    }
}

// A new loading block, or NULL if it is already there.
static blk *claim_locked(rentry *e, long long idx) {
    if (find_locked(e, idx)) return NULL;
    evict_locked();
    blk *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->e = e;
    b->idx = idx;
    b->used = ++g_tick;
    b->next = g_blocks;
    g_blocks = b;
    g_nblocks++;
    return b;
}

static void load(blk *b) {
    long long off = b->idx * (long long)BLK;
    long len = (long)(b->e->size - off < BLK ? b->e->size - off : BLK);
    unsigned char *data = len > 0 ? malloc((size_t)len) : NULL;
    int ok = data && fetch_range(b->e, off, len, data) == 0;
    pthread_mutex_lock(&g_cmu);
    if (ok) {
        b->data = data;
        b->len = (int)len;
        b->state = 1;
    } else {
        // Out of the cache at once, so the next read asks again; the last
        // reader waiting on it frees it (read_ahead's has none: freed here).
        free(data);
        b->state = 2;
        unlink_locked(b);
        if (!b->pins) free(b);
    }
    pthread_cond_broadcast(&g_ccv);
    pthread_mutex_unlock(&g_cmu);
}

static dispatch_queue_t ahead_queue(void) {
    static dispatch_queue_t q;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        q = dispatch_queue_create("klepton.remote.ahead",
                                  dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_CONCURRENT,
                                                                          QOS_CLASS_USER_INITIATED, 0));
    });
    return q;
}

static void read_ahead(rentry *e, long long idx) {
    long long last = (e->size - 1) / BLK;
    for (long long j = idx + 1; j <= idx + AHEAD && j <= last; j++) {
        pthread_mutex_lock(&g_cmu);
        blk *b = claim_locked(e, j);
        pthread_mutex_unlock(&g_cmu);
        if (b) dispatch_async(ahead_queue(), ^{ load(b); });
    }
}

// ---- handles ------------------------------------------------------------------

struct kl_remote {
    rentry *e;
    long long last_idx;        // for telling a sequential read from a jump
};

kl_remote *kl_remote_open(const char *path) {
    rentry *e = lookup(path);
    if (!e) return NULL;
    kl_remote *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->e = e;
    r->last_idx = -2;
    return r;
}

void kl_remote_close(kl_remote *r) { free(r); }
long long kl_remote_size(kl_remote *r) { return r ? r->e->size : -1; }

const char *kl_remote_name(kl_remote *r) {
    if (!r) return "";
    const char *s = strrchr(r->e->path, '/');
    return s ? s + 1 : r->e->path;
}

long kl_remote_read(kl_remote *r, long long off, void *buf, size_t n) {
    if (!r || off < 0) return -1;
    rentry *e = r->e;
    if (off >= e->size || !n) return 0;
    if ((long long)n > e->size - off) n = (size_t)(e->size - off);
    size_t done = 0;
    while (done < n) {
        long long pos = off + (long long)done;
        long long idx = pos / BLK;
        pthread_mutex_lock(&g_cmu);
        blk *b = find_locked(e, idx);
        blk *mine = NULL;
        if (!b) b = mine = claim_locked(e, idx);
        if (b) b->pins++;
        pthread_mutex_unlock(&g_cmu);
        if (!b) return done ? (long)done : -1;
        if (mine) load(mine);
        pthread_mutex_lock(&g_cmu);
        while (b->state == 0) pthread_cond_wait(&g_ccv, &g_cmu);
        if (b->state == 2) {
            if (--b->pins == 0) free(b);
            pthread_mutex_unlock(&g_cmu);
            return done ? (long)done : -1;
        }
        b->pins--;
        b->used = ++g_tick;
        long in = (long)(pos - idx * (long long)BLK);
        size_t take = (size_t)(b->len - in);
        if (take > n - done) take = n - done;
        memcpy((unsigned char *)buf + done, b->data + in, take);
        pthread_mutex_unlock(&g_cmu);
        done += take;
        // Read ahead only while the reader goes forward; a jump (the index at
        // the end of an MP4, a thumbnail's seek) fetches only what it asked for.
        int forward = idx == r->last_idx || idx == r->last_idx + 1;
        r->last_idx = idx;
        if (forward) read_ahead(e, idx);
    }
    return (long)done;
}

void kl_remote_stats(unsigned long *requests, unsigned long long *bytes,
                     unsigned long *resolves) {
    if (requests) *requests = atomic_load(&g_requests);
    if (bytes) *bytes = atomic_load(&g_bytes);
    if (resolves) *resolves = atomic_load(&g_resolves);
}
