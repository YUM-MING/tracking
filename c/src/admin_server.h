/*
 * 점주 페이지 내장 HTTP 서버 (점주 전용 Filter/환경설정 페이지 — 8/10 회의 ③)
 * - 외부 웹 서버 없이 BSD 소켓 위에 HTTP/1.1 응답을 직접 구현 (ws_sender와
 *   같은 원칙: 필요한 만큼만 직접 만든다). 매장 LAN 안에서 점주 폰/PC로 접속.
 * - 라우트:
 *     GET  /                → 점주 설정 페이지 (owner_page.html)
 *     GET  /api/status      → 파이프라인 상태 (FPS, CPU, 카메라 진단, 드롭 수)
 *     GET  /api/settings    → 현재 점주 설정 JSON
 *     POST /api/settings    → 설정 변경 (체크박스/슬라이더/구역) + 저장
 *     GET  /api/events?since=N → 최근 이벤트 (동선 첨부)
 *     GET  /api/live        → 현재 추적 인원 좌표 (구역 편집 캔버스 오버레이)
 *     GET  /api/logs        → 통합 로그 최근분
 *     POST /api/purchase    → POS 결제 웹훅 {"track_id":N} (결제 크로스체크)
 *     POST /api/feedback    → 오탐 신고 {"kind":"..."} → 자동 캘리브레이션
 * - 스레드 경계: HTTP 스레드는 SettingsStore(자체 락)와 이 모듈의 락 걸린
 *   스냅샷만 만진다. 파이프라인 개입이 필요한 결제 처리만 메일박스로 넘겨
 *   소비자 스레드가 폴링한다 (상태 머신을 다른 스레드에서 직접 만지지 않음).
 */
#ifndef ADMIN_SERVER_H
#define ADMIN_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "fsm.h"
#include "store_settings.h"
#include "types.h"

typedef struct AdminServer AdminServer;

typedef struct {
    double uptime_sec;
    double infer_fps;
    int n_people;
    double cpu_pct, rss_mb;
    int cam_state;             /* CamHealthState 값 */
    const char *cam_state_str;
    const char *mode;          /* 운영 모드: 정상/절전/영업시간외/캘리브레이션 */
    int calib_pct;             /* 캘리브레이션 진행률 (0~100, 미진행 시 -1) */
    uint64_t frames_dropped;   /* framebus 드롭 (실시간 유지 비용) */
    bool ws_enabled;
    long settings_version;
} AdminStatus;

/* 점주 페이지에서 요청한 캘리브레이션(세팅) 동작 */
typedef enum {
    CALIB_REQ_NONE = 0,
    CALIB_REQ_START,           /* [세팅 시작하기] — minutes 동안 오탐 셀 수집 */
    CALIB_REQ_STOP,            /* 조기 종료 (지금까지 수집분으로 마스크 생성) */
    CALIB_REQ_CLEAR,           /* 마스크 전체 해제 */
} CalibRequest;

AdminServer *admin_create(const PipelineConfig *cfg, SettingsStore *settings);
void admin_start(AdminServer *a);
void admin_stop(AdminServer *a);
void admin_destroy(AdminServer *a);

/* ── 파이프라인(소비자 스레드) → 서버 공유 상태 갱신 ── */
void admin_update_status(AdminServer *a, const AdminStatus *st);
void admin_update_people(AdminServer *a, const TrackedPerson *people, int n,
                         const PrecisionTarget *targets, int n_targets,
                         int frame_w, int frame_h);
/* 이벤트 워커 스레드에서 호출 (이벤트 단일 소비자) */
void admin_push_event(AdminServer *a, const Event *ev);

/* ── 점주 액션 폴링 (소비자 스레드 전용) ── */
/* POS 결제 웹훅으로 들어온 (track_id, 잔 수) 목록을 꺼낸다. 반환: 개수 */
int admin_take_purchases(AdminServer *a, int *track_ids, int *items, int max);

/* 캘리브레이션 요청을 꺼낸다 (1회성 — 꺼내면 NONE으로 초기화) */
CalibRequest admin_take_calib(AdminServer *a, int *minutes);

#endif /* ADMIN_SERVER_H */
