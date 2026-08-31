/*
 * OpenCV 격리 계층 (C 인터페이스)
 * - OpenCV는 C API가 제거된 C++ 라이브러리라서, 이 파일의 인터페이스만
 *   C++(cv_shim.cpp)로 빌드하고 나머지 프로그램은 전부 순수 C로 유지한다.
 *   "라이브러리를 인터페이스로 빼서 양쪽에 포함시킨다" — 멘토링 내용의 구현.
 * - 여기 있는 기능만 OpenCV에 의존한다:
 *   카메라 캡처(videoio), 디버그 창(highgui), 오버레이 그리기(imgproc draw).
 *   운영 배포(show_window=false)에서는 캡처만 쓴다.
 * - 알고리즘 경로(리사이즈, HSV, 추론 전처리)는 imgproc.c의 순수 C 구현 사용.
 */ 
#ifndef CV_SHIM_H
#define CV_SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CvsCapture CvsCapture;

/* source: "0" 같은 정수 문자열이면 웹캠 인덱스, 아니면 RTSP/파일 경로 */
CvsCapture *cvs_open(const char *source, int width, int height);
/* 반환 버퍼(BGR)는 다음 cvs_read 호출 전까지 유효. 실패 시 0. */
int cvs_read(CvsCapture *cap, uint8_t **data, int *w, int *h, int *stride);
void cvs_close(CvsCapture *cap);

/* 디버그 표시 */
void cvs_show(const char *window, const uint8_t *data, int w, int h, int stride);
int  cvs_waitkey(int ms);
void cvs_destroy_all_windows(void);

/* 오버레이 그리기 (BGR 버퍼 in-place) */
void cvs_rect(uint8_t *d, int w, int h, int stride,
              int x1, int y1, int x2, int y2, int b, int g, int r, int thick);
void cvs_line(uint8_t *d, int w, int h, int stride,
              int x1, int y1, int x2, int y2, int b, int g, int r, int thick);
void cvs_circle(uint8_t *d, int w, int h, int stride,
                int cx, int cy, int radius, int b, int g, int r, int thick);
void cvs_text(uint8_t *d, int w, int h, int stride, const char *text,
              int x, int y, double scale, int b, int g, int r, int thick);

#ifdef __cplusplus
}
#endif

#endif /* CV_SHIM_H */
