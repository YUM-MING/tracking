#include "analyzer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct Analyzer {
    const PipelineConfig *cfg;
    YoloPose *yolo;
};

Analyzer *analyzer_create(const PipelineConfig *cfg, YoloPose *yolo)
{
    Analyzer *a = calloc(1, sizeof(Analyzer));
    a->cfg = cfg;
    a->yolo = yolo;
    return a;
}

void analyzer_destroy(Analyzer *a) { free(a); }

static bool kv(const Detection *d, int i, float min_conf)
{
    return d->kpts[i].conf >= min_conf &&
           (d->kpts[i].x > 0.f || d->kpts[i].y > 0.f);
}

void analyzer_analyze(Analyzer *a, const FrameView *frame,
                      const PrecisionTarget *t, PrecisionResult *res)
{
    const PipelineConfig *cfg = a->cfg;
    memset(res, 0, sizeof(*res));
    res->track_id = t->track_id;
    res->reason = t->reason;
    res->face_ratio = -1.f;

    /* 크롭 영역만 고해상도 재추론 (zero-copy: 프레임 포인터 + 사각형) */
    Detection dets[8];
    int n = yolo_infer(a->yolo, frame, t->cx1, t->cy1, t->cx2, t->cy2,
                       cfg->crop_imgsz, cfg->crop_conf, dets, 8);
    if (n == 0) return;

    /* 크롭 안에서 가장 신뢰도 높은 사람 = 분석 대상 (NMS가 conf 내림차순) */
    const Detection *d = &dets[0];
    res->pose_detected = true;
    const float mc = cfg->kpt_valid_conf;

    /* ── 손 들어올림: 어느 한쪽 손목 y가 어깨 y보다 위 (이미지 좌표는 위가 작음) ── */
    if (kv(d, KPT_L_SHOULDER, mc) && kv(d, KPT_R_SHOULDER, mc)) {
        float sh_y = fminf(d->kpts[KPT_L_SHOULDER].y, d->kpts[KPT_R_SHOULDER].y);
        if ((kv(d, KPT_L_WRIST, mc) && d->kpts[KPT_L_WRIST].y < sh_y) ||
            (kv(d, KPT_R_WRIST, mc) && d->kpts[KPT_R_WRIST].y < sh_y))
            res->hand_raised = true;

        /* ── 상체 수평: 어깨 중심과 엉덩이 중심의 y 차이가 x 차이보다 작으면 수평 ── */
        if (kv(d, KPT_L_HIP, mc) && kv(d, KPT_R_HIP, mc)) {
            float sh_cx = (d->kpts[KPT_L_SHOULDER].x + d->kpts[KPT_R_SHOULDER].x) / 2;
            float sh_cy = (d->kpts[KPT_L_SHOULDER].y + d->kpts[KPT_R_SHOULDER].y) / 2;
            float hp_cx = (d->kpts[KPT_L_HIP].x + d->kpts[KPT_R_HIP].x) / 2;
            float hp_cy = (d->kpts[KPT_L_HIP].y + d->kpts[KPT_R_HIP].y) / 2;
            res->torso_horizontal = fabsf(sh_cy - hp_cy) < fabsf(sh_cx - hp_cx);
        }
    }

    /* ── 얼굴 방향 (비율 기반) ──
     * 거리 게이트: 크롭이 화면 대비 작으면(원거리) 얼굴 분석 생략 → 연산/오탐 절약 */
    float crop_frac = (float)(t->cy2 - t->cy1) / (float)(t->frame_h > 1 ? t->frame_h : 1);
    if (cfg->face_enabled && crop_frac >= cfg->face_min_crop_frac &&
        kv(d, KPT_NOSE, mc) && kv(d, KPT_L_EYE, mc) && kv(d, KPT_R_EYE, mc)) {
        res->face_detected = true;
        float eye_mid_x = (d->kpts[KPT_L_EYE].x + d->kpts[KPT_R_EYE].x) / 2;
        float eye_dist = fabsf(d->kpts[KPT_L_EYE].x - d->kpts[KPT_R_EYE].x);
        if (eye_dist < 1e-6f) eye_dist = 1e-6f;
        /* 코끝이 양눈 x 중앙 부근이면 정면 응시로 근사 (눈 간격 대비 비율) */
        res->gaze_forward = fabsf(d->kpts[KPT_NOSE].x - eye_mid_x) / eye_dist < 0.35f;
        /* 얼굴 비율 시그니처 (비식별 스칼라): 눈 간격 / 눈-코 세로 거리 */
        float eye_mid_y = (d->kpts[KPT_L_EYE].y + d->kpts[KPT_R_EYE].y) / 2;
        float v = fabsf(d->kpts[KPT_NOSE].y - eye_mid_y);
        if (v > 1e-6f) res->face_ratio = eye_dist / v;
    }
}
