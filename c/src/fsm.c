#include "fsm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

const char *event_kind_str(EventKind k)
{
    switch (k) {
    case EV_ANNOUNCE_DWELL: return "announce_dwell";
    case EV_FALL_ALERT:     return "fall_alert";
    case EV_KIOSK_ASSIST:   return "kiosk_assist";
    default:                return "unknown";
    }
}

/* ── 이벤트 큐 (고정 크기 링버퍼) ─────────────────────── */
#define EVQ_CAP 256

struct EventQueue {
    Event buf[EVQ_CAP];
    int head, tail, count;
    bool closed;
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE not_empty;
};

EventQueue *evq_create(void)
{
    EventQueue *q = calloc(1, sizeof(EventQueue));
    InitializeCriticalSection(&q->lock);
    InitializeConditionVariable(&q->not_empty);
    return q;
}

void evq_destroy(EventQueue *q)
{
    if (!q) return;
    DeleteCriticalSection(&q->lock);
    free(q);
}

void evq_push(EventQueue *q, const Event *ev)
{
    EnterCriticalSection(&q->lock);
    if (q->count == EVQ_CAP) {              /* 가득 차면 가장 오래된 것 폐기 */
        q->head = (q->head + 1) % EVQ_CAP;
        q->count--;
    }
    q->buf[q->tail] = *ev;
    q->tail = (q->tail + 1) % EVQ_CAP;
    q->count++;
    LeaveCriticalSection(&q->lock);
    WakeConditionVariable(&q->not_empty);
}

bool evq_pop(EventQueue *q, Event *out, int timeout_ms)
{
    EnterCriticalSection(&q->lock);
    while (q->count == 0 && !q->closed) {
        if (!SleepConditionVariableCS(&q->not_empty, &q->lock, timeout_ms)) {
            LeaveCriticalSection(&q->lock);   /* 타임아웃 */
            return false;
        }
    }
    if (q->count == 0) {                      /* closed & 비어 있음 */
        LeaveCriticalSection(&q->lock);
        return false;
    }
    *out = q->buf[q->head];
    q->head = (q->head + 1) % EVQ_CAP;
    q->count--;
    LeaveCriticalSection(&q->lock);
    return true;
}

void evq_close(EventQueue *q)
{
    EnterCriticalSection(&q->lock);
    q->closed = true;
    LeaveCriticalSection(&q->lock);
    WakeAllConditionVariable(&q->not_empty);
}

bool evq_is_closed(EventQueue *q)
{
    EnterCriticalSection(&q->lock);
    bool c = q->closed;
    LeaveCriticalSection(&q->lock);
    return c;
}

/* ── 상태 머신 ────────────────────────────────────────── */
#define FALL_CONFIRM_FRAMES 5   /* N회 연속 수평 자세일 때만 확정
                                 * (추론 프레임 기준 — 30fps·스킵5면 약 1초) */
#define MAX_STATES 256

typedef struct {
    bool used;
    int track_id;
    bool purchased;             /* POS 연동 시 갱신되는 구매 플래그 */
    double last_announce_ts;
    int fall_frames;            /* 쓰러짐 신호 연속 프레임 수 (디바운스) */
    double last_seen;
} PersonState;

struct StateMachine {
    const PipelineConfig *cfg;
    EventQueue *q;
    PersonState states[MAX_STATES];
};

StateMachine *fsm_create(const PipelineConfig *cfg, EventQueue *q)
{
    StateMachine *m = calloc(1, sizeof(StateMachine));
    m->cfg = cfg;
    m->q = q;
    return m;
}

void fsm_destroy(StateMachine *m) { free(m); }

static PersonState *state_get(StateMachine *m, int tid, double now)
{
    int free_slot = -1;
    for (int i = 0; i < MAX_STATES; i++) {
        if (m->states[i].used && m->states[i].track_id == tid) {
            m->states[i].last_seen = now;
            return &m->states[i];
        }
        if (!m->states[i].used && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return NULL;
    PersonState *st = &m->states[free_slot];
    memset(st, 0, sizeof(*st));
    st->used = true;
    st->track_id = tid;
    st->last_announce_ts = -1e9;
    st->last_seen = now;
    return st;
}

void fsm_mark_purchased(StateMachine *m, int track_id)
{
    PersonState *st = state_get(m, track_id, 0);
    if (st) st->purchased = true;
}

static void emit(StateMachine *m, EventKind kind, int tid, double now,
                 const char *fmt, ...)
{
    Event ev;
    ev.kind = kind;
    ev.track_id = tid;
    ev.ts = now;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ev.message, sizeof(ev.message), fmt, ap);
    va_end(ap);
    evq_push(m->q, &ev);
}

void fsm_update(StateMachine *m,
                const TrackedPerson *people, int n_people,
                const PrecisionResult *precision, int n_precision,
                DwellLookup dwell_lookup, void *dwell_ctx, double now)
{
    for (int i = 0; i < n_people; i++) {
        const TrackedPerson *p = &people[i];
        PersonState *st = state_get(m, p->track_id, now);
        if (!st) continue;

        const PrecisionResult *pr = NULL;
        for (int j = 0; j < n_precision; j++) {
            if (precision[j].track_id == p->track_id) { pr = &precision[j]; break; }
        }

        /* ── 규칙 1: 쓰러짐 확정 (디바운스 후 즉시 발행) ── */
        bool fall_signal = pr && pr->pose_detected && pr->torso_horizontal;
        st->fall_frames = fall_signal ? st->fall_frames + 1 : 0;
        if (st->fall_frames == FALL_CONFIRM_FRAMES) {
            emit(m, EV_FALL_ALERT, p->track_id, now,
                 "[긴급] ID %d 쓰러짐 감지 — 직원 확인 요망", p->track_id);
        }

        /* ── 규칙 2: 장기 체류 + 미구매 → 안내방송 ── */
        double dwell = dwell_lookup(dwell_ctx, p->track_id, now);
        if (dwell >= m->cfg->announce_dwell_sec && !st->purchased &&
            now - st->last_announce_ts >= m->cfg->announce_cooldown_sec) {
            st->last_announce_ts = now;
            emit(m, EV_ANNOUNCE_DWELL, p->track_id, now,
                 "ID %d 테이블 %d초 체류(미구매) — 안내방송 재생",
                 p->track_id, (int)dwell);
        }

        /* ── 규칙 3: 키오스크 앞 손 들어올림 → 도움 요청 ── */
        if (pr && pr->reason == REASON_KIOSK_ENTER && pr->hand_raised &&
            now - st->last_announce_ts >= m->cfg->announce_cooldown_sec) {
            st->last_announce_ts = now;
            emit(m, EV_KIOSK_ASSIST, p->track_id, now,
                 "ID %d 키오스크 앞 도움 요청 제스처 감지", p->track_id);
        }
    }

    /* GC: 오래 안 보인 트랙 상태 정리 */
    for (int i = 0; i < MAX_STATES; i++) {
        if (m->states[i].used &&
            now - m->states[i].last_seen > m->cfg->stale_track_ttl_sec)
            m->states[i].used = false;
    }
}
