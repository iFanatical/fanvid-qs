/* fanvid play <file> <output>:<mode>...: plays <file>, looped and silent,
 * as the wallpaper of each output named, from one decoder.
 *
 * VA-API decodes each frame on the GPU, and its video processor converts it
 * to RGB at each output's size in pixels (one conversion per distinct size,
 * whatever the number of outputs). The converted surfaces go to the
 * compositor as dmabufs, each exported and imported once and then reused,
 * and are held until the compositor releases them. Compositors may not show
 * NV12 dmabufs (Hyprland 0.57 shows their luma plane only), and take RGB.
 *
 * Each output has a background layer surface with a black single-pixel
 * buffer, and the video on a subsurface over it, which the compositor
 * crops, scales and places (wp_viewporter) for the mode. Frames go to an
 * output only once it has shown the last one (frame callbacks): one that
 * nobody can see costs nothing, and when none can, decoding stops too.
 *
 * Commands come on stdin, a line each; see README.md. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/pixdesc.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <wayland-client.h>

#include "alpha-modifier-v1-client-protocol.h"
#include "fanvid.h"
#include "fractional-scale-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* Decoded frames read ahead. */
#define QUEUE 3
/* VA surfaces a conversion may cycle through; its pool grows as needed. */
#define MAX_CACHED 32
#define MAX_HEADS 32

enum mode { MODE_FILL, MODE_MAX, MODE_SCALE, MODE_CENTER };

/* ------------------------------------------------------------ state */

/* A conversion to one size, shared by the outputs that need it, and the
 * wl_buffers made of its surfaces, per output. */
struct cached {
    VASurfaceID sid;
    struct wl_buffer *buffers[MAX_OUTPUTS];
};

struct group {
    bool used;
    int w, h;
    int refs; /* outputs using it */
    int live; /* pics not yet freed */
    AVFilterGraph *graph;
    AVFilterContext *src, *sink;
    struct cached cache[MAX_CACHED];
    int n_cache;
};

/* A converted frame, held while an output shows it or the compositor has
 * not released a buffer of it. */
struct pic {
    AVFrame *rgb;
    VASurfaceID sid;
    struct group *group;
    int held;
    int shown_on;
};

/* An output the compositor advertises. */
struct head {
    bool used;
    struct wl_output *wl;
    uint32_t global;
    char name[64];
    bool done;
};

/* An output asked for. */
struct output {
    bool used;
    char name[64];
    enum mode mode;
    struct head *head;
    struct wl_surface *bg, *video;
    struct wl_subsurface *sub;
    struct zwlr_layer_surface_v1 *layer;
    struct wp_viewport *bg_vp, *video_vp;
    struct wp_fractional_scale_v1 *frac;
    struct wp_alpha_modifier_surface_v1 *bg_alpha, *video_alpha;
    int lw, lh;   /* logical size, as configured */
    int scale120; /* preferred scale, in 120ths */
    bool configured;
    bool laid_out;
    bool paused;
    bool waiting; /* for the frame callback */
    bool announced;
    struct wl_callback *frame_cb;
    struct group *group;
    struct pic *shown;
    bool fading;
    double fade_start, fade_ms;
};

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_subcompositor *subcompositor;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwp_linux_dmabuf_v1 *dmabuf;
static struct wp_viewporter *viewporter;
static struct wp_fractional_scale_manager_v1 *frac_mgr;
static struct wp_alpha_modifier_v1 *alpha_mgr;
static struct wp_single_pixel_buffer_manager_v1 *pixel_mgr;
static struct wl_buffer *black;

static struct head heads[MAX_HEADS];
static struct output outputs[MAX_OUTPUTS];
static struct group groups[MAX_OUTPUTS];

static AVFormatContext *fmt;
static AVCodecContext *dec;
static AVBufferRef *hwdev;
static VADisplay va;
static int stream;
static AVPacket *pkt;
static int video_w, video_h;
static int64_t loop_offset = 0;
static int64_t last_pts = AV_NOPTS_VALUE;
static double frame_s;
static bool decoded_any;
static bool gpu_failed;

static volatile sig_atomic_t stop;

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void say(const char *what, const char *name)
{
    printf("%s %s\n", what, name);
    fflush(stdout);
}

/* ------------------------------------------------------------ pics */

static void group_unref(struct group *g);

static void pic_free_if_done(struct pic *p)
{
    if (!p || p->held || p->shown_on)
        return;
    struct group *g = p->group;
    av_frame_free(&p->rgb);
    free(p);
    g->live--;
    if (!g->refs && !g->live)
        group_unref(g);
}

static void buffer_release(void *data, struct wl_buffer *b)
{
    (void)b;
    struct pic *p = data;
    if (!p)
        return;
    wl_buffer_set_user_data(b, NULL);
    p->held--;
    pic_free_if_done(p);
}

static const struct wl_buffer_listener buffer_listener = {.release = buffer_release};

/* The wl_buffer of `p`'s surface for output `idx`: made the first time
 * that surface goes to that output, reused after. */
