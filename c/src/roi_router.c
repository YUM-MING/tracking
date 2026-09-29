#include "roi_router.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "logger.h"

#define MAX_TIMERS 256

const char *reason_str(TriggerReason r)
{
    switch (r) {
    case REASON_FALL_SUSPECT: return "fall_suspect";
    case REASON_KIOSK_ENTER:  return "kiosk_enter";
    case REASON_TABLE_DWELL:  return "table_dwell";
    default:                  return "none";
    }
}

typedef struct {
    bool used;
    int track_id;
    double table_enter_ts;     /* <0 = 테이블 밖 */
    double last_seen;
    /* 직전 판정 (전환 시에만 로그를 남기기 위한 상태 — 매 프레임 로그 방지) */
    bool prev_at_kiosk;
    bool prev_in_table;
    TriggerReason prev_reason;
} TrackTimer;

struct RoiRouter {
    const PipelineConfig *cfg;
    TrackTimer timers[MAX_TIMERS];
};

RoiRouter *router_create(const PipelineConfig *cfg)
{
    RoiRouter *r = calloc(1, sizeof(RoiRouter));
    r->cfg = cfg;
    return r;
}

void router_destroy(RoiRouter *r) { free(r); }

static TrackTimer *timer_get(RoiRouter *r, int track_id, bool create)
{
    int free_slot = -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (r->timers[i].used && r->timers[i].track_id == track_id)
            return &r->timers[i];
        if (!r->timers[i].used && free_slot < 0) free_slot = i;
    }
    if (!create || free_slot < 0) return NULL;
    TrackTimer *t = &r->timers[free_slot];
    t->used = true;
    t->track_id = track_id;
    t->table_enter_ts = -1;
    return t;
}

/* 종횡비 기반 1차 판정 + 키포인트 보조 판정 */
static bool check_fall(const RoiRouter *r, const TrackedPerson *p)
{
    if (person_aspect_ratio(p) >= r->cfg->fall_aspect_ratio) return true;

    /* 코와 엉덩이의 y 차이가 거의 없으면 수평 자세 의심 */
    const float mc = r->cfg->kpt_valid_conf;
    if (kpt_valid(p, KPT_NOSE, mc) &&
        kpt_valid(p, KPT_L_HIP, mc) && kpt_valid(p, KPT_R_HIP, mc)) {
        float nose_y = p->kpts[KPT_NOSE].y;
        float hip_y = (p->kpts[KPT_L_HIP].y + p->kpts[KPT_R_HIP].y) / 2;
        float bbox_h = p->bbox[3] - p->bbox[1];
        if (bbox_h > 0 && fabsf(hip_y - nose_y) / bbox_h < 0.15f && nose_y > 0)
            return true;
    }
    return false;
}

/*
 * 키오스크 근접 판정 (키오스크 부착 카메라 전제):
 * 1) 얼굴(머리 폭)이 화면 가로 대비 일정 비율 이상 = 사용 중 (주 기준)
 * 2) 폴백: 고개 숙임·역광·모자로 얼굴이 안 잡혀도, 세로형(서 있는) 몸이
 *    화면 세로를 크게 채우면 근접으로 인정 (9/30 — 주문 동선 ID 연결 보강).
 *    가로형(쓰러짐 의심) 박스는 제외해 쓰러짐 판정과 충돌하지 않는다.
 */
static bool is_kiosk_near(const RoiRouter *r, const TrackedPerson *p,
                          int frame_w, int frame_h)
{
    float fw = person_face_width(p, r->cfg->kpt_valid_conf);
    if (fw >= 0 &&
        fw / (float)(frame_w > 1 ? frame_w : 1) >= r->cfg->kiosk_face_w_frac)
        return true;

    float bw = p->bbox[2] - p->bbox[0];
    float bh = p->bbox[3] - p->bbox[1];
    return bw < bh && r->cfg->kiosk_body_h_frac < 0.999f &&
           bh / (float)(frame_h > 1 ? frame_h : 1) >= r->cfg->kiosk_body_h_frac;
}

/* BBox에 패딩을 더해 프레임 경계로 클램프한 크롭 사각형 */
static void crop_rect(const RoiRouter *r, const FrameView *frame,
                      const TrackedPerson *p, PrecisionTarget *t)
{
    float pw = (p->bbox[2] - p->bbox[0]) * r->cfg->crop_padding;
    float ph = (p->bbox[3] - p->bbox[1]) * r->cfg->crop_padding;
    t->cx1 = (int)fmaxf(0, p->bbox[0] - pw);
    t->cy1 = (int)fmaxf(0, p->bbox[1] - ph);
    t->cx2 = (int)fminf((float)frame->w, p->bbox[2] + pw);
    t->cy2 = (int)fminf((float)frame->h, p->bbox[3] + ph);
}

static void router_gc(RoiRouter *r, double now)
{
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (r->timers[i].used &&
            now - r->timers[i].last_seen > r->cfg->stale_track_ttl_sec)
            r->timers[i].used = false;
    }
}

