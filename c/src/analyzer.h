/*
 * 3단계: 지역 정밀 분석 (stage3_local_analyzer.py 포팅)
 *
 * 파이썬판은 MediaPipe Pose(33포인트) + FaceDetection(6포인트)을 썼지만
 * MediaPipe는 C API를 제공하지 않는다. 대신 전역 탐지와 같은 YOLO-pose
 * 모델을 '크롭에만' 재추론한다:
 *   - 전역 추론은 416px에 프레임 전체 → 사람 1명당 유효 해상도가 낮다.
 *   - 크롭 재추론은 256px에 사람 1명 → 키포인트 정밀도가 크게 오른다.
 *   - 모델 1개 재사용 → 메모리 추가 부담 없음 ("필요한 것만" 원칙).
 * 판정 로직은 파이썬판과 동일한 비율 기반:
 *   - 손 들어올림: 손목 y < 어깨 y
 *   - 상체 수평:   |어깨중심-엉덩이중심| y 차 < x 차
 *   - 정면 응시:   |코 x - 양눈 중앙 x| / 눈 간격 < 0.35
 * 얼굴 비율 시그니처는 COCO에 입 키포인트가 없어 눈간격/눈-코 거리로 대체.
 */
#ifndef ANALYZER_H
#define ANALYZER_H

#include "config.h"
#include "types.h"
#include "yolo_pose.h"

typedef struct Analyzer Analyzer;

/* yolo는 1단계와 공유하는 추론기 (소유하지 않음) */
Analyzer *analyzer_create(const PipelineConfig *cfg, YoloPose *yolo);
void analyzer_destroy(Analyzer *a);

void analyzer_analyze(Analyzer *a, const FrameView *frame,
                      const PrecisionTarget *target, PrecisionResult *res);

#endif /* ANALYZER_H */