static struct wl_buffer *pic_buffer(struct pic *p, int idx)
{
    struct group *g = p->group;
    struct cached *c = NULL;
    for (int i = 0; i < g->n_cache; i++)
        if (g->cache[i].sid == p->sid)
            c = &g->cache[i];
    if (!c) {
        if (g->n_cache == MAX_CACHED)
            return NULL;
        c = &g->cache[g->n_cache++];
        memset(c, 0, sizeof *c);
        c->sid = p->sid;
    }
    if (c->buffers[idx])
        return c->buffers[idx];

    VADRMPRIMESurfaceDescriptor d;
    if (vaExportSurfaceHandle(va, p->sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                              VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                              &d)
        != VA_STATUS_SUCCESS) {
        fprintf(stderr, "fanvid: cannot export a frame\n");
        return NULL;
    }
    struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(dmabuf);
    for (uint32_t i = 0; i < d.layers[0].num_planes; i++) {
        const uint32_t obj = d.layers[0].object_index[i];
        const uint64_t mod = d.objects[obj].drm_format_modifier;
        zwp_linux_buffer_params_v1_add(params, d.objects[obj].fd, i, d.layers[0].offset[i],
                                       d.layers[0].pitch[i], mod >> 32, mod & 0xffffffff);
    }
    struct wl_buffer *b = zwp_linux_buffer_params_v1_create_immed(
        params, p->rgb->width, p->rgb->height, d.layers[0].drm_format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    /* libwayland has its own copies of the fds once the request is made. */
    for (uint32_t i = 0; i < d.num_objects; i++)
        close(d.objects[i].fd);
    wl_buffer_add_listener(b, &buffer_listener, NULL);
    c->buffers[idx] = b;
    return b;
}

/* ------------------------------------------------------------ conversion */

static const char *or_bt709(const char *name)
{
    return name && strcmp(name, "unknown") && strcmp(name, "unspecified") ? name : "bt709";
}

/* The colour properties are spelt out, unknown ones as BT.709: with any
 * left unknown, Intel's driver converts with BT.601's matrix. Full range is
 * asked for, which AMD's otherwise leaves limited. */
static int group_build(struct group *g, const AVFrame *f)
{
    const AVStream *s = fmt->streams[stream];
    g->graph = avfilter_graph_alloc();
    /* The source wants the decoder's surfaces before its format. */
    g->src = avfilter_graph_alloc_filter(g->graph, avfilter_get_by_name("buffer"), "in");
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
    par->format = AV_PIX_FMT_VAAPI;
    par->width = f->width;
    par->height = f->height;
    par->time_base = s->time_base;
    par->sample_aspect_ratio = (AVRational){1, 1};
    par->hw_frames_ctx = f->hw_frames_ctx;
    par->color_space = f->colorspace;
    par->color_range = f->color_range;
    int r = av_buffersrc_parameters_set(g->src, par);
    av_free(par);
    if (r < 0 || (r = avfilter_init_str(g->src, NULL)) < 0)
        return r;
    if ((r = avfilter_graph_create_filter(&g->sink, avfilter_get_by_name("buffersink"), "out",
                                          NULL, NULL, g->graph))
        < 0)
        return r;
    char desc[512];
    snprintf(desc, sizeof desc,
             "setparams=color_primaries=%s:color_trc=%s:colorspace=%s:range=%s,"
             "scale_vaapi=w=%d:h=%d:format=rgb0:out_range=full:"
             "out_color_primaries=bt709:out_color_transfer=bt709",
             or_bt709(av_color_primaries_name(f->color_primaries)),
             or_bt709(av_color_transfer_name(f->color_trc)),
             or_bt709(av_color_space_name(f->colorspace)),
             f->color_range == AVCOL_RANGE_JPEG ? "pc" : "tv", g->w, g->h);
    AVFilterInOut *outs = avfilter_inout_alloc(), *ins = avfilter_inout_alloc();
    outs->name = av_strdup("in");
    outs->filter_ctx = g->src;
    ins->name = av_strdup("out");
    ins->filter_ctx = g->sink;
    r = avfilter_graph_parse_ptr(g->graph, desc, &ins, &outs, NULL);
    avfilter_inout_free(&ins);
    avfilter_inout_free(&outs);
    if (r >= 0)
        r = avfilter_graph_config(g->graph, NULL);
    return r;
}

static struct group *group_get(int w, int h)
{
    struct group *free_slot = NULL;
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        if (groups[i].used && groups[i].w == w && groups[i].h == h) {
            groups[i].refs++;
            return &groups[i];
        }
        if (!groups[i].used && !free_slot)
            free_slot = &groups[i];
    }
    if (!free_slot)
        return NULL;
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->used = true;
    free_slot->w = w;
    free_slot->h = h;
    free_slot->refs = 1;
    return free_slot;
}

