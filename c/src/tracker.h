/*
 * ByteTrack식 경량 다중 객체 추적기 (1단계 후반부)
 * - ultralytics 내장 ByteTrack을 대체하는 직접 구현.
 * - 핵심 아이디어만 채택: 고신뢰 검출을 먼저 매칭하고,
 *   남은 트랙을 저신뢰 검출과 2차 매칭해 가림에 강하게 만든다.
 * - 칼만 필터 대신 등속(선형 속도) 예측 — 검출 주기가 일정(N프레임 스킵)해서
 *   저비용 등속 모델로 충분하고, ID 스위칭은 상위 재식별(reid)이 보정한다.
 */
#ifndef TRACKER_H
#define TRACKER_H

#include "config.h"
#include "types.h"
#include "yolo_pose.h"

typedef struct Tracker Tracker;

Tracker *tracker_create(const PipelineConfig *cfg);
void tracker_destroy(Tracker *t);

/*
 * 검출 결과(dets, ROI가 전체 프레임이므로 프레임 좌표)를 갱신하고
 * 이번 프레임에 매칭·확정된 사람 목록을 out에 채운다. 반환: 인원수.
 */
int tracker_update(Tracker *t, const Detection *dets, int n_dets,
                   TrackedPerson *out, int max_out);

#endif /* TRACKER_H */