int router_route(RoiRouter *r, const FrameView *frame,
                 const TrackedPerson *people, int n_people, double now,
                 PrecisionTarget *out, int max_out, ZoneFlags *flags)
{
    const PipelineConfig *cfg = r->cfg;
    int n_out = 0;

    /* 대상 상한(n_out)과 무관하게 전 인원의 타이머/플래그는 항상 갱신한다 */
    for (int i = 0; i < n_people; i++) {
        const TrackedPerson *p = &people[i];
        TrackTimer *tm = timer_get(r, p->track_id, true);
        if (tm) tm->last_seen = now;

        /* 키오스크 판정: near 모드(부착 카메라)는 얼굴 크기 비율, zone 모드는 화면 구역 */
        bool at_kiosk = (cfg->kiosk_trigger_mode == KIOSK_TRIGGER_NEAR)
            ? is_kiosk_near(r, p, frame->w, frame->h)
            : zone_contains(&cfg->kiosk_zone, p->center_x, p->center_y);

        /* 테이블 체류 타이머는 트리거 우선순위와 무관하게 구역 기준으로만 관리
         * (키오스크로 이동해도 타이머가 남는 오탐 방지) */
        bool in_table = zone_contains(&cfg->table_zone, p->center_x, p->center_y)
                        && !at_kiosk;
        if (flags) {
            flags[i].at_kiosk = at_kiosk;
            flags[i].in_table = in_table;
            /* 비품대 구역: 어뷰징 방문 카운트용 (키오스크/테이블과 독립) */
            flags[i].in_supply = zone_contains(&cfg->supply_zone,
                                               p->center_x, p->center_y);
        }
        if (tm) {
            /* 판단 로그: 구역 진입/이탈 전환 시에만 근거와 함께 기록 */
            if (at_kiosk != tm->prev_at_kiosk) {
                float fw_px = person_face_width(p, cfg->kpt_valid_conf);
                LOGD("판정", "ID %d 키오스크 %s (얼굴폭 비율 %.3f / 기준 %.3f)",
                     p->track_id, at_kiosk ? "근접" : "이탈",
                     fw_px >= 0 ? (double)(fw_px / (float)frame->w) : -1.0,
                     (double)cfg->kiosk_face_w_frac);
                tm->prev_at_kiosk = at_kiosk;
            }
            if (in_table != tm->prev_in_table) {
                LOGD("판정", "ID %d 테이블 구역 %s (발밑 %.0f,%.0f)",
                     p->track_id, in_table ? "진입 — 체류 타이머 시작" : "이탈 — 타이머 리셋",
                     (double)p->center_x, (double)p->center_y);
                tm->prev_in_table = in_table;
            }
            if (in_table) {
                if (tm->table_enter_ts < 0) tm->table_enter_ts = now;
            } else {
                tm->table_enter_ts = -1;
            }
        }

        TriggerReason reason = REASON_NONE;

        /* 조건 3: 쓰러짐 징후 (최우선)
         * 단, 근접 상태는 제외 — 카메라 앞에 가까이 오면 상반신만 잡혀
         * BBox가 가로로 넓어지고 종횡비 기준이 서있어도 '쓰러짐'으로 오탐한다. */
        if (!at_kiosk && check_fall(r, p))
            reason = REASON_FALL_SUSPECT;
        /* 조건 1: 키오스크 근접/진입 */
        else if (at_kiosk)
            reason = REASON_KIOSK_ENTER;
        /* 조건 2: 테이블 구역 정체 */
        else if (in_table && tm && tm->table_enter_ts >= 0 &&
                 now - tm->table_enter_ts >= cfg->table_dwell_trigger_sec)
            reason = REASON_TABLE_DWELL;

        /* 판단 로그: 정밀 분석 트리거 사유가 바뀔 때만 기록 */
        if (tm && reason != tm->prev_reason) {
            LOGD("판정", "ID %d 정밀 분석 트리거: %s → %s (종횡비 %.2f)",
                 p->track_id, reason_str(tm->prev_reason), reason_str(reason),
                 (double)person_aspect_ratio(p));
            tm->prev_reason = reason;
        }

        if (reason != REASON_NONE && n_out < max_out) {
            PrecisionTarget *t = &out[n_out];
            t->track_id = p->track_id;
            t->reason = reason;
            t->person = p;
            t->frame_h = frame->h;
            crop_rect(r, frame, p, t);
            if (t->cx2 > t->cx1 && t->cy2 > t->cy1) n_out++;
        }
    }

    /* 연산 예산 보호: 쓰러짐 의심 우선 정렬 후 인원 상한 적용 */
    for (int i = 1; i < n_out; i++) {          /* 안정 삽입 정렬 (대상 수 적음) */
        PrecisionTarget key = out[i];
        int prio = key.reason == REASON_FALL_SUSPECT ? 0 : 1;
        int j = i - 1;
        while (j >= 0 && (out[j].reason == REASON_FALL_SUSPECT ? 0 : 1) > prio) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    if (n_out > cfg->max_precision_targets) n_out = cfg->max_precision_targets;

    router_gc(r, now);
    return n_out;
}

double router_table_dwell_seconds(const RoiRouter *r, int track_id, double now)
{
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (r->timers[i].used && r->timers[i].track_id == track_id) {
            if (r->timers[i].table_enter_ts < 0) return 0.0;
            return now - r->timers[i].table_enter_ts;
        }
    }
    return 0.0;
}
