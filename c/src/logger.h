/*
 * 통합 로깅 시스템 (8/10 회의 ④ 디버깅 효율화)
 * - 카메라/네트워크 장애 시 스텝 단위 추적 없이 "한 번에 원인을 추측"할 수 있도록
 *   모든 모듈이 [시각][레벨][모듈태그] 형식으로 stderr(+선택 파일)에 남긴다.
 * - 최근 로그는 고정 크기 링버퍼에 보관 → 점주 페이지 /api/logs로 원격 열람.
 * - 스레드 세이프 (mutex 1개, 로그는 핫패스가 아니므로 락 비용 무시 가능).
 */
#ifndef LOGGER_H
#define LOGGER_H

#include <stddef.h>

typedef enum {
    LOGL_DEBUG = 0,
    LOGL_INFO,
    LOGL_WARN,
    LOGL_ERROR,
} LogLevel;

/* file_path가 NULL이 아니면 append 모드로 파일에도 기록 */
void log_init(LogLevel min_level, const char *file_path);
void log_shutdown(void);

/* 실행 중 레벨 변경 — 점주 페이지 '상세 판단 로그' 토글이 호출 */
void log_set_level(LogLevel min_level);

void log_msg(LogLevel lv, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define LOGD(tag, ...) log_msg(LOGL_DEBUG, tag, __VA_ARGS__)
#define LOGI(tag, ...) log_msg(LOGL_INFO,  tag, __VA_ARGS__)
#define LOGW(tag, ...) log_msg(LOGL_WARN,  tag, __VA_ARGS__)
#define LOGE(tag, ...) log_msg(LOGL_ERROR, tag, __VA_ARGS__)

/* 최근 로그를 JSON 배열로 직렬화 (점주 페이지용). 쓴 바이트 수 반환. */
int log_recent_json(char *buf, size_t len);

#endif /* LOGGER_H */
