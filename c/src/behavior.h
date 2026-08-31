/*
 * 제로샷 이상행동 분석기 — 폭력·기물 파손·낙상(급강하)·배회·무단 조작
 *
 * 원리 (8/10 회의 "AI 최소화, 룰 우선"의 연장):
 *   별도 행동인식 모델(학습) 없이, ByteTrack식 추적이 이미 만들어 주는
 *   좌표·관절(17키포인트) 시퀀스에서 기구학 특징 벡터를 뽑아 판정한다.
 *   - 손목 스윙 속도    : |Δ손목| / BBox높이 / Δt  (원근 무관 정규화)
 *   - 몸 이동 속도      : |Δ중심| / BBox높이 / Δt
 *   - 수직 급강하 속도  : Δ중심y / BBox높이 / Δt  (낙상)
 *   - 근접도            : 두 사람 중심 거리 / BBox높이  (폭력 = 상호작용)
 *   - 체류·점유 시간    : 배회(정착 없는 장기 체류), 무단 조작(키오스크 독점)
 *
 * 판정 정의 (전부 점주 토글 + 민감도 다이얼로 제어):
 *   폭력     : 두 사람이 근접(<1.5H)한 상태에서 어느 한쪽의 고속 스윙이
 *              연속 N샘플 지속 (다툼/몸싸움의 특징적 패턴)
 *   기물파손 : 주변 2.5H 안에 아무도 없는 단독 인원이 제자리에서
 *              고속 스윙을 더 길게 반복 (집기 내려치기/걷어차기)
 *   낙상     : 몸 중심이 1초 내 BBox 높이 이상 급강하 + 직후 종횡비 붕괴
 *              (기존 자세 기반 쓰러짐 룰의 '빠른 경로' — 같은 이벤트로 발행)
 *   배회     : 착석도 구매도 없이 loiter_sec 이상 매장 안을 계속 이동
 *   무단조작 : 키오스크를 tamper_sec 이상 연속 점유 (정상 주문은 1~3분)
 *
 * 오탐 억제 3중 장치: 연속 샘플 디바운스 + 쿨다운/트랙당 1회 + 점주 민감도.
 * 모든 판정 근거는 [판정] 태그 DEBUG 로그로 남는다 (모든 판단에 로그).
 */
#ifndef BEHAVIOR_H
#define BEHAVIOR_H

#include "config.h"
#include "fsm.h"
#include "journey.h"
#include "types.h"

typedef struct Behavior Behavior;

/* evq/journey는 소유하지 않음. journey는 이벤트 동선 첨부용 (NULL 허용). */
Behavior *behavior_create(const PipelineConfig *cfg, EventQueue *evq,
                          Journey *journey);
void behavior_destroy(Behavior *b);

/* 매 추론 사이클 호출. 내부 상태 갱신 + 판정 + 이벤트 발행까지 수행. */
void behavior_update(Behavior *b, const TrackedPerson *people, int n_people,
                     const ZoneFlags *flags, double now);

#endif /* BEHAVIOR_H */
