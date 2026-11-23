// WebM (Matroska) demuxing for VP8 clips — see kl_webm.h.
//
// Why it exists: Unity's VideoPlayer on Android plays .webm through MediaCodec
// like any other clip, and games ship them — Mixture (x4691626700899382) has
// seven VP8 clips, one of them on the player's hands before a character is
// picked. AVFoundation cannot open WebM, so kl_avdemux said "carries no video
// track", AMediaExtractor answered -10002, and Unity retried the clip every
// frame for as long as it was on screen (device logs, 2026-11-17/18).
//
// What is read: the EBML header (DocType webm/matroska), Segment > Info
// (TimecodeScale, Duration), Segment > Tracks (the first video track whose
// CodecID is V_VP8: number, PixelWidth/Height, FrameRate, DefaultDuration) and
// every Cluster's Timecode, SimpleBlocks and BlockGroups. Each frame of that
// track becomes one index entry pointing into the mapped file, so reading is a
// memcpy and seeking is a search. Laced blocks (never seen for video) are
// skipped and counted. Everything else in the file is stepped over by size.
#include "kl_webm.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { size_t off, len; long long pts_us; int key; } klw_frame;

struct kl_webm {
    const uint8_t *map;              // the WebM's first byte
    size_t         size;
    void          *map_base;         // what mmap returned (page aligned)
    size_t         map_len;
    int            width, height;
    long long      duration_us;
    float          fps;
    klw_frame     *frames;
    size_t         n, cap, cur;
    unsigned       laced;           // frames we could not index (lacing)
};

// EBML element ids used here.
enum {
    ID_EBML = 0x1A45DFA3, ID_DOCTYPE = 0x4282, ID_SEGMENT = 0x18538067,
    ID_INFO = 0x1549A966, ID_TIMECODESCALE = 0x2AD7B1, ID_DURATION = 0x4489,
    ID_TRACKS = 0x1654AE6B, ID_TRACKENTRY = 0xAE, ID_TRACKNUMBER = 0xD7,
    ID_TRACKTYPE = 0x83, ID_CODECID = 0x86, ID_DEFAULTDURATION = 0x23E383,
    ID_VIDEO = 0xE0, ID_PIXELWIDTH = 0xB0, ID_PIXELHEIGHT = 0xBA,
    ID_FRAMERATE = 0x2383E3, ID_CLUSTER = 0x1F43B675, ID_TIMECODE = 0xE7,
    ID_SIMPLEBLOCK = 0xA3, ID_BLOCKGROUP = 0xA0, ID_BLOCK = 0xA1,
};

// An element id: 1-4 bytes, the length marker kept (as Matroska writes ids).
static int klw_id(const uint8_t *p, const uint8_t *end, uint32_t *id, size_t *len) {
    if (p >= end) return 0;
    unsigned l = 1;
    while (l <= 4 && !(p[0] & (0x80 >> (l - 1)))) l++;
    if (l > 4 || p + l > end) return 0;
    uint32_t v = 0;
    for (unsigned i = 0; i < l; i++) v = (v << 8) | p[i];
    *id = v; *len = l;
    return 1;
}

// A data size: 1-8 bytes, the marker removed. All-ones is "unknown".
static int klw_size(const uint8_t *p, const uint8_t *end, uint64_t *sz, size_t *len,
                    int *unknown) {
    if (p >= end) return 0;
    unsigned l = 1;
    while (l <= 8 && !(p[0] & (0x80 >> (l - 1)))) l++;
    if (l > 8 || p + l > end) return 0;
    uint64_t v = p[0] & (0xFF >> l), all = 0xFF >> l;
    for (unsigned i = 1; i < l; i++) { v = (v << 8) | p[i]; all = (all << 8) | 0xFF; }
    *sz = v; *len = l; *unknown = (v == all);
    return 1;
}

