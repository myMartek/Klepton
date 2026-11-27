// The C API of KleptonFFmpeg.framework (runtime/media/klff.h) on top of
// FFmpeg's libavformat, libavcodec, libswscale and libswresample.
//
// Compiled by tools/ffmpeg_build.sh against the FFmpeg it builds and linked
// with it into one dylib; nothing in the runtime includes an FFmpeg header.
// One handle reads one stream (video or audio) of one file, with the other
// streams discarded at the demuxer.
//
// Video: H.264, HEVC, AV1, VP9, MPEG-1/2/4 and H.263 go through VideoToolbox
// where the device has it (the hwaccel falls back to FFmpeg's own decoder by
// itself when VideoToolbox refuses a stream); everything is turned into BGRA
// by libswscale with the frame's own colour properties.
// Audio: whatever FFmpeg decodes, to s16 interleaved at the stream's rate (at
// most 48 kHz) and at most two channels.
#include "../../runtime/media/klff.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct klff {
    int kind;
    AVFormatContext *fmt;
    AVCodecContext *dec;
    AVBufferRef *hw;
    int idx;
    AVStream *st;
    AVPacket *pkt;
    AVFrame *frm, *sw, *out;
    SwsContext *sws;
    SwrContext *swr;
    int out_rate, out_ch;
    unsigned char *bgra;       // tightly packed picture handed out
    size_t bgra_cap;
    short *pcm;                // PCM handed out
    int pcm_cap;               // in frames
    long long skip_to_us;      // after a seek: drop what lies before this
    int eof_sent;              // the demuxer ran out and the decoder was told
    int last_hw;
    // klff_open_io: the caller's reads instead of a file.
    AVIOContext *avio;
    klff_read_fn io_read;
    klff_done_fn io_done;
    void *io_ctx;
    long long io_size, io_pos;
};

static int klff_io_packet(void *opaque, uint8_t *buf, int n) {
    klff *f = opaque;
    if (f->io_pos >= f->io_size) return AVERROR_EOF;
    long got = f->io_read(f->io_ctx, f->io_pos, buf, (unsigned long)n);
    if (got < 0) return AVERROR(EIO);
    if (got == 0) return AVERROR_EOF;
    f->io_pos += got;
    return (int)got;
}

static int64_t klff_io_seek(void *opaque, int64_t off, int whence) {
    klff *f = opaque;
    switch (whence & ~AVSEEK_FORCE) {
    case AVSEEK_SIZE: return f->io_size;
    case SEEK_SET: break;
    case SEEK_CUR: off += f->io_pos; break;
    case SEEK_END: off += f->io_size; break;
    default: return AVERROR(EINVAL);
    }
    if (off < 0) return AVERROR(EINVAL);
    f->io_pos = off;
    return off;
}

static long long klff_us(klff *f, int64_t ts) {
    if (ts == AV_NOPTS_VALUE) return -1;
    long long us = av_rescale_q(ts, f->st->time_base, AV_TIME_BASE_Q);
    if (f->fmt->start_time != AV_NOPTS_VALUE) us -= f->fmt->start_time;
    return us < 0 ? 0 : us;
}

static enum AVPixelFormat klff_get_format(AVCodecContext *c, const enum AVPixelFormat *fmts) {
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == AV_PIX_FMT_VIDEOTOOLBOX && c->hw_device_ctx) return *p;
    // No VideoToolbox for this stream: FFmpeg's own decoder, first software format.
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(*p);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    }
    return fmts[0];
}

static void klff_err(char *err, int cap, const char *what, int rc) {
    if (!err || cap <= 0) return;
    char e[128] = "";
    if (rc < 0) av_strerror(rc, e, sizeof e);
    snprintf(err, (size_t)cap, "%s%s%s", what, *e ? ": " : "", e);
}

void klff_close(klff *f) {
    if (!f) return;
    sws_free_context(&f->sws);
    swr_free(&f->swr);
    av_frame_free(&f->frm);
    av_frame_free(&f->sw);
    av_frame_free(&f->out);
    av_packet_free(&f->pkt);
    avcodec_free_context(&f->dec);
    av_buffer_unref(&f->hw);
    avformat_close_input(&f->fmt);
    if (f->avio) {
        av_freep(&f->avio->buffer);
        avio_context_free(&f->avio);
    }
    if (f->io_done) f->io_done(f->io_ctx);
    free(f->bgra);
    free(f->pcm);
    free(f);
}