/* Drops an output's use of `g`; the group goes once no pic of it is left. */
static void group_unref(struct group *g)
{
    if (!g || !g->used)
        return;
    if (g->refs)
        g->refs--;
    if (g->refs || g->live)
        return;
    for (int i = 0; i < g->n_cache; i++)
        for (int j = 0; j < MAX_OUTPUTS; j++)
            if (g->cache[i].buffers[j])
                wl_buffer_destroy(g->cache[i].buffers[j]);
    avfilter_graph_free(&g->graph);
    memset(g, 0, sizeof *g);
}

/* `f` converted for `g`; NULL when it cannot be. */
static struct pic *convert(struct group *g, const AVFrame *f)
{
    if (!g->graph && group_build(g, f) < 0) {
        fprintf(stderr, "fanvid: cannot convert to %dx%d\n", g->w, g->h);
        avfilter_graph_free(&g->graph);
        return NULL;
    }
    AVFrame *in = av_frame_clone(f);
    AVFrame *rgb = av_frame_alloc();
    int r = av_buffersrc_add_frame(g->src, in);
    av_frame_free(&in);
    if (r >= 0)
        r = av_buffersink_get_frame(g->sink, rgb);
    if (r < 0) {
        fprintf(stderr, "fanvid: cannot convert a frame: %s\n", av_err2str(r));
        av_frame_free(&rgb);
        return NULL;
    }
    struct pic *p = calloc(1, sizeof *p);
    p->rgb = rgb;
    p->sid = (VASurfaceID)(uintptr_t)rgb->data[3];
    p->group = g;
    g->live++;
    /* Done before the compositor reads it. */
    vaSyncSurface(va, p->sid);
    return p;
}

/* ------------------------------------------------------------ decoding */

static enum AVPixelFormat pick_vaapi(AVCodecContext *c, const enum AVPixelFormat *fmts)
{
    (void)c;
    for (; *fmts != AV_PIX_FMT_NONE; fmts++)
        if (*fmts == AV_PIX_FMT_VAAPI)
            return *fmts;
    gpu_failed = true;
    return AV_PIX_FMT_NONE;
}

static int open_video(const char *path)
{
    int r;
    if ((r = avformat_open_input(&fmt, path, NULL, NULL)) < 0
        || (r = avformat_find_stream_info(fmt, NULL)) < 0) {
        fprintf(stderr, "fanvid: cannot read %s: %s\n", path, av_err2str(r));
        return EXIT_ERROR;
    }
    const AVCodec *codec = NULL;
    stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (stream < 0 || !codec) {
        fprintf(stderr, "fanvid: no video in %s\n", path);
        return EXIT_ERROR;
    }
    bool vaapi = false;
    for (int i = 0;; i++) {
        const AVCodecHWConfig *hc = avcodec_get_hw_config(codec, i);
        if (!hc)
            break;
        vaapi |= hc->device_type == AV_HWDEVICE_TYPE_VAAPI
                 && (hc->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX);
    }
    if (!vaapi || av_hwdevice_ctx_create(&hwdev, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0) < 0) {
        fprintf(stderr, "fanvid: no VA-API decoding for %s\n", path);
        return EXIT_NO_GPU;
    }
    va = ((AVVAAPIDeviceContext *)((AVHWDeviceContext *)hwdev->data)->hwctx)->display;
    dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, fmt->streams[stream]->codecpar);
    dec->hw_device_ctx = av_buffer_ref(hwdev);
    dec->get_format = pick_vaapi;
    /* The queue holds decoded frames beyond the decoder's own. */
    dec->extra_hw_frames = QUEUE + 2;
    if ((r = avcodec_open2(dec, codec, NULL)) < 0) {
        fprintf(stderr, "fanvid: cannot decode %s: %s\n", path, av_err2str(r));
        return EXIT_NO_GPU;
    }
    pkt = av_packet_alloc();
    video_w = dec->width;
    video_h = dec->height;
    const AVRational fps = fmt->streams[stream]->avg_frame_rate;
    frame_s = fps.num > 0 && fps.den > 0 ? av_q2d(av_inv_q(fps)) : 1.0 / 30;
    return EXIT_OK;
}

/* The next frame, looping at the end, and how long it shows; NULL on an
 * error. */
static AVFrame *decode_next(double *duration)
{
    AVFrame *f = av_frame_alloc();
    int empty_loops = 0;
    for (;;) {
        int r = avcodec_receive_frame(dec, f);
        if (r == 0)
            break;
        if (r == AVERROR_EOF) {
            /* No frame in a whole pass: nothing to play. */
            if (++empty_loops > 1 || !decoded_any)
                goto fail;
            avcodec_flush_buffers(dec);
            av_seek_frame(fmt, stream, 0, AVSEEK_FLAG_BACKWARD);
            if (last_pts != AV_NOPTS_VALUE) {
                const AVStream *s = fmt->streams[stream];
                loop_offset = last_pts + av_rescale_q(1, av_inv_q(s->avg_frame_rate),
                                                      s->time_base);
            }
            continue;
        }
        if (r != AVERROR(EAGAIN))
            goto fail;
        r = av_read_frame(fmt, pkt);
        if (r == AVERROR_EOF) {
            avcodec_send_packet(dec, NULL);
            continue;
        }
        if (r < 0)
            goto fail;
        if (pkt->stream_index == stream)
            avcodec_send_packet(dec, pkt);
        av_packet_unref(pkt);
    }
    decoded_any = true;
    /* Timestamps run on across loops, so each step is right at the seam. */
    const AVStream *s = fmt->streams[stream];
    int64_t pts = f->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE)
        pts = last_pts == AV_NOPTS_VALUE ? 0 : last_pts - loop_offset + 1;
    pts += loop_offset;
    *duration = last_pts == AV_NOPTS_VALUE || pts <= last_pts
                    ? frame_s
                    : fmin((double)(pts - last_pts) * av_q2d(s->time_base), 1.0);
    last_pts = pts;
    return f;
