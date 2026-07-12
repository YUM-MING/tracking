/*
 * 순수 C 이미지 처리 (OpenCV 비의존)
 * - 알고리즘 경로(전처리/시그니처)는 라이브러리 없이 직접 구현한다.
 *   "라이브러리 전체를 불러오지 않고 필요한 것만 직접 만든다" 원칙.
 * - OpenCV는 카메라 캡처와 디버그 창 표시(cv_shim)에만 쓴다.
 */
#ifndef IMGPROC_H
#define IMGPROC_H

#include <stdint.h>
#include "types.h"

/* letterbox 결과 좌표 역변환용 파라미터 */
typedef struct {
    float scale;         /* 원본 → 모델 입력 축소 배율 */
    float pad_x, pad_y;  /* 모델 입력 내 여백(px) */
} LetterboxInfo;

/*
 * BGR8 ROI → letterbox(비율 유지 + 회색 패딩 114) → RGB CHW float(0~1)
 * dst: dst_size*dst_size*3 float 버퍼 (호출자 소유)
 * roi: 프레임 내 크롭 사각형 (전체 프레임이면 0,0,w,h) — zero-copy 뷰에 해당
 */
void letterbox_chw(const FrameView *frame,
                   int rx1, int ry1, int rx2, int ry2,
                   float *dst, int dst_size, LetterboxInfo *info);

/*
 * BGR8 ROI의 HSV 평균 (OpenCV uint8 HSV 규약: H 0~180, S/V 0~255)
 * reid 시그니처용. hue는 파이썬 구현과 동일하게 '단순 평균'(비순환)으로 계산.
 */
void bgr_roi_hsv_mean(const FrameView *frame,
                      int rx1, int ry1, int rx2, int ry2,
                      float *mean_h, float *mean_s);

#endif /* IMGPROC_H */