static klff *klff_open_with(klff *f, const char *path, int kind, char *err, int cap);

klff *klff_open(const char *path, int kind, char *err, int cap) {
    av_log_set_level(AV_LOG_ERROR);
    klff *f = calloc(1, sizeof *f);
    if (!f) { klff_err(err, cap, "out of memory", 0); return NULL; }
    return klff_open_with(f, path, kind, err, cap);
}

klff *klff_open_io(klff_read_fn read, klff_done_fn done, void *ctx, long long size,
                   const char *name, int kind, char *err, int cap) {
    av_log_set_level(AV_LOG_ERROR);
    klff *f = calloc(1, sizeof *f);
    if (!f) {
        if (done) done(ctx);
        klff_err(err, cap, "out of memory", 0);
        return NULL;
    }
    f->io_read = read;
    f->io_done = done;
    f->io_ctx = ctx;
    f->io_size = size;
    // 256 KiB a read: a server answers a range per read that misses its cache.
    enum { IOBUF = 256 * 1024 };
    unsigned char *buf = av_malloc(IOBUF);
    f->avio = buf ? avio_alloc_context(buf, IOBUF, 0, f, klff_io_packet, NULL, klff_io_seek) : NULL;
    f->fmt = f->avio ? avformat_alloc_context() : NULL;
    if (!f->avio || !f->fmt) {
        if (!f->avio) av_free(buf);
        klff_err(err, cap, "out of memory", 0);
        klff_close(f);
        return NULL;
    }
    f->avio->seekable = AVIO_SEEKABLE_NORMAL;
    f->fmt->pb = f->avio;
    f->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    return klff_open_with(f, name && *name ? name : "film", kind, err, cap);
}

static klff *klff_open_with(klff *f, const char *path, int kind, char *err, int cap) {
    f->kind = kind;
    f->skip_to_us = -1;
    // Chapters are not read: an MP4's chapter track is a sample per chapter
    // spread over the whole file, and reading them at open is a jump through
    // it per chapter. A 1.8 GB film on pCloud with 12 chapters opened its
    // sound in 8.9 s / 41 MB with them, 2.2 s / 8.5 MB without (2026-11-27).
    // Nothing here shows chapters; demuxers without the option ignore it.
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "ignore_chapters", "1", 0);
    int rc = avformat_open_input(&f->fmt, path, NULL, &opts);
    av_dict_free(&opts);
    if (rc < 0) { klff_err(err, cap, "cannot open", rc); klff_close(f); return NULL; }
    rc = avformat_find_stream_info(f->fmt, NULL);
    if (rc < 0) { klff_err(err, cap, "cannot read the streams", rc); klff_close(f); return NULL; }
    enum AVMediaType type = kind == KLFF_VIDEO ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
    const AVCodec *codec = NULL;
    f->idx = av_find_best_stream(f->fmt, type, -1, -1, &codec, 0);
    if (f->idx < 0 || !codec) {
        klff_err(err, cap, kind == KLFF_VIDEO ? "no video stream FFmpeg can decode"
                                              : "no audio stream FFmpeg can decode", f->idx);
        klff_close(f);
        return NULL;
    }
    f->st = f->fmt->streams[f->idx];
    for (unsigned i = 0; i < f->fmt->nb_streams; i++)
        if ((int)i != f->idx) f->fmt->streams[i]->discard = AVDISCARD_ALL;
    f->dec = avcodec_alloc_context3(codec);
    if (!f->dec || avcodec_parameters_to_context(f->dec, f->st->codecpar) < 0) {
        klff_err(err, cap, "cannot set up the decoder", 0);
        klff_close(f);
        return NULL;
    }
    f->dec->pkt_timebase = f->st->time_base;
    f->dec->thread_count = 0;
    if (kind == KLFF_VIDEO &&
        av_hwdevice_ctx_create(&f->hw, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, NULL, NULL, 0) == 0) {
        f->dec->hw_device_ctx = av_buffer_ref(f->hw);
        f->dec->get_format = klff_get_format;
    }
    rc = avcodec_open2(f->dec, codec, NULL);
    if (rc < 0) { klff_err(err, cap, "cannot open the decoder", rc); klff_close(f); return NULL; }
    f->pkt = av_packet_alloc();
    f->frm = av_frame_alloc();
    f->sw = av_frame_alloc();
    f->out = av_frame_alloc();
    if (!f->pkt || !f->frm || !f->sw || !f->out) {
        klff_err(err, cap, "out of memory", 0);
        klff_close(f);
        return NULL;
    }
    if (kind == KLFF_AUDIO) {
        int in_rate = f->dec->sample_rate > 0 ? f->dec->sample_rate : 48000;
        f->out_rate = in_rate > 48000 ? 48000 : in_rate;
        f->out_ch = f->dec->ch_layout.nb_channels >= 2 ? 2 : 1;
    }
    return f;
}

