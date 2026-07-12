/*
 * 비율 기반 태깅/재식별 (reid_tagger.py 포팅)
 * - 입장 시 옷 색상·채도 + 스켈레톤 신체 '비율'로 시그니처를 만들어
 *   트래커 ID가 바뀌어도(가림, 잠깐 퇴장 후 재진입) 동일인에게
 *   안정 ID(stable_id)를 유지한다.
 * - 길이(px) 기반은 거리·화각에 따라 오차가 크므로 반드시 비율만 사용.
 * - 시그니처는 스칼라 몇 개뿐 — 원본 이미지는 저장하지 않는다.
 */
#ifndef REID_H
#define REID_H

#include "config.h"
#include "types.h"

typedef struct Reid Reid;

Reid *reid_create(const PipelineConfig *cfg);
void reid_destroy(Reid *r);

/*
 * 1단계 detect 직후 호출. 각 TrackedPerson.track_id가 안정 ID로 치환되어
 * 이후 단계(ROI, 상태 머신)의 체류 타이머·구매 플래그가 ID 스위칭에도 끊기지 않는다.
 * now: 단조 증가 초 (time.monotonic 대응)
 */
void reid_assign(Reid *r, const FrameView *frame,
                 TrackedPerson *people, int n_people, double now);

#endif /* REID_H */
