#include "camhealth.h"

#include <math.h>
#include <stdio.h>

#define SAMPLE_STEP 8          /* 8px 간격 샘플링 (1280x720 → 약 14,400 픽셀) */
#define CONFIRM_N 3            /* 연속 N회 이상일 때만 상태 확정 (플리커 방지) */

/* 임계값 — 실측 기반 보수적 설정 (오탐지보다 미탐지가 낫다: 알람 신뢰도 우선) */
#define WHITEOUT_LUMA 230.0    /* 평균 밝기 상한 */
#define BLACKOUT_LUMA 18.0     /* 평균 밝기 하한 */
#define FLAT_GRAD 2.0          /* 백화/블랙아웃 동반 조건: 디테일 소실 */
#define BLUR_GRAD 2.8          /* 밝기 정상인데 그래디언트가 이 미만 → 초점 상실 */

const char *camhealth_str(CamHealthState s)
{
    switch (s) {
    case CAM_WHITEOUT:     return "백화현상(과노출/렌즈 오염)";
    case CAM_BLACKOUT:     return "영상 신호 이상(가림/소등)";
    case CAM_LOW_CONTRAST: return "초점 상실/김서림 의심";
    case CAM_FROZEN:       return "프레임 정지(카메라/네트워크)";
    default:               return "정상";
    }
}

void camhealth_init(CamHealth *ch, double now)
{
    ch->state = CAM_OK;
    ch->pending = CAM_OK;
    ch->abnormal_streak = 0;
    ch->last_frame_ts = now;
    ch->last_seq = 0;
    ch->mean_luma = 0;
    ch->grad = 0;
}

/* 성긴 샘플링으로 평균 밝기와 수평 그래디언트(디테일 양)를 잰다 */
static void frame_stats(const FrameView *f, double *mean_out, double *grad_out)
{
    double sum = 0, grad_sum = 0;
    long n = 0, gn = 0;
    for (int y = 0; y < f->h; y += SAMPLE_STEP) {
        const uint8_t *row = f->data + (size_t)y * f->stride;
        int prev = -1;
        for (int x = 0; x < f->w; x += SAMPLE_STEP) {
            const uint8_t *px = row + (size_t)x * 3;
            /* 근사 luma: (B + 2G + R) / 4 — 곱셈/부동소수점 없이 계산 */
            int luma = (px[0] + (px[1] << 1) + px[2]) >> 2;
            sum += luma;
            n++;
            if (prev >= 0) {
                int d = luma - prev;
                grad_sum += d < 0 ? -d : d;
                gn++;
            }
            prev = luma;
        }
    }
    *mean_out = n > 0 ? sum / (double)n : 0;
    *grad_out = gn > 0 ? grad_sum / (double)gn : 0;
}

static CamHealthState classify(double mean, double grad)
{
    if (mean >= WHITEOUT_LUMA && grad < FLAT_GRAD) return CAM_WHITEOUT;
    if (mean <= BLACKOUT_LUMA && grad < FLAT_GRAD) return CAM_BLACKOUT;
    if (grad < BLUR_GRAD)                          return CAM_LOW_CONTRAST;
    return CAM_OK;
}

/* 디바운스 후 상태 전환. 전환 시에만 true + 알림 문구. */
static bool transition(CamHealth *ch, CamHealthState judged,
                       char *msg, int msg_len)
{
    if (judged == CAM_OK) {
        ch->abnormal_streak = 0;
        ch->pending = CAM_OK;
        if (ch->state != CAM_OK) {                /* 이상 → 정상 복구 */
            snprintf(msg, (size_t)msg_len, "카메라 상태 복구 (이전: %s)",
                     camhealth_str(ch->state));
            ch->state = CAM_OK;
            return true;
        }
        return false;
    }
    if (judged != ch->pending) {                  /* 다른 종류 이상 — 재집계 */
        ch->pending = judged;
        ch->abnormal_streak = 1;
        return false;
    }
    if (++ch->abnormal_streak < CONFIRM_N || ch->state == judged)
        return false;
    ch->state = judged;
    snprintf(msg, (size_t)msg_len,
             "카메라 이상 감지: %s — 현장 점검 필요 (밝기 %.0f / 디테일 %.1f)",
             camhealth_str(judged), ch->mean_luma, ch->grad);
    return true;
}

bool camhealth_check_frame(CamHealth *ch, const FrameView *f, double now,
                           char *msg, int msg_len)
{
    ch->last_frame_ts = now;
    frame_stats(f, &ch->mean_luma, &ch->grad);

    /* 프레임이 다시 들어오면 FROZEN 상태부터 해제 */
    if (ch->state == CAM_FROZEN) {
        ch->state = CAM_OK;
        ch->pending = CAM_OK;
        ch->abnormal_streak = 0;
        snprintf(msg, (size_t)msg_len, "카메라 프레임 수신 재개");
        /* 복구 알림 후 이번 프레임 판정은 다음 사이클부터 */
        return true;
    }
    return transition(ch, classify(ch->mean_luma, ch->grad), msg, msg_len);
}

bool camhealth_check_freeze(CamHealth *ch, uint64_t published_seq, double now,
                            double freeze_sec, char *msg, int msg_len)
{
    if (published_seq != ch->last_seq) {          /* 생산자는 살아 있음 */
        ch->last_seq = published_seq;
        ch->last_frame_ts = now;
        return false;
    }
    if (ch->state != CAM_FROZEN && now - ch->last_frame_ts > freeze_sec) {
        ch->state = CAM_FROZEN;
        snprintf(msg, (size_t)msg_len,
                 "카메라 이상 감지: %s — %.0f초간 새 프레임 없음",
                 camhealth_str(CAM_FROZEN), now - ch->last_frame_ts);
        return true;
    }
    return false;
}
