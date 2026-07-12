/*
 * 2단계: 동적 관심 구역(Dynamic ROI) 필터링 및 라우팅 (stage2_roi_router.py 포팅)
 * - 모든 사람을 정밀 분석으로 넘기지 않고, 트리거 조건에 해당하는 ID만 선별
 * - 크롭은 "프레임 내 사각형"으로만 표현 — 메모리 복사 없음 (numpy 뷰 대응)
 * - 트리거 조건:
 *     1) 키오스크 근접(얼굴 크기 비율) 또는 구역 진입
 *     2) 테이블 구역 N분 이상 정체
 *     3) 스켈레톤 붕괴(쓰러짐 징후: BBox 종횡비 급변)
 */
#ifndef ROI_ROUTER_H
#define ROI_ROUTER_H

#include "config.h"
#include "types.h"

typedef struct RoiRouter RoiRouter;

RoiRouter *router_create(const PipelineConfig *cfg);
void router_destroy(RoiRouter *r);

/* 트리거 대상 선별. 반환: 대상 수 (최대 cfg->max_precision_targets) */
int router_route(RoiRouter *r, const FrameView *frame,
                 const TrackedPerson *people, int n_people, double now,
                 PrecisionTarget *out, int max_out);

/* 상태 머신에서 참조하는 테이블 체류 시간 */
double router_table_dwell_seconds(const RoiRouter *r, int track_id, double now);

#endif /* ROI_ROUTER_H */
