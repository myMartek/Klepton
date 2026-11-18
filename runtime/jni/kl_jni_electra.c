// Unreal Engine 5: com.epicgames.unreal.ElectraVideoDecoderH264
//
// UE5's Electra media plugin, Android backend. Where UE4 shipped MediaPlayer14
// (a whole player, see kl_jni_ue4.c), UE5's Electra splits the job: the engine
// demuxes the MP4 and drives a bare H.264 DECODER through a MediaCodec-shaped
// Java class. Wanderer's intro video reaches exactly this class, and the guest
// calls it in a fixed order — CreateDecoder, ConfigureDecoder, SetOutputSurface,
// Start, then the per-access-unit loop QueueInputBuffer / DequeueOutputBuffer /
// GetOutputBuffer.
//
// The contract is transcribed from the guest's own GetMethodID / GetFieldID
// traffic (klepton-wanderer.log around the FindClass for this class), not from a
// version of Epic's sources — the ids it resolves ARE the contract. It is a
// MediaCodec wearing Electra's names: DequeueInputBuffer hands out an index,
// QueueInputBuffer feeds an access unit at that index, DequeueOutputBuffer says
// whether a frame is ready and at which output index, GetOutputBuffer copies
// that frame out as a byte[].
//
// The decoder is kl_vtdec (VideoToolbox), the same host device UE4's cutscene
// path uses. kl_vtdec is shaped for precisely this — "here is an access unit,
// give me back a frame" — so this file is only the contract and the two format
// conversions the seam needs: the guest's Annex-B/AVCC in, and NV12 out.
//
// See runtime/jni/kl_jni_int.h for the seam and kl_jni_ue4.c's MediaPlayer14 for
// the same pattern at one more remove.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <AudioToolbox/AudioToolbox.h>
#include "kl_jni.h"
#include "kl_env.h"
#include "kl_vtdec.h"   // pulls in <CoreVideo/CoreVideo.h>
#include "kl_jni_int.h"

struct kle_tex;   // UE 4.27's ElectraTextureSample, below
static void kle_tex_push(struct kle_tex *t, CVPixelBufferRef pb, int64_t ts);

// Spelled in the ue4 package: the JNI surface folds every
// `com/epicgames/unreal/` class onto `com/epicgames/ue4/` at its intern funnel
// (kl_jni.c, klj_fold_class_name), so the class the guest asks for as
// com/epicgames/unreal/ElectraVideoDecoderH264 is interned under this name.
#define KLE_H264_CLASS  "com/epicgames/ue4/ElectraVideoDecoderH264"
#define KLE_H265_CLASS  "com/epicgames/ue4/ElectraVideoDecoderH265"

// UE5's Electra registers a decoder class per codec — H264 and H265 — and the
// engine probes BOTH at startup (GetDecoderInformation) to enumerate codec
// support before it picks one. Serving only H264 fatals the run the instant it
// asks H265 for its capabilities (kl_jni aborts on a called method with no host
// implementation). The two classes are the same MediaCodec seam with different
// nested-class names and a different kl_vtdec mime; one set of handlers drives
// both, keyed on the instance's (or, for a constructor, the class's) own name.
// kl_vtdec decodes both video/avc and video/hevc through VideoToolbox, and the
// Annex-B/AVCC framing this file converts is identical for the two codecs.
typedef struct {
    const char *cls;      // decoder class
    const char *params;   // $FCreateParameters
    const char *decinfo;  // $FDecoderInformation
    const char *outfmt;   // $FOutputFormatInfo
    const char *outbuf;   // $FOutputBufferInfo
    const char *mime;     // kl_vtdec mime
} kle_codec;

static const kle_codec KLE_CODEC_H264 = {
    KLE_H264_CLASS, KLE_H264_CLASS "$FCreateParameters",
    KLE_H264_CLASS "$FDecoderInformation", KLE_H264_CLASS "$FOutputFormatInfo",
    KLE_H264_CLASS "$FOutputBufferInfo", "video/avc",
};
static const kle_codec KLE_CODEC_H265 = {
    KLE_H265_CLASS, KLE_H265_CLASS "$FCreateParameters",
    KLE_H265_CLASS "$FDecoderInformation", KLE_H265_CLASS "$FOutputFormatInfo",
    KLE_H265_CLASS "$FOutputBufferInfo", "video/hevc",
};

static const kle_codec *kle_codec_by_class(const char *cls) {
    return (cls && strcmp(cls, KLE_H265_CLASS) == 0) ? &KLE_CODEC_H265
                                                     : &KLE_CODEC_H264;
}

// COLOR_FormatYUV420SemiPlanar — the MediaCodec constant for NV12, which is what
// GetOutputBuffer packs. The guest reads FOutputFormatInfo.ColorFormat and
// unpacks accordingly, so the two must agree.
#define KLE_COLOR_NV12  21

// A frame decoded but not yet handed out. The guest dequeues an output index,
// then reads the format and the bytes for that index, then releases it — so a
// small ring of pixel buffers keyed by index sits between DequeueOutputBuffer
// and ReleaseOutputBuffer. 8 is far more than the one-frame-at-a-time the guest
// actually keeps live.
#define KLE_SLOTS 8

typedef struct {
    const kle_codec *codec;        // which decoder class this instance is
    kl_vtdec        *dec;
    CVPixelBufferRef slots[KLE_SLOTS];
    int64_t          slot_pts[KLE_SLOTS];   // presentation time (us) of each slot
    int              next_slot;
    int              started;
    int              eos;          // QueueEOSInputBuffer seen
    int              fmt_warned;   // GetOutputBuffer's unexpected-format log-once
    struct kle_tex  *tex;          // UE 4.27: the codec surface frames are rendered to
    unsigned         rendered;     // frames handed to that surface (for the log)
    // The answer objects, one per instance and reused: a fresh object per call
    // adds its fields to kl_jni's write table (128 entries) every time the
    // guest polls, and a guest that does not delete each local ref fills it
    // within a second of video and ends the run ("field write table full").
    void            *outbuf_obj, *outfmt_obj, *decinfo_obj;
    // UE 4.27 (CreateDecoder(FCreateParameters)): end of stream the way a
    // MediaCodec says it — the held-back frames are let out, then ONE output
    // buffer with a valid index, no data and bIsEOS. An index of -1 is
    // INFO_TRY_AGAIN_LATER, whose flags the engine does not look at, so it
    // waited for the end of the logo movie for ever.
    int              ue427, eos_sent;
    int64_t          last_pts;
    unsigned         frames_out;
} kl_electra;

static void *kle_answer(kl_electra *e, void **slot, const char *cls) {
    if (!e) return kl_jni_new_object(cls);
    if (!*slot) *slot = klj_host_cache(kl_jni_new_object(cls));
    return *slot;
}

static kl_electra *kle_of(void *self) {
    klj_object *o = klj_as_object(self);
    if (!o) return NULL;
    if (strcmp(o->cls, KLE_H264_CLASS) && strcmp(o->cls, KLE_H265_CLASS))
        return NULL;
    return o->data;
}

// The codec for a handler whose `self` is a decoder instance (every method but
// the two constructors, which get the class object instead). Prefer the payload
// this file stamped at construction, but fall back to the object's own class
// name so a return object is still namespaced correctly even if the instance
// reached us without going through our ctor.
static const kle_codec *kle_codec_of(void *self) {
    kl_electra *e = kle_of(self);
    if (e && e->codec) return e->codec;
    klj_object *o = klj_as_object(self);
    return kle_codec_by_class(o ? o->cls : NULL);
}