int klff_info(klff *f, int *w, int *h, long long *duration_us, double *fps,
              char *codec, int codec_cap, int *rate, int *channels) {
    if (!f) return 0;
    if (w) *w = f->kind == KLFF_VIDEO ? f->st->codecpar->width : 0;
    if (h) *h = f->kind == KLFF_VIDEO ? f->st->codecpar->height : 0;
    if (duration_us) {
        long long d = f->fmt->duration != AV_NOPTS_VALUE ? f->fmt->duration : 0;
        if (d <= 0 && f->st->duration != AV_NOPTS_VALUE)
            d = av_rescale_q(f->st->duration, f->st->time_base, AV_TIME_BASE_Q);
        *duration_us = d > 0 ? d : 0;
    }
    if (fps) {
        AVRational r = av_guess_frame_rate(f->fmt, f->st, NULL);
        *fps = r.num > 0 && r.den > 0 ? av_q2d(r) : 0;
    }
    if (codec && codec_cap > 0) snprintf(codec, (size_t)codec_cap, "%s", avcodec_get_name(f->st->codecpar->codec_id));
    if (rate) *rate = f->out_rate;
    if (channels) *channels = f->out_ch;
    return 1;
}

// One decoded frame of this stream into f->frm: 1, 0 at the end, -1 on error.
static int klff_decode(klff *f) {
    for (;;) {
        int rc = avcodec_receive_frame(f->dec, f->frm);
        if (rc == 0) return 1;
        if (rc == AVERROR_EOF) return 0;
        if (rc != AVERROR(EAGAIN)) return -1;
        if (f->eof_sent) return 0;
        rc = av_read_frame(f->fmt, f->pkt);
        if (rc < 0) {
            // The end of the file, or a read error that ends it: drain.
            avcodec_send_packet(f->dec, NULL);
            f->eof_sent = 1;
            continue;
        }
        if (f->pkt->stream_index == f->idx) {
            rc = avcodec_send_packet(f->dec, f->pkt);
            // A broken packet is skipped, as a player would; only a decoder
            // that refuses everything ends the stream (receive says so).
            (void)rc;
        }
        av_packet_unref(f->pkt);
    }
}

