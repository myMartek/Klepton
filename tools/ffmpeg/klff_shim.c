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
};

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
    free(f->bgra);
    free(f->pcm);
    free(f);
}

klff *klff_open(const char *path, int kind, char *err, int cap) {
    av_log_set_level(AV_LOG_ERROR);
    klff *f = calloc(1, sizeof *f);
    if (!f) { klff_err(err, cap, "out of memory", 0); return NULL; }
    f->kind = kind;
    f->skip_to_us = -1;
    int rc = avformat_open_input(&f->fmt, path, NULL, NULL);
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

const char *klff_version(void) {
    static char v[64];
    if (!*v) snprintf(v, sizeof v, "FFmpeg %s", av_version_info());
    return v;
}
