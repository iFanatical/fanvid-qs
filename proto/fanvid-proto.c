/* fanvid-proto <file> [output...]: the NV12 prototype. Plays <file>, looped
 * and silent, on a background layer surface on each named output (every
 * output when none are named), from one decoder: VA-API decodes, each frame
 * is exported as a dmabuf and handed to the compositor as is, which scales
 * it (wp_viewporter, "fill") and converts it from YUV. Nothing is drawn
 * here.
 *
 * It reports what the compositor offers for NV12, the format and modifier
 * the decoder exports, and every 10 s the frames shown and RssAnon. */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
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
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
#include <wayland-client.h>

#include "color-representation-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define MAX_OUTPUTS 8
#define MAX_MODS 64
#define QUEUE 4

/* ------------------------------------------------------------ wayland */

struct output {
    struct wl_output *wl;
    uint32_t global;
    char name[64];
    /* The layer surface on it, when it is one we play on. */
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer;
    struct wp_viewport *viewport;
    struct wp_color_representation_surface_v1 *colorrep;
    int width, height; /* configured, logical */
    bool configured;
    /* A frame callback is pending: the last frame is not shown yet. */
    bool waiting;
    struct wl_callback *frame_cb;
    /* The frame it shows now. */
    struct frame *shown;
};

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwp_linux_dmabuf_v1 *dmabuf;
static struct wp_viewporter *viewporter;
static struct wp_color_representation_manager_v1 *colorrep_mgr;
static struct output outputs[MAX_OUTPUTS];
static int n_outputs;
static uint64_t nv12_mods[MAX_MODS];
static int n_nv12_mods;
static uint64_t p010_mods[MAX_MODS];
static int n_p010_mods;
/* The RGB format the video processor makes (rgb0: R, G, B, X in memory). */
static const uint32_t rgb_format = DRM_FORMAT_XBGR8888;
static uint64_t rgb_mods[MAX_MODS];
static int n_rgb_mods;
static volatile sig_atomic_t stop;

static void note_format(uint32_t format, uint64_t mod)
{
    if (format == DRM_FORMAT_NV12 && n_nv12_mods < MAX_MODS)
        nv12_mods[n_nv12_mods++] = mod;
    if (format == DRM_FORMAT_P010 && n_p010_mods < MAX_MODS)
        p010_mods[n_p010_mods++] = mod;
    if (format == rgb_format && n_rgb_mods < MAX_MODS)
        rgb_mods[n_rgb_mods++] = mod;
}

/* linux-dmabuf v4: the default feedback's format table, and the indices
 * into it each tranche offers. */
struct format_entry {
    uint32_t format;
    uint32_t pad;
    uint64_t modifier;
};
static struct format_entry *format_table;
static size_t format_table_len;
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
    if (map == MAP_FAILED)
        return;
    format_table = map;
    format_table_len = size / sizeof *format_table;
}
static void fb_main_device(void *d, struct zwp_linux_dmabuf_feedback_v1 *f,
                           struct wl_array *dev)
{
    (void)d; (void)f; (void)dev;
}
static void fb_tranche_done(void *d, struct zwp_linux_dmabuf_feedback_v1 *f)
{
    (void)d; (void)f;
}
static void fb_tranche_target_device(void *d, struct zwp_linux_dmabuf_feedback_v1 *f,
                                     struct wl_array *dev)
{
    (void)d; (void)f; (void)dev;
}
static void fb_tranche_formats(void *d, struct zwp_linux_dmabuf_feedback_v1 *f,
                               struct wl_array *indices)
{
    (void)d; (void)f;
    const uint16_t *idx;
    uint32_t seen[64];
    int n_seen = 0;
    wl_array_for_each(idx, indices)
    {
        if (*idx >= format_table_len)
            continue;
        const uint32_t format = format_table[*idx].format;
        note_format(format, format_table[*idx].modifier);
        bool dup = false;
        for (int i = 0; i < n_seen; i++)
            dup |= seen[i] == format;
        if (!dup && n_seen < 64)
            seen[n_seen++] = format;
    }
    printf("compositor: tranche of %zu formats:", indices->size / sizeof(uint16_t));
    for (int i = 0; i < n_seen; i++)
        printf(" %.4s", (const char *)&seen[i]);
    printf("\n");
}
static void fb_tranche_flags(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, uint32_t flags)
{
    (void)d; (void)f; (void)flags;
}

