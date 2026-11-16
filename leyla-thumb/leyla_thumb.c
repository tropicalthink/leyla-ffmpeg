/*
 * leyla_thumb: Leyla's keyframe thumbnailer, a mode of ffprobe
 * (`ffprobe -leyla_thumb [workers] [threads]`).
 *
 * A long-lived helper that leyla-server starts once and talks to over
 * stdin/stdout. It decodes the one keyframe a scrub preview needs, scales
 * it and answers with a JPEG, keeping files and decoders open between
 * requests. Nothing is written to disk.
 *
 * It is built into Leyla's ffmpeg (README.md) and links
 * libav*, which leyla-server must never do: this ffmpeg is a GPL build
 * and the server is not. A process boundary and this text protocol keep it
 * so. It lives inside ffprobe rather than beside it because a static build
 * links every codec into each program: a third 80 MB binary for one
 * function.
 *
 * Requests, one per line, fields separated by one tab:
 *
 *   thumb  <id> <prio> <path> <ms> <mode> <width>
 *       <prio>  i  interactive: the newest is decoded first (a scrub only
 *                  cares where the finger is now), any worker
 *               b  background: oldest first, one worker at a time, and
 *                  only when no interactive request waits
 *       <mode>  a  <ms> is on the stream's own timestamps (a keyframe from
 *                  the server's index)
 *               r  <ms> is from the start of the stream
 *       <width> 16..1920; the height follows the display aspect ratio
 *   prime  <id> <path>          open the file now, for a scrub to come
 *   cancel <id>                 drop a queued request
 *   ping   <id>                 liveness and version
 *
 * Replies, in completion order, never interleaved:
 *
 *   <id> <status> <micros> <len>\n<len bytes>
 *
 * <status> is ok (len bytes of JPEG; for ping, the version text), primed,
 * cancelled, busy (the queue was full, or a newer request pushed this one
 * out), or an error: e_bad (unparsable request), e_open (no such file, or
 * no video in it), e_seek, e_decode, e_encode. An error is an answer, never
 * an exit: a broken file must not cost every other viewer their previews.
 * <micros> is the time spent on it inside the helper.
 *
 * Usage: ffprobe -leyla_thumb [workers] [threads per decoder]
 *        ffprobe -leyla_thumb -version
 */

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <wchar.h>
#include <windows.h>
#endif

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/display.h"
#include "libavutil/imgutils.h"
#include "libavutil/time.h"
#include "libswscale/swscale.h"

int leyla_thumb_main(int argc, char **argv);

#define PROTOCOL_VERSION 1
#define MAX_WORKERS 16
#define OPEN_PER_WORKER 3
/* Interactive requests past this are the oldest, which LIFO would serve
 * last anyway: they are answered busy. */
#define MAX_INTERACTIVE 256
#define MAX_BACKGROUND 64
/* A file nobody has asked about for this long is closed, decoder and all:
 * a 4K HEVC decoder holds tens of MB of frames. */
#define IDLE_CLOSE_US (60 * 1000000LL)
#define LINE_MAX_BYTES 8192
#define MIN_WIDTH 16
#define MAX_WIDTH 1920
/* When the keyframe alone does not give a picture (a field-coded frame
 * split over two packets, a decoder that wants more), read on this far. */
#define SLOW_PATH_PACKETS 96

enum Kind { K_THUMB, K_PRIME };

typedef struct Job {
    char id[64];
    enum Kind kind;
    int background;
    char *path;
    int64_t ms;
    int absolute;
    int width;
    struct Job *next;
} Job;

typedef struct Queue {
    Job *head, *tail; /* head is the oldest */
    int len;
} Queue;

static Queue interactive, background;
static int background_running;
static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qcv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t outmu = PTHREAD_MUTEX_INITIALIZER;
static int slice_threads = 2;

typedef struct Open {
    char *path;
    int64_t mtime, size;
    AVFormatContext *fmt;
    AVCodecContext *dec;
    int vidx;
    double rotation; /* degrees counter-clockwise, from the display matrix */
    int64_t last_use;
} Open;

typedef struct Worker {
    Open open[OPEN_PER_WORKER];
    AVPacket *pkt;
    AVFrame *frame;
    AVFrame *scaled; /* not "small": <windows.h> defines that */
    AVFrame *turned;
    struct SwsContext *sws;
    AVCodecContext *enc;
    int enc_w, enc_h;
    int64_t enc_pts;
} Worker;

