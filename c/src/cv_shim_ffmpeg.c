/*
 * cv_shim의 FFmpeg 구현 (윈도우 패키징용 — 8/31 회의)
 * - OpenCV는 MSVC C++ ABI라 mingw 크로스 빌드에서 못 쓴다.
 *   캡처를 FFmpeg C API(libavformat/avcodec/swscale/avdevice)로 대체한다.
 * - 소스 해석:
 *     "0", "1"      → dshow 웹캠 (장치 목록에서 N번째 비디오 장치)
 *     "video=이름"  → dshow 장치 이름 직접 지정
 *     그 외          → RTSP URL 또는 동영상 파일
 * - 디버그 창/그리기는 미지원(스텁) — 운영은 헤드리스, 확인은 점주 페이지.
 */
#include "cv_shim.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>

struct CvsCapture {
    AVFormatContext *fmt;
    AVCodecContext *dec;
    AVPacket *pkt;
    AVFrame *frame;
    struct SwsContext *sws;
    int stream_idx;
    int hint_w;                /* 요청 해상도 (초과 시 축소) */
    uint8_t *out;              /* BGR24 출력 버퍼 */
    int out_w, out_h, out_stride;
};

/* dshow 비디오 장치 N번째의 이름을 "video=..." 형태로 만든다 */
static bool dshow_nth_video(int idx, char *out, size_t out_len)
{
    const AVInputFormat *dshow = av_find_input_format("dshow");
    if (!dshow) return false;
    AVDeviceInfoList *list = NULL;
    if (avdevice_list_input_sources(dshow, NULL, NULL, &list) < 0 || !list)
        return false;
    bool found = false;
    int n = 0;
    for (int i = 0; i < list->nb_devices && !found; i++) {
        const AVDeviceInfo *d = list->devices[i];
        bool is_video = d->nb_media_types == 0;   /* 정보 없으면 후보로 취급 */
        for (int m = 0; m < d->nb_media_types; m++)
            if (d->media_types[m] == AVMEDIA_TYPE_VIDEO) is_video = true;
        if (!is_video) continue;
        if (n++ == idx) {
            const char *name = d->device_description && d->device_description[0]
                               ? d->device_description : d->device_name;
            snprintf(out, out_len, "video=%s", name);
            fprintf(stderr, "[캡처] dshow 웹캠 %d번: %s\n", idx, name);
            found = true;
        }
    }
    avdevice_free_list_devices(&list);
    if (!found)
        fprintf(stderr, "[캡처] dshow 비디오 장치 %d번 없음 — "
                        "\"video=장치이름\" 으로 직접 지정하세요\n", idx);
    return found;
}

CvsCapture *cvs_open(const char *source, int width, int height)
{
    (void)height;
    static bool inited = false;
    if (!inited) {
        avdevice_register_all();
        avformat_network_init();
        av_log_set_level(AV_LOG_ERROR);
        inited = true;
    }

    char url[512];
    const AVInputFormat *ifmt = NULL;
    AVDictionary *opts = NULL;

    char *end = NULL;
    long idx = strtol(source, &end, 10);
    if (end && *end == '\0') {                     /* 웹캠 인덱스 */
        if (!dshow_nth_video((int)idx, url, sizeof(url))) return NULL;
        ifmt = av_find_input_format("dshow");
        av_dict_set(&opts, "rtbufsize", "50M", 0);
    } else if (strncmp(source, "video=", 6) == 0) { /* dshow 장치 이름 */
        snprintf(url, sizeof(url), "%s", source);
        ifmt = av_find_input_format("dshow");
        av_dict_set(&opts, "rtbufsize", "50M", 0);
    } else {                                        /* RTSP/파일 */
        snprintf(url, sizeof(url), "%s", source);
        if (strncmp(source, "rtsp://", 7) == 0) {
            av_dict_set(&opts, "rtsp_transport", "tcp", 0);
            av_dict_set(&opts, "stimeout", "5000000", 0);   /* 5초 */
        }
    }

    CvsCapture *c = calloc(1, sizeof(CvsCapture));
    c->hint_w = width;
    if (avformat_open_input(&c->fmt, url, ifmt, &opts) < 0) {
        fprintf(stderr, "[캡처] 소스 열기 실패: %s\n", url);
        av_dict_free(&opts);
        free(c);
        return NULL;
    }
    av_dict_free(&opts);
    if (avformat_find_stream_info(c->fmt, NULL) < 0) goto fail;

    const AVCodec *codec = NULL;
    c->stream_idx = av_find_best_stream(c->fmt, AVMEDIA_TYPE_VIDEO, -1, -1,
                                        &codec, 0);
    if (c->stream_idx < 0 || !codec) goto fail;

    c->dec = avcodec_alloc_context3(codec);
    if (!c->dec) goto fail;
    avcodec_parameters_to_context(c->dec, c->fmt->streams[c->stream_idx]->codecpar);
    c->dec->thread_count = 1;                       /* 리소스 정책: 캡처는 1스레드 */
    if (avcodec_open2(c->dec, codec, NULL) < 0) goto fail;

    c->pkt = av_packet_alloc();
    c->frame = av_frame_alloc();
    if (!c->pkt || !c->frame) goto fail;
    return c;

fail:
    cvs_close(c);
    return NULL;
}