static const struct zwp_linux_dmabuf_feedback_v1_listener feedback_listener = {
    .done = fb_done,
    .format_table = fb_format_table,
    .main_device = fb_main_device,
    .tranche_done = fb_tranche_done,
    .tranche_target_device = fb_tranche_target_device,
    .tranche_formats = fb_tranche_formats,
    .tranche_flags = fb_tranche_flags,
};

static void output_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw,
                            int32_t ph, int32_t sp, const char *make, const char *model,
                            int32_t t)
{
    (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sp; (void)make;
    (void)model; (void)t;
}
static void output_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h,
                        int32_t r)
{
    (void)d; (void)o; (void)f; (void)w; (void)h; (void)r;
}
static void output_done(void *d, struct wl_output *o) { (void)d; (void)o; }
static void output_scale(void *d, struct wl_output *o, int32_t s) { (void)d; (void)o; (void)s; }
static void output_name(void *data, struct wl_output *o, const char *name)
{
    (void)o;
    struct output *out = data;
    snprintf(out->name, sizeof out->name, "%s", name);
}
static void output_description(void *d, struct wl_output *o, const char *s)
{
    (void)d; (void)o; (void)s;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version)
{
    (void)data;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
        layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name)) {
        /* v4: what it takes comes as feedback (below). */
        if (version >= 4)
            dmabuf = wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, 4);
    } else if (!strcmp(iface, wp_viewporter_interface.name)) {
        viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
    } else if (!strcmp(iface, wp_color_representation_manager_v1_interface.name)) {
        colorrep_mgr = wl_registry_bind(reg, name,
                                        &wp_color_representation_manager_v1_interface, 1);
    } else if (!strcmp(iface, wl_output_interface.name) && n_outputs < MAX_OUTPUTS
               && version >= 4) {
        struct output *out = &outputs[n_outputs++];
        out->global = name;
        out->wl = wl_registry_bind(reg, name, &wl_output_interface, 4);
        wl_output_add_listener(out->wl, &output_listener, out);
    }
}

