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
#include "journey.h"
#include "types.h"

typedef enum {
    EV_ANNOUNCE_DWELL,
    EV_FALL_ALERT,
    EV_KIOSK_ASSIST,
    EV_NO_KIOSK_SIT,           /* 키오스크 미방문 착석 (8/10 회의 ① 행동 필터) */
    EV_HW_FAULT,               /* 카메라 하드웨어 이상 (8/18 회의 ②) */
    EV_OVERSTAY,               /* 구매 후 허용량 초과 체류 (8/18 회의 ③ 1잔당 N시간) */
    EV_PET,                    /* 반려동물 감지 (8/18 회의 ④ — 확장 모델) */
    EV_MINOR_SUSPECT,          /* 노키즈존 — 미성년 의심 (나이 추정 누적 표) */
    EV_OUTSIDE_FOOD,           /* 외부 음식 반입 의심 (확장 모델) */
    EV_SUPPLY_ABUSE,           /* 비품 구역 반복 접근 (시럽·빨대 어뷰징) */
    EV_GROUP_MISMATCH,         /* 일행 수 대비 주문 수 부족 (POS 잔수) */
    EV_VIOLENCE,               /* 폭력 의심 — 제로샷 관절 벡터 (behavior.c) */
    EV_VANDALISM,              /* 기물 파손 의심 — 제로샷 관절 벡터 */
    EV_LOITERING,              /* 장시간 배회 */
    EV_TAMPERING,              /* 키오스크 무단/장시간 조작 */
    EV_CLEAN_NEEDED,           /* 스마트 청소 알림 (퇴석 후 잔여물) */
    EV_KIOSK_RECO,             /* 키오스크 동적 추천 컨텍스트 (서버/키오스크용) */
    EVK_COUNT,                 /* 종류 수 (오탐율 집계 배열 크기) */
} EventKind;

typedef struct {
    EventKind kind;
    int track_id;              /* 하드웨어 이벤트 등 비인물 이벤트는 -1 */
    char message[192];
    char journey[224];         /* 설명 가능한 로깅: 해당 인원의 동선 요약 */
    double ts;                 /* 단조 시각 (발생 시점) */
} Event;

const char *event_kind_str(EventKind k);
/* 문자열 → 종류 (점주 페이지 오탐 신고 역변환). 미일치 시 EVK_COUNT. */
EventKind event_kind_from_str(const char *s);

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

/* journey는 이벤트에 동선 요약을 첨부하고 '키오스크 방문 여부' 룰 판정에
 * 쓰인다. NULL이면 동선 첨부와 미방문 착석 룰이 비활성화된다. */
StateMachine *fsm_create(const PipelineConfig *cfg, EventQueue *q, Journey *journey);
void fsm_destroy(StateMachine *m);

/* POS/키오스크 결제 완료 웹훅에서 호출 (파이프라인 스레드에서만).
 * items = 결제 잔/개수 (허용 체류 계산과 일행 수 대조에 사용, 최소 1). */
void fsm_mark_purchased(StateMachine *m, int track_id, int items);

/* 나이 추정 결과 1표 등록 (파이프라인 스레드 — 모델은 main이 돌린다).
 * kids_confirm_votes 만큼 쌓이면 fsm_update에서 미성년 의심 판정. */
void fsm_note_age(StateMachine *m, int track_id, int age);

/* 이 트랙에 나이 추정이 더 필요한가 (결론 났으면 false → 추론 생략) */
bool fsm_age_needs_vote(StateMachine *m, int track_id);

typedef double (*DwellLookup)(void *ctx, int track_id, double now);

/* flags: 라우터가 계산한 사람별 구역 판정 (people과 같은 순서, NULL 허용) */
void fsm_update(StateMachine *m,
                const TrackedPerson *people, int n_people,
                const PrecisionResult *precision, int n_precision,
                const ZoneFlags *flags,
                DwellLookup dwell_lookup, void *dwell_ctx, double now);

#endif /* FSM_H */
