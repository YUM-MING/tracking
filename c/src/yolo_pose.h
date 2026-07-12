/*
 * YOLO11n-pose ONNX 추론기 (ONNX Runtime C API)
 * - ultralytics 파이썬 래퍼 없이 전처리(letterbox) → 세션 Run →
 *   디코드 → NMS 를 직접 구현한다.
 * - 동적 입력 크기 모델 1개를 전역(416)과 크롭(256) 추론에 공유한다.
 */
#ifndef YOLO_POSE_H
#define YOLO_POSE_H

#include "config.h"
#include "types.h"

#define MAX_DETECTIONS 128

/* NMS 통과한 검출 1건 (트래커 입력) */
typedef struct {
    float bbox[4];             /* x1,y1,x2,y2 — 추론한 ROI 기준 좌표 */
    float conf;
    Keypoint kpts[KPT_COUNT];
} Detection;

typedef struct YoloPose YoloPose;

/* 모델 로드. 실패 시 NULL (stderr에 원인 출력). */
YoloPose *yolo_create(const PipelineConfig *cfg);
void yolo_destroy(YoloPose *y);

/*
 * frame의 ROI(rx1..ry2)를 imgsz로 letterbox 추론.
 * 반환: 검출 수 (좌표는 ROI 좌상단 기준 픽셀 — 호출자가 필요 시 오프셋).
 * conf_thres: 이 값 미만은 디코드 단계에서 버림.
 */
int yolo_infer(YoloPose *y, const FrameView *frame,
               int rx1, int ry1, int rx2, int ry2,
               int imgsz, float conf_thres,
               Detection *out, int max_out);

#endif /* YOLO_POSE_H */