// ---- format in: the guest's bitstream -> Annex-B ---------------------------
//
// kl_vtdec wants Annex-B (00 00 00 01 start codes). Electra's CSD and frame
// data may arrive already Annex-B, OR as AVCC (4-byte big-endian length prefix
// per NAL). Detect and convert; a start code at the front means Annex-B and is
// passed through untouched.
static int kle_is_annexb(const uint8_t *p, size_t n) {
    if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return 1;
    if (n >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) return 1;
    return 0;
}

// AVCC (4-byte length-prefixed NALs) -> Annex-B, into a fresh buffer the caller
// frees. Each 4-byte length becomes a 4-byte start code, so the output is the
// same size as the input. Returns 0 on success. A prefix that runs past the end
// (or a zero length) means this was not AVCC after all — reported by a negative
// return so the caller can fall back rather than corrupt the stream.
static int kle_avcc_to_annexb(const uint8_t *in, size_t len,
                              uint8_t **out, size_t *outlen) {
    uint8_t *buf = malloc(len ? len : 1);
    if (!buf) return -1;
    size_t ip = 0, op = 0;
    while (ip + 4 <= len) {
        uint32_t nal = ((uint32_t)in[ip] << 24) | ((uint32_t)in[ip + 1] << 16) |
                       ((uint32_t)in[ip + 2] << 8) | (uint32_t)in[ip + 3];
        ip += 4;
        if (nal == 0 || ip + nal > len) { free(buf); return -1; }
        buf[op++] = 0; buf[op++] = 0; buf[op++] = 0; buf[op++] = 1;
        memcpy(buf + op, in + ip, nal);
        op += nal; ip += nal;
    }
    if (ip != len) { free(buf); return -1; }
    *out = buf; *outlen = op;
    return 0;
}

static void kle_submit(kl_electra *e, const uint8_t *data, size_t len,
                       int64_t pts_us) {
    if (!e || !e->dec || !data || !len) return;
    if (kle_is_annexb(data, len)) {
        kl_vtdec_submit(e->dec, data, len, pts_us);
        return;
    }
    uint8_t *ab = NULL; size_t abl = 0;
    if (kle_avcc_to_annexb(data, len, &ab, &abl) == 0) {
        kl_vtdec_submit(e->dec, ab, abl, pts_us);
        free(ab);
    } else {
        // Neither a recognisable start code nor a clean AVCC framing. Submit as
        // is — kl_vtdec's own NAL scan is the last word on whether it is usable.
        kl_vtdec_submit(e->dec, data, len, pts_us);
    }
}

// A byte[] argument arrives as a klj_array in a[idx].l.
static const uint8_t *kle_bytes(const klj_val *a, int idx, int n, size_t *len) {
    *len = 0;
    if (idx >= n) return NULL;
    klj_array *arr = klj_arr(a[idx].l);
    if (!arr || arr->kind != 'B') return NULL;
    *len = (size_t)arr->len;
    return arr->data;
}

// Store one primitive field on an object the guest will read back with
// Get{Int,Long,Boolean}Field. All three read the .j half (see klj_field_value in
// kl_jni.c), so one setter serves them. The jfieldID is resolved by identity —
// klj_want dedupes, so calling this per frame does not grow the id table.
static void kle_set(void *obj, const char *clsname, const char *field,
                    const char *sig, int64_t v) {
    void *cls = klj_class_object(clsname);
    klj_field_store(obj, klj_want(cls, field, sig, 'f'), (klj_val){.j = (uint64_t)v});
}

// ---- lifecycle -------------------------------------------------------------
static klj_val klj_Electra_ctor(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    // A constructor is dispatched with the class object as `self`.
    const kle_codec *c = kle_codec_by_class(klj_class_name(self));
    kl_electra *e = calloc(1, sizeof *e);
    if (!e) return (klj_val){.l = NULL};
    e->codec = c;
    KLJ_LOG("%s: new decoder", c->cls);
    return (klj_val){.l = klj_new_object_data(c->cls, e)};
}

// The kl_vtdec is created here; ConfigureDecoder's parameter sets (and the
// per-frame QueueCSDInputBuffer) are absorbed by submit, so there is nothing to
// configure the session with up front.
static klj_val klj_Electra_create(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kl_electra *e = kle_of(self);
    if (e && !e->dec) {
        e->dec = kl_vtdec_create(e->codec->mime);
        KLJ_LOG("%s.CreateDecoder -> %s", e->codec->cls,
                e->dec ? "ok" : "no VideoToolbox decoder for that mime");
    }
    return (klj_val){.j = 0};
}

// ConfigureDecoder(FCreateParameters). The parameters carry Width/Height and
// CSD0/CSD1, but Klepton cannot read instance fields the guest wrote (the write
// table is private to kl_jni.c) and does not need to: the CSDs also arrive
// through QueueCSDInputBuffer, and kl_vtdec builds its format description from
// the SPS/PPS in the stream either way. Ensure the decoder exists and return ok.
static klj_val klj_Electra_configure(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kl_electra *e = kle_of(self);
    if (e && !e->dec) e->dec = kl_vtdec_create(e->codec->mime);
    return (klj_val){.j = 0};
}

// We output frames as byte[] buffers, not to a Surface. Ignore the surface.
static klj_val klj_Electra_setSurface(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 0};
}

static klj_val klj_Electra_start(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kl_electra *e = kle_of(self);
    if (e) { e->started = 1; e->eos = 0; e->eos_sent = 0; }
    return (klj_val){.j = 0};
}
static klj_val klj_Electra_stop(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kl_electra *e = kle_of(self);
    if (e) e->started = 0;
    return (klj_val){.j = 0};
}

static void kle_drop_slots(kl_electra *e) {
    for (int i = 0; i < KLE_SLOTS; i++)
        if (e->slots[i]) { CVPixelBufferRelease(e->slots[i]); e->slots[i] = NULL; }
    e->next_slot = 0;
}

static klj_val klj_Electra_flush(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kl_electra *e = kle_of(self);
    if (e) { if (e->dec) kl_vtdec_flush(e->dec); kle_drop_slots(e); e->eos = 0; e->eos_sent = 0; }
    return (klj_val){.j = 0};
}
static klj_val klj_Electra_reset(void *env, void *self, const klj_val *a, int n) {
    return klj_Electra_flush(env, self, a, n);
}

static klj_val klj_Electra_release(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kl_electra *e = kle_of(self);
    if (e) {
        kle_drop_slots(e);
        if (e->dec) { kl_vtdec_destroy(e->dec); e->dec = NULL; }
        e->started = 0;
    }
    return (klj_val){.j = 0};
}

// ---- input -----------------------------------------------------------------
// kl_vtdec takes bytes directly — there is no input-buffer pool to draw an index
// from — so any valid index will do, and the guest hands it straight back to
// QueueInputBuffer.
//
// UE 4.27 feeds as fast as it is given input buffers and takes the output when
// a frame is due, so without back-pressure kl_vtdec's ring (16 frames) ran over
// and dropped the oldest: 34 of the logo movie's ~236 frames were shown. A
// MediaCodec runs out of input buffers instead — answer -1 (try again) while
// enough frames are in flight.
#define KLE_MAX_IN_FLIGHT 8
static klj_val klj_Electra_dequeueInput(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kl_electra *e = kle_of(self);
    if (e && e->ue427 && e->dec && kl_vtdec_pending(e->dec) >= KLE_MAX_IN_FLIGHT)
        return (klj_val){.j = (uint64_t)(int64_t)-1};
    return (klj_val){.j = 0};
}