fail:
    av_frame_free(&f);
    return NULL;
}

/* ------------------------------------------------------------ outputs */

static void layout(struct output *o);

static void frame_done(void *data, struct wl_callback *cb, uint32_t t)
{
    (void)t;
    struct output *o = data;
    wl_callback_destroy(cb);
    o->frame_cb = NULL;
    o->waiting = false;
}

static const struct wl_callback_listener frame_listener = {.done = frame_done};

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *l, uint32_t serial,
                            uint32_t w, uint32_t h)
{
    struct output *o = data;
    zwlr_layer_surface_v1_ack_configure(l, serial);
    if (o->configured && o->lw == (int)w && o->lh == (int)h)
        return;
    o->lw = (int)w;
    o->lh = (int)h;
    o->configured = true;
    o->laid_out = false;
}

static void output_drop(struct output *o, bool forget);

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *l)
{
    (void)l;
    output_drop(data, true);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static void preferred_scale(void *data, struct wp_fractional_scale_v1 *f, uint32_t scale)
{
    (void)f;
    struct output *o = data;
    if ((int)scale != o->scale120) {
        o->scale120 = (int)scale;
        o->laid_out = false;
    }
}

static const struct wp_fractional_scale_v1_listener frac_listener = {
    .preferred_scale = preferred_scale,
};

static int even(double v)
{
    int i = (int)lround(v);
    return i < 2 ? 2 : i & ~1;
}

/* Sizes the conversion for the output's mode, size and scale, and has the
 * compositor crop, scale and place it. */
static void layout(struct output *o)
{
    const double s = o->scale120 / 120.0;
    const int pw = (int)lround(o->lw * s), ph = (int)lround(o->lh * s);
    int tw, th;        /* converted size, in pixels */
    double sx, sy, sw, sh; /* the part shown, in buffer pixels */
    int dx, dy, dw, dh;    /* where, in logical pixels */
    if (o->mode == MODE_SCALE) {
        tw = even(pw);
        th = even(ph);
        sx = sy = 0;
        sw = tw;
        sh = th;
        dx = dy = 0;
        dw = o->lw;
        dh = o->lh;
    } else if (o->mode == MODE_MAX) {
        const double k = fmin((double)pw / video_w, (double)ph / video_h);
        tw = even(video_w * k);
        th = even(video_h * k);
        sx = sy = 0;
        sw = tw;
        sh = th;
        dw = (int)lround(tw / s);
        dh = (int)lround(th / s);
        dx = (o->lw - dw) / 2;
        dy = (o->lh - dh) / 2;
    } else if (o->mode == MODE_CENTER) {
        tw = even(video_w);
        th = even(video_h);
        sw = fmin(tw, pw);
        sh = fmin(th, ph);
        sx = (tw - sw) / 2;
        sy = (th - sh) / 2;
        dw = (int)lround(sw / s);
        dh = (int)lround(sh / s);
        dx = (o->lw - dw) / 2;
        dy = (o->lh - dh) / 2;
    } else {
        const double k = fmax((double)pw / video_w, (double)ph / video_h);
        tw = even(video_w * k);
        th = even(video_h * k);
        sw = fmin(tw, pw);
        sh = fmin(th, ph);
        sx = (tw - sw) / 2;
        sy = (th - sh) / 2;
        dx = dy = 0;
        dw = o->lw;
        dh = o->lh;
    }
    struct group *old = o->group;
    if (!old || old->w != tw || old->h != th) {
        o->group = group_get(tw, th);
        group_unref(old);
    }
    wp_viewport_set_source(o->video_vp, wl_fixed_from_double(sx), wl_fixed_from_double(sy),
                           wl_fixed_from_double(sw), wl_fixed_from_double(sh));
    wp_viewport_set_destination(o->video_vp, dw, dh);
    wl_subsurface_set_position(o->sub, dx, dy);
    wp_viewport_set_destination(o->bg_vp, o->lw, o->lh);
    wl_surface_attach(o->bg, black, 0, 0);
    wl_surface_damage_buffer(o->bg, 0, 0, 1, 1);
    wl_surface_commit(o->bg);
    o->laid_out = true;
}

static void set_alpha(struct output *o, double a)
{
    const uint32_t v = (uint32_t)(fmax(0, fmin(1, a)) * UINT32_MAX);
    wp_alpha_modifier_surface_v1_set_multiplier(o->bg_alpha, v);
    wp_alpha_modifier_surface_v1_set_multiplier(o->video_alpha, v);
    wl_surface_commit(o->video);
    wl_surface_commit(o->bg);
}

static void output_create(struct output *o)
{
    struct wl_region *none = wl_compositor_create_region(compositor);
    o->bg = wl_compositor_create_surface(compositor);
    o->video = wl_compositor_create_surface(compositor);
    /* Clicks go through, to whatever is beneath. */
    wl_surface_set_input_region(o->bg, none);
    wl_surface_set_input_region(o->video, none);
    wl_region_destroy(none);
    o->sub = wl_subcompositor_get_subsurface(subcompositor, o->video, o->bg);
    wl_subsurface_set_desync(o->sub);
    o->bg_vp = wp_viewporter_get_viewport(viewporter, o->bg);
    o->video_vp = wp_viewporter_get_viewport(viewporter, o->video);
    o->scale120 = 120;
    if (frac_mgr) {
        o->frac = wp_fractional_scale_manager_v1_get_fractional_scale(frac_mgr, o->bg);
        wp_fractional_scale_v1_add_listener(o->frac, &frac_listener, o);
    }
    if (alpha_mgr) {
        o->bg_alpha = wp_alpha_modifier_v1_get_surface(alpha_mgr, o->bg);
        o->video_alpha = wp_alpha_modifier_v1_get_surface(alpha_mgr, o->video);
    }
    /* FANVID_LAYER=overlay: over the windows, for screenshots in tests. */
    const char *layer = getenv("FANVID_LAYER");
    o->layer = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, o->bg, o->head->wl,
        layer && !strcmp(layer, "overlay") ? ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY
                                           : ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
        "fanvid");
    zwlr_layer_surface_v1_add_listener(o->layer, &layer_listener, o);
    zwlr_layer_surface_v1_set_anchor(o->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
                                                   | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
                                                   | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
                                                   | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(o->layer, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(o->layer, 0);
    wl_surface_commit(o->bg);
}

/* Takes the output off the screen; `forget`: and no longer plays on it,
 * else it comes back if its output does. */
static void output_drop(struct output *o, bool forget)
{
    if (o->frame_cb)
        wl_callback_destroy(o->frame_cb);
    if (o->frac)
        wp_fractional_scale_v1_destroy(o->frac);
    if (o->bg_alpha)
        wp_alpha_modifier_surface_v1_destroy(o->bg_alpha);
    if (o->video_alpha)
        wp_alpha_modifier_surface_v1_destroy(o->video_alpha);
    if (o->video_vp)
        wp_viewport_destroy(o->video_vp);
    if (o->bg_vp)
        wp_viewport_destroy(o->bg_vp);
    if (o->sub)
        wl_subsurface_destroy(o->sub);
    if (o->layer)
        zwlr_layer_surface_v1_destroy(o->layer);
    if (o->video)
        wl_surface_destroy(o->video);
    if (o->bg)
        wl_surface_destroy(o->bg);
    struct pic *shown = o->shown;
    struct group *g = o->group;
    const bool had = o->head != NULL;
    char name[64];
    snprintf(name, sizeof name, "%s", o->name);
    const enum mode mode = o->mode;
    const bool paused = o->paused;
    memset(o, 0, sizeof *o);
    if (shown) {
        shown->shown_on--;
        pic_free_if_done(shown);
    }
    group_unref(g);
    if (!forget) {
        o->used = true;
        snprintf(o->name, sizeof o->name, "%s", name);
        o->mode = mode;
        o->paused = paused;
    }
    if (had || forget)
        say("gone", name);
}

/* Puts each output asked for on its head, once the compositor has named
 * it. */
static void attach_heads(void)
{
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        struct output *o = &outputs[i];
        if (!o->used || o->head)
            continue;
        for (int j = 0; j < MAX_HEADS; j++)
            if (heads[j].used && heads[j].done && !strcmp(heads[j].name, o->name)) {
                o->head = &heads[j];
                output_create(o);
                break;
            }
    }
}

static bool parse_mode(const char *s, enum mode *m)
{
    if (!strcmp(s, "fill") || !strcmp(s, "tile"))
        *m = MODE_FILL;
    else if (!strcmp(s, "max"))
        *m = MODE_MAX;
    else if (!strcmp(s, "scale"))
        *m = MODE_SCALE;
    else if (!strcmp(s, "center"))
        *m = MODE_CENTER;
    else
        return false;
    return true;
}

static struct output *find_output(const char *name)
{
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (outputs[i].used && !strcmp(outputs[i].name, name))
            return &outputs[i];
    return NULL;
}

static bool add_output(const char *name, const char *mode_name)
{
    enum mode mode;
    if (!parse_mode(mode_name, &mode)) {
        fprintf(stderr, "fanvid: no mode %s\n", mode_name);
        return false;
    }
    struct output *o = find_output(name);
    if (o) {
        /* Again, perhaps in another mode. */
        if (o->mode != mode) {
            o->mode = mode;
            o->laid_out = false;
        }
        return true;
    }
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (!outputs[i].used) {
            o = &outputs[i];
            o->used = true;
            snprintf(o->name, sizeof o->name, "%s", name);
            o->mode = mode;
            return true;
        }
    fprintf(stderr, "fanvid: too many outputs\n");
    return false;
}

static int outputs_left(void)
{
    int n = 0;
    for (int i = 0; i < MAX_OUTPUTS; i++)
        n += outputs[i].used;
    return n;
}

/* ------------------------------------------------------------ wayland */

static void head_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw,
                          int32_t ph, int32_t sp, const char *make, const char *model, int32_t t)
{
    (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sp; (void)make; (void)model;
    (void)t;
}
static void head_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r)
{
    (void)d; (void)o; (void)f; (void)w; (void)h; (void)r;
}
static void head_done(void *data, struct wl_output *o)
{
    (void)o;
    struct head *h = data;
    h->done = true;
}
static void head_scale(void *d, struct wl_output *o, int32_t s) { (void)d; (void)o; (void)s; }
static void head_name(void *data, struct wl_output *o, const char *name)
{
    (void)o;
    struct head *h = data;
    snprintf(h->name, sizeof h->name, "%s", name);
}
static void head_description(void *d, struct wl_output *o, const char *s)
{
    (void)d; (void)o; (void)s;
}