/* ------------------------------------------------------------------ */
/* Output                                                              */

static void reply(const char *id, const char *status, int64_t micros,
                  const uint8_t *data, int len)
{
    pthread_mutex_lock(&outmu);
    fprintf(stdout, "%s %s %lld %d\n", id, status, (long long)micros, data ? len : 0);
    if (data && len > 0)
        fwrite(data, 1, len, stdout);
    fflush(stdout);
    pthread_mutex_unlock(&outmu);
}

/* ------------------------------------------------------------------ */
/* Queue                                                               */

static void push(Queue *q, Job *j)
{
    j->next = NULL;
    if (q->tail)
        q->tail->next = j;
    else
        q->head = j;
    q->tail = j;
    q->len++;
}

static Job *pop_oldest(Queue *q)
{
    Job *j = q->head;
    if (!j)
        return NULL;
    q->head = j->next;
    if (!q->head)
        q->tail = NULL;
    q->len--;
    j->next = NULL;
    return j;
}

static Job *pop_newest(Queue *q)
{
    Job *j = q->tail, *prev = NULL;
    if (!j)
        return NULL;
    for (Job *k = q->head; k != j; k = k->next)
        prev = k;
    if (prev)
        prev->next = NULL;
    else
        q->head = NULL;
    q->tail = prev;
    q->len--;
    return j;
}

static Job *remove_id(Queue *q, const char *id)
{
    Job *prev = NULL;
    for (Job *j = q->head; j; prev = j, j = j->next) {
        if (strcmp(j->id, id))
            continue;
        if (prev)
            prev->next = j->next;
        else
            q->head = j->next;
        if (q->tail == j)
            q->tail = prev;
        q->len--;
        j->next = NULL;
        return j;
    }
    return NULL;
}

static void free_job(Job *j)
{
    if (!j)
        return;
    av_free(j->path);
    free(j);
}

/* ------------------------------------------------------------------ */
/* Files                                                               */

static int stat_path(const char *path, int64_t *mtime, int64_t *size)
{
#ifdef _WIN32
    /* The protocol is UTF-8, as libavformat's file protocol reads it. */
    wchar_t wpath[4096];
    struct _stat64 st;
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 4096) || _wstat64(wpath, &st))
        return -1;
#else
    struct stat st;
    if (stat(path, &st))
        return -1;
#endif
    *mtime = (int64_t)st.st_mtime;
    *size = (int64_t)st.st_size;
    return 0;
}

static void close_open(Open *o)
{
    avcodec_free_context(&o->dec);
    avformat_close_input(&o->fmt);
    av_freep(&o->path);
    o->last_use = 0;
}

static int open_file(Open *o, const char *path, int64_t mtime, int64_t size)
{
    AVFormatContext *fmt = NULL;
    AVCodecContext *dec = NULL;
    const AVCodec *codec;
    AVStream *st;
    int v;

    if (avformat_open_input(&fmt, path, NULL, NULL) < 0)
        return -1;
    v = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    /* An attached picture (an MP4's cover art) is not the programme. */
    if (v >= 0 && (fmt->streams[v]->disposition & AV_DISPOSITION_ATTACHED_PIC))
        v = -1;
    if (v < 0 || fmt->streams[v]->codecpar->width <= 0) {
        /* Only a header that does not describe the video pays for a probe. */
        if (avformat_find_stream_info(fmt, NULL) < 0)
            goto fail;
        v = -1;
        for (unsigned i = 0; i < fmt->nb_streams; i++) {
            AVStream *s = fmt->streams[i];
            if (s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                !(s->disposition & AV_DISPOSITION_ATTACHED_PIC) && s->codecpar->width > 0) {
                v = (int)i;
                break;
            }
        }
        if (v < 0)
            goto fail;
    }
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        fmt->streams[i]->discard = (int)i == v ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    st = fmt->streams[v];
    codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec)
        goto fail;
    dec = avcodec_alloc_context3(codec);
    if (!dec || avcodec_parameters_to_context(dec, st->codecpar) < 0)
        goto fail;
    dec->pkt_timebase = st->time_base;
    /* Slices only: frame threads would hold the picture back a frame per
     * thread, and there is only ever one frame wanted. */
    dec->thread_type = FF_THREAD_SLICE;
    dec->thread_count = slice_threads;
    /* An open-GOP keyframe (an I frame with a recovery point, not an IDR)
     * is a whole picture; without this H.264 waits for a "recovered" one. */
    dec->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
    if (avcodec_open2(dec, codec, NULL) < 0)
        goto fail;

    o->rotation = 0;
    {
        const AVPacketSideData *sd = av_packet_side_data_get(
            st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data,
            AV_PKT_DATA_DISPLAYMATRIX);
        if (sd && sd->size >= 9 * sizeof(int32_t)) {
            double r = av_display_rotation_get((const int32_t *)sd->data);
            if (r == r) /* not NaN */
                o->rotation = r;
        }
    }
    o->path = av_strdup(path);
    if (!o->path)
        goto fail;
    o->mtime = mtime;
    o->size = size;
    o->fmt = fmt;
    o->dec = dec;
    o->vidx = v;
    return 0;