// QueueCSDInputBuffer(index, timestampUs, csd) — a codec-config buffer. Feeding
// it as an access unit is correct: kl_vtdec absorbs the parameter sets and
// treats a CSD-only submit as a no-op.
static klj_val klj_Electra_queueCSD(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    kl_electra *e = kle_of(self);
    size_t len = 0;
    const uint8_t *csd = kle_bytes(a, 2, n, &len);
    kle_submit(e, csd, len, n > 1 ? (int64_t)a[1].j : 0);
    return (klj_val){.j = 0};
}

// QueueInputBuffer(index, timestampUs, data) — one access unit.
static klj_val klj_Electra_queueInput(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    kl_electra *e = kle_of(self);
    size_t len = 0;
    const uint8_t *data = kle_bytes(a, 2, n, &len);
    kle_submit(e, data, len, n > 1 ? (int64_t)a[1].j : 0);
    return (klj_val){.j = 0};
}

static klj_val klj_Electra_queueEOS(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kl_electra *e = kle_of(self);
    if (e) {
        e->eos = 1;
        if (e->ue427) {
            if (e->dec) kl_vtdec_finish(e->dec);
            unsigned sub = 0, dec = 0, drop = 0;
            if (e->dec) kl_vtdec_stats(e->dec, &sub, &dec, &drop, NULL, NULL);
            KLJ_LOG("%s.QueueEOSInputBuffer after %u frame(s) out (%u submitted, "
                    "%u decoded, %u dropped)", e->codec->cls, e->frames_out, sub, dec, drop);
        }
    }
    return (klj_val){.j = 0};
}

// ---- output ----------------------------------------------------------------
// DequeueOutputBuffer(timeoutMs) -> FOutputBufferInfo. Pull the oldest decoded
// frame; if one is ready, park it in a slot and report that slot as BufferIndex.
// If none, report BufferIndex = -1 (MediaCodec's INFO_TRY_AGAIN_LATER), carrying
// bIsEOS once the guest has signalled end of stream and the decoder has drained.
static klj_val klj_Electra_dequeueOutput(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kl_electra *e = kle_of(self);
    const char *ob = kle_codec_of(self)->outbuf;
    void *info = kle_answer(e, e ? &e->outbuf_obj : NULL, ob);

    int64_t pts = 0;
    CVPixelBufferRef pb = (e && e->dec) ? kl_vtdec_pull(e->dec, &pts) : NULL;
    if (pb) {
        // pts is the guest's own timestamp round-tripped through VideoToolbox.
        int slot = e->next_slot;
        if (e->slots[slot]) CVPixelBufferRelease(e->slots[slot]);
        e->slots[slot] = pb;
        e->slot_pts[slot] = pts;
        e->next_slot = (slot + 1) % KLE_SLOTS;
        e->last_pts = pts;
        e->frames_out++;

        int w = (int)CVPixelBufferGetWidth(pb);
        int h = (int)CVPixelBufferGetHeight(pb);
        int size = w * h * 3 / 2;    // NV12
        kle_set(info, ob, "BufferIndex", "I", slot);
        kle_set(info, ob, "Size", "I", size);
        kle_set(info, ob, "PresentationTimestamp", "J", pts);
        kle_set(info, ob, "bIsEOS", "Z", 0);
        kle_set(info, ob, "bIsConfig", "Z", 0);
    } else if (e && e->ue427 && e->eos && !e->eos_sent &&
               (!e->dec || kl_vtdec_pending(e->dec) == 0)) {
        // The EOS buffer: an empty slot's index, released like any other.
        int slot = e->next_slot;
        if (e->slots[slot]) { CVPixelBufferRelease(e->slots[slot]); e->slots[slot] = NULL; }
        e->eos_sent = 1;
        KLJ_LOG("%s: end of stream handed out (index %d) after %u frame(s)",
                e->codec->cls, slot, e->frames_out);
        kle_set(info, ob, "BufferIndex", "I", slot);
        kle_set(info, ob, "Size", "I", 0);
        kle_set(info, ob, "PresentationTimestamp", "J", e->last_pts);
        kle_set(info, ob, "bIsEOS", "Z", 1);
        kle_set(info, ob, "bIsConfig", "Z", 0);
    } else {
        kle_set(info, ob, "BufferIndex", "I", -1);
        kle_set(info, ob, "Size", "I", 0);
        kle_set(info, ob, "PresentationTimestamp", "J", 0);
        kle_set(info, ob, "bIsEOS", "Z", (e && e->eos && !e->ue427) ? 1 : 0);
        kle_set(info, ob, "bIsConfig", "Z", 0);
    }
    return (klj_val){.l = info};
}

static CVPixelBufferRef kle_slot(kl_electra *e, int idx) {
    if (!e || idx < 0 || idx >= KLE_SLOTS) return NULL;
    return e->slots[idx];
}

// GetOutputBuffer(index) -> byte[] in NV12 (COLOR_FormatYUV420SemiPlanar): the
// full Y plane (width*height) followed by the interleaved CbCr plane
// (width*height/2), copied out of the biplanar 4:2:0 CVPixelBuffer honouring
// each plane's own bytes-per-row.
static klj_val klj_Electra_getOutputBuffer(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    kl_electra *e = kle_of(self);
    CVPixelBufferRef pb = kle_slot(e, n > 0 ? (int)a[0].j : -1);
    if (!pb) return (klj_val){.l = klj_new_array('B', NULL, 0)};

    size_t w = CVPixelBufferGetWidth(pb);
    size_t h = CVPixelBufferGetHeight(pb);
    size_t ysize = w * h;
    size_t csize = w * h / 2;
    void      *arrobj = klj_new_array('B', NULL, (int)(ysize + csize));
    klj_array *arr = klj_arr(arrobj);
    uint8_t   *dst = arr ? arr->data : NULL;
    if (!dst) return (klj_val){.l = arrobj};

    OSType fmt = CVPixelBufferGetPixelFormatType(pb);
    if (fmt == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
        fmt == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
        CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
        const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
        size_t ystride = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
        size_t yh      = CVPixelBufferGetHeightOfPlane(pb, 0);
        for (size_t r = 0; r < h && r < yh; r++)
            memcpy(dst + r * w, y + r * ystride, w);
        // Plane 1 is interleaved CbCr at half height; each row is w bytes
        // (w/2 chroma pairs * 2), which is exactly NV12's chroma row.
        const uint8_t *c = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
        size_t cstride = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
        size_t ch      = CVPixelBufferGetHeightOfPlane(pb, 1);
        uint8_t *cdst = dst + ysize;
        for (size_t r = 0; r < h / 2 && r < ch; r++)
            memcpy(cdst + r * w, c + r * cstride, w);
        CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    } else if (e && !e->fmt_warned) {
        e->fmt_warned = 1;
        KLJ_LOG("%s.GetOutputBuffer: pixel format 0x%x is not "
                "biplanar NV12 — returning a zero frame (needs an on-device "
                "iteration to wrap the private format)", e->codec->cls,
                (unsigned)fmt);
    }
    // Non-NV12 falls through with the calloc'd (zero) buffer, which the guest
    // renders as a black frame rather than crashing on.
    return (klj_val){.l = arrobj};
}