static uint64_t klw_uint(const uint8_t *p, uint64_t n) {
    uint64_t v = 0;
    for (uint64_t i = 0; i < n && i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static double klw_float(const uint8_t *p, uint64_t n) {
    if (n == 4) { uint32_t u = (uint32_t)klw_uint(p, 4); float f; memcpy(&f, &u, 4); return f; }
    if (n == 8) { uint64_t u = klw_uint(p, 8); double d; memcpy(&d, &u, 8); return d; }
    return 0;
}

// One element header: id, payload start and end (clamped to the parent).
typedef struct { uint32_t id; const uint8_t *body, *end; int unknown; } klw_el;

static int klw_next(const uint8_t *p, const uint8_t *end, klw_el *e) {
    size_t il, sl; uint64_t sz; int unk;
    if (!klw_id(p, end, &e->id, &il)) return 0;
    if (!klw_size(p + il, end, &sz, &sl, &unk)) return 0;
    e->body = p + il + sl;
    e->unknown = unk;
    e->end = (unk || sz > (uint64_t)(end - e->body)) ? end : e->body + sz;
    return 1;
}

typedef struct {
    uint64_t number;
    int      video, vp8, w, h;
    double   fps;
    uint64_t default_ns;
} klw_track;

static void klw_track_entry(const uint8_t *p, const uint8_t *end, klw_track *t) {
    klw_el e;
    memset(t, 0, sizeof *t);
    for (; p < end && klw_next(p, end, &e); p = e.end) {
        uint64_t n = (uint64_t)(e.end - e.body);
        switch (e.id) {
        case ID_TRACKNUMBER: t->number = klw_uint(e.body, n); break;
        case ID_TRACKTYPE:   t->video = klw_uint(e.body, n) == 1; break;
        case ID_CODECID:     t->vp8 = n == 5 && !memcmp(e.body, "V_VP8", 5); break;
        case ID_DEFAULTDURATION: t->default_ns = klw_uint(e.body, n); break;
        case ID_VIDEO: {
            klw_el v;
            for (const uint8_t *q = e.body; q < e.end && klw_next(q, e.end, &v); q = v.end) {
                uint64_t vn = (uint64_t)(v.end - v.body);
                if (v.id == ID_PIXELWIDTH)  t->w = (int)klw_uint(v.body, vn);
                if (v.id == ID_PIXELHEIGHT) t->h = (int)klw_uint(v.body, vn);
                if (v.id == ID_FRAMERATE)   t->fps = klw_float(v.body, vn);
            }
            break;
        }
        }
    }
}

static void klw_add(kl_webm *m, const uint8_t *data, size_t len, long long pts_us) {
    if (!len) return;
    if (m->n == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 256;
        klw_frame *f = realloc(m->frames, cap * sizeof *f);
        if (!f) return;
        m->frames = f; m->cap = cap;
    }
    // VP8's own frame tag says whether this is a key frame (bit 0 clear).
    m->frames[m->n++] = (klw_frame){ (size_t)(data - m->map), len, pts_us, !(data[0] & 1) };
}

// A Block or SimpleBlock body: track number, int16 timecode, flags, frame.
static void klw_block(kl_webm *m, const uint8_t *p, const uint8_t *end, uint64_t track,
                      long long cluster_tc, uint64_t scale_ns) {
    uint64_t tn; size_t tl; int unk;
    if (!klw_size(p, end, &tn, &tl, &unk) || tn != track) return;
    p += tl;
    if (end - p < 3) return;
    int16_t rel = (int16_t)((p[0] << 8) | p[1]);
    uint8_t flags = p[2];
    p += 3;
    if (flags & 0x06) { m->laced++; return; }
    long long tc = cluster_tc + rel;
    long long us = (long long)((double)tc * (double)scale_ns / 1000.0);
    klw_add(m, p, (size_t)(end - p), us);
}

static void klw_cluster(kl_webm *m, const uint8_t *p, const uint8_t *end, int unknown,
                        uint64_t track, uint64_t scale_ns, const uint8_t **stop) {
    klw_el e;
    long long tc = 0;
    while (p < end && klw_next(p, end, &e)) {
        // An unknown-size cluster ends where the next top-level element starts.
        if (unknown && (e.id == ID_CLUSTER || e.id == 0x1C53BB6B /* Cues */ ||
                        e.id == 0x1254C367 /* Tags */)) break;
        if (e.id == ID_TIMECODE) tc = (long long)klw_uint(e.body, (uint64_t)(e.end - e.body));
        else if (e.id == ID_SIMPLEBLOCK) klw_block(m, e.body, e.end, track, tc, scale_ns);
        else if (e.id == ID_BLOCKGROUP) {
            klw_el b;
            for (const uint8_t *q = e.body; q < e.end && klw_next(q, e.end, &b); q = b.end)
                if (b.id == ID_BLOCK) klw_block(m, b.body, b.end, track, tc, scale_ns);
        }
        p = e.end;
    }
    *stop = p;
}

kl_webm *kl_webm_open(const char *path) { return kl_webm_open_range(path, 0, 0); }

kl_webm *kl_webm_open_range(const char *path, long long offset, long long size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    uint8_t magic[4] = {0};
    if (offset < 0 || fstat(fd, &st) || offset >= st.st_size) { close(fd); return NULL; }
    if (size <= 0 || offset + size > st.st_size) size = st.st_size - offset;
    if (size < 64 || pread(fd, magic, 4, offset) != 4 || klw_uint(magic, 4) != ID_EBML) {
        close(fd);
        return NULL;
    }
    long long page = getpagesize(), start = offset - offset % page;
    size_t map_len = (size_t)(offset - start + size);
    void *map = mmap(NULL, map_len, PROT_READ, MAP_PRIVATE, fd, (off_t)start);
    close(fd);
    if (map == MAP_FAILED) return NULL;

    kl_webm *m = calloc(1, sizeof *m);
    if (!m) { munmap(map, map_len); return NULL; }
    m->map_base = map;
    m->map_len = map_len;
    m->map = (const uint8_t *)map + (offset - start);
    m->size = (size_t)size;
    const uint8_t *p = m->map, *end = m->map + m->size;

    klw_el e;
    if (!klw_next(p, end, &e) || e.id != ID_EBML) goto fail;
    int doc_ok = 0;
    for (const uint8_t *q = e.body; q < e.end; ) {
        klw_el d;
        if (!klw_next(q, e.end, &d)) break;
        size_t n = (size_t)(d.end - d.body);
        if (d.id == ID_DOCTYPE &&
            ((n >= 4 && !memcmp(d.body, "webm", 4)) || (n >= 8 && !memcmp(d.body, "matroska", 8))))
            doc_ok = 1;
        q = d.end;
    }
    if (!doc_ok) goto fail;
    p = e.end;
    while (p < end && klw_next(p, end, &e) && e.id != ID_SEGMENT) p = e.end;
    if (p >= end || e.id != ID_SEGMENT) goto fail;

    uint64_t scale_ns = 1000000;
    double duration = 0;
    klw_track vt = {0};
    int have_track = 0, saw_video = 0;
    const uint8_t *seg_end = e.end;
    for (p = e.body; p < seg_end && klw_next(p, seg_end, &e); ) {
        if (e.id == ID_INFO) {
            klw_el i;
            for (const uint8_t *q = e.body; q < e.end && klw_next(q, e.end, &i); q = i.end) {
                uint64_t n = (uint64_t)(i.end - i.body);
                if (i.id == ID_TIMECODESCALE) scale_ns = klw_uint(i.body, n);
                if (i.id == ID_DURATION) duration = klw_float(i.body, n);
            }
        } else if (e.id == ID_TRACKS) {
            klw_el t;
            for (const uint8_t *q = e.body; q < e.end && klw_next(q, e.end, &t); q = t.end) {
                if (t.id != ID_TRACKENTRY || have_track) continue;
                klw_track tr;
                klw_track_entry(t.body, t.end, &tr);
                saw_video |= tr.video;
                if (tr.video && tr.vp8) { vt = tr; have_track = 1; }
            }
        } else if (e.id == ID_CLUSTER && have_track) {
            const uint8_t *stop;
            klw_cluster(m, e.body, e.end, e.unknown, vt.number, scale_ns, &stop);
            if (e.unknown) { p = stop; continue; }
        }
        p = e.end;
    }
    if (!have_track) {
        fprintf(stderr, "  [webm] %s: %s\n", path,
                saw_video ? "its video track is not VP8" : "no video track");
        goto fail;
    }
    if (!m->n) {
        fprintf(stderr, "  [webm] %s: VP8 track %llu has no frames\n", path,
                (unsigned long long)vt.number);
        goto fail;
    }
    if (!scale_ns) scale_ns = 1000000;
    m->width = vt.w;
    m->height = vt.h;
    m->duration_us = duration > 0 ? (long long)(duration * (double)scale_ns / 1000.0)
                                  : m->frames[m->n - 1].pts_us;
    if (vt.fps > 0) m->fps = (float)vt.fps;
    else if (vt.default_ns) m->fps = (float)(1e9 / (double)vt.default_ns);
    else if (m->n > 1 && m->frames[m->n - 1].pts_us > m->frames[0].pts_us)
        m->fps = (float)((double)(m->n - 1) * 1e6 /
                         (double)(m->frames[m->n - 1].pts_us - m->frames[0].pts_us));
    fprintf(stderr, "  [webm] %s: VP8 %dx%d, %zu frames, %lld us, %.2f fps%s\n", path,
            m->width, m->height, m->n, m->duration_us, m->fps,
            m->laced ? " (laced blocks skipped)" : "");
    return m;
fail:
    kl_webm_close(m);
    return NULL;
}

void kl_webm_close(kl_webm *m) {
    if (!m) return;
    if (m->map_base) munmap(m->map_base, m->map_len);
    free(m->frames);
    free(m);
}

const char *kl_webm_mime(const kl_webm *m) { (void)m; return "video/x-vnd.on2.vp8"; }

void kl_webm_info(const kl_webm *m, int *w, int *h, long long *duration_us, float *fps) {
    if (w) *w = m ? m->width : 0;
    if (h) *h = m ? m->height : 0;
    if (duration_us) *duration_us = m ? m->duration_us : 0;
    if (fps) *fps = m ? m->fps : 0;
}

long long kl_webm_time_us(const kl_webm *m) {
    return (m && m->cur < m->n) ? m->frames[m->cur].pts_us : -1;
}

int kl_webm_keyframe(const kl_webm *m) {
    return (m && m->cur < m->n) ? m->frames[m->cur].key : 0;
}

long kl_webm_read(const kl_webm *m, unsigned char *buf, unsigned long cap) {
    if (!m || m->cur >= m->n) return -1;
    const klw_frame *f = &m->frames[m->cur];
    if (buf && cap) memcpy(buf, m->map + f->off, f->len < cap ? f->len : cap);
    return (long)f->len;               // full size, AMediaExtractor convention
}

int kl_webm_advance(kl_webm *m) {
    if (!m || m->cur >= m->n) return 0;
    m->cur++;
    return m->cur < m->n;
}

int kl_webm_seek_us(kl_webm *m, long long us) {
    if (!m || !m->n) return 0;
    size_t at = 0;
    for (size_t i = 0; i < m->n; i++) {
        if (m->frames[i].pts_us > us) break;
        if (m->frames[i].key) at = i;
    }
    m->cur = at;
    return 1;
}
