/*
 * 파일럿 데이터 축적 (JSONL)
 * - 점주 페이지 링버퍼(휘발)·텍스트 로그(사람용)와 별개로,
 *   나중에 pandas 등으로 바로 분석할 수 있는 기계용 레코드를 남긴다.
 * - 날짜별 파일 자동 분리:
 *     <dir>/events_YYYY-MM-DD.jsonl    이벤트 1건 = 1줄 (WS 전송 스키마 + 시각)
 *     <dir>/journeys_YYYY-MM-DD.jsonl  퇴장 트랙 1명 = 1줄 (전체 동선 요약)
 * - 이미지/개인정보는 기록하지 않는다 (동선 단계·시각·메시지뿐).
 */
#ifndef DATASTORE_H
#define DATASTORE_H

#include <stdbool.h>

typedef enum {
    DS_EVENTS = 0,
    DS_JOURNEYS,
    DS_STREAM_COUNT,
} DsStream;

/* dir가 NULL/빈 문자열이면 비활성 (기존 동작과 동일). 디렉터리는 자동 생성. */
void datastore_init(const char *dir);
void datastore_shutdown(void);
bool datastore_enabled(void);

/* JSON 한 줄 추가 (개행 자동, 즉시 flush — 프로세스 강제 종료에도 보존) */
void datastore_append(DsStream st, const char *json_line);

/* 이벤트 레코드 헬퍼: 벽시계 시각을 찍어 events 스트림에 기록 */
void datastore_event(const char *type, int track_id,
                     const char *message, const char *journey);

#endif /* DATASTORE_H */