// GetOutputFormatInfo(index) -> FOutputFormatInfo. Dimensions come from the slot
// when it holds a frame, else from the decoder's running stats. Crops are 0 and
// the color format is NV12; Stride is the Y-plane row and SliceHeight the height.
static klj_val klj_Electra_getOutputFormat(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    kl_electra *e = kle_of(self);
    const char *of = kle_codec_of(self)->outfmt;
    void *info = kle_answer(e, e ? &e->outfmt_obj : NULL, of);
    int w = 0, h = 0;
    CVPixelBufferRef pb = kle_slot(e, n > 0 ? (int)a[0].j : -1);
    if (pb) {
        w = (int)CVPixelBufferGetWidth(pb);
        h = (int)CVPixelBufferGetHeight(pb);
    } else if (e && e->dec) {
        kl_vtdec_stats(e->dec, NULL, NULL, NULL, &w, &h);
    }
    kle_set(info, of, "Width", "I", w);
    kle_set(info, of, "Height", "I", h);
    kle_set(info, of, "CropTop", "I", 0);
    kle_set(info, of, "CropBottom", "I", (e && e->ue427 && h > 0) ? h - 1 : 0);
    kle_set(info, of, "CropLeft", "I", 0);
    // Android's crop rectangle is INCLUSIVE: crop-right is width-1. UE 4.27
    // takes the picture as CropRight-CropLeft+1 wide, so 0 made it one pixel:
    // the logo movie showed as a plain dark-red square, its top-left texel.
    kle_set(info, of, "CropRight", "I", (e && e->ue427 && w > 0) ? w - 1 : 0);
    kle_set(info, of, "Stride", "I", w);
    kle_set(info, of, "SliceHeight", "I", h);
    kle_set(info, of, "ColorFormat", "I", KLE_COLOR_NV12);
    return (klj_val){.l = info};
}

// ReleaseOutputBuffer(index, render, ts) — free the frame at that index.
static klj_val klj_Electra_releaseOutput(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    kl_electra *e = kle_of(self);
    int idx = n > 0 ? (int)a[0].j : -1;
    if (e && idx >= 0 && idx < KLE_SLOTS && e->slots[idx]) {
        // UE 4.27 renders through a codec surface (its ElectraTextureSample):
        // ReleaseOutputBuffer(index, render=true, time) is the frame going to
        // that surface, where GetVideoFrameUpdateInfo picks it up.
        int render = n > 1 && (a[1].j & 0xff) != 0;
        if (render && e->tex) {
            // A surface stamps a frame with the release time it was given,
            // or with the buffer's presentation time (in ns) when that is 0.
            int64_t ts = n > 2 ? (int64_t)a[2].j : 0;
            if (ts <= 0) ts = e->slot_pts[idx] * 1000;   // -1: "no release time"
            if (e->rendered++ < 3 || e->rendered % 120 == 0)
                KLJ_LOG("%s.ReleaseOutputBuffer(%d, render, %lld) -> codec surface",
                        e->codec->cls, idx, (long long)ts);
            kle_tex_push(e->tex, e->slots[idx], ts);
        }
        CVPixelBufferRelease(e->slots[idx]);
        e->slots[idx] = NULL;
    }
    return (klj_val){.j = 0};
}

// GetDecoderInformation -> FDecoderInformation. Not adaptive, API level 29 (the
// Quest's, matching the rest of Klepton's Android surface), and no
// SetOutputSurface support — we decode to byte[] buffers, not a surface.
static klj_val klj_Electra_decoderInfo(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    const char *di = kle_codec_of(self)->decinfo;
    kl_electra *e = kle_of(self);
    void *info = kle_answer(e, e ? &e->decinfo_obj : NULL, di);
    kle_set(info, di, "bIsAdaptive", "Z", 0);
    kle_set(info, di, "ApiLevel", "I", 29);
    kle_set(info, di, "bCanUse_SetOutputSurface", "Z", 0);
    return (klj_val){.l = info};
}

// The guest news up an FCreateParameters, sets its fields, and passes it to
// ConfigureDecoder. Hand back a bare object for it to populate.
static klj_val klj_Electra_params_ctor(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    // Dispatched with the FCreateParameters class object as `self`; hand back a
    // bare instance of exactly that class (H264's or H265's) for the guest to
    // populate. The fields are absorbed elsewhere, so its identity is all that
    // matters here.
    const char *cls = klj_class_name(self);
    return (klj_val){.l = kl_jni_new_object(cls ? cls : KLE_CODEC_H264.params)};
}

// ============================================================================
// Unreal Engine 4.27's Electra (com.epicgames.ue4.*)
//
// UE 4.27 ships the same Java decoder class under the ue4 package, with one
// difference in the contract and two classes UE5's buffer path never needs.
// Transcribed from the guest's classes.dex method and field ids (Journey to
// Foundation, x4822681107767272, whose start-up logo movie is played through
// Electra and whose start waits behind it):
//
//   - ElectraVideoDecoderH264.CreateDecoder(FCreateParameters)I creates AND
//     configures in one call (UE5 splits it into CreateDecoder()I and
//     ConfigureDecoder). Its VideoCodecSurface field is the Surface of an
//     ElectraTextureSample; frames reach that surface through
//     ReleaseOutputBuffer(index, render=true, time).
//   - ElectraTextureSample: Initialize(Z bVulkan), GetCodecSurface,
//     GetVideoFrameUpdateInfo(destTexture, width, height), Release. On Android
//     it is a SurfaceTexture the engine reads back: on Vulkan as packed RGBA in
//     FFrameUpdateInfo.Buffer, on GLES drawn into destTexture.
//   - ElectraAudioDecoderAAC: a MediaCodec for AAC. CreateDecoder(channels,
//     rate, AudioSpecificConfig), QueueInputBuffer per access unit,
//     DequeueOutputBuffer / GetOutputFormatInfo / GetOutputBufferAndRelease
//     hand out 16-bit interleaved PCM. Decoded here with AudioToolbox.
//
// Before these existed every call answered zero (KL_JNI_SOFT_UNKNOWN): the
// audio decoder's DequeueOutputBuffer returned null, the player stopped with
// an error at once, and the guest sat on a black screen behind a movie that
// never ended.
#define KLE_TEX_CLASS   "com/epicgames/ue4/ElectraTextureSample"
#define KLE_TEX_INFO    KLE_TEX_CLASS "$FFrameUpdateInfo"
#define KLE_AAC_CLASS   "com/epicgames/ue4/ElectraAudioDecoderAAC"
#define KLE_AAC_OUTBUF  KLE_AAC_CLASS "$FOutputBufferInfo"
#define KLE_AAC_OUTFMT  KLE_AAC_CLASS "$FOutputFormatInfo"

static void kle_setf(void *obj, const char *clsname, const char *field, double v) {
    void *cls = klj_class_object(clsname);
    klj_field_store(obj, klj_want(cls, field, "F", 'f'), (klj_val){.d = v});
}
static void kle_setl(void *obj, const char *clsname, const char *field,
                     const char *sig, void *v) {
    void *cls = klj_class_object(clsname);
    klj_field_store(obj, klj_want(cls, field, sig, 'f'), (klj_val){.l = v});
}