fail:
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return -1;
}

/* The worker's open copy of `path`, opening it (and closing the least
 * recently used) when it has none, or when the file changed on disk. */
static Open *get_open(Worker *w, const char *path)
{
    Open *slot = NULL;
    int64_t mtime, size, now = av_gettime_relative();

    if (stat_path(path, &mtime, &size))
        return NULL;
    for (int i = 0; i < OPEN_PER_WORKER; i++) {
        Open *o = &w->open[i];
        if (o->fmt && !strcmp(o->path, path)) {
            if (o->mtime == mtime && o->size == size) {
                o->last_use = now;
                return o;
            }
            close_open(o); /* replaced since: read the new one */
            slot = o;
            break;
        }
    }
    if (!slot) {
        slot = &w->open[0];
        for (int i = 0; i < OPEN_PER_WORKER; i++) {
            Open *o = &w->open[i];
            if (!o->fmt) {
                slot = o;
                break;
            }
            if (o->last_use < slot->last_use)
                slot = o;
        }
        close_open(slot);
    }
    if (open_file(slot, path, mtime, size))
        return NULL;
    slot->last_use = now;
    return slot;
}

static void close_idle(Worker *w)
{
    int64_t now = av_gettime_relative();
    for (int i = 0; i < OPEN_PER_WORKER; i++)
        if (w->open[i].fmt && now - w->open[i].last_use > IDLE_CLOSE_US)
            close_open(&w->open[i]);
}

/* ------------------------------------------------------------------ */
/* One thumbnail                                                       */

/* Feed packets from the current position until a picture comes out.
 * `keyframe_only`: send the first video keyframe and drain, which gives its
 * picture at once with no reorder delay. Otherwise send packets in order
 * (the slow path, for what the fast one could not decode). */
static int decode_one(Worker *w, Open *o, int keyframe_only)
{
    int got = 0, sent = 0, ret;
    while (!got && av_read_frame(o->fmt, w->pkt) >= 0) {
        if (w->pkt->stream_index != o->vidx ||
            (!sent && !(w->pkt->flags & AV_PKT_FLAG_KEY))) {
            av_packet_unref(w->pkt);
            continue;
        }
        ret = avcodec_send_packet(o->dec, w->pkt);
        av_packet_unref(w->pkt);
        sent++;
        if (ret < 0 && ret != AVERROR(EAGAIN) && keyframe_only)
            break;
        if (keyframe_only) {
            avcodec_send_packet(o->dec, NULL); /* drain: no waiting on later frames */
            got = avcodec_receive_frame(o->dec, w->frame) == 0;
            break;
        }
        got = avcodec_receive_frame(o->dec, w->frame) == 0;
        if (sent >= SLOW_PATH_PACKETS)
            break;
    }
    if (!got && !keyframe_only && sent) {
        avcodec_send_packet(o->dec, NULL);
        got = avcodec_receive_frame(o->dec, w->frame) == 0;
    }
    avcodec_flush_buffers(o->dec);
    return got;
}

static int seek_to(Open *o, const Job *j)
{
    AVStream *st = o->fmt->streams[o->vidx];
    int64_t ts = av_rescale_q(j->ms, (AVRational){ 1, 1000 }, st->time_base);
    if (!j->absolute && st->start_time != AV_NOPTS_VALUE)
        ts += st->start_time;
    if (av_seek_frame(o->fmt, o->vidx, ts, AVSEEK_FLAG_BACKWARD) >= 0)
        return 0;
    /* Some demuxers cannot seek backward to a keyframe on a stream (no
     * index); a byte or any-frame seek still beats no picture. */
    return avformat_seek_file(o->fmt, o->vidx, INT64_MIN, ts, ts, 0) >= 0 ? 0 : -1;
}

