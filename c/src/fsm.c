/*
 * 4단계: 경량 상태 머신 + 이벤트 큐 (stage4_state_machine.py 포팅)
 *
 * 설계 원칙 (8/10 회의 ②): 리소스가 큰 AI 판단을 최소화하기 위해
 * 순수 조건문(룰) 영역으로 유지한다. 모든 룰은 점주 토글로 게이트되고,
 * 모든 판정은 근거와 함께 로그로 남는다 ("모든 판단에 로그").
 *
 * 룰 목록:
 *   1. 쓰러짐 확정        — N회 연속 상체 수평 (디바운스)
 *   2. 장기 체류 + 미구매  — 안내방송 (활동 중 손님 제외 옵션)
 *   3. 키오스크 도움 요청  — 근접 상태에서 손 들어올림
 *   4. 키오스크 미방문 착석 — 동선(journey) 기반 행동 필터
 *   5. 구매 후 초과 체류   — 결제 1건당 허용 시간 초과 (1잔당 1.5시간 룰)
 *
 * 행동 분석 (8/3 회의 ② '정지 상태 맥락 파악'):
 *   손목 키포인트의 프레임 간 이동량으로 '작업 중(공부/노트북/조립)'을
 *   분류한다. 몸은 정지해도 손이 꾸준히 움직이면 정상 이용으로 본다.
 *   → 별도 AI 모델 없이 이미 추출된 스켈레톤을 재사용 (연산 추가 ≈ 0).
 */
#include "fsm.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "logger.h"

const char *event_kind_str(EventKind k)
{
    switch (k) {
    case EV_ANNOUNCE_DWELL: return "announce_dwell";
    case EV_FALL_ALERT:     return "fall_alert";
    case EV_KIOSK_ASSIST:   return "kiosk_assist";
    case EV_NO_KIOSK_SIT:   return "no_kiosk_sit";
    case EV_HW_FAULT:       return "hw_fault";
    case EV_OVERSTAY:       return "overstay";
    case EV_PET:            return "pet";
    case EV_MINOR_SUSPECT:  return "minor_suspect";
    case EV_OUTSIDE_FOOD:   return "outside_food";
    case EV_SUPPLY_ABUSE:   return "supply_abuse";
    case EV_GROUP_MISMATCH: return "group_mismatch";
    case EV_VIOLENCE:       return "violence";
    case EV_VANDALISM:      return "vandalism";
    case EV_LOITERING:      return "loitering";
    case EV_TAMPERING:      return "tampering";
    case EV_CLEAN_NEEDED:   return "clean_needed";
    case EV_KIOSK_RECO:     return "kiosk_reco";
    default:                return "unknown";
    }
}

EventKind event_kind_from_str(const char *s)
{
    for (int k = 0; k < (int)EVK_COUNT; k++)
        if (strcmp(event_kind_str((EventKind)k), s) == 0) return (EventKind)k;
    return EVK_COUNT;
}

/* ── 이벤트 큐 (고정 크기 링버퍼 + 조건변수 — 8/7 회의 자료구조 원칙) ── */
#define EVQ_CAP 256

struct EventQueue {
    Event buf[EVQ_CAP];
    int head, tail, count;
    bool closed;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
};

EventQueue *evq_create(void)
{
    EventQueue *q = calloc(1, sizeof(EventQueue));
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    return q;
}

void evq_destroy(EventQueue *q)
{
    if (!q) return;
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
    free(q);
}

void evq_push(EventQueue *q, const Event *ev)
{
    pthread_mutex_lock(&q->lock);
    if (q->count == EVQ_CAP) {              /* 가득 차면 가장 오래된 것 폐기 */
        q->head = (q->head + 1) % EVQ_CAP;
        q->count--;
    }
    q->buf[q->tail] = *ev;
    q->tail = (q->tail + 1) % EVQ_CAP;
    q->count++;
    pthread_mutex_unlock(&q->lock);
    pthread_cond_signal(&q->not_empty);
}

static void timeout_from_now(struct timespec *ts, int timeout_ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += timeout_ms / 1000;
    ts->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec += 1;
    }
}