// ---- ElectraTextureSample ---------------------------------------------------
#define KLE_TEX_QUEUE 4
typedef struct kle_tex {
    pthread_mutex_t  mu;
    int              vulkan;
    void            *surface;      // the android/view/Surface handed out (pinned)
    CVPixelBufferRef q[KLE_TEX_QUEUE];
    int64_t          q_ts[KLE_TEX_QUEUE];
    int              q_n;
    uint8_t         *rgba;         // the last frame converted, w*h*4
    int              rgba_w, rgba_h;
    void            *buffer;       // direct ByteBuffer over rgba (pinned)
    void            *info;         // the one FFrameUpdateInfo handed back (pinned)
    int64_t          last_ts;
    unsigned         delivered, asked;
    int              bgra;         // KL_ELECTRA_BGRA: hand out BGRA instead
} kle_tex;

// Surfaces handed out, so CreateDecoder can find the sample behind one.
#define KLE_MAX_TEX 8
static pthread_mutex_t g_kle_tex_mu = PTHREAD_MUTEX_INITIALIZER;
static kle_tex *g_kle_tex[KLE_MAX_TEX];

static kle_tex *kle_tex_of(void *self) {
    klj_object *o = klj_as_object(self);
    return (o && strcmp(o->cls, KLE_TEX_CLASS) == 0) ? o->data : NULL;
}

static kle_tex *kle_tex_by_surface(void *surface) {
    if (!surface) return NULL;
    kle_tex *t = NULL;
    pthread_mutex_lock(&g_kle_tex_mu);
    for (int i = 0; i < KLE_MAX_TEX; i++)
        if (g_kle_tex[i] && g_kle_tex[i]->surface == surface) { t = g_kle_tex[i]; break; }
    pthread_mutex_unlock(&g_kle_tex_mu);
    return t;
}

static void kle_tex_clear_locked(kle_tex *t) {
    for (int i = 0; i < t->q_n; i++) CVPixelBufferRelease(t->q[i]);
    t->q_n = 0;
}

static void kle_tex_push(kle_tex *t, CVPixelBufferRef pb, int64_t ts) {
    if (!t || !pb) return;
    pthread_mutex_lock(&t->mu);
    if (t->q_n == KLE_TEX_QUEUE) {     // a surface keeps the newest frames
        CVPixelBufferRelease(t->q[0]);
        memmove(t->q, t->q + 1, (KLE_TEX_QUEUE - 1) * sizeof t->q[0]);
        memmove(t->q_ts, t->q_ts + 1, (KLE_TEX_QUEUE - 1) * sizeof t->q_ts[0]);
        t->q_n--;
    }
    t->q[t->q_n] = CVPixelBufferRetain(pb);
    t->q_ts[t->q_n++] = ts;
    pthread_mutex_unlock(&t->mu);
}

