/*
 * 웹소켓 전송 테스트 도구
 * - RFC 6455 직접 구현(ws_sender.c)이 실제 서버와 통신되는지 검증.
 * - 로컬 에코/수집 서버를 띄워 두고 실행하면 이벤트 3건을 보낸다.
 *
 * 사용: test_ws.exe ws://127.0.0.1:8765/events
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "fsm.h"
#include "ws_sender.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "사용법: %s ws://호스트:포트/경로\n", argv[0]);
        return 1;
    }
    PipelineConfig cfg = CFG_DEFAULT;
    cfg.ws_url = argv[1];

    WsSender *s = ws_create(&cfg);
    ws_start(s);

    const EventKind kinds[] = { EV_FALL_ALERT, EV_ANNOUNCE_DWELL, EV_KIOSK_ASSIST };
    for (int i = 0; i < 3; i++) {
        Event ev;
        memset(&ev, 0, sizeof(ev));              /* journey 등 신규 필드 초기화 */
        ev.kind = kinds[i];
        ev.track_id = i + 1;
        ev.ts = 0;
        snprintf(ev.message, sizeof(ev.message),
                 "테스트 이벤트 %d (\"따옴표\" 이스케이프 확인)", i + 1);
        snprintf(ev.journey, sizeof(ev.journey),
                 "12:00:00 입장 → 12:0%d:00 착석", i + 1);
        ws_send_event(s, &ev);
    }

    sleep(3);   /* 워커 스레드가 연결·전송을 마칠 시간 */
    ws_stop(s);
    ws_destroy(s);
    fprintf(stderr, "전송 시도 완료 — 서버 수신 로그를 확인하세요\n");
    return 0;
}