int klff_video_next(klff *f, const unsigned char **bgra, int *w, int *h,
                    long long *pts_us, int *hw) {
    if (!f || f->kind != KLFF_VIDEO) return -1;
    for (;;) {
        int rc = klff_decode(f);
        if (rc <= 0) return rc;
        long long pts = klff_us(f, f->frm->best_effort_timestamp);
        if (f->skip_to_us >= 0 && pts >= 0 && pts + 20000 < f->skip_to_us) {
            av_frame_unref(f->frm);
            continue;
        }
        f->skip_to_us = -1;
        const AVFrame *src = f->frm;
        f->last_hw = 0;
        if (f->frm->format == AV_PIX_FMT_VIDEOTOOLBOX) {
            av_frame_unref(f->sw);
            if (av_hwframe_transfer_data(f->sw, f->frm, 0) < 0) { av_frame_unref(f->frm); continue; }
            av_frame_copy_props(f->sw, f->frm);
            src = f->sw;
            f->last_hw = 1;
        }
        if (!f->sws) {
            f->sws = sws_alloc_context();
            if (!f->sws) return -1;
            f->sws->threads = 0;
            f->sws->flags = SWS_BILINEAR;
        }
        av_frame_unref(f->out);
        f->out->format = AV_PIX_FMT_BGRA;
        f->out->width = src->width;
        f->out->height = src->height;
        if (sws_scale_frame(f->sws, f->out, src) < 0) { av_frame_unref(f->frm); continue; }
        size_t row = (size_t)src->width * 4, need = row * (size_t)src->height;
        if (need > f->bgra_cap) {
            unsigned char *nb = realloc(f->bgra, need);
            if (!nb) return -1;
            f->bgra = nb;
            f->bgra_cap = need;
        }
        for (int y = 0; y < src->height; y++)
            memcpy(f->bgra + row * (size_t)y, f->out->data[0] + (size_t)f->out->linesize[0] * (size_t)y, row);
        if (bgra) *bgra = f->bgra;
        if (w) *w = src->width;
        if (h) *h = src->height;
        if (pts_us) *pts_us = pts;
        if (hw) *hw = f->last_hw;
        av_frame_unref(f->frm);
        return 1;
    }
}

int klff_audio_next(klff *f, const short **pcm, int *frames, long long *pts_us) {
    if (!f || f->kind != KLFF_AUDIO) return -1;
    for (;;) {
        int rc = klff_decode(f);
        if (rc <= 0) return rc;
        long long pts = klff_us(f, f->frm->best_effort_timestamp);
        int in_rate = f->frm->sample_rate > 0 ? f->frm->sample_rate : f->dec->sample_rate;
        long long len_us = in_rate > 0 ? (long long)f->frm->nb_samples * 1000000 / in_rate : 0;
        if (f->skip_to_us >= 0 && pts >= 0 && pts + len_us < f->skip_to_us) {
            av_frame_unref(f->frm);
            continue;
        }
        f->skip_to_us = -1;
        if (!f->swr) {
            AVChannelLayout out;
            av_channel_layout_default(&out, f->out_ch);
            if (swr_alloc_set_opts2(&f->swr, &out, AV_SAMPLE_FMT_S16, f->out_rate,
                                    &f->frm->ch_layout, (enum AVSampleFormat)f->frm->format,
                                    in_rate, 0, NULL) < 0 || swr_init(f->swr) < 0) {
                swr_free(&f->swr);
                return -1;
            }
        }
        int cap = swr_get_out_samples(f->swr, f->frm->nb_samples);
        if (cap > f->pcm_cap) {
            short *np = realloc(f->pcm, (size_t)cap * (size_t)f->out_ch * sizeof(short));
            if (!np) return -1;
            f->pcm = np;
            f->pcm_cap = cap;
        }
        uint8_t *outp[1] = { (uint8_t *)f->pcm };
        int n = swr_convert(f->swr, outp, f->pcm_cap,
                            (const uint8_t **)f->frm->extended_data, f->frm->nb_samples);
        av_frame_unref(f->frm);
        if (n <= 0) continue;
        if (pcm) *pcm = f->pcm;
        if (frames) *frames = n;
        if (pts_us) *pts_us = pts;
        return 1;
    }
}