bool evq_pop(EventQueue *q, Event *out, int timeout_ms)
{
    struct timespec deadline;
    timeout_from_now(&deadline, timeout_ms);

    pthread_mutex_lock(&q->lock);
    while (q->count == 0 && !q->closed) {
        if (pthread_cond_timedwait(&q->not_empty, &q->lock, &deadline) != 0) {
            pthread_mutex_unlock(&q->lock);   /* 타임아웃 */
            return false;
        }
    }
    if (q->count == 0) {                      /* closed & 비어 있음 */
        pthread_mutex_unlock(&q->lock);
        return false;
    }
    *out = q->buf[q->head];
    q->head = (q->head + 1) % EVQ_CAP;
    q->count--;
    pthread_mutex_unlock(&q->lock);
    return true;
}

void evq_close(EventQueue *q)
{
    pthread_mutex_lock(&q->lock);
    q->closed = true;
    pthread_mutex_unlock(&q->lock);
    pthread_cond_broadcast(&q->not_empty);
}

bool evq_is_closed(EventQueue *q)
{
    pthread_mutex_lock(&q->lock);
    bool c = q->closed;
    pthread_mutex_unlock(&q->lock);
    return c;
}

/* ── 상태 머신 ────────────────────────────────────────── */
#define MAX_STATES 256

/* 트랙(안정 ID)별 판정 상태. 전부 정적 배열 — 루프 내 할당 없음. */
typedef struct {
    bool used;
    int track_id;
    int purchase_count;         /* POS 웹훅 누적 잔/개수 (허용 체류 = 수량 × 단가시간) */
    bool no_kiosk_flagged;      /* 미방문 착석은 트랙당 1회만 알림 */
    bool overstay_flagged;      /* 초과 체류도 쿨다운 대신 단계별 1회 */
    double last_announce_ts;    /* 방송류(2/3/5번 룰) 공용 쿨다운 기준 */
    int fall_frames;            /* 쓰러짐 신호 연속 프레임 수 (디바운스) */
    double last_seen;
    /* 행동 분석: 손목 이동량 추적 */
    float prev_wrist[2][2];     /* [L/R][x,y] 직전 추론 프레임의 손목 위치 */
    bool has_prev_wrist[2];
    float activity_ema;         /* 손 움직임 EMA (BBox 높이로 정규화) */
    bool activity_active;       /* 현재 '작업 중' 판정 (로그는 전환 시만) */
    /* 노키즈존: 나이 추정 표 집계 (오차 ±4~6세라 여러 표로 확정) */
    int age_votes;              /* 총 추정 횟수 */
    int age_child_votes;        /* 임계 이하로 추정된 횟수 */
    int age_last;               /* 마지막 추정 나이 (메시지용) */
    bool minor_flagged;         /* 트랙당 1회만 알림 */
    /* 비품 어뷰징: 구역 방문 카운트 */
    bool in_supply_prev;
    int supply_visits;
    bool supply_flagged;
} PersonState;

/* 일행 수 대조용: 최근 결제(잔 수)의 발생 시각 링 (오래된 결제는 제외) */
#define PURCHASE_LOG_CAP 64

struct StateMachine {
    const PipelineConfig *cfg;
    EventQueue *q;
    Journey *journey;           /* 소유하지 않음 (NULL 허용) */
    PersonState states[MAX_STATES];
    double item_ts[PURCHASE_LOG_CAP];   /* 결제 아이템 1개당 시각 1개 */
    int item_head, item_count;
    double last_group_alert_ts; /* 일행 불일치 매장 단위 쿨다운 */
    double last_update_now;     /* 최근 fsm_update 시각 (웹훅 시각 근사용) */
};

/* 일행 수 대조 시 인정하는 결제 유효 시간 (이보다 오래된 잔은 제외) */
#define GROUP_WINDOW_SEC (2 * 3600.0)

StateMachine *fsm_create(const PipelineConfig *cfg, EventQueue *q, Journey *journey)
{
    StateMachine *m = calloc(1, sizeof(StateMachine));
    m->cfg = cfg;
    m->q = q;
    m->journey = journey;
    m->last_group_alert_ts = -1e9;
    return m;
}

