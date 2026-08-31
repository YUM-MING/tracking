/*
 * 파일럿 데이터 축적 (SQLite + JSONL)
 * - 8/31 회의: 로컬 저장은 SQLite가 적합 — "파일만 가져오면 바로 볼 수 있다".
 *   <dir>/tracking.db 하나에 events/journeys 테이블로 쌓인다 (인계용 원본).
 * - JSONL도 병행 기록 (grep/pandas 즉석 분석용):
 *     <dir>/events_YYYY-MM-DD.jsonl    이벤트 1건 = 1줄
 *     <dir>/journeys_YYYY-MM-DD.jsonl  퇴장 트랙 1명 = 1줄
 * - 이미지/개인정보는 기록하지 않는다 (동선 단계·시각·메시지뿐).
 */
#ifndef DATASTORE_H
#define DATASTORE_H

#include <stdbool.h>

/* dir가 NULL/빈 문자열이면 비활성 (기존 동작과 동일). 디렉터리는 자동 생성. */
void datastore_init(const char *dir);
void datastore_shutdown(void);
bool datastore_enabled(void);

/* 이벤트 1건 기록 (벽시계 시각 자동 부여) */
void datastore_event(const char *type, int track_id,
                     const char *message, const char *journey);

/* 퇴장 트랙 1명의 여정 레코드. steps_json은 "[{...},...]" 형태의 JSON 배열. */
void datastore_journey(int track_id, const char *enter_str,
                       double enter_ts, double exit_ts,
                       bool visited_kiosk, bool sat, bool purchased,
                       const char *steps_json);

#endif /* DATASTORE_H */