static inline uint8_t kle_clamp(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

// NV12 (video or full range, BT.709 for HD else BT.601) -> packed RGBA (or
// BGRA), w x h, sampling the source with nearest neighbour when the guest
// asks for another size than the frame has.
static void kle_nv12_to_rgba(CVPixelBufferRef pb, uint8_t *dst, int w, int h, int bgra) {
    OSType fmt = CVPixelBufferGetPixelFormatType(pb);
    int full = fmt == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
    if (fmt != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange && !full) {
        memset(dst, 0, (size_t)w * h * 4);
        return;
    }
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    const uint8_t *yp = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
    const uint8_t *cp = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
    size_t cs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    int sw = (int)CVPixelBufferGetWidthOfPlane(pb, 0);
    int sh = (int)CVPixelBufferGetHeightOfPlane(pb, 0);
    int hd = sh >= 720;
    // 16.16 fixed point coefficients.
    int cy  = full ? 65536 : 76309;                 // 255/219
    int crv = hd ? (full ? 103206 : 117489) : (full ? 91881 : 104597);
    int cgu = hd ? (full ? 12276 : 13975)  : (full ? 22554 : 25675);
    int cgv = hd ? (full ? 30679 : 34925)  : (full ? 46802 : 53279);
    int cbu = hd ? (full ? 121609 : 138438) : (full ? 116130 : 132201);
    int ri = bgra ? 2 : 0, bi = bgra ? 0 : 2;
    for (int r = 0; r < h; r++) {
        int sy = (int)((int64_t)r * sh / h);
        const uint8_t *yr = yp + (size_t)sy * ys;
        const uint8_t *cr = cp + (size_t)(sy / 2) * cs;
        uint8_t *o = dst + (size_t)r * w * 4;
        for (int c = 0; c < w; c++) {
            int sx = (w == sw) ? c : (int)((int64_t)c * sw / w);
            int Y = yr[sx] - (full ? 0 : 16);
            int U = cr[(sx & ~1)] - 128, V = cr[(sx & ~1) + 1] - 128;
            int yy = Y * cy;
            o[ri] = kle_clamp((yy + crv * V + 32768) >> 16);
            o[1]  = kle_clamp((yy - cgu * U - cgv * V + 32768) >> 16);
            o[bi] = kle_clamp((yy + cbu * U + 32768) >> 16);
            o[3]  = 255;
            o += 4;
        }
    }
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
}

static klj_val klj_ElectraTex_ctor(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    kle_tex *t = calloc(1, sizeof *t);
    if (!t) return (klj_val){.l = NULL};
    pthread_mutex_init(&t->mu, NULL);
    t->bgra = kl_env_on("KL_ELECTRA_BGRA", 0);
    return (klj_val){.l = klj_new_object_data(KLE_TEX_CLASS, t)};
}

static klj_val klj_ElectraTex_init(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    kle_tex *t = kle_tex_of(self);
    if (!t) return (klj_val){.j = 0};
    t->vulkan = n > 0 && (a[0].j & 0xff) != 0;
    if (!t->surface) {
        t->surface = klj_host_cache(kl_jni_new_object("android/view/Surface"));
        pthread_mutex_lock(&g_kle_tex_mu);
        for (int i = 0; i < KLE_MAX_TEX; i++)
            if (!g_kle_tex[i]) { g_kle_tex[i] = t; break; }
        pthread_mutex_unlock(&g_kle_tex_mu);
    }
    KLJ_LOG("ElectraTextureSample.Initialize(vulkan=%d): codec surface %p",
            t->vulkan, t->surface);
    return (klj_val){.j = 0};
}

static klj_val klj_ElectraTex_surface(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kle_tex *t = kle_tex_of(self);
    return (klj_val){.l = t ? t->surface : NULL};
}

static klj_val klj_ElectraTex_release(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kle_tex *t = kle_tex_of(self);
    if (!t) return (klj_val){.j = 0};
    pthread_mutex_lock(&t->mu);
    kle_tex_clear_locked(t);
    pthread_mutex_unlock(&t->mu);
    KLJ_LOG("ElectraTextureSample.Release: %u frame(s) handed to the engine", t->delivered);
    // The surface stays registered (and the sample allocated): a decoder may
    // still hold it, and a push into a released sample is only a dropped frame.
    return (klj_val){.j = 0};
}

// GetVideoFrameUpdateInfo(destTexture, width, height). Like SurfaceTexture's
// updateTexImage: take the oldest frame rendered to the surface, if any.
static klj_val klj_ElectraTex_update(void *env, void *self, const klj_val *a, int n) {
    kle_tex *t = kle_tex_of(self);
    if (!t) return (klj_val){.l = NULL};
    uint32_t dest = n > 0 ? (uint32_t)a[0].j : 0;
    int w = n > 1 ? (int)(int32_t)a[1].j : 0, h = n > 2 ? (int)(int32_t)a[2].j : 0;
    if (!t->info) t->info = klj_host_cache(kl_jni_new_object(KLE_TEX_INFO));
    void *info = t->info;

    pthread_mutex_lock(&t->mu);
    CVPixelBufferRef pb = NULL;
    int64_t ts = t->last_ts;
    if (t->q_n) {
        pb = t->q[0]; ts = t->q_ts[0];
        memmove(t->q, t->q + 1, (t->q_n - 1) * sizeof t->q[0]);
        memmove(t->q_ts, t->q_ts + 1, (t->q_n - 1) * sizeof t->q_ts[0]);
        t->q_n--;
    }
    int pending = t->q_n;
    pthread_mutex_unlock(&t->mu);

    int ready = 0;
    if (pb) {
        if (w <= 0 || h <= 0) { w = (int)CVPixelBufferGetWidth(pb); h = (int)CVPixelBufferGetHeight(pb); }
        if (w != t->rgba_w || h != t->rgba_h || !t->rgba) {
            free(t->rgba);
            t->rgba = malloc((size_t)w * h * 4);
            t->rgba_w = t->rgba ? w : 0; t->rgba_h = t->rgba ? h : 0;
            t->buffer = t->rgba
                ? klj_host_cache(klj_NewDirectByteBuffer(env, t->rgba, (int64_t)w * h * 4))
                : NULL;
        }
        if (t->rgba) {
            kle_nv12_to_rgba(pb, t->rgba, w, h, t->bgra);
            if (!t->vulkan && dest) {
                extern int kl_glfb_upload_picture(uint32_t, int, int, const void *, int);
                kl_glfb_upload_picture(dest, w, h, t->rgba, t->bgra);
            }
            ready = 1;
            t->last_ts = ts;
            if (t->delivered++ < 3 || t->delivered % 60 == 0) {
                // The picture's mean colour, so a black or flat frame on the
                // headset can be told apart from a black or flat movie.
                uint64_t sum[3] = {0};
                size_t np = (size_t)w * h, cnt = 0;
                for (size_t i = 0; i < np; i += 97, cnt++)
                    for (int k = 0; k < 3; k++) sum[k] += t->rgba[i * 4 + k];
                KLJ_LOG("ElectraTextureSample.GetVideoFrameUpdateInfo(%u, %d, %d): frame "
                        "%dx%d ts %lld, %d pending, mean %llu/%llu/%llu", dest, w, h,
                        (int)CVPixelBufferGetWidth(pb), (int)CVPixelBufferGetHeight(pb),
                        (long long)ts, pending,
                        (unsigned long long)(sum[0] / (cnt ? cnt : 1)),
                        (unsigned long long)(sum[1] / (cnt ? cnt : 1)),
                        (unsigned long long)(sum[2] / (cnt ? cnt : 1)));
            }
        }
        CVPixelBufferRelease(pb);
    } else if (t->asked++ == 0) {
        KLJ_LOG("ElectraTextureSample.GetVideoFrameUpdateInfo(%u, %d, %d): no frame yet",
                dest, w, h);
    }
    kle_setl(info, KLE_TEX_INFO, "Buffer", "Ljava/nio/Buffer;",
             (ready && t->vulkan) ? t->buffer : NULL);
    kle_set(info, KLE_TEX_INFO, "Timestamp", "J", ts);
    kle_set(info, KLE_TEX_INFO, "Duration", "J", 0);
    kle_set(info, KLE_TEX_INFO, "bFrameReady", "Z", ready);
    kle_set(info, KLE_TEX_INFO, "bRegionChanged", "Z", 0);
    kle_setf(info, KLE_TEX_INFO, "UScale", 1.0);
    kle_setf(info, KLE_TEX_INFO, "UOffset", 0.0);
    kle_setf(info, KLE_TEX_INFO, "VScale", 1.0);
    kle_setf(info, KLE_TEX_INFO, "VOffset", 0.0);
    kle_set(info, KLE_TEX_INFO, "NumPending", "I", pending);
    return (klj_val){.l = info};
}

// ---- ElectraVideoDecoderH264.CreateDecoder(FCreateParameters)I --------------
static klj_val klj_Electra_create427(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    kl_electra *e = kle_of(self);
    if (!e) return (klj_val){.j = 1};
    void *params = n > 0 ? a[0].l : NULL;
    void *surface = NULL;
    int width = 0, height = 0;
    if (params) {
        const char *pc = e->codec->params;
        void *cls = klj_class_object(pc);
        surface = klj_field_written(params, klj_want(cls, "VideoCodecSurface",
                                                     "Landroid/view/Surface;", 'f')).l;
        width  = (int)(int32_t)klj_field_written(params, klj_want(cls, "Width", "I", 'f')).j;
        height = (int)(int32_t)klj_field_written(params, klj_want(cls, "Height", "I", 'f')).j;
    }
    if (!e->dec) {
        e->dec = kl_vtdec_create(e->codec->mime);
        if (e->dec) {
            // Frames are read on the CPU (converted for the texture sample, or
            // copied by GetOutputBuffer), and a MediaCodec hands them out in
            // presentation order.
            kl_vtdec_want_nv12(e->dec, 1);
            kl_vtdec_want_reorder(e->dec, 1);
        }
    }
    e->tex = kle_tex_by_surface(surface);
    e->ue427 = 1;
    KLJ_LOG("%s.CreateDecoder(%dx%d, surface %p%s) -> %s", e->codec->cls, width, height,
            surface, surface ? (e->tex ? " = an ElectraTextureSample" : " (unknown)") : "",
            e->dec ? "ok" : "no VideoToolbox decoder for that mime");
    return (klj_val){.j = e->dec ? 0 : 1};
}

// ---- ElectraAudioDecoderAAC ------------------------------------------------
#define KLE_AAC_SLOTS 16
typedef struct {
    AudioConverterRef conv;
    int      channels, rate, sbr;
    int      started, eos, eos_sent;
    // the access unit being fed to the converter
    const uint8_t *in; uint32_t in_len; int in_used;
    AudioStreamPacketDescription pd;
    // decoded, not yet taken by the guest
    uint8_t *pcm[KLE_AAC_SLOTS];
    int      pcm_len[KLE_AAC_SLOTS];
    int64_t  pcm_pts[KLE_AAC_SLOTS];
    int      head, count;
    void    *outbuf, *outfmt;          // reused answer objects (pinned)
    unsigned decoded, errors;
} kle_aac;

static kle_aac *kle_aac_of(void *self) {
    klj_object *o = klj_as_object(self);
    return (o && strcmp(o->cls, KLE_AAC_CLASS) == 0) ? o->data : NULL;
}

static OSStatus kle_aac_feed(AudioConverterRef c, UInt32 *npk, AudioBufferList *bl,
                             AudioStreamPacketDescription **pd, void *ud) {
    (void)c;
    kle_aac *d = ud;
    if (d->in_used || !d->in_len) { *npk = 0; return 1; }   // "no more for now"
    bl->mNumberBuffers = 1;
    bl->mBuffers[0].mData = (void *)d->in;
    bl->mBuffers[0].mDataByteSize = d->in_len;
    bl->mBuffers[0].mNumberChannels = (UInt32)d->channels;
    d->pd = (AudioStreamPacketDescription){0, 0, d->in_len};
    if (pd) *pd = &d->pd;
    *npk = 1;
    d->in_used = 1;
    return noErr;
}

static void kle_aac_drop(kle_aac *d) {
    for (int i = 0; i < KLE_AAC_SLOTS; i++) { free(d->pcm[i]); d->pcm[i] = NULL; }
    d->head = d->count = 0;
}

static klj_val klj_ElectraAAC_ctor(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    kle_aac *d = calloc(1, sizeof *d);
    return (klj_val){.l = d ? klj_new_object_data(KLE_AAC_CLASS, d) : NULL};
}

// CreateDecoder(numChannels, sampleRate, csd) -> 0 ok, 1 error.
static klj_val klj_ElectraAAC_create(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    kle_aac *d = kle_aac_of(self);
    if (!d) return (klj_val){.j = 1};
    if (d->conv) { AudioConverterDispose(d->conv); d->conv = NULL; }
    d->channels = n > 0 ? (int)(int32_t)a[0].j : 2;
    d->rate     = n > 1 ? (int)(int32_t)a[1].j : 48000;
    size_t clen = 0;
    const uint8_t *csd = kle_bytes(a, 2, n, &clen);
    // AudioSpecificConfig: 5 bits object type (5 = SBR, 29 = PS: HE-AAC).
    int aot = clen ? (csd[0] >> 3) : 2;
    d->sbr = aot == 5 || aot == 29;
    if (d->channels <= 0) d->channels = 2;
    if (d->rate <= 0) d->rate = 48000;
    AudioStreamBasicDescription in = {
        .mSampleRate = d->rate, .mFormatID = d->sbr ? kAudioFormatMPEG4AAC_HE : kAudioFormatMPEG4AAC,
        .mFramesPerPacket = d->sbr ? 2048 : 1024, .mChannelsPerFrame = (UInt32)d->channels,
    };
    AudioStreamBasicDescription out = {
        .mSampleRate = d->rate, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
        .mFramesPerPacket = 1, .mChannelsPerFrame = (UInt32)d->channels,
        .mBitsPerChannel = 16, .mBytesPerFrame = 2 * (UInt32)d->channels,
        .mBytesPerPacket = 2 * (UInt32)d->channels,
    };
    OSStatus st = AudioConverterNew(&in, &out, &d->conv);
    if (st == noErr && clen) {
        // Optional: a raw AudioSpecificConfig is not always taken as a cookie;
        // AAC-LC decodes from the stream description alone.
        (void)AudioConverterSetProperty(d->conv, kAudioConverterDecompressionMagicCookie,
                                        (UInt32)clen, csd);
    }
    KLJ_LOG("ElectraAudioDecoderAAC.CreateDecoder(%d ch, %d Hz, %zu byte csd, aot %d) -> %s (%d)",
            d->channels, d->rate, clen, aot, st == noErr ? "ok" : "AudioConverterNew failed",
            (int)st);
    if (st != noErr) { d->conv = NULL; return (klj_val){.j = 1}; }
    return (klj_val){.j = 0};
}

static klj_val klj_ElectraAAC_start(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kle_aac *d = kle_aac_of(self);
    if (d) { d->started = 1; d->eos = d->eos_sent = 0; }
    return (klj_val){.j = 0};
}
static klj_val klj_ElectraAAC_stop(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kle_aac *d = kle_aac_of(self);
    if (d) d->started = 0;
    return (klj_val){.j = 0};
}
static klj_val klj_ElectraAAC_flush(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kle_aac *d = kle_aac_of(self);
    if (d) {
        kle_aac_drop(d);
        if (d->conv) AudioConverterReset(d->conv);
        d->eos = d->eos_sent = 0;
    }
    return (klj_val){.j = 0};
}
static klj_val klj_ElectraAAC_release(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kle_aac *d = kle_aac_of(self);
    if (d) {
        kle_aac_drop(d);
        if (d->conv) { AudioConverterDispose(d->conv); d->conv = NULL; }
        KLJ_LOG("ElectraAudioDecoderAAC.ReleaseDecoder: %u access unit(s) decoded, %u error(s)",
                d->decoded, d->errors);
    }
    return (klj_val){.j = 0};
}

// DequeueInputBuffer(timeout) -> an input index, or -1 (try again) while the
// decoded queue is full.
static klj_val klj_ElectraAAC_dequeueInput(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kle_aac *d = kle_aac_of(self);
    if (!d || d->count >= KLE_AAC_SLOTS) return (klj_val){.j = (uint64_t)(int64_t)-1};
    return (klj_val){.j = 0};
}

// QueueInputBuffer(index, ptsUs, accessUnit): decode it now. An empty access
// unit is end of stream.
static klj_val klj_ElectraAAC_queueInput(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    kle_aac *d = kle_aac_of(self);
    if (!d) return (klj_val){.j = 1};
    size_t len = 0;
    const uint8_t *au = kle_bytes(a, 2, n, &len);
    int64_t pts = n > 1 ? (int64_t)a[1].j : 0;
    if (!au || !len) {
        KLJ_LOG("ElectraAudioDecoderAAC: end of stream after %u access unit(s)", d->decoded);
        d->eos = 1;
        return (klj_val){.j = 0};
    }
    if (!d->conv || d->count >= KLE_AAC_SLOTS) { d->errors++; return (klj_val){.j = 0}; }

    UInt32 frames = d->sbr ? 2048 : 1024;
    int bytes = (int)frames * 2 * d->channels;
    uint8_t *pcm = malloc((size_t)bytes);
    if (!pcm) return (klj_val){.j = 1};
    d->in = au; d->in_len = (uint32_t)len; d->in_used = 0;
    AudioBufferList bl = {1, {{(UInt32)d->channels, (UInt32)bytes, pcm}}};
    UInt32 got = frames;
    OSStatus st = AudioConverterFillComplexBuffer(d->conv, kle_aac_feed, d, &got, &bl, NULL);
    d->in = NULL; d->in_len = 0;
    if ((st != noErr && st != 1) || !got) {
        if (d->errors++ < 3)
            KLJ_LOG("ElectraAudioDecoderAAC: access unit of %zu bytes at %lld us -> %u "
                    "frames (status %d)", len, (long long)pts, (unsigned)got, (int)st);
        free(pcm);
        return (klj_val){.j = 0};
    }
    int slot = (d->head + d->count) % KLE_AAC_SLOTS;
    d->pcm[slot] = pcm;
    d->pcm_len[slot] = (int)(got * 2 * (UInt32)d->channels);
    d->pcm_pts[slot] = pts;
    d->count++;
    if (++d->decoded % 250 == 0)
        KLJ_LOG("ElectraAudioDecoderAAC: %u access units decoded (at %lld us)",
                d->decoded, (long long)pts);
    return (klj_val){.j = 0};
}

// DequeueOutputBuffer(timeout) -> FOutputBufferInfo: the oldest decoded buffer
// (its slot is the index), or index -1 (INFO_TRY_AGAIN_LATER); at end of stream
// once drained, one empty buffer flagged bIsEOS.
static klj_val klj_ElectraAAC_dequeueOutput(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kle_aac *d = kle_aac_of(self);
    if (!d) return (klj_val){.l = NULL};
    if (!d->outbuf) d->outbuf = klj_host_cache(kl_jni_new_object(KLE_AAC_OUTBUF));
    void *info = d->outbuf;
    int idx = -1, size = 0, eos = 0;
    int64_t pts = 0;
    if (d->count) {
        idx = d->head; size = d->pcm_len[idx]; pts = d->pcm_pts[idx];
    } else if (d->eos && !d->eos_sent) {
        idx = KLE_AAC_SLOTS; eos = 1; d->eos_sent = 1;   // an index with no data
        KLJ_LOG("ElectraAudioDecoderAAC: end of stream handed out");
    }
    kle_set(info, KLE_AAC_OUTBUF, "BufferIndex", "I", idx);
    kle_set(info, KLE_AAC_OUTBUF, "PresentationTimestamp", "J", pts);
    kle_set(info, KLE_AAC_OUTBUF, "Size", "I", size);
    kle_set(info, KLE_AAC_OUTBUF, "bIsEOS", "Z", eos);
    kle_set(info, KLE_AAC_OUTBUF, "bIsConfig", "Z", 0);
    return (klj_val){.l = info};
}

static klj_val klj_ElectraAAC_outputFormat(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)a; (void)n;
    kle_aac *d = kle_aac_of(self);
    if (!d) return (klj_val){.l = NULL};
    if (!d->outfmt) d->outfmt = klj_host_cache(kl_jni_new_object(KLE_AAC_OUTFMT));
    kle_set(d->outfmt, KLE_AAC_OUTFMT, "SampleRate", "I", d->rate);
    kle_set(d->outfmt, KLE_AAC_OUTFMT, "NumChannels", "I", d->channels);
    kle_set(d->outfmt, KLE_AAC_OUTFMT, "BytesPerSample", "I", 2);
    return (klj_val){.l = d->outfmt};
}

