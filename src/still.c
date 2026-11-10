/* fanvid still <file> <out.png>: the video's first frame, at its own size,
 * for fanshell-qs to show beneath the video, in its picker, and on the lock
 * screen. Decoded on the CPU: it is one frame, and must work where the GPU
 * cannot decode the video. Written aside and renamed, so a half-written
 * file is never read. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include "fanvid.h"

static AVFrame *first_frame(AVFormatContext *fmt, int stream, AVCodecContext *dec)
{
    AVPacket *pkt = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    bool flushed = false;
    for (;;) {
        int r = avcodec_receive_frame(dec, f);
        if (r == 0)
            break;
        if (r != AVERROR(EAGAIN) || flushed) {
            av_frame_free(&f);
            break;
        }
        r = av_read_frame(fmt, pkt);
        if (r < 0) {
            avcodec_send_packet(dec, NULL);
            flushed = true;
            continue;
        }
        if (pkt->stream_index == stream)
            avcodec_send_packet(dec, pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    return f;
}

/* `f` as RGB24, colour-converted with its own matrix and range. */
static AVFrame *to_rgb(const AVFrame *f)
{
    AVFrame *rgb = av_frame_alloc();
    rgb->format = AV_PIX_FMT_RGB24;
    rgb->width = f->width;
    rgb->height = f->height;
    if (av_frame_get_buffer(rgb, 0) < 0) {
        av_frame_free(&rgb);
        return NULL;
    }
    struct SwsContext *sws =
        sws_getContext(f->width, f->height, f->format, f->width, f->height, AV_PIX_FMT_RGB24,
                       SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT, NULL, NULL, NULL);
    if (!sws) {
        av_frame_free(&rgb);
        return NULL;
    }
    const int csp = f->colorspace == AVCOL_SPC_BT470BG || f->colorspace == AVCOL_SPC_SMPTE170M
                        ? SWS_CS_ITU601
                    : f->colorspace == AVCOL_SPC_BT2020_NCL ? SWS_CS_BT2020
                                                            : SWS_CS_ITU709;
    const int full = f->color_range == AVCOL_RANGE_JPEG;
    sws_setColorspaceDetails(sws, sws_getCoefficients(csp), full, sws_getCoefficients(csp), 1,
                             0, 1 << 16, 1 << 16);
    sws_scale(sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, rgb->data,
              rgb->linesize);
    sws_freeContext(sws);
    return rgb;
}

static int write_png(const AVFrame *rgb, const char *out)
{
    const AVCodec *png = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!png)
        return -1;
    AVCodecContext *enc = avcodec_alloc_context3(png);
    enc->width = rgb->width;
    enc->height = rgb->height;
    enc->pix_fmt = AV_PIX_FMT_RGB24;
    enc->time_base = (AVRational){1, 1};
    AVPacket *pkt = av_packet_alloc();
    int r = avcodec_open2(enc, png, NULL);
    if (r >= 0)
        r = avcodec_send_frame(enc, rgb);
    if (r >= 0)
        r = avcodec_receive_packet(enc, pkt);
    if (r >= 0) {
        FILE *fp = fopen(out, "wb");
        if (!fp || fwrite(pkt->data, 1, pkt->size, fp) != (size_t)pkt->size)
            r = -1;
        if (fp && fclose(fp) != 0)
            r = -1;
    }
    av_packet_free(&pkt);
    avcodec_free_context(&enc);
    return r;
}

int still_main(const char *path, const char *out)
{
    av_log_set_level(AV_LOG_ERROR);
    AVFormatContext *fmt = NULL;
    int r;
    if ((r = avformat_open_input(&fmt, path, NULL, NULL)) < 0
        || (r = avformat_find_stream_info(fmt, NULL)) < 0) {
        fprintf(stderr, "fanvid: cannot read %s: %s\n", path, av_err2str(r));
        return EXIT_ERROR;
    }
    const AVCodec *codec = NULL;
    const int stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (stream < 0 || !codec) {
        fprintf(stderr, "fanvid: no video in %s\n", path);
        avformat_close_input(&fmt);
        return EXIT_ERROR;
    }
    AVCodecContext *dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, fmt->streams[stream]->codecpar);
    if ((r = avcodec_open2(dec, codec, NULL)) < 0) {
        fprintf(stderr, "fanvid: cannot decode %s: %s\n", path, av_err2str(r));
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return EXIT_ERROR;
    }
    AVFrame *f = first_frame(fmt, stream, dec);
    AVFrame *rgb = f ? to_rgb(f) : NULL;
    int status = EXIT_ERROR;
    if (!rgb) {
        fprintf(stderr, "fanvid: no frame in %s\n", path);
    } else {
        const size_t len = strlen(out) + 6;
        char *part = malloc(len);
        snprintf(part, len, "%s.part", out);
        if (write_png(rgb, part) < 0 || rename(part, out) != 0) {
            fprintf(stderr, "fanvid: cannot write %s\n", out);
            remove(part);
        } else {
            status = EXIT_OK;
        }
        free(part);
    }
    av_frame_free(&rgb);
    av_frame_free(&f);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return status;
}