static const struct wl_output_listener head_listener = {
    .geometry = head_geometry,
    .mode = head_mode,
    .done = head_done,
    .scale = head_scale,
    .name = head_name,
    .description = head_description,
};

/* linux-dmabuf v4's default feedback: only to say when the compositor does
 * not offer what the video processor makes. */
static struct {
    uint32_t format;
    uint32_t pad;
    uint64_t modifier;
} *format_table;
static size_t format_table_len;
static uint64_t xbgr_mods[64];
static int n_xbgr_mods;
static bool feedback_done;

static void fb_done(void *d, struct zwp_linux_dmabuf_feedback_v1 *f)
{
    (void)d; (void)f;
    feedback_done = true;
}
static void fb_format_table(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, int32_t fd,
                            uint32_t size)
{
    (void)d; (void)f;
    void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map != MAP_FAILED) {
        format_table = map;
        format_table_len = size / sizeof *format_table;
    }
}
static void fb_device(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, struct wl_array *a)
{
    (void)d; (void)f; (void)a;
}
static void fb_tranche_done(void *d, struct zwp_linux_dmabuf_feedback_v1 *f)
{
    (void)d; (void)f;
}
static void fb_tranche_formats(void *d, struct zwp_linux_dmabuf_feedback_v1 *f,
                               struct wl_array *indices)
{
    (void)d; (void)f;
    const uint16_t *idx;
    wl_array_for_each(idx, indices)
    {
        if (*idx < format_table_len && format_table[*idx].format == DRM_FORMAT_XBGR8888
            && n_xbgr_mods < 64)
            xbgr_mods[n_xbgr_mods++] = format_table[*idx].modifier;
    }
}
static void fb_tranche_flags(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, uint32_t flags)
{
    (void)d; (void)f; (void)flags;
}

