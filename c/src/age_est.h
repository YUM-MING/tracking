/*
 * 얼굴 나이 추정기 — 노키즈존 필터 (8/18 회의 ④)
 *
 * 모델: InsightFace genderage.onnx (1.3MB)
 *   입력 (1,3,96,96) RGB float 0~255 / 출력 (1,3) = [여성 logit, 남성 logit, 나이/100]
 *   docs/age_estimation_research.md 리서치 1순위를 그대로 채택.
 *
 * 사양(4200U급) 최적화 전략:
 *   - 상시 추론하지 않는다. 키오스크 근접자에게만, 트랙당 표(vote)가
 *     확정될 때까지만 돌린다 (96px CNN 1회 ≈ 주 모델의 1/50 연산).
 *   - 노키즈존 룰이 꺼져 있으면 로드조차 안 함. 모델 없으면 조용히 비활성.
 *
 * 정확도 주의 (연구 결론): 얼굴 나이 추정 MAE는 ±4~6세.
 *   → '차단'이 아니라 '점주 알림'으로만 쓰고, 여러 표를 모아 확정한다.
 *   → 얼굴 이미지는 저장하지 않는다 — 나이 스칼라만 사용 (비식별 원칙).
 */
#ifndef AGE_EST_H
#define AGE_EST_H

#include "config.h"
#include "types.h"

typedef struct AgeEst AgeEst;

/* 모델 로드. 파일이 없거나 실패하면 NULL (호출부는 룰 자동 비활성). */
AgeEst *age_create(const PipelineConfig *cfg);
void age_destroy(AgeEst *a);

/*
 * 프레임 내 얼굴 사각형을 잘라 나이 추정. 실패 시 -1.
 * 얼굴 사각형은 호출자가 키포인트(눈/귀)로 계산해 넘긴다.
 */
int age_estimate(AgeEst *a, const FrameView *frame,
                 int fx1, int fy1, int fx2, int fy2);

/*
 * 사람 키포인트로 얼굴 사각형 근사 (눈 간격 기준).
 * 반환 false = 얼굴 키포인트 부족 (뒤돌아 있음 등).
 */
bool age_face_box(const TrackedPerson *p, float min_conf, int frame_w, int frame_h,
                  int *fx1, int *fy1, int *fx2, int *fy2);

#endif /* AGE_EST_H */
