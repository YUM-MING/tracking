#include "imgproc.h"

#include <math.h>
#include <string.h>

/*
 * 양선형(bilinear) 보간 + letterbox.
 * 목적지 픽셀마다 원본 좌표를 역산해 한 번에 처리한다.
 * (중간 리사이즈 버퍼 없음 — 캐시/메모리 효율)
 */
void letterbox_chw(const FrameView *frame,
                   int rx1, int ry1, int rx2, int ry2,
                   float *dst, int dst_size, LetterboxInfo *info)
{
    const int rw = rx2 - rx1;
    const int rh = ry2 - ry1;
    const int plane = dst_size * dst_size;

    float scale = (float)dst_size / (float)(rw > rh ? rw : rh);
    if (scale > 1.0f) scale = 1.0f;      /* 업스케일 금지 (ultralytics 기본과 동일) */
    const int new_w = (int)lroundf(rw * scale);
    const int new_h = (int)lroundf(rh * scale);
    const int pad_x = (dst_size - new_w) / 2;
    const int pad_y = (dst_size - new_h) / 2;

    info->scale = scale;
    info->pad_x = (float)pad_x;
    info->pad_y = (float)pad_y;

    /* 패딩 색 114/255 로 전체 초기화 후 유효 영역만 채운다 */
    const float pad_val = 114.0f / 255.0f;
    for (int i = 0; i < plane * 3; i++) dst[i] = pad_val;

    const float inv = 1.0f / scale;
    for (int dy = 0; dy < new_h; dy++) {
        /* 원본 좌표 (ROI 기준) — 픽셀 중심 정렬 */
        float sy = ((float)dy + 0.5f) * inv - 0.5f;
        if (sy < 0) sy = 0;
        int y0 = (int)sy;
        if (y0 > rh - 2) y0 = rh - 2 < 0 ? 0 : rh - 2;
        float fy = sy - (float)y0;
        if (rh == 1) { y0 = 0; fy = 0; }

        const uint8_t *row0 = frame->data + (size_t)(ry1 + y0) * frame->stride;
        const uint8_t *row1 = (rh == 1) ? row0 : row0 + frame->stride;
        float *out_r = dst + 0 * plane + (size_t)(dy + pad_y) * dst_size + pad_x;
        float *out_g = dst + 1 * plane + (size_t)(dy + pad_y) * dst_size + pad_x;
        float *out_b = dst + 2 * plane + (size_t)(dy + pad_y) * dst_size + pad_x;

        for (int dx = 0; dx < new_w; dx++) {
            float sx = ((float)dx + 0.5f) * inv - 0.5f;
            if (sx < 0) sx = 0;
            int x0 = (int)sx;
            if (x0 > rw - 2) x0 = rw - 2 < 0 ? 0 : rw - 2;
            float fx = sx - (float)x0;
            if (rw == 1) { x0 = 0; fx = 0; }

            const uint8_t *p00 = row0 + (size_t)(rx1 + x0) * 3;
            const uint8_t *p01 = p00 + (rw == 1 ? 0 : 3);
            const uint8_t *p10 = row1 + (size_t)(rx1 + x0) * 3;
            const uint8_t *p11 = p10 + (rw == 1 ? 0 : 3);

            float w00 = (1 - fx) * (1 - fy), w01 = fx * (1 - fy);
            float w10 = (1 - fx) * fy,       w11 = fx * fy;

            /* BGR → RGB 채널 스왑 + 0~1 정규화를 한 번에 */
            float b = w00 * p00[0] + w01 * p01[0] + w10 * p10[0] + w11 * p11[0];
            float g = w00 * p00[1] + w01 * p01[1] + w10 * p10[1] + w11 * p11[1];
            float r = w00 * p00[2] + w01 * p01[2] + w10 * p10[2] + w11 * p11[2];
            out_r[dx] = r * (1.0f / 255.0f);
            out_g[dx] = g * (1.0f / 255.0f);
            out_b[dx] = b * (1.0f / 255.0f);
        }
    }
}

/*
 * OpenCV uint8 HSV 변환 규약을 그대로 구현:
 *   V = max(R,G,B)
 *   S = V==0 ? 0 : 255*(V-min)/V
 *   H = 30*(G-B)/(V-min)      (V==R)
 *       60 + 30*(B-R)/(V-min) (V==G)
 *       120 + 30*(R-G)/(V-min)(V==B)   음수면 +180
 * 파이썬 reid_tagger가 cv2.cvtColor(BGR2HSV) 평균을 쓰므로 결과 호환 필수.
 */
void bgr_roi_hsv_mean(const FrameView *frame,
                      int rx1, int ry1, int rx2, int ry2,
                      float *mean_h, float *mean_s)
{
    if (rx1 < 0) rx1 = 0;
    if (ry1 < 0) ry1 = 0;
    if (rx2 > frame->w) rx2 = frame->w;
    if (ry2 > frame->h) ry2 = frame->h;
    if (rx2 <= rx1 || ry2 <= ry1) { *mean_h = 0; *mean_s = 0; return; }

    double sum_h = 0, sum_s = 0;
    long n = 0;
    for (int y = ry1; y < ry2; y++) {
        const uint8_t *px = frame->data + (size_t)y * frame->stride + (size_t)rx1 * 3;
        for (int x = rx1; x < rx2; x++, px += 3) {
            int b = px[0], g = px[1], r = px[2];
            int v = r > g ? (r > b ? r : b) : (g > b ? g : b);
            int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
            int diff = v - mn;
            float h = 0, s = 0;
            if (v != 0) s = 255.0f * diff / (float)v;
            if (diff != 0) {
                if (v == r)      h = 30.0f * (g - b) / (float)diff;
                else if (v == g) h = 60.0f + 30.0f * (b - r) / (float)diff;
                else             h = 120.0f + 30.0f * (r - g) / (float)diff;
                if (h < 0) h += 180.0f;
            }
            sum_h += h;
            sum_s += s;
            n++;
        }
    }
    *mean_h = (float)(sum_h / n);
    *mean_s = (float)(sum_s / n);
}