static void registry_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data; (void)reg;
    for (int i = 0; i < n_outputs; i++)
        if (outputs[i].global == name && outputs[i].surface) {
            fprintf(stderr, "fanvid: %s went away\n", outputs[i].name);
            stop = 1;
        }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *l, uint32_t serial,
                            uint32_t w, uint32_t h)
{
    struct output *out = data;
    out->width = (int)w;
    out->height = (int)h;
    out->configured = true;
    zwlr_layer_surface_v1_ack_configure(l, serial);
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *l)
{
    (void)l;
    struct output *out = data;
    fprintf(stderr, "fanvid: the compositor closed the surface on %s\n", out->name);
    stop = 1;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

/* ------------------------------------------------------------ frames */

/* A decoded frame, mapped to a dmabuf, and the wl_buffers made of it, one
 * per output (a buffer attached to several surfaces is released once,
 * which says nothing about the others). Freed when no output shows it and
 * the compositor has released every buffer. */
struct frame {
    AVFrame *va;
    AVFrame *drm;
    struct wl_buffer *buffers[MAX_OUTPUTS];
    int held; /* buffers not yet released */
    int shown_on; /* outputs showing it */
    int64_t pts;
    double duration; /* seconds */
};

static long frames_shown;
static long frames_freed;

static void frame_free_if_done(struct frame *f)
{
    if (!f || f->held || f->shown_on)
        return;
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (f->buffers[i])
            wl_buffer_destroy(f->buffers[i]);
    av_frame_free(&f->drm);
    av_frame_free(&f->va);
    free(f);
    frames_freed++;
}

static void buffer_release(void *data, struct wl_buffer *b)
{
    struct frame *f = data;
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (f->buffers[i] == b) {
            wl_buffer_destroy(b);
            f->buffers[i] = NULL;
            f->held--;
        }
    frame_free_if_done(f);
}

static const struct wl_buffer_listener buffer_listener = {.release = buffer_release};

/* The DRM format of the frame as one buffer: VA-API exports the luma and
 * chroma planes as layers of their own (R8 and GR88 for NV12). */
static uint32_t frame_format(const AVDRMFrameDescriptor *d)
{
    if (d->nb_layers == 1)
        return d->layers[0].format;
    if (d->nb_layers == 2 && d->layers[0].format == DRM_FORMAT_R8
        && d->layers[1].format == DRM_FORMAT_GR88)
        return DRM_FORMAT_NV12;
    if (d->nb_layers == 2 && d->layers[0].format == DRM_FORMAT_R16
        && d->layers[1].format == DRM_FORMAT_GR1616)
        return DRM_FORMAT_P010;
    return 0;
}

static struct wl_buffer *frame_buffer(struct frame *f, int w, int h)
{
    const AVDRMFrameDescriptor *d = (const AVDRMFrameDescriptor *)f->drm->data[0];
    const uint32_t format = frame_format(d);
    struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(dmabuf);
    int plane = 0;
    for (int l = 0; l < d->nb_layers; l++)
        for (int p = 0; p < d->layers[l].nb_planes; p++) {
            const AVDRMPlaneDescriptor *pd = &d->layers[l].planes[p];
            const AVDRMObjectDescriptor *obj = &d->objects[pd->object_index];
            zwp_linux_buffer_params_v1_add(params, obj->fd, plane++, (uint32_t)pd->offset,
                                           (uint32_t)pd->pitch, obj->format_modifier >> 32,
                                           obj->format_modifier & 0xffffffff);
        }
    struct wl_buffer *b = zwp_linux_buffer_params_v1_create_immed(params, w, h, format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    return b;
}

/* ------------------------------------------------------------ decoding */

static AVFormatContext *fmt;
static AVCodecContext *dec;
static AVBufferRef *hwdev;
static int stream;
static AVPacket *pkt;
static int64_t loop_offset; /* added to pts after each loop, in stream units */
static int64_t last_pts = AV_NOPTS_VALUE;

/* VA-API's video processor turns each frame into RGB at the output's size
 * (vpp_w x vpp_h), as compositors take RGB dmabufs where they may not
 * show NV12 ones (Hyprland 0.57 shows only the luma plane). 0: frames go
 * out as decoded (FANVID_NV12=1). */
static int vpp_w, vpp_h;
static int vpp_frames; /* surfaces the converted frames may hold at once */
static AVFilterGraph *graph;
static AVFilterContext *src_ctx, *sink_ctx;

static const char *or_default(const char *name, const char *fallback)
{
    return name && strcmp(name, "unknown") && strcmp(name, "unspecified") ? name : fallback;
}

/* Built for the first frame, which brings the decoder's surfaces. The
 * colour properties are spelt out, unknown ones as BT.709: with any left
 * unknown, Intel's driver converts with BT.601's matrix; and the output is
 * asked for in full range, which AMD's otherwise leaves limited. */
static int build_graph(const AVFrame *f)
{
    const AVStream *s = fmt->streams[stream];
    char desc[512];
    graph = avfilter_graph_alloc();
    /* Its surfaces before its format, which it checks on init. */
    src_ctx = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffer"), "in");
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
    par->format = AV_PIX_FMT_VAAPI;
    par->width = f->width;
    par->height = f->height;
    par->time_base = s->time_base;
    par->sample_aspect_ratio = (AVRational){1, 1};
    par->hw_frames_ctx = f->hw_frames_ctx;
    par->color_space = f->colorspace;
    par->color_range = f->color_range;
    int r = av_buffersrc_parameters_set(src_ctx, par);
    av_free(par);
    if (r < 0 || (r = avfilter_init_str(src_ctx, NULL)) < 0)
        return r;
    if ((r = avfilter_graph_create_filter(&sink_ctx, avfilter_get_by_name("buffersink"), "out",
                                          NULL, NULL, graph)) < 0)
        return r;
    snprintf(desc, sizeof desc,
             "setparams=color_primaries=%s:color_trc=%s:colorspace=%s:range=%s,"
             "scale_vaapi=w=%d:h=%d:format=rgb0:out_range=full:"
             "out_color_primaries=bt709:out_color_transfer=bt709",
             or_default(av_color_primaries_name(f->color_primaries), "bt709"),
             or_default(av_color_transfer_name(f->color_trc), "bt709"),
             or_default(av_color_space_name(f->colorspace), "bt709"),
             f->color_range == AVCOL_RANGE_JPEG ? "pc" : "tv", vpp_w, vpp_h);
    AVFilterInOut *outs = avfilter_inout_alloc(), *ins = avfilter_inout_alloc();
    outs->name = av_strdup("in");
    outs->filter_ctx = src_ctx;
    ins->name = av_strdup("out");
    ins->filter_ctx = sink_ctx;
    r = avfilter_graph_parse_ptr(graph, desc, &ins, &outs, NULL);
    avfilter_inout_free(&ins);
    avfilter_inout_free(&outs);
    if (r < 0)
        return r;
    for (unsigned i = 0; i < graph->nb_filters; i++)
        if (!strcmp(graph->filters[i]->filter->name, "scale_vaapi"))
            graph->filters[i]->extra_hw_frames = vpp_frames;
    if ((r = avfilter_graph_config(graph, NULL)) < 0)
        return r;
    printf("vpp: %s\n", desc);
    return 0;
}

static enum AVPixelFormat pick_vaapi(AVCodecContext *c, const enum AVPixelFormat *fmts)
{
    (void)c;
    for (; *fmts != AV_PIX_FMT_NONE; fmts++)
        if (*fmts == AV_PIX_FMT_VAAPI)
            return *fmts;
    fprintf(stderr, "fanvid: no VA-API decoding for this video\n");
    return AV_PIX_FMT_NONE;
}

static int open_video(const char *path)
{
    int r;
    if ((r = avformat_open_input(&fmt, path, NULL, NULL)) < 0
        || (r = avformat_find_stream_info(fmt, NULL)) < 0) {
        fprintf(stderr, "fanvid: cannot read %s: %s\n", path, av_err2str(r));
        return -1;
    }
    const AVCodec *codec = NULL;
    stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (stream < 0) {
        fprintf(stderr, "fanvid: no video in %s\n", path);
        return -1;
    }
    if ((r = av_hwdevice_ctx_create(&hwdev, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0)) < 0) {
        fprintf(stderr, "fanvid: no VA-API device: %s\n", av_err2str(r));
        return -1;
    }
    dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, fmt->streams[stream]->codecpar);
    dec->hw_device_ctx = av_buffer_ref(hwdev);
    dec->get_format = pick_vaapi;
    /* Frames held by the compositor and the queue, beyond the decoder's. */
    dec->extra_hw_frames = QUEUE + 2 * MAX_OUTPUTS;
    if ((r = avcodec_open2(dec, codec, NULL)) < 0) {
        fprintf(stderr, "fanvid: cannot decode %s: %s\n", path, av_err2str(r));
        return -1;
    }
    pkt = av_packet_alloc();
    const AVStream *s = fmt->streams[stream];
    printf("video: %s %dx%d %s, %.3f fps, %s range, %s matrix\n", codec->name, dec->width,
           dec->height, av_get_pix_fmt_name(dec->sw_pix_fmt),
           av_q2d(s->avg_frame_rate), av_color_range_name(dec->color_range),
           av_color_space_name(dec->colorspace));
    return 0;
}

/* The next decoded frame, looping at the end; NULL on an error. */
static AVFrame *decode_next(void)
{
    AVFrame *f = av_frame_alloc();
    for (;;) {
        int r = avcodec_receive_frame(dec, f);
        if (r == 0)
            return f;
        if (r == AVERROR_EOF) {
            /* The end: from the start again. */
            avcodec_flush_buffers(dec);
            av_seek_frame(fmt, stream, 0, AVSEEK_FLAG_BACKWARD);
            if (last_pts != AV_NOPTS_VALUE) {
                const AVStream *s = fmt->streams[stream];
                const int64_t step = av_rescale_q(1, av_inv_q(s->avg_frame_rate), s->time_base);
                loop_offset = last_pts + step;
            }
            continue;
        }
        if (r != AVERROR(EAGAIN)) {
            fprintf(stderr, "fanvid: decoding: %s\n", av_err2str(r));
            av_frame_free(&f);
            return NULL;
        }
        r = av_read_frame(fmt, pkt);
        if (r == AVERROR_EOF) {
            avcodec_send_packet(dec, NULL);
            continue;
        }
        if (r < 0) {
            fprintf(stderr, "fanvid: reading: %s\n", av_err2str(r));
            av_frame_free(&f);
            return NULL;
        }
        if (pkt->stream_index == stream)
            avcodec_send_packet(dec, pkt);
        av_packet_unref(pkt);
    }
}

static struct frame *next_frame(void)
{
    AVFrame *va = decode_next();
    if (!va)
        return NULL;
    const int64_t decoded_pts = va->best_effort_timestamp;
    if (vpp_w) {
        int r = graph ? 0 : build_graph(va);
        if (r >= 0)
            r = av_buffersrc_add_frame(src_ctx, va);
        av_frame_free(&va);
        AVFrame *rgb = av_frame_alloc();
        if (r >= 0)
            r = av_buffersink_get_frame(sink_ctx, rgb);
        if (r < 0) {
            fprintf(stderr, "fanvid: cannot convert a frame: %s\n", av_err2str(r));
            av_frame_free(&rgb);
            return NULL;
        }
        va = rgb;
    }
    struct frame *f = calloc(1, sizeof *f);
    f->va = va;
    f->drm = av_frame_alloc();
    f->drm->format = AV_PIX_FMT_DRM_PRIME;
    int r = av_hwframe_map(f->drm, va, AV_HWFRAME_MAP_READ);
    if (r < 0) {
        fprintf(stderr, "fanvid: cannot export a frame: %s\n", av_err2str(r));
        av_frame_free(&f->drm);
        av_frame_free(&f->va);
        free(f);
        return NULL;
    }
    /* Timestamps run on across loops (loop_offset), so each frame's
     * duration is the step to the next even at the seam. */
    const AVStream *s = fmt->streams[stream];
    const double frame_s = 1.0 / av_q2d(s->avg_frame_rate);
    const int64_t pts = (decoded_pts == AV_NOPTS_VALUE
                             ? (last_pts == AV_NOPTS_VALUE ? 0 : last_pts - loop_offset + 1)
                             : decoded_pts)
                        + loop_offset;
    f->pts = pts;
    f->duration = last_pts == AV_NOPTS_VALUE || pts <= last_pts
                      ? frame_s
                      : (double)(pts - last_pts) * av_q2d(s->time_base);
    last_pts = pts;
    return f;
}

/* ------------------------------------------------------------ playing */

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static long rss_anon(void)
{
    FILE *fp = fopen("/proc/self/status", "r");
    char line[256];
    long kb = -1;
    while (fp && fgets(line, sizeof line, fp))
        if (sscanf(line, "RssAnon: %ld", &kb) == 1)
            break;
    if (fp)
        fclose(fp);
    return kb;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t t)
{
    (void)t;
    struct output *out = data;
    wl_callback_destroy(cb);
    out->frame_cb = NULL;
    out->waiting = false;
}

static const struct wl_callback_listener frame_listener = {.done = frame_done};

/* The part of the frame "fill" shows: all of one side, centred on the
 * other, so the output is covered without stretching. */
static void set_fill(struct output *out, int fw, int fh)
{
    double sx = 0, sy = 0, sw = fw, sh = fh;
    const double fa = (double)fw / fh, oa = (double)out->width / out->height;
    if (fa > oa) {
        sw = fh * oa;
        sx = (fw - sw) / 2;
    } else {
        sh = fw / oa;
        sy = (fh - sh) / 2;
    }
    wp_viewport_set_source(out->viewport, wl_fixed_from_double(sx), wl_fixed_from_double(sy),
                           wl_fixed_from_double(sw), wl_fixed_from_double(sh));
    wp_viewport_set_destination(out->viewport, out->width, out->height);
}

static void show(struct frame *f, struct output *out, int idx)
{
    const int w = f->va->width, h = f->va->height;
    struct wl_buffer *b = frame_buffer(f, w, h);
    f->buffers[idx] = b;
    f->held++;
    wl_buffer_add_listener(b, &buffer_listener, f);
    if (!out->shown)
        set_fill(out, w, h);
    wl_surface_attach(out->surface, b, 0, 0);
    wl_surface_damage_buffer(out->surface, 0, 0, w, h);
    out->frame_cb = wl_surface_frame(out->surface);
    wl_callback_add_listener(out->frame_cb, &frame_listener, out);
    out->waiting = true;
    wl_surface_commit(out->surface);
    struct frame *old = out->shown;
    out->shown = f;
    f->shown_on++;
    if (old) {
        old->shown_on--;
        frame_free_if_done(old);
    }
}

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static bool wanted(const char *name, int argc, char **argv)
{
    if (argc <= 2)
        return true;
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], name))
            return true;
    return false;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: fanvid-proto <file> [output...]\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "fanvid: no Wayland display\n");
        return 1;
    }
    struct wl_registry *reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
    if (!compositor || !layer_shell || !dmabuf || !viewporter) {
        fprintf(stderr, "fanvid: the compositor lacks %s%s%s%s\n", compositor ? "" : "wl_compositor ",
                layer_shell ? "" : "wlr-layer-shell ", dmabuf ? "" : "linux-dmabuf(v4) ",
                viewporter ? "" : "wp_viewporter");
        return 1;
    }
    struct zwp_linux_dmabuf_feedback_v1 *feedback =
        zwp_linux_dmabuf_v1_get_default_feedback(dmabuf);
    zwp_linux_dmabuf_feedback_v1_add_listener(feedback, &feedback_listener, NULL);
    while (!feedback_done && wl_display_roundtrip(display) >= 0) {
    }
    printf("compositor: NV12 with %d modifiers:", n_nv12_mods);
    for (int i = 0; i < n_nv12_mods; i++)
        printf(" 0x%" PRIx64, nv12_mods[i]);
    printf("\ncompositor: P010 with %d modifiers; XB24 with %d:", n_p010_mods, n_rgb_mods);
    for (int i = 0; i < n_rgb_mods; i++)
        printf(" 0x%" PRIx64, rgb_mods[i]);
    printf("; color-representation %s\n", colorrep_mgr ? "offered" : "not offered");

    if (open_video(argv[1]) < 0)
        return 1;

    for (int i = 0; i < n_outputs; i++) {
        struct output *out = &outputs[i];
        if (!wanted(out->name, argc, argv))
            continue;
        out->surface = wl_compositor_create_surface(compositor);
        out->layer = zwlr_layer_shell_v1_get_layer_surface(
            layer_shell, out->surface, out->wl,
            getenv("FANVID_LAYER") && !strcmp(getenv("FANVID_LAYER"), "overlay")
                ? ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY
                : ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
            "fanvid");
        zwlr_layer_surface_v1_add_listener(out->layer, &layer_listener, out);
        zwlr_layer_surface_v1_set_anchor(out->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
                                                         | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
                                                         | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
                                                         | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_exclusive_zone(out->layer, -1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(out->layer, 0);
        /* Clicks go through, to what is beneath. */
        struct wl_region *none = wl_compositor_create_region(compositor);
        wl_surface_set_input_region(out->surface, none);
        wl_region_destroy(none);
        out->viewport = wp_viewporter_get_viewport(viewporter, out->surface);
        if (colorrep_mgr) {
            out->colorrep = wp_color_representation_manager_v1_get_surface(colorrep_mgr,
                                                                           out->surface);
            const uint32_t coef = dec->colorspace == AVCOL_SPC_BT470BG
                                          || dec->colorspace == AVCOL_SPC_SMPTE170M
                                      ? WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT601
                                  : dec->colorspace == AVCOL_SPC_BT2020_NCL
                                      ? WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT2020
                                      : WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT709;
            const uint32_t range = dec->color_range == AVCOL_RANGE_JPEG
                                       ? WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL
                                       : WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_LIMITED;
            wp_color_representation_surface_v1_set_coefficients_and_range(out->colorrep, coef,
                                                                          range);
        }
        wl_surface_commit(out->surface);
        printf("output: %s\n", out->name);
    }
    wl_display_roundtrip(display);

    int active = 0;
    for (int i = 0; i < n_outputs; i++)
        if (outputs[i].configured) {
            active++;
            if (!vpp_w && !getenv("FANVID_NV12")) {
                const double sc = fmax((double)outputs[i].width / dec->width,
                                       (double)outputs[i].height / dec->height);
                vpp_w = ((int)(dec->width * sc + 1)) & ~1;
                vpp_h = ((int)(dec->height * sc + 1)) & ~1;
            }
        }
    /* The queue, plus one shown and one being replaced on each output. */
    vpp_frames = QUEUE + 2 * active + 2;

    struct frame *queue[QUEUE] = {0};
    int queued = 0;
    bool reported = false;
    double due = 0; /* when the next frame shows */
    double next_report = now() + 10;
    const int fd = wl_display_get_fd(display);

    while (!stop) {
        while (queued < QUEUE) {
            struct frame *f = next_frame();
            if (!f) {
                stop = 1;
                break;
            }
            if (!reported) {
                reported = true;
                const AVDRMFrameDescriptor *d = (const AVDRMFrameDescriptor *)f->drm->data[0];
                const uint32_t format = frame_format(d);
                const uint64_t mod = d->objects[0].format_modifier;
                bool offered = false;
                for (int i = 0; i < n_nv12_mods; i++)
                    offered |= format == DRM_FORMAT_NV12 && nv12_mods[i] == mod;
                for (int i = 0; i < n_rgb_mods; i++)
                    offered |= format == rgb_format && rgb_mods[i] == mod;
                for (int i = 0; i < n_p010_mods; i++)
                    offered |= format == DRM_FORMAT_P010 && p010_mods[i] == mod;
                printf("decoder: %d layers, %d objects, format %.4s, modifier 0x%" PRIx64
                       " -- %s by the compositor\n",
                       d->nb_layers, d->nb_objects, (const char *)&format, mod,
                       offered ? "offered" : "NOT offered");
            }
            queue[queued++] = f;
        }
        if (stop)
            break;

        /* Show the next frame on every output that has shown the last. */
        const double t = now();
        bool ready = false;
        for (int i = 0; i < n_outputs; i++)
            ready |= outputs[i].configured && !outputs[i].waiting;
        if (ready && t >= due) {
            struct frame *f = queue[0];
            memmove(queue, queue + 1, sizeof queue[0] * (QUEUE - 1));
            queued--;
            for (int i = 0; i < n_outputs; i++)
                if (outputs[i].configured && !outputs[i].waiting)
                    show(f, &outputs[i], i);
            frames_shown++;
            frame_free_if_done(f);
            /* Late by more than a frame (hidden, or stalled): start over
             * from now rather than catch up. */
            due = t - due > f->duration ? t + f->duration : due + f->duration;
        }
        wl_display_flush(display);

        if (t >= next_report) {
            next_report += 10;
            printf("shown %ld frames, %ld freed, RssAnon %ld kB\n", frames_shown, frames_freed,
                   rss_anon());
        }

        /* Wait for the compositor, or for the next frame to be due. */
        int timeout = ready ? (int)((due - now()) * 1000) : 1000;
        if (timeout < 0)
            timeout = 0;
        if (wl_display_prepare_read(display) == 0) {
            struct pollfd pfd = {fd, POLLIN, 0};
            if (poll(&pfd, 1, timeout) > 0)
                wl_display_read_events(display);
            else
                wl_display_cancel_read(display);
        }
        if (wl_display_dispatch_pending(display) < 0) {
            fprintf(stderr, "fanvid: lost the compositor (%s)\n", strerror(errno));
            return 1;
        }
    }
    printf("stopping: shown %ld frames\n", frames_shown);
    wl_display_disconnect(display);
    return 0;
}