static const struct zwp_linux_dmabuf_feedback_v1_listener feedback_listener = {
    .done = fb_done,
    .format_table = fb_format_table,
    .main_device = fb_device,
    .tranche_done = fb_tranche_done,
    .tranche_target_device = fb_device,
    .tranche_formats = fb_tranche_formats,
    .tranche_flags = fb_tranche_flags,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version)
{
    (void)data;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (!strcmp(iface, wl_subcompositor_interface.name)) {
        subcompositor = wl_registry_bind(reg, name, &wl_subcompositor_interface, 1);
    } else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
        layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name) && version >= 4) {
        dmabuf = wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, 4);
    } else if (!strcmp(iface, wp_viewporter_interface.name)) {
        viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
    } else if (!strcmp(iface, wp_fractional_scale_manager_v1_interface.name)) {
        frac_mgr = wl_registry_bind(reg, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (!strcmp(iface, wp_alpha_modifier_v1_interface.name)) {
        alpha_mgr = wl_registry_bind(reg, name, &wp_alpha_modifier_v1_interface, 1);
    } else if (!strcmp(iface, wp_single_pixel_buffer_manager_v1_interface.name)) {
        pixel_mgr =
            wl_registry_bind(reg, name, &wp_single_pixel_buffer_manager_v1_interface, 1);
    } else if (!strcmp(iface, wl_output_interface.name) && version >= 4) {
        for (int i = 0; i < MAX_HEADS; i++)
            if (!heads[i].used) {
                heads[i].used = true;
                heads[i].global = name;
                heads[i].wl = wl_registry_bind(reg, name, &wl_output_interface, 4);
                wl_output_add_listener(heads[i].wl, &head_listener, &heads[i]);
                break;
            }
    }
}

static void registry_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data; (void)reg;
    for (int i = 0; i < MAX_HEADS; i++) {
        struct head *h = &heads[i];
        if (!h->used || h->global != name)
            continue;
        for (int j = 0; j < MAX_OUTPUTS; j++)
            if (outputs[j].used && outputs[j].head == h)
                output_drop(&outputs[j], false);
        wl_output_release(h->wl);
        memset(h, 0, sizeof *h);
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

/* ------------------------------------------------------------ commands */

static void command(char *line)
{
    char *save = NULL;
    const char *verb = strtok_r(line, " \t", &save);
    const char *a = strtok_r(NULL, " \t", &save);
    const char *b = strtok_r(NULL, " \t", &save);
    if (!verb)
        return;
    if (!strcmp(verb, "pause") || !strcmp(verb, "play")) {
        const bool paused = !strcmp(verb, "pause");
        for (int i = 0; i < MAX_OUTPUTS; i++)
            if (outputs[i].used && (!a || !strcmp(outputs[i].name, a)))
                outputs[i].paused = paused;
    } else if (!strcmp(verb, "add") && a && b) {
        if (add_output(a, b))
            attach_heads();
    } else if (!strcmp(verb, "remove") && a) {
        struct output *o = find_output(a);
        if (o)
            output_drop(o, true);
    } else if (!strcmp(verb, "fade") && a && b) {
        struct output *o = find_output(a);
        const double ms = atof(b);
        if (!o)
            return;
        if (ms <= 0 || !o->bg_alpha || !o->configured) {
            output_drop(o, true);
            return;
        }
        o->fading = true;
        o->fade_start = now();
        o->fade_ms = ms;
    } else {
        fprintf(stderr, "fanvid: no command %s\n", verb);
    }
}

/* Reads what stdin has; false once it has closed. */
static bool read_commands(void)
{
    static char buf[4096];
    static size_t len;
    const ssize_t n = read(STDIN_FILENO, buf + len, sizeof buf - 1 - len);
    if (n == 0)
        return false;
    if (n < 0)
        return errno == EAGAIN || errno == EINTR;
    len += (size_t)n;
    buf[len] = '\0';
    char *start = buf, *nl;
    while ((nl = strchr(start, '\n'))) {
        *nl = '\0';
        command(start);
        start = nl + 1;
    }
    len -= (size_t)(start - buf);
    memmove(buf, start, len);
    /* A line longer than the buffer: dropped. */
    if (len == sizeof buf - 1)
        len = 0;
    return true;
}

/* ------------------------------------------------------------ playing */

static bool can_show(const struct output *o)
{
    return o->used && o->head && o->configured && o->laid_out && !o->paused && !o->waiting
           && o->group;
}

static void show(struct output *o, int idx, struct pic *p)
{
    struct wl_buffer *b = pic_buffer(p, idx);
    if (!b)
        return;
    wl_buffer_set_user_data(b, p);
    p->held++;
    wl_surface_attach(o->video, b, 0, 0);
    wl_surface_damage_buffer(o->video, 0, 0, p->rgb->width, p->rgb->height);
    o->frame_cb = wl_surface_frame(o->video);
    wl_callback_add_listener(o->frame_cb, &frame_listener, o);
    o->waiting = true;
    wl_surface_commit(o->video);
    /* Damage the layer surface too. Hyprland does not repaint a rotated
     * output for the subsurface's damage alone, and refreshes its cached
     * blur of the background layer only for the layer surface's own. */
    wl_surface_damage_buffer(o->bg, 0, 0, 1, 1);
    wl_surface_commit(o->bg);
    struct pic *old = o->shown;
    o->shown = p;
    p->shown_on++;
    if (old) {
        old->shown_on--;
        pic_free_if_done(old);
    }
    if (!o->announced) {
        o->announced = true;
        say("playing", o->name);
    }
}

/* Shows `f` on every output ready for it, converted once per size. */
static void present(const AVFrame *f)
{
    for (int g = 0; g < MAX_OUTPUTS; g++) {
        if (!groups[g].used)
            continue;
        struct pic *p = NULL;
        for (int i = 0; i < MAX_OUTPUTS; i++) {
            if (!can_show(&outputs[i]) || outputs[i].group != &groups[g])
                continue;
            if (!p && !(p = convert(&groups[g], f)))
                break;
            show(&outputs[i], i, p);
        }
        pic_free_if_done(p);
    }
}

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

/* FANVID_STATS=1: every 10 s on stderr, for finding leaks. */
static void stats(void)
{
    static double next;
    static long frames;
    frames++;
    if (!getenv("FANVID_STATS") || now() < next)
        return;
    next = now() + 10;
    int n_groups = 0, live = 0, cached = 0;
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (groups[i].used) {
            n_groups++;
            live += groups[i].live;
            cached += groups[i].n_cache;
        }
    long kb = -1;
    char line[256];
    FILE *fp = fopen("/proc/self/status", "r");
    while (fp && fgets(line, sizeof line, fp))
        if (sscanf(line, "RssAnon: %ld", &kb) == 1)
            break;
    if (fp)
        fclose(fp);
    fprintf(stderr, "fanvid: %ld frames, %d conversions, %d frames held, %d surfaces cached, "
                    "RssAnon %ld kB\n",
            frames, n_groups, live, cached, kb);
}

int play_main(const char *path, int n_specs, char **specs)
{
    av_log_set_level(AV_LOG_ERROR);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    for (int i = 0; i < n_specs; i++) {
        char *colon = strrchr(specs[i], ':');
        if (!colon) {
            fprintf(stderr, "fanvid: %s: not <output>:<mode>\n", specs[i]);
            return EXIT_USAGE;
        }
        *colon = '\0';
        if (!add_output(specs[i], colon + 1))
            return EXIT_USAGE;
    }

    int status = open_video(path);
    if (status != EXIT_OK)
        return status;

    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "fanvid: no Wayland display\n");
        return EXIT_ERROR;
    }
    struct wl_registry *reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !subcompositor || !layer_shell || !dmabuf || !viewporter || !pixel_mgr) {
        fprintf(stderr, "fanvid: the compositor lacks one of wl_subcompositor, "
                        "wlr-layer-shell, linux-dmabuf v4, wp_viewporter and "
                        "wp_single_pixel_buffer\n");
        return EXIT_ERROR;
    }
    struct zwp_linux_dmabuf_feedback_v1 *fb = zwp_linux_dmabuf_v1_get_default_feedback(dmabuf);
    zwp_linux_dmabuf_feedback_v1_add_listener(fb, &feedback_listener, NULL);
    while (!feedback_done && wl_display_roundtrip(display) >= 0) {
    }
    zwp_linux_dmabuf_feedback_v1_destroy(fb);
    if (!n_xbgr_mods)
        fprintf(stderr, "fanvid: the compositor does not offer XBGR8888 dmabufs\n");
    black = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(pixel_mgr, 0, 0, 0,
                                                                     UINT32_MAX);
    attach_heads();

    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
    bool stdin_open = true;

    AVFrame *queue[QUEUE] = {0};
    double durations[QUEUE] = {0};
    int queued = 0;
    double due = 0;
    const int wl_fd = wl_display_get_fd(display);

    while (!stop && outputs_left()) {
        attach_heads();
        bool any = false;
        double fade_wait = -1;
        const double t = now();
        for (int i = 0; i < MAX_OUTPUTS; i++) {
            struct output *o = &outputs[i];
            if (o->used && o->head && o->configured && !o->laid_out)
                layout(o);
            if (o->used && o->fading) {
                const double k = (t - o->fade_start) * 1000 / o->fade_ms;
                if (k >= 1) {
                    output_drop(o, true);
                    continue;
                }
                set_alpha(o, 1 - k);
                fade_wait = 0.016;
            }
            any |= can_show(o);
        }

        /* Read ahead only while something will show it. */
        while (any && queued < QUEUE) {
            double d;
            AVFrame *f = decode_next(&d);
            if (!f) {
                if (gpu_failed || !decoded_any) {
                    fprintf(stderr, "fanvid: the GPU cannot decode %s\n", path);
                    status = gpu_failed ? EXIT_NO_GPU : EXIT_ERROR;
                } else {
                    fprintf(stderr, "fanvid: decoding %s failed\n", path);
                    status = EXIT_ERROR;
                }
                stop = 1;
                break;
            }
            queue[queued] = f;
            durations[queued++] = d;
        }
        if (stop)
            break;

        if (any && queued && t >= due) {
            present(queue[0]);
            stats();
            /* Late by more than a frame (hidden, paused, or stalled):
             * from now, rather than catching up. */
            due = t - due > durations[0] ? t + durations[0] : due + durations[0];
            av_frame_free(&queue[0]);
            memmove(queue, queue + 1, sizeof queue[0] * (QUEUE - 1));
            memmove(durations, durations + 1, sizeof durations[0] * (QUEUE - 1));
            queued--;
        }
        wl_display_flush(display);

        double wait = any ? due - now() : 1.0;
        if (fade_wait >= 0 && fade_wait < wait)
            wait = fade_wait;
        if (wait < 0)
            wait = 0;
        struct pollfd pfd[2] = {{wl_fd, POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
        if (wl_display_prepare_read(display) != 0) {
            wl_display_dispatch_pending(display);
            continue;
        }
        const int n = poll(pfd, stdin_open ? 2 : 1, (int)(wait * 1000));
        if (n > 0 && (pfd[0].revents & POLLIN))
            wl_display_read_events(display);
        else
            wl_display_cancel_read(display);
        if (n > 0 && stdin_open && (pfd[1].revents & (POLLIN | POLLHUP))) {
            if (!read_commands()) {
                stdin_open = false;
                stop = 1;
            }
        }
        if (wl_display_dispatch_pending(display) < 0) {
            fprintf(stderr, "fanvid: lost the compositor\n");
            status = EXIT_ERROR;
            break;
        }
    }
    for (int i = 0; i < QUEUE; i++)
        av_frame_free(&queue[i]);
    wl_display_disconnect(display);
    return status;
}
