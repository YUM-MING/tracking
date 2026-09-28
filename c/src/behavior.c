#include "behavior.h"

#include "os_compat.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "logger.h"

#define B_TRACKS 128           /* 동시 분석 트랙 상한 (정적 할당) */
#define B_TTL_SEC 5.0          /* 트랙 소실 후 상태 유지 시간 */

/* ── 기구학 임계값 (behavior_sense=1.0 기준, 단위: BBox높이/초) ──
 * sense를 올리면 임계값이 낮아져 민감해진다 (effective = base / sense). */
#define SWING_SPEED_BASE 2.5f  /* 고속 스윙 판정 손목 속도 */
#define DROP_SPEED_BASE 1.5f   /* 낙상 판정 수직 급강하 속도 */
#define VIOLENCE_DIST_H 1.2f   /* 폭력: 두 사람 중심 거리 상한 (H배) — 카페 실측 오탐으로 보수화 */
#define VANDAL_ALONE_H 2.5f    /* 파손: 이 거리 안에 타인이 없어야 '단독' */
#define VANDAL_BODY_MAX 0.6f   /* 파손: 제자리 판정 몸 속도 상한 */
#define LOITER_MOVE_MIN 0.08f  /* 배회: '계속 이동' 판정 평균 몸 속도 하한 */

/* 디바운스 (연속 샘플 수) — 추론 6회/초 기준 폭력 ≈ 0.7초, 파손 ≈ 1.3초 */
#define VIOLENCE_CONFIRM 8      /* 연속 스윙 ~1.3초 (9월 카페 실측: 4샘플은 컵 젓기 오탐) */
#define VANDAL_CONFIRM 8
#define DROP_CONFIRM 2

/* 쿨다운 (초) — 같은 상황 반복 알림 방지 */
#define VIOLENCE_COOLDOWN 180.0
#define VANDAL_COOLDOWN 120.0
#define RECO_COOLDOWN 90.0     /* 같은 손님에게 추천 컨텍스트 재발행 간격 */

typedef struct {
    bool used;
    int track_id;
    double first_seen, last_seen;
    /* 이동 벡터 계산용 직전 샘플 */
    float prev_cx, prev_cy;
    double prev_ts;
    bool has_prev;
    /* 특징 벡터 (EMA) */
    float body_speed;          /* 몸 이동 속도 (H/s) */
    float wrist_speed;         /* 손목 속도 — 빠른 EMA (스윙은 순간 신호) */
    float prev_wrist[2][2];
    bool has_pw[2];
    /* 판정 상태 */
    int swing_streak;          /* 연속 고속 스윙 샘플 수 */
    int drop_streak;           /* 연속 급강하 샘플 수 (낙상) */
    double kiosk_since;        /* 키오스크 연속 점유 시작 (<0 = 점유 아님) */
    bool was_at_kiosk;         /* 추천 컨텍스트: 근접 '전환' 감지용 */
    double move_sum;           /* 배회: 몸 속도 누적 (평균용) */
    int move_n;
    bool loiter_flagged, tamper_flagged, fall_flagged;
    double last_vandal_ts, last_reco_ts;
} BTrack;

struct Behavior {
    const PipelineConfig *cfg;
    EventQueue *evq;
    Journey *journey;
    BTrack tracks[B_TRACKS];
    double last_violence_ts;   /* 폭력은 쌍 단위 사건 — 매장 단위 쿨다운 */
};

Behavior *behavior_create(const PipelineConfig *cfg, EventQueue *evq,
                          Journey *journey)
{
    Behavior *b = calloc(1, sizeof(Behavior));
    b->cfg = cfg;
    b->evq = evq;
    b->journey = journey;
    b->last_violence_ts = -1e9;
    return b;
}

void behavior_destroy(Behavior *b) { free(b); }