// GetOutputBufferAndRelease(index) -> the PCM as byte[], and the slot is free.
static klj_val klj_ElectraAAC_takeOutput(void *env, void *self, const klj_val *a, int n) {
    (void)env;
    kle_aac *d = kle_aac_of(self);
    int idx = n > 0 ? (int)(int32_t)a[0].j : -1;
    if (!d || !d->count || idx != d->head || !d->pcm[idx])
        return (klj_val){.l = klj_new_array('B', NULL, 0)};
    void *arrobj = klj_new_array('B', NULL, d->pcm_len[idx]);
    klj_array *arr = klj_arr(arrobj);
    if (arr && arr->data) memcpy(arr->data, d->pcm[idx], (size_t)d->pcm_len[idx]);
    free(d->pcm[idx]); d->pcm[idx] = NULL;
    d->head = (d->head + 1) % KLE_AAC_SLOTS;
    d->count--;
    return (klj_val){.l = arrobj};
}

// One table body per codec, generated from the decoder class name C: the nested
// class names in the signatures are C with a "$..." suffix, exactly as the guest
// resolves them. Both instantiations point at the same handlers, which key their
// per-codec behaviour on the instance/class they are given.
#define KLE_TABLE(C) \
    {C, "<init>", "()V", klj_Electra_ctor}, \
    {C, "CreateDecoder", "()I", klj_Electra_create}, \
    {C, "ConfigureDecoder", "(L" C "$FCreateParameters;)I", klj_Electra_configure}, \
    {C, "SetOutputSurface", "(Landroid/view/Surface;)I", klj_Electra_setSurface}, \
    {C, "Start", "()I", klj_Electra_start}, \
    {C, "Stop", "()I", klj_Electra_stop}, \
    {C, "Flush", "()I", klj_Electra_flush}, \
    {C, "Reset", "()I", klj_Electra_reset}, \
    {C, "ReleaseDecoder", "()I", klj_Electra_release}, \
    {C, "release", "()I", klj_Electra_release}, \
    {C, "DequeueInputBuffer", "(I)I", klj_Electra_dequeueInput}, \
    {C, "QueueInputBuffer", "(IJ[B)I", klj_Electra_queueInput}, \
    {C, "QueueCSDInputBuffer", "(IJ[B)I", klj_Electra_queueCSD}, \
    {C, "QueueEOSInputBuffer", "(IJ)I", klj_Electra_queueEOS}, \
    {C, "DequeueOutputBuffer", "(I)L" C "$FOutputBufferInfo;", klj_Electra_dequeueOutput}, \
    {C, "GetOutputBuffer", "(I)[B", klj_Electra_getOutputBuffer}, \
    {C, "GetOutputFormatInfo", "(I)L" C "$FOutputFormatInfo;", klj_Electra_getOutputFormat}, \
    {C, "ReleaseOutputBuffer", "(IZJ)I", klj_Electra_releaseOutput}, \
    {C, "GetDecoderInformation", "()L" C "$FDecoderInformation;", klj_Electra_decoderInfo}, \
    {C "$FCreateParameters", "<init>", "()V", klj_Electra_params_ctor},