static int ensure_frame(AVFrame **f, int width, int height)
{
    if (*f && (*f)->width == width && (*f)->height == height)
        return 0;
    av_frame_free(f);
    *f = av_frame_alloc();
    if (!*f)
        return -1;
    (*f)->width = width;
    (*f)->height = height;
    (*f)->format = AV_PIX_FMT_YUVJ420P;
    (*f)->color_range = AVCOL_RANGE_JPEG;
    return av_frame_get_buffer(*f, 0);
}

/* Quarter turns of a 4:2:0 picture, plane by plane. */
static void turn_plane(const uint8_t *src, int sls, int sw, int sh,
                       uint8_t *dst, int dls, int quarter)
{
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            uint8_t v = src[y * sls + x];
            switch (quarter) {
            case 1: dst[(sw - 1 - x) * dls + y] = v; break;           /* 90 ccw */
            case 2: dst[(sh - 1 - y) * dls + (sw - 1 - x)] = v; break; /* 180 */
            case 3: dst[x * dls + (sh - 1 - y)] = v; break;           /* 90 cw */
            }
        }
}

static const char *thumb(Worker *w, const Job *j, uint8_t **out, int *outlen)
{
    Open *o = get_open(w, j->path);
    AVFrame *src;
    AVPacket *jp;
    int sw, sh, quarter;
    double dar;

    if (!o)
        return "e_open";
    if (seek_to(o, j))
        return "e_seek";
    if (!decode_one(w, o, 1)) {
        if (seek_to(o, j) || !decode_one(w, o, 0))
            return "e_decode";
    }
    if (w->frame->width <= 0 || w->frame->height <= 0) {
        av_frame_unref(w->frame);
        return "e_decode";
    }

    quarter = (int)((o->rotation + 3600 + 45) / 90) % 4;
    {
        AVRational sar = w->frame->sample_aspect_ratio;
        if (sar.num <= 0 || sar.den <= 0)
            sar = o->fmt->streams[o->vidx]->sample_aspect_ratio;
        if (sar.num <= 0 || sar.den <= 0)
            sar = (AVRational){ 1, 1 };
        dar = (double)w->frame->width * sar.num / ((double)w->frame->height * sar.den);
    }
    if (quarter % 2)
        dar = 1.0 / dar;
    /* Width and height of the picture as shown, even (4:2:0). */
    sw = j->width & ~1;
    sh = (int)(sw / dar / 2 + 0.5) * 2;
    if (sh < 2)
        sh = 2;
    if (sh > 4 * MAX_WIDTH)
        sh = 4 * MAX_WIDTH;
    {
        int scale_w = quarter % 2 ? sh : sw, scale_h = quarter % 2 ? sw : sh;
        w->sws = sws_getCachedContext(w->sws, w->frame->width, w->frame->height,
                                      w->frame->format, scale_w, scale_h,
                                      AV_PIX_FMT_YUVJ420P, SWS_BILINEAR, NULL, NULL, NULL);
        if (!w->sws || ensure_frame(&w->scaled, scale_w, scale_h) < 0) {
            av_frame_unref(w->frame);
            return "e_encode";
        }
        av_frame_make_writable(w->scaled);
        sws_scale(w->sws, (const uint8_t *const *)w->frame->data, w->frame->linesize, 0,
                  w->frame->height, w->scaled->data, w->scaled->linesize);
        av_frame_unref(w->frame);
    }
    src = w->scaled;
    if (quarter) {
        if (ensure_frame(&w->turned, sw, sh) < 0)
            return "e_encode";
        av_frame_make_writable(w->turned);
        for (int p = 0; p < 3; p++) {
            int pw = p ? w->scaled->width / 2 : w->scaled->width;
            int ph = p ? w->scaled->height / 2 : w->scaled->height;
            turn_plane(w->scaled->data[p], w->scaled->linesize[p], pw, ph,
                       w->turned->data[p], w->turned->linesize[p], quarter);
        }
        src = w->turned;
    }

    if (!w->enc || w->enc_w != sw || w->enc_h != sh) {
        const AVCodec *mj = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        avcodec_free_context(&w->enc);
        if (!mj || !(w->enc = avcodec_alloc_context3(mj)))
            return "e_encode";
        w->enc->width = sw;
        w->enc->height = sh;
        w->enc->pix_fmt = AV_PIX_FMT_YUVJ420P;
        w->enc->color_range = AVCOL_RANGE_JPEG;
        w->enc->time_base = (AVRational){ 1, 25 };
        w->enc->flags |= AV_CODEC_FLAG_QSCALE;
        w->enc->global_quality = FF_QP2LAMBDA * 5;
        w->enc->thread_count = 1;
        if (avcodec_open2(w->enc, mj, NULL) < 0) {
            avcodec_free_context(&w->enc);
            return "e_encode";
        }
        w->enc_w = sw;
        w->enc_h = sh;
    }
    src->pts = ++w->enc_pts; /* the encoder wants rising timestamps */
    src->quality = w->enc->global_quality;
    if (avcodec_send_frame(w->enc, src) < 0)
        return "e_encode";
    jp = av_packet_alloc();
    if (!jp || avcodec_receive_packet(w->enc, jp) < 0) {
        av_packet_free(&jp);
        return "e_encode";
    }
    *out = av_malloc(jp->size);
    if (!*out) {
        av_packet_free(&jp);
        return "e_encode";
    }
    memcpy(*out, jp->data, jp->size);
    *outlen = jp->size;
    av_packet_free(&jp);
    return "ok";
}