void fsm_destroy(StateMachine *m) { free(m); }

/* 트랙 상태 조회/생성. 슬롯 고갈 시 NULL (해당 인원 룰 판정 생략). */
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

void fsm_mark_purchased(StateMachine *m, int track_id, int items)
{
    if (items < 1) items = 1;
    PersonState *st = state_get(m, track_id, m->last_update_now);
    if (!st) return;
    st->purchase_count += items;
    st->overstay_flagged = false;   /* 추가 주문 → 허용 체류 연장, 재판정 허용 */
    LOGD("판정", "ID %d 결제 +%d잔 (누적 %d) — 허용 체류 재계산",
         track_id, items, st->purchase_count);

    /* 일행 수 대조용 결제 로그: 잔 1개당 시각 1개 (링버퍼, 가득 차면 최고령 폐기) */
    for (int i = 0; i < items; i++) {
        m->item_ts[(m->item_head + m->item_count) % PURCHASE_LOG_CAP] = m->last_update_now;
        if (m->item_count < PURCHASE_LOG_CAP) m->item_count++;
        else m->item_head = (m->item_head + 1) % PURCHASE_LOG_CAP;
    }
}

/* 노키즈존: main이 나이 추정을 돌리기 전에 물어보는 게이트 —
 * 이미 결론이 났거나 룰이 꺼져 있으면 추론 자체를 생략 (연산 절약) */
bool fsm_age_needs_vote(StateMachine *m, int track_id)
{
    if (!m->cfg->rule_no_kids) return false;
    PersonState *st = state_get(m, track_id, m->last_update_now);
    if (!st) return false;
    return !st->minor_flagged && st->age_votes < m->cfg->kids_confirm_votes * 2;
}

void fsm_note_age(StateMachine *m, int track_id, int age)
{
    if (age < 0) return;
    PersonState *st = state_get(m, track_id, m->last_update_now);
    if (!st) return;
    st->age_votes++;
    st->age_last = age;
    if (age <= m->cfg->kids_age_limit) st->age_child_votes++;
    LOGD("판정", "ID %d 나이 추정 %d세 (누적 %d표, 미성년 %d표 / 기준 %d세)",
         track_id, age, st->age_votes, st->age_child_votes,
         m->cfg->kids_age_limit);
}

/* 이벤트 발행: 메시지 + 해당 인원의 동선 요약(설명 가능한 로깅)을 함께 싣는다 */
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
    ev.journey[0] = 0;
    if (m->journey && tid >= 0)
        journey_format(m->journey, tid, ev.journey, sizeof(ev.journey));
    evq_push(m->q, &ev);
}

/*
 * 행동 분석: 손목 이동량으로 '작업 중' 여부 갱신.
 * - 이동량은 BBox 높이로 나눠 원근(카메라와의 거리)에 무관하게 만든다.
 * - EMA(지수 이동 평균)로 순간 노이즈를 흡수 — 컵 한 번 든 것으로
 *   '작업 중'이 되지 않고, 꾸준한 손 움직임만 인정된다.
 */