int klff_seek(klff *f, long long us) {
    if (!f) return -1;
    if (us < 0) us = 0;
    int64_t ts = us;
    if (f->fmt->start_time != AV_NOPTS_VALUE) ts += f->fmt->start_time;
    int rc = avformat_seek_file(f->fmt, -1, INT64_MIN, ts, ts, 0);
    if (rc < 0) rc = av_seek_frame(f->fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(f->dec);
    if (f->swr) swr_free(&f->swr);
    f->eof_sent = 0;
    f->skip_to_us = us;
    return rc < 0 ? -1 : 0;
}

// ---- audio tracks (Bigscreen Beta's Audio Tracks menu, 2026-11-27) ----

// The nth audio stream's index in the file, or -1.
static int klff_nth_audio(klff *f, int nth) {
    int k = 0;
    for (unsigned i = 0; i < f->fmt->nb_streams; i++) {
        if (f->fmt->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        if (k++ == nth) return (int)i;
    }
    return -1;
}

static void klff_clean_field(char *s) {
    for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
}

int klff_audio_tracks(klff *f, char *buf, int cap) {
    if (!f || !buf || cap <= 0) return -1;
    buf[0] = 0;
    int k = 0, len = 0;
    for (unsigned i = 0; i < f->fmt->nb_streams; i++) {
        AVStream *st = f->fmt->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        const AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);
        const AVDictionaryEntry *title = av_dict_get(st->metadata, "title", NULL, 0);
        char l[16] = "", t[160] = "", c[48] = "";
        if (lang) snprintf(l, sizeof l, "%s", lang->value);
        if (title) snprintf(t, sizeof t, "%s", title->value);
        // What a menu shows: "AAC", "AC-3", "TrueHD"; DTS by its profile,
        // which names the kind ("DTS-HD MA", "DTS-HD HRA", "DTS").
        enum AVCodecID cid = st->codecpar->codec_id;
        const AVCodecDescriptor *d = avcodec_descriptor_get(cid);
        const char *prof = cid == AV_CODEC_ID_DTS
            ? avcodec_profile_name(cid, st->codecpar->profile) : NULL;
        if (prof) snprintf(c, sizeof c, "%s", prof);
        else if (cid == AV_CODEC_ID_AC3) snprintf(c, sizeof c, "AC-3");
        else if (cid == AV_CODEC_ID_EAC3) snprintf(c, sizeof c, "E-AC-3");
        else if (cid == AV_CODEC_ID_TRUEHD) snprintf(c, sizeof c, "TrueHD");
        else if (cid == AV_CODEC_ID_DTS) snprintf(c, sizeof c, "DTS");
        else {
            snprintf(c, sizeof c, "%s", d ? d->name : "?");
            for (char *q = c; *q; q++) if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 32);
        }
        klff_clean_field(l); klff_clean_field(t); klff_clean_field(c);
        int n = snprintf(buf + len, (size_t)(cap - len), "%s\t%s\t%s\t%d\t%d\t%d\t%d\n", l, t, c,
                         st->codecpar->ch_layout.nb_channels,
                         (st->disposition & AV_DISPOSITION_DEFAULT) ? 1 : 0,
                         (int)i == f->idx ? 1 : 0, st->id);
        if (n < 0 || n >= cap - len) { buf[len] = 0; break; }
        len += n;
        k++;
    }
    return k;
}

int klff_audio_select(klff *f, int nth) {
    if (!f || f->kind != KLFF_AUDIO) return -1;
    int idx = klff_nth_audio(f, nth);
    if (idx < 0) return -1;
    if (idx == f->idx) return 0;
    AVStream *st = f->fmt->streams[idx];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return -1;
    AVCodecContext *dec = avcodec_alloc_context3(codec);
    if (!dec || avcodec_parameters_to_context(dec, st->codecpar) < 0) {
        avcodec_free_context(&dec);
        return -1;
    }
    dec->pkt_timebase = st->time_base;
    dec->thread_count = 0;
    if (avcodec_open2(dec, codec, NULL) < 0) { avcodec_free_context(&dec); return -1; }
    avcodec_free_context(&f->dec);
    f->dec = dec;
    f->fmt->streams[f->idx]->discard = AVDISCARD_ALL;
    st->discard = AVDISCARD_DEFAULT;
    f->idx = idx;
    f->st = st;
    if (f->swr) swr_free(&f->swr);
    int in_rate = dec->sample_rate > 0 ? dec->sample_rate : 48000;
    f->out_rate = in_rate > 48000 ? 48000 : in_rate;
    f->out_ch = dec->ch_layout.nb_channels >= 2 ? 2 : 1;
    f->eof_sent = 0;
    return 0;
}

const char *klff_version(void) {
    static char v[64];
    if (!*v) snprintf(v, sizeof v, "FFmpeg %s", av_version_info());
    return v;
}