/* ------------------------------------------------------------------ */
/* Workers                                                             */

/* The next job for a worker: any interactive one, newest first; else the
 * oldest background one if no other worker has one. Waits, closing idle
 * files now and then. */
static Job *next_job(Worker *w)
{
    Job *j = NULL;
    pthread_mutex_lock(&qmu);
    for (;;) {
        if ((j = pop_newest(&interactive)))
            break;
        if (!background_running && (j = pop_oldest(&background))) {
            background_running = 1;
            break;
        }
        {
            int64_t until = av_gettime() + 15 * 1000000LL;
            struct timespec ts = { (time_t)(until / 1000000), (long)(until % 1000000) * 1000 };
            if (pthread_cond_timedwait(&qcv, &qmu, &ts) == ETIMEDOUT) {
                pthread_mutex_unlock(&qmu);
                close_idle(w);
                pthread_mutex_lock(&qmu);
            }
        }
    }
    pthread_mutex_unlock(&qmu);
    return j;
}

static void *worker_main(void *arg)
{
    Worker *w = calloc(1, sizeof *w);
    (void)arg;
    if (!w || !(w->pkt = av_packet_alloc()) || !(w->frame = av_frame_alloc())) {
        fprintf(stderr, "leyla_thumb: out of memory starting a worker\n");
        exit(70);
    }
    for (;;) {
        Job *j = next_job(w);
        int64_t start = av_gettime_relative();
        if (j->kind == K_PRIME) {
            reply(j->id, get_open(w, j->path) ? "primed" : "e_open",
                  av_gettime_relative() - start, NULL, 0);
        } else {
            uint8_t *jpg = NULL;
            int len = 0;
            const char *status = thumb(w, j, &jpg, &len);
            reply(j->id, status, av_gettime_relative() - start, jpg, len);
            av_free(jpg);
        }
        if (j->background) {
            pthread_mutex_lock(&qmu);
            background_running = 0;
            pthread_cond_signal(&qcv);
            pthread_mutex_unlock(&qmu);
        }
        free_job(j);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Requests                                                            */

static int split(char *line, char **fields, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        fields[n++] = p;
        p = strchr(p, '\t');
        if (!p)
            break;
        *p++ = 0;
    }
    return p ? -1 : n; /* more fields than expected is malformed */
}

static int parse_int64(const char *s, int64_t *out)
{
    char *end;
    long long v;
    if (!*s)
        return -1;
    errno = 0;
    v = strtoll(s, &end, 10);
    if (errno || *end)
        return -1;
    *out = v;
    return 0;
}

static void handle_line(char *line)
{
    char *f[8];
    int n = split(line, f, 8);
    const char *id;
    Job *j, *dropped = NULL;
    int64_t ms, width;

    if (n < 2) {
        reply("-", "e_bad", 0, NULL, 0);
        return;
    }
    id = f[1];
    if (strlen(id) >= sizeof j->id || strchr(id, ' ') || !*id) {
        reply("-", "e_bad", 0, NULL, 0);
        return;
    }
    if (!strcmp(f[0], "ping") && n == 2) {
        char version[256];
        int len = snprintf(version, sizeof version, "leyla_thumb %d %s", PROTOCOL_VERSION,
                           av_version_info());
        reply(id, "ok", 0, (const uint8_t *)version, len);
        return;
    }
    if (!strcmp(f[0], "cancel") && n == 2) {
        pthread_mutex_lock(&qmu);
        j = remove_id(&interactive, id);
        if (!j)
            j = remove_id(&background, id);
        pthread_mutex_unlock(&qmu);
        if (j) {
            reply(j->id, "cancelled", 0, NULL, 0);
            free_job(j);
        }
        return;
    }
    j = calloc(1, sizeof *j);
    if (!j) {
        reply(id, "busy", 0, NULL, 0);
        return;
    }
    snprintf(j->id, sizeof j->id, "%s", id);
    if (!strcmp(f[0], "thumb") && n == 7 && (!strcmp(f[2], "i") || !strcmp(f[2], "b")) &&
        *f[3] && !parse_int64(f[4], &ms) && ms >= 0 &&
        (!strcmp(f[5], "a") || !strcmp(f[5], "r")) && !parse_int64(f[6], &width) &&
        width >= MIN_WIDTH && width <= MAX_WIDTH) {
        j->kind = K_THUMB;
        j->background = f[2][0] == 'b';
        j->path = av_strdup(f[3]);
        j->ms = ms;
        j->absolute = f[5][0] == 'a';
        j->width = (int)width;
    } else if (!strcmp(f[0], "prime") && n == 3 && *f[2]) {
        j->kind = K_PRIME;
        j->path = av_strdup(f[2]);
    } else {
        reply(j->id, "e_bad", 0, NULL, 0);
        free(j);
        return;
    }
    if (!j->path) {
        reply(j->id, "busy", 0, NULL, 0);
        free(j);
        return;
    }
    pthread_mutex_lock(&qmu);
    if (j->background) {
        if (background.len >= MAX_BACKGROUND) {
            dropped = j;
            j = NULL;
        } else {
            push(&background, j);
        }
    } else {
        if (interactive.len >= MAX_INTERACTIVE)
            dropped = pop_oldest(&interactive);
        push(&interactive, j);
    }
    pthread_cond_broadcast(&qcv);
    pthread_mutex_unlock(&qmu);
    if (dropped) {
        reply(dropped->id, "busy", 0, NULL, 0);
        free_job(dropped);
    }
}

/* Called by ffprobe's main for `-leyla_thumb`; argv[0] is "-leyla_thumb". */
int leyla_thumb_main(int argc, char **argv)
{
    int workers;
    char *line;
    pthread_t th;

    if (argc > 1 && (!strcmp(argv[1], "-version") || !strcmp(argv[1], "--version"))) {
        printf("leyla_thumb %d %s\n", PROTOCOL_VERSION, av_version_info());
        return 0;
    }
    workers = argc > 1 ? atoi(argv[1]) : 4;

    if (workers < 1)
        workers = 1;
    if (workers > MAX_WORKERS)
        workers = MAX_WORKERS;
    slice_threads = argc > 2 ? atoi(argv[2]) : 2;
    if (slice_threads < 1)
        slice_threads = 1;
    if (slice_threads > 16)
        slice_threads = 16;
#ifdef _WIN32
    /* JPEG bytes, not text: no CRLF translation either way. */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    av_log_set_level(AV_LOG_QUIET);
    line = malloc(LINE_MAX_BYTES);
    if (!line)
        return 70;
    for (int i = 0; i < workers; i++) {
        if (pthread_create(&th, NULL, worker_main, NULL)) {
            fprintf(stderr, "leyla_thumb: cannot start worker %d\n", i);
            return 70;
        }
        pthread_detach(th);
    }
    while (fgets(line, LINE_MAX_BYTES, stdin)) {
        size_t len = strlen(line);
        if (len && line[len - 1] != '\n' && !feof(stdin)) {
            /* Longer than any request: skip the rest of it. */
            int c;
            while ((c = getchar()) != EOF && c != '\n')
                ;
            reply("-", "e_bad", 0, NULL, 0);
            continue;
        }
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = 0;
        if (len)
            handle_line(line);
    }
    /* stdin closed: the server is gone or wants us gone. Nothing here is
     * worth finishing. */
    fflush(stdout);
    _Exit(0);
}