static void update_activity(const PipelineConfig *cfg, PersonState *st,
                            const TrackedPerson *p)
{
    float bbox_h = p->bbox[3] - p->bbox[1];
    if (bbox_h < 1.f) bbox_h = 1.f;

    const int wrist_kpt[2] = { KPT_L_WRIST, KPT_R_WRIST };
    float move_sum = 0;
    int move_n = 0;
    for (int w = 0; w < 2; w++) {
        bool valid = kpt_valid(p, wrist_kpt[w], cfg->kpt_valid_conf);
        if (valid && st->has_prev_wrist[w]) {
            float dx = p->kpts[wrist_kpt[w]].x - st->prev_wrist[w][0];
            float dy = p->kpts[wrist_kpt[w]].y - st->prev_wrist[w][1];
            move_sum += sqrtf(dx * dx + dy * dy) / bbox_h;
            move_n++;
        }
        if (valid) {
            st->prev_wrist[w][0] = p->kpts[wrist_kpt[w]].x;
            st->prev_wrist[w][1] = p->kpts[wrist_kpt[w]].y;
            st->has_prev_wrist[w] = true;
        } else {
            st->has_prev_wrist[w] = false;   /* 미검출 구간은 이동량 계산 제외 */
        }
    }
    if (move_n > 0) {
        float move = move_sum / (float)move_n;
        st->activity_ema = st->activity_ema * 0.7f + move * 0.3f;
    }

    bool active = st->activity_ema >= cfg->activity_min_move;
    if (active != st->activity_active) {     /* 판정 전환 시에만 로그 */
        LOGD("판정", "ID %d 행동 분석: %s (손 이동량 EMA %.3f / 임계 %.3f)",
             st->track_id, active ? "작업 중(공부/조립 등)" : "비활동 정지",
             (double)st->activity_ema, (double)cfg->activity_min_move);
        st->activity_active = active;
    }
}