const klj_binding klj_bind_electra[] = {
    KLE_TABLE(KLE_H264_CLASS)
    KLE_TABLE(KLE_H265_CLASS)
    // UE 4.27 (see the block above klj_Electra_create427)
    {KLE_H264_CLASS, "CreateDecoder", "(L" KLE_H264_CLASS "$FCreateParameters;)I",
     klj_Electra_create427},
    {KLE_TEX_CLASS, "<init>", "()V", klj_ElectraTex_ctor},
    {KLE_TEX_CLASS, "Initialize", "(Z)V", klj_ElectraTex_init},
    {KLE_TEX_CLASS, "GetCodecSurface", "()Landroid/view/Surface;", klj_ElectraTex_surface},
    {KLE_TEX_CLASS, "GetVideoFrameUpdateInfo", "(III)L" KLE_TEX_INFO ";", klj_ElectraTex_update},
    {KLE_TEX_CLASS, "Release", "()V", klj_ElectraTex_release},
    {KLE_AAC_CLASS, "<init>", "()V", klj_ElectraAAC_ctor},
    {KLE_AAC_CLASS, "CreateDecoder", "(II[B)I", klj_ElectraAAC_create},
    {KLE_AAC_CLASS, "Start", "()I", klj_ElectraAAC_start},
    {KLE_AAC_CLASS, "Stop", "()I", klj_ElectraAAC_stop},
    {KLE_AAC_CLASS, "Flush", "()I", klj_ElectraAAC_flush},
    {KLE_AAC_CLASS, "ReleaseDecoder", "()I", klj_ElectraAAC_release},
    {KLE_AAC_CLASS, "DequeueInputBuffer", "(I)I", klj_ElectraAAC_dequeueInput},
    {KLE_AAC_CLASS, "QueueInputBuffer", "(IJ[B)I", klj_ElectraAAC_queueInput},
    {KLE_AAC_CLASS, "DequeueOutputBuffer", "(I)L" KLE_AAC_OUTBUF ";", klj_ElectraAAC_dequeueOutput},
    {KLE_AAC_CLASS, "GetOutputFormatInfo", "(I)L" KLE_AAC_OUTFMT ";", klj_ElectraAAC_outputFormat},
    {KLE_AAC_CLASS, "GetOutputBufferAndRelease", "(I)[B", klj_ElectraAAC_takeOutput},
    {0}
};
