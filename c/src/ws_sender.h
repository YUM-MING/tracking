/*
 * 서버 전송: WebSocket 이벤트 발행 (event_sender.py 포팅)
 * - websocket-client 라이브러리 대신 Winsock 위에 RFC 6455 클라이언트를
 *   직접 구현한다 (핸드셰이크 + 마스킹된 텍스트 프레임).
 *   "웹소켓은 TCP/IP 소켓 위의 약속일 뿐" — 멘토링 내용 그대로,
 *   소켓을 열고 HTTP Upgrade 약속을 지킨 뒤 프레임 규격으로 보낸다.
 * - 원본 영상이 아닌 추상화된 이벤트 메타데이터(JSON)만 전송.
 * - 상시 연결 + 끊김 시 지수 백오프 재연결.
 * - send는 큐 적재만 하므로 핫패스를 막지 않는다 (전송은 워커 스레드).
 * - cfg->ws_url이 비어 있으면 비활성 (로컬 로그만).
 */
#ifndef WS_SENDER_H
#define WS_SENDER_H

#include "config.h"
#include "fsm.h"

typedef struct WsSender WsSender;

WsSender *ws_create(const PipelineConfig *cfg);
void ws_start(WsSender *s);
/* 비차단 적재. 백로그 초과 시 가장 오래된 것부터 버린다. */
void ws_send_event(WsSender *s, const Event *ev);
void ws_stop(WsSender *s);
void ws_destroy(WsSender *s);

#endif /* WS_SENDER_H */