static BTrack *track_get(Behavior *b, int tid, double now)
{
    int free_slot = -1;
    for (int i = 0; i < B_TRACKS; i++) {
        if (b->tracks[i].used && b->tracks[i].track_id == tid) {
            b->tracks[i].last_seen = now;
            return &b->tracks[i];
        }
        if (!b->tracks[i].used && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return NULL;
    BTrack *t = &b->tracks[free_slot];
    memset(t, 0, sizeof(*t));
    t->used = true;
    t->track_id = tid;
    t->first_seen = now;
    t->last_seen = now;
    t->kiosk_since = -1;
    t->last_vandal_ts = -1e9;
    t->last_reco_ts = -1e9;
    return t;
}

/* 이벤트 발행 (동선 요약 첨부 — 설명 가능한 로깅) */
static void emit(Behavior *b, EventKind kind, int tid, double now,
                 const char *fmt, ...)
{
    Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = kind;
    ev.track_id = tid;
    ev.ts = now;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ev.message, sizeof(ev.message), fmt, ap);
    va_end(ap);
    if (b->journey && tid >= 0)
        journey_format(b->journey, tid, ev.journey, sizeof(ev.journey));
    evq_push(b->evq, &ev);
}

/*
 * 특징 벡터 갱신: 직전 샘플과의 차분으로 속도(H/s)를 계산한다.
 * 시간 정규화(Δt) 덕분에 추론 주기(detect_every_n)가 바뀌어도 판정이 유지된다.
 */
static void update_features(const PipelineConfig *cfg, BTrack *t,
                            const TrackedPerson *p, double now,
                            float *drop_speed_out)
{
    float H = p->bbox[3] - p->bbox[1];
    if (H < 1.f) H = 1.f;
    float cx = (p->bbox[0] + p->bbox[2]) / 2;
    float cy = (p->bbox[1] + p->bbox[3]) / 2;
    *drop_speed_out = 0;

    if (t->has_prev) {
        double dt = now - t->prev_ts;
        if (dt > 0.01 && dt < 2.0) {           /* 소실 구간 재등장은 차분 무효 */
            float dx = cx - t->prev_cx, dy = cy - t->prev_cy;
            float speed = sqrtf(dx * dx + dy * dy) / H / (float)dt;
            t->body_speed = t->body_speed * 0.6f + speed * 0.4f;
            t->move_sum += speed;
            t->move_n++;
            /* 수직 급강하 (아래 방향 = +y) — 낙상 신호 */
            *drop_speed_out = dy > 0 ? dy / H / (float)dt : 0;
        }
    }
    t->prev_cx = cx;
    t->prev_cy = cy;
    t->prev_ts = now;
    t->has_prev = true;

    /* 손목 속도: 양 손목 중 빠른 쪽 (스윙은 편측 동작) */
    float wmax = 0;
    const int WK[2] = { KPT_L_WRIST, KPT_R_WRIST };
    for (int w = 0; w < 2; w++) {
        bool valid = kpt_valid(p, WK[w], cfg->kpt_valid_conf);
        if (valid && t->has_pw[w]) {
            double dt = now - t->prev_ts;      /* prev_ts는 방금 갱신 — 아래 참조 */
            (void)dt;
        }
        if (valid) {
            /* 주의: 손목 차분은 같은 사이클의 prev_wrist와 비교해야 하므로
             * prev_ts 갱신 전 값을 쓰지 않고 몸 속도와 동일한 Δt를 가정한다.
             * (추론 간격이 일정하므로 오차는 미미) */
            if (t->has_pw[w]) {
                float dwx = p->kpts[WK[w]].x - t->prev_wrist[w][0];
                float dwy = p->kpts[WK[w]].y - t->prev_wrist[w][1];
                float d = sqrtf(dwx * dwx + dwy * dwy) / H;
                if (d > wmax) wmax = d;
            }
            t->prev_wrist[w][0] = p->kpts[WK[w]].x;
            t->prev_wrist[w][1] = p->kpts[WK[w]].y;
            t->has_pw[w] = true;
        } else {
            t->has_pw[w] = false;
        }
    }
    /* 샘플 간격을 추론 주기 근사(cfg 기반)로 나눠 속도화 —
     * 30fps 입력·detect_every_n 스킵 기준 (실측 dt와 오차 ±20% 이내) */
    float dt_est = (float)cfg->detect_every_n / 30.0f;
    if (dt_est < 0.03f) dt_est = 0.03f;
    float wspeed = wmax / dt_est;
    /* 빠른 공격/느린 감쇠 EMA — 순간 스윙을 놓치지 않되 노이즈는 흡수 */
    t->wrist_speed = wspeed > t->wrist_speed
        ? t->wrist_speed * 0.4f + wspeed * 0.6f
        : t->wrist_speed * 0.7f + wspeed * 0.3f;
}

/* 시간대 버킷 (키오스크 추천 컨텍스트용) */
static const char *time_bucket(void)
{
    time_t now = time(NULL);
    struct tm tmv;
    os_localtime(&now, &tmv);
    int h = tmv.tm_hour;
    if (h < 6)  return "심야";
    if (h < 11) return "아침";
    if (h < 14) return "점심";
    if (h < 17) return "오후";
    if (h < 21) return "저녁";
    return "밤";
}

void behavior_update(Behavior *b, const TrackedPerson *people, int n_people,
                     const ZoneFlags *flags, double now)
{
    const PipelineConfig *cfg = b->cfg;
    float sense = cfg->behavior_sense;
    if (sense < 0.5f) sense = 0.5f;
    if (sense > 2.0f) sense = 2.0f;
    const float swing_thresh = SWING_SPEED_BASE / sense;
    const float drop_thresh = DROP_SPEED_BASE / sense;

    BTrack *bt[MAX_PEOPLE] = { 0 };

    /* 1) 전 인원 특징 벡터 갱신 + 단독 판정(낙상/배회/무단조작/추천) */
    for (int i = 0; i < n_people; i++) {
        const TrackedPerson *p = &people[i];
        BTrack *t = track_get(b, p->track_id, now);
        if (!t) continue;
        bt[i] = t;

        float drop_speed = 0;
        update_features(cfg, t, p, now, &drop_speed);

        /* ── 고속 스윙 스트릭 (폭력/파손 공용 신호) ── */
        int prev_streak = t->swing_streak;
        t->swing_streak = (p->has_kpts && t->wrist_speed >= swing_thresh)
                          ? t->swing_streak + 1 : 0;
        if (t->swing_streak == 1)
            LOGD("판정", "ID %d 고속 스윙 감지 시작 (손목 %.1fH/s ≥ %.1f)",
                 p->track_id, (double)t->wrist_speed, (double)swing_thresh);
        else if (t->swing_streak == 0 && prev_streak >= 2)
            LOGD("판정", "ID %d 고속 스윙 종료 (%d샘플 지속)", p->track_id,
                 prev_streak);

        /* ── 낙상 빠른 경로: 급강하 + 직후 종횡비 붕괴 ──
         * 기존 자세 기반 룰(N회 확정)보다 빠르게 잡는 보조 경로. */
        bool dropping = drop_speed >= drop_thresh;
        t->drop_streak = dropping ? t->drop_streak + 1 : 0;
        if (dropping)
            LOGD("판정", "ID %d 수직 급강하 %.1fH/s (%d/%d)", p->track_id,
                 (double)drop_speed, t->drop_streak, DROP_CONFIRM);
        if (cfg->rule_fall && !t->fall_flagged &&
            t->drop_streak >= DROP_CONFIRM &&
            person_aspect_ratio(p) >= cfg->fall_aspect_ratio) {
            t->fall_flagged = true;
            emit(b, EV_FALL_ALERT, p->track_id, now,
                 "[긴급] ID %d 낙상 감지 (급강하 %.1f배속 + 자세 붕괴) — 즉시 확인",
                 p->track_id, (double)drop_speed);
        }

        /* ── 배회: 착석/구매 없이 loiter_sec 이상 계속 이동 ── */
        double present = now - t->first_seen;
        if (cfg->rule_loitering && !t->loiter_flagged &&
            present >= cfg->loiter_sec) {
            float avg_move = t->move_n > 0
                ? (float)(t->move_sum / t->move_n) : 0;
            bool settled = b->journey &&
                (journey_visited(b->journey, p->track_id, STEP_SIT) ||
                 journey_visited(b->journey, p->track_id, STEP_PURCHASE));
            if (!settled && avg_move >= LOITER_MOVE_MIN) {
                t->loiter_flagged = true;
                emit(b, EV_LOITERING, p->track_id, now,
                     "ID %d %d분째 착석·구매 없이 배회 중 (평균 이동 %.2fH/s) — 확인 요망",
                     p->track_id, (int)(present / 60), (double)avg_move);
            } else if (!settled) {
                LOGD("판정", "ID %d 장기 체류(%.0f초)이나 이동량 낮음(%.2f) — 배회 아님",
                     p->track_id, present, (double)avg_move);
                t->loiter_flagged = true;       /* 재평가 없음 (다른 룰 담당) */
            } else {
                t->loiter_flagged = true;       /* 착석/구매 이력 — 정상 이용 */
            }
        }

        /* ── 무단/장시간 조작: 키오스크 연속 점유 ── */
        bool at_kiosk = flags && flags[i].at_kiosk;
        if (at_kiosk) {
            if (t->kiosk_since < 0) t->kiosk_since = now;
            double occupied = now - t->kiosk_since;
            if (cfg->rule_tampering && !t->tamper_flagged &&
                occupied >= cfg->tamper_sec) {
                t->tamper_flagged = true;
                emit(b, EV_TAMPERING, p->track_id, now,
                     "ID %d 키오스크 %d분 연속 점유 — 무단 조작/기기 이상 확인 요망",
                     p->track_id, (int)(occupied / 60));
            }
        } else if (t->kiosk_since >= 0) {
            LOGD("판정", "ID %d 키오스크 점유 종료 (%.0f초)", p->track_id,
                 now - t->kiosk_since);
            t->kiosk_since = -1;
        }

        /* ── 키오스크 동적 추천 컨텍스트 (근접 '전환' 시 1회) ──
         * 엣지는 동행 수·시간대 컨텍스트를 이벤트로 발행하고,
         * 실제 팝업 표출은 키오스크 SW/서버가 담당한다 (8/6 기획). */
        if (cfg->rule_reco && at_kiosk && !t->was_at_kiosk &&
            now - t->last_reco_ts >= RECO_COOLDOWN) {
            t->last_reco_ts = now;
            /* 동행 수: 이 사람 반경 2.5H 안의 다른 인원 */
            float H = p->bbox[3] - p->bbox[1];
            if (H < 1.f) H = 1.f;
            int companions = 0;
            for (int j = 0; j < n_people; j++) {
                if (j == i) continue;
                float dx = (people[j].bbox[0] + people[j].bbox[2]) / 2
                         - (p->bbox[0] + p->bbox[2]) / 2;
                float dy = (people[j].bbox[1] + people[j].bbox[3]) / 2
                         - (p->bbox[1] + p->bbox[3]) / 2;
                if (sqrtf(dx * dx + dy * dy) <= 2.5f * H) companions++;
            }
            emit(b, EV_KIOSK_RECO, p->track_id, now,
                 "추천 컨텍스트: %s / %d인 (본인+동행 %d) — 키오스크 추천 팝업용",
                 time_bucket(), companions + 1, companions);
        }
        if (flags) t->was_at_kiosk = at_kiosk;
    }

    /* 2) 상호작용 판정: 폭력 (근접 쌍 + 고속 스윙 지속) */
    if (cfg->rule_violence &&
        now - b->last_violence_ts >= VIOLENCE_COOLDOWN) {
        for (int i = 0; i < n_people && b->last_violence_ts + VIOLENCE_COOLDOWN <= now; i++) {
            if (!bt[i] || bt[i]->swing_streak < VIOLENCE_CONFIRM) continue;
            const TrackedPerson *pi = &people[i];
            float H = pi->bbox[3] - pi->bbox[1];
            if (H < 1.f) H = 1.f;
            for (int j = 0; j < n_people; j++) {
                if (j == i) continue;
                float dx = (people[j].bbox[0] + people[j].bbox[2]) / 2
                         - (pi->bbox[0] + pi->bbox[2]) / 2;
                float dy = (people[j].bbox[1] + people[j].bbox[3]) / 2
                         - (pi->bbox[1] + pi->bbox[3]) / 2;
                float dist_h = sqrtf(dx * dx + dy * dy) / H;
                if (dist_h > VIOLENCE_DIST_H) continue;
                b->last_violence_ts = now;
                emit(b, EV_VIOLENCE, pi->track_id, now,
                     "[긴급] ID %d↔ID %d 폭력 의심 (근접 %.1fH + 고속 스윙 %d샘플) — 즉시 확인",
                     pi->track_id, people[j].track_id, (double)dist_h,
                     bt[i]->swing_streak);
                break;
            }
        }
    }

    /* 3) 단독 판정: 기물 파손 (주변에 아무도 없는데 제자리 고속 스윙 반복) */
    if (cfg->rule_vandalism) {
        for (int i = 0; i < n_people; i++) {
            if (!bt[i] || bt[i]->swing_streak < VANDAL_CONFIRM) continue;
            if (now - bt[i]->last_vandal_ts < VANDAL_COOLDOWN) continue;
            if (bt[i]->body_speed > VANDAL_BODY_MAX) {
                LOGD("판정", "ID %d 고속 스윙이나 이동 중(%.2fH/s) — 파손 아님(달리기 등)",
                     people[i].track_id, (double)bt[i]->body_speed);
                continue;
            }
            const TrackedPerson *pi = &people[i];
            float H = pi->bbox[3] - pi->bbox[1];
            if (H < 1.f) H = 1.f;
            bool alone = true;
            for (int j = 0; j < n_people && alone; j++) {
                if (j == i) continue;
                float dx = (people[j].bbox[0] + people[j].bbox[2]) / 2
                         - (pi->bbox[0] + pi->bbox[2]) / 2;
                float dy = (people[j].bbox[1] + people[j].bbox[3]) / 2
                         - (pi->bbox[1] + pi->bbox[3]) / 2;
                if (sqrtf(dx * dx + dy * dy) / H <= VANDAL_ALONE_H) alone = false;
            }
            if (!alone) continue;               /* 근접자 있으면 폭력 룰 관할 */
            bt[i]->last_vandal_ts = now;
            emit(b, EV_VANDALISM, pi->track_id, now,
                 "[긴급] ID %d 기물 파손 의심 (단독 제자리 고속 스윙 %d샘플) — 즉시 확인",
                 pi->track_id, bt[i]->swing_streak);
        }
    }

    /* 4) GC: 오래 안 보인 트랙 해제 */
    for (int i = 0; i < B_TRACKS; i++) {
        if (b->tracks[i].used && now - b->tracks[i].last_seen > B_TTL_SEC)
            b->tracks[i].used = false;
    }
}