/* 첫 프레임 크기 기준으로 BGR24 변환기·버퍼 준비 (요청 폭 초과 시 축소) */
static bool ensure_sws(CvsCapture *c)
{
    if (c->sws) return true;
    int w = c->frame->width, h = c->frame->height;
    if (c->hint_w > 0 && w > c->hint_w) {
        h = (int)((long long)h * c->hint_w / w) & ~1;
        w = c->hint_w & ~1;
    }
    c->out_w = w;
    c->out_h = h;
    c->out_stride = w * 3;
    c->out = malloc((size_t)c->out_stride * (size_t)h);
    c->sws = sws_getContext(c->frame->width, c->frame->height,
                            (enum AVPixelFormat)c->frame->format,
                            w, h, AV_PIX_FMT_BGR24,
                            SWS_BILINEAR, NULL, NULL, NULL);
    return c->sws && c->out;
}

/* 프레임 1장 디코딩. convert=0이면 색 변환(sws_scale) 생략 — grab 전용. */
static int decode_next(CvsCapture *c, int convert,
                       uint8_t **data, int *w, int *h, int *stride)
{
    if (!c || !c->fmt) return 0;
    for (;;) {
        int rc = av_read_frame(c->fmt, c->pkt);
        if (rc < 0) return 0;                       /* EOF/오류 — 호출측이 재시도 */
        if (c->pkt->stream_index != c->stream_idx) {
            av_packet_unref(c->pkt);
            continue;
        }
        rc = avcodec_send_packet(c->dec, c->pkt);
        av_packet_unref(c->pkt);
        if (rc < 0 && rc != AVERROR(EAGAIN)) return 0;
        rc = avcodec_receive_frame(c->dec, c->frame);
        if (rc == AVERROR(EAGAIN)) continue;        /* 프레임 완성 전 — 더 읽기 */
        if (rc < 0) return 0;

        if (!convert) return 1;                     /* 스트림 전진만 (변환 생략) */

        if (!ensure_sws(c)) return 0;
        uint8_t *dst[4] = { c->out, NULL, NULL, NULL };
        int dst_stride[4] = { c->out_stride, 0, 0, 0 };
        sws_scale(c->sws, (const uint8_t *const *)c->frame->data,
                  c->frame->linesize, 0, c->frame->height, dst, dst_stride);
        *data = c->out;
        *w = c->out_w;
        *h = c->out_h;
        *stride = c->out_stride;
        return 1;
    }
}

int cvs_read(CvsCapture *c, uint8_t **data, int *w, int *h, int *stride)
{
    return decode_next(c, 1, data, w, h, stride);
}

int cvs_grab(CvsCapture *c)
{
    return decode_next(c, 0, NULL, NULL, NULL, NULL);
}

void cvs_close(CvsCapture *c)
{
    if (!c) return;
    if (c->sws) sws_freeContext(c->sws);
    free(c->out);
    if (c->frame) av_frame_free(&c->frame);
    if (c->pkt) av_packet_free(&c->pkt);
    if (c->dec) avcodec_free_context(&c->dec);
    if (c->fmt) avformat_close_input(&c->fmt);
    free(c);
}

/* ── 디버그 창/그리기: 윈도우판 미지원 스텁 (헤드리스 운영 전용) ── */
void cvs_show(const char *window, const uint8_t *data, int w, int h, int stride)
{ (void)window; (void)data; (void)w; (void)h; (void)stride; }
int cvs_waitkey(int ms) { (void)ms; return -1; }
void cvs_destroy_all_windows(void) {}
void cvs_rect(uint8_t *d, int w, int h, int stride, int x1, int y1, int x2,
              int y2, int b, int g, int r, int thick)
{ (void)d;(void)w;(void)h;(void)stride;(void)x1;(void)y1;(void)x2;(void)y2;
  (void)b;(void)g;(void)r;(void)thick; }
void cvs_line(uint8_t *d, int w, int h, int stride, int x1, int y1, int x2,
              int y2, int b, int g, int r, int thick)
{ (void)d;(void)w;(void)h;(void)stride;(void)x1;(void)y1;(void)x2;(void)y2;
  (void)b;(void)g;(void)r;(void)thick; }
void cvs_circle(uint8_t *d, int w, int h, int stride, int cx, int cy,
                int radius, int b, int g, int r, int thick)
{ (void)d;(void)w;(void)h;(void)stride;(void)cx;(void)cy;(void)radius;
  (void)b;(void)g;(void)r;(void)thick; }
void cvs_text(uint8_t *d, int w, int h, int stride, const char *text,
              int x, int y, double scale, int b, int g, int r, int thick)
{ (void)d;(void)w;(void)h;(void)stride;(void)text;(void)x;(void)y;(void)scale;
  (void)b;(void)g;(void)r;(void)thick; }
