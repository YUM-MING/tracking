/*
 * 4단계: 경량 상태 머신 + 이벤트 큐 (stage4_state_machine.py 포팅)
 * - 딥러닝 연산이 전혀 없는 순수 조건문 영역
 * - [테이블 체류 300초 + 미구매] → 안내방송, [쓰러짐 확정] → 긴급 이벤트
 * - 이벤트는 스레드 세이프 큐에 적재, 별도 워커 스레드가 소비
 *   (Win32 CRITICAL_SECTION + CONDITION_VARIABLE — 멘토링에서 나온
 *    "멀티스레드는 이벤트/세마포로 신호를 준다"의 표준 구현)
 */
#ifndef FSM_H
#define FSM_H

#include "config.h"
#include "types.h"

typedef enum {
    EV_ANNOUNCE_DWELL,
    EV_FALL_ALERT,
    EV_KIOSK_ASSIST,
} EventKind;

typedef struct {
    EventKind kind;
    int track_id;
    char message[192];
    double ts;                 /* 단조 시각 (발생 시점) */
} Event;

const char *event_kind_str(EventKind k);

/* ── 스레드 세이프 이벤트 큐 ── */
typedef struct EventQueue EventQueue;

EventQueue *evq_create(void);
void evq_destroy(EventQueue *q);
void evq_push(EventQueue *q, const Event *ev);
/* timeout_ms 안에 이벤트가 오면 true. 종료 신호(evq_close) 후엔 즉시 false. */
bool evq_pop(EventQueue *q, Event *out, int timeout_ms);
void evq_close(EventQueue *q);
bool evq_is_closed(EventQueue *q);

/* ── 상태 머신 ── */
typedef struct StateMachine StateMachine;

StateMachine *fsm_create(const PipelineConfig *cfg, EventQueue *q);
void fsm_destroy(StateMachine *m);

/* POS/키오스크 결제 완료 웹훅에서 호출 */
void fsm_mark_purchased(StateMachine *m, int track_id);

typedef double (*DwellLookup)(void *ctx, int track_id, double now);

void fsm_update(StateMachine *m,
                const TrackedPerson *people, int n_people,
                const PrecisionResult *precision, int n_precision,
                DwellLookup dwell_lookup, void *dwell_ctx, double now);

#endif /* FSM_H */