void fsm_update(StateMachine *m,
                const TrackedPerson *people, int n_people,
                const PrecisionResult *precision, int n_precision,
                const ZoneFlags *flags,
                DwellLookup dwell_lookup, void *dwell_ctx, double now)
{
    const PipelineConfig *cfg = m->cfg;
    m->last_update_now = now;      /* 웹훅/표 등록의 시각 근사 기준 */

    for (int i = 0; i < n_people; i++) {
        const TrackedPerson *p = &people[i];
        PersonState *st = state_get(m, p->track_id, now);
        if (!st) continue;

        /* 이 사람에 대한 3단계 정밀 분석 결과 (트리거 대상이었을 때만 존재) */
        const PrecisionResult *pr = NULL;
        for (int j = 0; j < n_precision; j++) {
            if (precision[j].track_id == p->track_id) { pr = &precision[j]; break; }
        }

        /* 행동 분석(작업 중 여부)은 룰 판정 전에 항상 갱신 */
        if (p->has_kpts) update_activity(cfg, st, p);

        /* ── 규칙 1: 쓰러짐 확정 (N회 연속 디바운스 후 즉시 발행) ── */
        bool fall_signal = pr && pr->pose_detected && pr->torso_horizontal;
        int prev_fall = st->fall_frames;
        st->fall_frames = fall_signal ? st->fall_frames + 1 : 0;
        if (fall_signal)
            LOGD("판정", "ID %d 쓰러짐 신호 %d/%d회 (상체 수평)",
                 p->track_id, st->fall_frames, cfg->fall_confirm_frames);
        else if (prev_fall > 0)
            LOGD("판정", "ID %d 쓰러짐 신호 해제 (%d회에서 리셋)",
                 p->track_id, prev_fall);
        if (st->fall_frames == cfg->fall_confirm_frames) {
            if (cfg->rule_fall) {
                emit(m, EV_FALL_ALERT, p->track_id, now,
                     "[긴급] ID %d 쓰러짐 감지 — 직원 확인 요망", p->track_id);
            } else {
                LOGD("판정", "ID %d 쓰러짐 확정이나 룰 OFF — 알림 생략", p->track_id);
            }
        }

        /* ── 규칙 2: 장기 체류 + 미구매 → 안내방송 ── */
        double dwell = dwell_lookup(dwell_ctx, p->track_id, now);
        if (dwell >= cfg->announce_dwell_sec && st->purchase_count == 0) {
            if (!cfg->rule_announce) {
                /* 룰 OFF — 조용히 통과 (로그도 최초 1회 수준이면 충분하나
                 * 상태 저장 비용을 아끼려 DEBUG로만 남긴다) */
                LOGD("판정", "ID %d 미구매 체류 %d초 — 안내방송 룰 OFF",
                     p->track_id, (int)dwell);
            } else if (cfg->activity_exempt && st->activity_active) {
                /* 공부/노트북/조립 등 활동 중 → 점주 선택에 따라 방송 제외 */
                LOGD("판정", "ID %d 미구매 체류 %d초이나 작업 중 — 방송 제외(설정)",
                     p->track_id, (int)dwell);
            } else if (now - st->last_announce_ts < cfg->announce_cooldown_sec) {
                LOGD("판정", "ID %d 미구매 체류 %d초 — 쿨다운 %d초 남음",
                     p->track_id, (int)dwell,
                     (int)(cfg->announce_cooldown_sec - (now - st->last_announce_ts)));
            } else {
                st->last_announce_ts = now;
                emit(m, EV_ANNOUNCE_DWELL, p->track_id, now,
                     "ID %d 테이블 %d초 체류(미구매) — 안내방송 재생",
                     p->track_id, (int)dwell);
            }
        }

        /* ── 규칙 3: 키오스크 앞 손 들어올림 → 도움 요청 ── */
        if (cfg->rule_assist &&
            pr && pr->reason == REASON_KIOSK_ENTER && pr->hand_raised &&
            now - st->last_announce_ts >= cfg->announce_cooldown_sec) {
            st->last_announce_ts = now;
            emit(m, EV_KIOSK_ASSIST, p->track_id, now,
                 "ID %d 키오스크 앞 도움 요청 제스처 감지", p->track_id);
        }

        /* ── 규칙 4: 키오스크 미방문 착석 (8/10 회의 ① 행동 패턴 필터)
         * 입장 후 키오스크를 거치지 않고 테이블에 앉아 유예 시간을 넘기면
         * 구매 목적이 아닌 이용으로 보고 점주에게 알린다.
         * 결제(POS)나 키오스크 방문 이력이 생기면 해당 없음. */
        if (cfg->rule_no_kiosk_sit && m->journey && flags &&
            !st->no_kiosk_flagged && st->purchase_count == 0 &&
            flags[i].in_table &&
            dwell >= cfg->no_kiosk_sit_grace_sec &&
            !journey_visited(m->journey, p->track_id, STEP_KIOSK) &&
            !journey_visited(m->journey, p->track_id, STEP_PURCHASE)) {
            st->no_kiosk_flagged = true;
            emit(m, EV_NO_KIOSK_SIT, p->track_id, now,
                 "ID %d 주문 없이 착석 %d초 경과 (키오스크 미방문) — 확인 요망",
                 p->track_id, (int)dwell);
        }

        /* ── 규칙 6: 노키즈존 — 나이 추정 표 확정 (8/18 회의 ④)
         * 추정 오차(±4~6세)를 감안해 표의 80% 이상이 임계 이하일 때만 알림.
         * '차단'이 아니라 '확인 요청' — 최종 결정은 점주가 한다. */
        if (cfg->rule_no_kids && !st->minor_flagged &&
            st->age_votes >= cfg->kids_confirm_votes) {
            if (st->age_child_votes * 5 >= st->age_votes * 4) {
                st->minor_flagged = true;
                emit(m, EV_MINOR_SUSPECT, p->track_id, now,
                     "ID %d 미성년 의심 (추정 %d세, %d표 중 %d표) — 노키즈존 확인 요망",
                     p->track_id, st->age_last, st->age_votes, st->age_child_votes);
            } else if (st->age_votes >= cfg->kids_confirm_votes * 2) {
                /* 표가 충분히 모였는데 성인 우세 → 결론 확정, 추가 추론 중단 */
                st->minor_flagged = true;   /* '결론 남' 표시로 재사용 (알림 없음) */
                LOGD("판정", "ID %d 성인으로 결론 (추정 %d세) — 나이 추정 종료",
                     p->track_id, st->age_last);
            }
        }

        /* ── 규칙 7: 비품 구역 반복 접근 (8/18 회의 ③ 시럽·빨대 어뷰징)
         * 손동작 인식 없이도 잡히는 근사: 같은 사람이 비품대를 N회 이상
         * 들락거리면 알림. 정상 이용은 보통 1~2회로 끝난다. */
        if (flags) {
            bool in_sup = flags[i].in_supply;
            if (in_sup && !st->in_supply_prev) {
                st->supply_visits++;
                LOGD("판정", "ID %d 비품 구역 방문 %d회째 (기준 %d회)",
                     p->track_id, st->supply_visits, cfg->supply_abuse_visits);
                if (cfg->rule_supply_abuse && !st->supply_flagged &&
                    st->supply_visits >= cfg->supply_abuse_visits) {
                    st->supply_flagged = true;
                    emit(m, EV_SUPPLY_ABUSE, p->track_id, now,
                         "ID %d 비품 구역 %d회 반복 접근 — 비품 어뷰징 확인 요망",
                         p->track_id, st->supply_visits);
                }
            }
            st->in_supply_prev = in_sup;
        }

        /* ── 규칙 5: 구매 후 허용량 초과 체류 (8/18 회의 ③)
         * 기본 룰: 결제 1건당 stay_per_purchase_sec (기본 1.5시간) 허용.
         * 2잔 시키면 3시간 — 인원수 대비 잔 수 판별은 POS 상세 연동 후 확장.
         * 시스템이 강제하지 않는다: 알림만 보내고 재주문 유도/퇴장 안내는
         * 점주가 결정 (8/10 회의 '최종 결정권은 점주에게'). */
        if (cfg->rule_overstay && st->purchase_count > 0 && !st->overstay_flagged) {
            double allowed = st->purchase_count * cfg->stay_per_purchase_sec;
            if (dwell > allowed) {
                if (cfg->activity_exempt && st->activity_active) {
                    LOGD("판정", "ID %d 허용 체류 초과(%d/%d초)이나 작업 중 — 알림 제외(설정)",
                         p->track_id, (int)dwell, (int)allowed);
                } else {
                    st->overstay_flagged = true;
                    emit(m, EV_OVERSTAY, p->track_id, now,
                         "ID %d 결제 %d건 기준 허용 체류(%d분) 초과 — 재주문 유도 또는 안내 필요",
                         p->track_id, st->purchase_count, (int)(allowed / 60));
                }
            }
        }
    }

    /* ── 규칙 8: 일행 수 대비 주문 수 부족 (8/18 회의 ③ "3명이 1잔")
     * 테이블에 유의미하게 앉아 있는 인원 수 > 최근 결제 잔 수면 알림.
     * 미결제 0잔인 경우는 규칙 2/4가 이미 담당하므로 1잔 이상일 때만 본다.
     * 매장 단위 쿨다운 — 같은 상황을 반복 알림하지 않는다. */
    if (cfg->rule_group_mismatch && flags &&
        now - m->last_group_alert_ts >= cfg->group_mismatch_cooldown_sec) {
        /* 유효 기간 지난 결제 잔 만료 */
        while (m->item_count > 0 &&
               now - m->item_ts[m->item_head] > GROUP_WINDOW_SEC) {
            m->item_head = (m->item_head + 1) % PURCHASE_LOG_CAP;
            m->item_count--;
        }
        int seated = 0;
        for (int i = 0; i < n_people; i++) {
            if (flags[i].in_table &&
                dwell_lookup(dwell_ctx, people[i].track_id, now) >=
                    cfg->no_kiosk_sit_grace_sec)
                seated++;
        }
        if (seated >= 2 && m->item_count >= 1 && seated > m->item_count) {
            m->last_group_alert_ts = now;
            emit(m, EV_GROUP_MISMATCH, -1, now,
                 "테이블 체류 %d명 대비 최근 결제 %d잔 — 추가 주문 유도 검토",
                 seated, m->item_count);
        } else if (seated >= 2) {
            LOGD("판정", "일행 대조: 체류 %d명 / 결제 %d잔 — 정상 범위",
                 seated, m->item_count);
        }
    }

    /* GC: 오래 안 보인 트랙 상태 정리 (재입장 시 새 판정 시작) */
    for (int i = 0; i < MAX_STATES; i++) {
        if (m->states[i].used &&
            now - m->states[i].last_seen > m->cfg->stale_track_ttl_sec) {
            LOGD("판정", "ID %d 상태 해제 (%.0f초간 미관측)",
                 m->states[i].track_id, now - m->states[i].last_seen);
            m->states[i].used = false;
        }
    }
}
