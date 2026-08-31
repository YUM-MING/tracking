/*
 * 고객 동선 기록 (8/18 회의 ① '설명 가능한 로깅')
 * - "주문 안 하고 앉아있다" 알림에 점주가 납득할 증거를 붙인다:
 *   "14:00 입장 → 14:02 키오스크 → 14:05 착석 → 14:35 미결제 체류"
 * - 트랙(안정 ID)별로 단계 전환만 고정 배열에 기록 (이미지/개인정보 없음).
 * - 이벤트 발행 시 문자열로 포맷해 메시지에 첨부하고,
 *   퇴장(GC) 시 전체 동선을 로그로 남긴다.
 * - 결제 크로스체크: POS 웹훅(/api/purchase)이 STEP_PURCHASE를 기록하면
 *   상태 머신의 미구매 판정과 대조된다.
 */
#ifndef JOURNEY_H
#define JOURNEY_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    STEP_ENTER = 0,            /* 첫 감지 (입장) */
    STEP_KIOSK,                /* 키오스크 접근 */
    STEP_SIT,                  /* 테이블 착석 */
    STEP_LEAVE_TABLE,          /* 테이블 이탈 */
    STEP_PURCHASE,             /* 결제 완료 (POS 연동) */
    STEP_EXIT,                 /* 트랙 소실 (퇴장 추정) */
} JourneyStep;

typedef struct Journey Journey;

Journey *journey_create(void);
void journey_destroy(Journey *j);

/* 단계 기록. 직전 단계와 같으면 중복 기록하지 않는다. 첫 기록은 ENTER로 승격. */
void journey_note(Journey *j, int track_id, JourneyStep step, double now);

/* 매 사이클, 보이는 트랙의 생존 신고 (GC 기준 시각 갱신) */
void journey_touch(Journey *j, int track_id, double now);

/* 해당 트랙이 특정 단계를 거쳤는가 (예: 키오스크 방문 여부) */
bool journey_visited(const Journey *j, int track_id, JourneyStep step);

/* 마지막 기록 단계 (착석→이탈 전환 감지용). 기록 없으면 -1 */
int journey_last_step(const Journey *j, int track_id);

/* "입장 14:00:01 → 키오스크 14:02:10 → 착석 14:05:00" 형식으로 포맷 */
void journey_format(const Journey *j, int track_id, char *buf, size_t len);

/* ttl 초 이상 안 보인 트랙: EXIT 기록 → 전체 동선 로그 출력 → 슬롯 해제 */
void journey_gc(Journey *j, double now, double ttl);

#endif /* JOURNEY_H */
