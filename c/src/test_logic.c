/*
 * 순수 로직 단위 테스트 (모델/카메라 불필요)
 * - 2단계 라우터: 쓰러짐/키오스크 근접/테이블 정체 트리거
 * - 4단계 상태 머신: 쓰러짐 디바운스, 안내방송 쿨다운
 * - 재식별: 트래커 ID가 바뀌어도 시그니처로 안정 ID 복원
 * 파이썬판과 동일 임계값으로 동작하는지 검증한다.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camhealth.h"
#include "config.h"
#include "framebus.h"
#include "fsm.h"
#include "journey.h"
#include "mask.h"
#include "reid.h"
#include "roi_router.h"
#include "store_settings.h"
#include "types.h"

static int n_pass = 0, n_fail = 0;

#define CHECK(cond, name) do { \
    if (cond) { n_pass++; } \
    else { n_fail++; printf("FAIL: %s (%s:%d)\n", name, __FILE__, __LINE__); } \
} while (0)

/* 유효 키포인트 생성 헬퍼 */
static void set_kpt(TrackedPerson *p, int i, float x, float y)
{
    p->kpts[i].x = x;
    p->kpts[i].y = y;
    p->kpts[i].conf = 0.9f;
}

static TrackedPerson make_person(int id, float x1, float y1, float x2, float y2)
{
    TrackedPerson p;
    memset(&p, 0, sizeof(p));
    p.track_id = id;
    p.bbox[0] = x1; p.bbox[1] = y1; p.bbox[2] = x2; p.bbox[3] = y2;
    p.conf = 0.9f;
    p.has_kpts = true;
    p.center_x = (x1 + x2) / 2;
    p.center_y = y2;
    return p;
}

static void test_router(const PipelineConfig *cfg)
{
    FrameView frame = { NULL, 1280, 720, 1280 * 3 };
    PrecisionTarget targets[MAX_TARGETS];

    /* 1) 종횡비 1.4 이상 → fall_suspect */
    {
        RoiRouter *r = router_create(cfg);
        TrackedPerson p = make_person(1, 100, 100, 500, 300);   /* 400x200 = 2.0 */
        int n = router_route(r, &frame, &p, 1, 0.0, targets, MAX_TARGETS, NULL);
        CHECK(n == 1 && targets[0].reason == REASON_FALL_SUSPECT,
              "종횡비 쓰러짐 트리거");
        router_destroy(r);
    }

    /* 2) 얼굴 폭 비율 ≥ 0.13 → kiosk_enter, 그리고 근접 시 쓰러짐 오탐 차단 */
    {
        RoiRouter *r = router_create(cfg);
        TrackedPerson p = make_person(2, 100, 100, 900, 400);   /* 종횡비 2.67 (오탐 조건) */
        set_kpt(&p, KPT_L_EAR, 600, 150);                       /* 귀 간격 200px / 1280 = 0.156 */
        set_kpt(&p, KPT_R_EAR, 400, 150);
        int n = router_route(r, &frame, &p, 1, 0.0, targets, MAX_TARGETS, NULL);
        CHECK(n == 1 && targets[0].reason == REASON_KIOSK_ENTER,
              "키오스크 근접 트리거 + 근접 시 쓰러짐 차단");
        router_destroy(r);
    }

    /* 3) 귀 대신 눈 간격×2 근사 */
    {
        RoiRouter *r = router_create(cfg);
        TrackedPerson p = make_person(3, 100, 100, 900, 400);
        set_kpt(&p, KPT_L_EYE, 590, 150);                       /* 눈 간격 100 → ×2 = 200px */
        set_kpt(&p, KPT_R_EYE, 490, 150);
        int n = router_route(r, &frame, &p, 1, 0.0, targets, MAX_TARGETS, NULL);
        CHECK(n == 1 && targets[0].reason == REASON_KIOSK_ENTER,
              "눈 간격 근사 키오스크 트리거");
        router_destroy(r);
    }

    /* 4) 테이블 구역 180초 정체 → table_dwell (그 전에는 트리거 없음) */
    {
        RoiRouter *r = router_create(cfg);
        TrackedPerson p = make_person(4, 300, 400, 400, 700);   /* 발밑 (350,700) = table 구역 */
        int n = router_route(r, &frame, &p, 1, 0.0, targets, MAX_TARGETS, NULL);
        CHECK(n == 0, "정체 시간 미달 시 트리거 없음");
        n = router_route(r, &frame, &p, 1, 100.0, targets, MAX_TARGETS, NULL);
        CHECK(n == 0, "100초 시점에도 트리거 없음");
        n = router_route(r, &frame, &p, 1, 181.0, targets, MAX_TARGETS, NULL);
        CHECK(n == 1 && targets[0].reason == REASON_TABLE_DWELL,
              "테이블 181초 정체 트리거");
        CHECK(router_table_dwell_seconds(r, 4, 181.0) > 180.0, "체류 시간 조회");
        /* 구역 이탈 → 타이머 리셋 */
        TrackedPerson out_p = make_person(4, 900, 100, 1000, 400);
        router_route(r, &frame, &out_p, 1, 182.0, targets, MAX_TARGETS, NULL);
        CHECK(router_table_dwell_seconds(r, 4, 182.0) == 0.0, "구역 이탈 시 타이머 리셋");
        router_destroy(r);
    }

    /* 5) 코-엉덩이 수평 보조 판정 (종횡비는 정상이어도) */
    {
        RoiRouter *r = router_create(cfg);
        TrackedPerson p = make_person(5, 100, 100, 400, 500);   /* 300x400 = 0.75 (정상) */
        set_kpt(&p, KPT_NOSE, 150, 300);
        set_kpt(&p, KPT_L_HIP, 300, 310);                       /* 코-엉덩이 y차 10/400 < 0.15 */
        set_kpt(&p, KPT_R_HIP, 320, 310);
        int n = router_route(r, &frame, &p, 1, 0.0, targets, MAX_TARGETS, NULL);
        CHECK(n == 1 && targets[0].reason == REASON_FALL_SUSPECT,
              "키포인트 수평 자세 보조 판정");
        router_destroy(r);
    }
}

static double zero_dwell(void *ctx, int tid, double now) { (void)ctx; (void)tid; (void)now; return 0.0; }
static double big_dwell(void *ctx, int tid, double now)  { (void)ctx; (void)tid; (void)now; return 301.0; }

static void test_fsm(const PipelineConfig *cfg)
{
    EventQueue *q = evq_create();
    StateMachine *m = fsm_create(cfg, q, NULL);
    TrackedPerson p = make_person(10, 100, 100, 200, 400);
    Event ev;

    /* 쓰러짐 디바운스: 4회까지는 이벤트 없음, 5회째 발행, 6회째 중복 없음 */
    PrecisionResult pr;
    memset(&pr, 0, sizeof(pr));
    pr.track_id = 10;
    pr.reason = REASON_FALL_SUSPECT;
    pr.pose_detected = true;
    pr.torso_horizontal = true;

    for (int i = 1; i <= 4; i++)
        fsm_update(m, &p, 1, &pr, 1, NULL, zero_dwell, NULL, (double)i);
    CHECK(!evq_pop(q, &ev, 0), "쓰러짐 4회까지 이벤트 없음");
    fsm_update(m, &p, 1, &pr, 1, NULL, zero_dwell, NULL, 5.0);
    CHECK(evq_pop(q, &ev, 0) && ev.kind == EV_FALL_ALERT, "5회째 쓰러짐 확정");
    fsm_update(m, &p, 1, &pr, 1, NULL, zero_dwell, NULL, 6.0);
    CHECK(!evq_pop(q, &ev, 0), "6회째 중복 발행 없음");

    /* 신호 끊기면 카운터 리셋 */
    pr.torso_horizontal = false;
    fsm_update(m, &p, 1, &pr, 1, NULL, zero_dwell, NULL, 7.0);
    pr.torso_horizontal = true;
    for (int i = 8; i <= 11; i++)
        fsm_update(m, &p, 1, &pr, 1, NULL, zero_dwell, NULL, (double)i);
    CHECK(!evq_pop(q, &ev, 0), "리셋 후 4회까지 이벤트 없음");

    /* 장기 체류 안내방송 + 쿨다운 */
    memset(&pr, 0, sizeof(pr));
    pr.track_id = 10;
    fsm_update(m, &p, 1, &pr, 1, NULL, big_dwell, NULL, 100.0);
    CHECK(evq_pop(q, &ev, 0) && ev.kind == EV_ANNOUNCE_DWELL, "장기 체류 안내방송");
    fsm_update(m, &p, 1, &pr, 1, NULL, big_dwell, NULL, 150.0);
    CHECK(!evq_pop(q, &ev, 0), "쿨다운(120초) 내 재방송 없음");
    fsm_update(m, &p, 1, &pr, 1, NULL, big_dwell, NULL, 221.0);
    CHECK(evq_pop(q, &ev, 0) && ev.kind == EV_ANNOUNCE_DWELL, "쿨다운 후 재방송");

    /* 구매 완료 후에는 방송 없음 */
    fsm_mark_purchased(m, 10, 1);
    fsm_update(m, &p, 1, &pr, 1, NULL, big_dwell, NULL, 400.0);
    CHECK(!evq_pop(q, &ev, 0), "구매 후 방송 없음");

    /* 키오스크 앞 손들기 → 도움 요청 */
    TrackedPerson p2 = make_person(20, 100, 100, 200, 400);
    PrecisionResult pr2;
    memset(&pr2, 0, sizeof(pr2));
    pr2.track_id = 20;
    pr2.reason = REASON_KIOSK_ENTER;
    pr2.pose_detected = true;
    pr2.hand_raised = true;
    fsm_update(m, &p2, 1, &pr2, 1, NULL, zero_dwell, NULL, 500.0);
    CHECK(evq_pop(q, &ev, 0) && ev.kind == EV_KIOSK_ASSIST, "키오스크 도움 요청");

    fsm_destroy(m);
    evq_destroy(q);
}

static void test_reid(PipelineConfig *cfg)
{
    /* 합성 프레임: 사람 BBox(y 100~460) 기준 상의 구간(172~280)은 파랑,
     * 하의 구간(298~406)은 초록이 되도록 y=290에서 색 경계 */
    int w = 640, h = 480;
    uint8_t *buf = calloc((size_t)w * h * 3, 1);
    FrameView frame = { buf, w, h, w * 3 };
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *px = buf + (size_t)y * frame.stride + (size_t)x * 3;
            if (y < 290) { px[0] = 200; px[1] = 30; px[2] = 30; }   /* 파랑 */
            else         { px[0] = 30;  px[1] = 200; px[2] = 30; }  /* 초록 */
        }
    }

    Reid *r = reid_create(cfg);

    TrackedPerson p = make_person(1, 200, 100, 400, 460);
    reid_assign(r, &frame, &p, 1, 0.0);
    int stable1 = p.track_id;
    CHECK(stable1 == 1, "최초 안정 ID = 1");

    /* 같은 raw ID 유지 → 같은 안정 ID */
    p.track_id = 1;
    reid_assign(r, &frame, &p, 1, 1.0);
    CHECK(p.track_id == stable1, "raw ID 유지 시 안정 ID 유지");

    /* raw 트랙 소실: TTL(5초) 지난 뒤의 프레임에서 GC → 소실 갤러리로 이동 */
    reid_assign(r, &frame, NULL, 0, 8.0);   /* 사람 없는 프레임 (파이썬판과 동일하게 GC는 assign 끝에 수행) */

    /* 새 raw ID + 같은 옷으로 재등장 → 시그니처로 안정 ID 복원 */
    p.track_id = 99;
    reid_assign(r, &frame, &p, 1, 10.0);
    CHECK(p.track_id == stable1, "재입장 시 안정 ID 복원");

    /* 상·하의가 모두 다른 옷 (노랑/빨강) → 복원 없이 새 ID */
    reid_assign(r, &frame, NULL, 0, 18.0);  /* raw 99 → 소실 갤러리 */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *px = buf + (size_t)y * frame.stride + (size_t)x * 3;
            if (y < 290) { px[0] = 0;  px[1] = 220; px[2] = 220; }  /* 노랑 */
            else         { px[0] = 30; px[1] = 30;  px[2] = 200; }  /* 빨강 */
        }
    }
    TrackedPerson p3 = make_person(150, 200, 100, 400, 460);
    reid_assign(r, &frame, &p3, 1, 20.0);
    CHECK(p3.track_id != stable1, "다른 시그니처 → 새 안정 ID");

    reid_destroy(r);
    free(buf);
}

/* 유예(30초)는 넘고 안내방송 기준(300초)에는 못 미치는 체류 시간 */
static double mid_dwell(void *ctx, int tid, double now)
{
    (void)ctx; (void)tid; (void)now;
    return 100.0;
}

/* ── 신규: 키오스크 미방문 착석 룰 (8/10 회의 행동 필터) ── */
static void test_no_kiosk_sit(const PipelineConfig *base)
{
    PipelineConfig cfg = *base;                   /* grace 30초 (기본값) */
    EventQueue *q = evq_create();
    Journey *j = journey_create();
    StateMachine *m = fsm_create(&cfg, q, j);
    Event ev;

    /* 케이스 A: 입장 → 키오스크 안 거치고 착석 → 유예 초과 → 알림 1회 */
    TrackedPerson p = make_person(30, 100, 400, 200, 700);
    ZoneFlags fl = { .at_kiosk = false, .in_table = true };
    journey_note(j, 30, STEP_SIT, 0.0);
    PrecisionResult pr;
    memset(&pr, 0, sizeof(pr));
    pr.track_id = 30;
    fsm_update(m, &p, 1, &pr, 1, &fl, mid_dwell, NULL, 100.0);
    CHECK(evq_pop(q, &ev, 0) && ev.kind == EV_NO_KIOSK_SIT,
          "미방문 착석 알림 발행");
    CHECK(ev.journey[0] != 0, "이벤트에 동선 요약 첨부");
    fsm_update(m, &p, 1, &pr, 1, &fl, mid_dwell, NULL, 101.0);
    CHECK(!evq_pop(q, &ev, 0), "미방문 착석 알림은 트랙당 1회");

    /* 케이스 B: 키오스크를 거친 손님은 알림 없음 */
    TrackedPerson p2 = make_person(31, 100, 400, 200, 700);
    PrecisionResult pr2;
    memset(&pr2, 0, sizeof(pr2));
    pr2.track_id = 31;
    journey_note(j, 31, STEP_KIOSK, 0.0);
    journey_note(j, 31, STEP_SIT, 10.0);
    fsm_update(m, &p2, 1, &pr2, 1, &fl, mid_dwell, NULL, 100.0);
    while (evq_pop(q, &ev, 0))                    /* 장기체류 방송은 있을 수 있음 */
        CHECK(ev.kind != EV_NO_KIOSK_SIT, "키오스크 방문자는 미방문 알림 없음");

    /* 케이스 C: 룰 토글 OFF면 알림 없음 */
    cfg.rule_no_kiosk_sit = false;
    TrackedPerson p3 = make_person(32, 100, 400, 200, 700);
    PrecisionResult pr3;
    memset(&pr3, 0, sizeof(pr3));
    pr3.track_id = 32;
    journey_note(j, 32, STEP_SIT, 0.0);
    fsm_update(m, &p3, 1, &pr3, 1, &fl, mid_dwell, NULL, 200.0);
    while (evq_pop(q, &ev, 0))
        CHECK(ev.kind != EV_NO_KIOSK_SIT, "룰 OFF 시 미방문 알림 없음");

    fsm_destroy(m);
    journey_destroy(j);
    evq_destroy(q);
}

/* ── 신규: 동선 기록 ── */
static void test_journey(void)
{
    Journey *j = journey_create();

    journey_note(j, 1, STEP_KIOSK, 10.0);         /* 첫 기록 → 입장 자동 삽입 */
    CHECK(journey_visited(j, 1, STEP_ENTER), "첫 기록 시 입장 자동 기록");
    CHECK(journey_visited(j, 1, STEP_KIOSK), "키오스크 방문 기록");
    CHECK(!journey_visited(j, 1, STEP_SIT), "미기록 단계는 false");

    journey_note(j, 1, STEP_SIT, 20.0);
    journey_note(j, 1, STEP_SIT, 21.0);           /* 연속 중복 억제 */
    CHECK(journey_last_step(j, 1) == STEP_SIT, "마지막 단계 조회");

    char buf[512];
    journey_format(j, 1, buf, sizeof(buf));
    CHECK(strstr(buf, "입장") && strstr(buf, "키오스크") && strstr(buf, "착석"),
          "동선 문자열 포맷");
    CHECK(strstr(buf, "키오스크") < strstr(buf, "착석"), "단계 순서 유지");

    journey_gc(j, 100.0, 5.0);                    /* last_seen 21 → 소실 */
    CHECK(!journey_visited(j, 1, STEP_KIOSK), "GC 후 슬롯 해제");
    journey_destroy(j);
}

/* ── 신규: 카메라 자가 진단 ── */
static void test_camhealth(void)
{
    int w = 320, h = 180;
    uint8_t *buf = malloc((size_t)w * h * 3);
    FrameView f = { buf, w, h, w * 3 };
    char msg[192];

    /* 정상 프레임: 체크무늬 (디테일 풍부) */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t v = ((x / 8 + y / 8) % 2) ? 200 : 40;
            uint8_t *px = buf + (size_t)y * f.stride + (size_t)x * 3;
            px[0] = px[1] = px[2] = v;
        }
    CamHealth ch;
    camhealth_init(&ch, 0.0);
    CHECK(!camhealth_check_frame(&ch, &f, 1.0, msg, sizeof(msg)),
          "정상 프레임은 알림 없음");
    CHECK(ch.state == CAM_OK, "정상 상태 유지");

    /* 백화 프레임: 전체 흰색 — 3회 연속 후에만 확정 */
    memset(buf, 250, (size_t)w * h * 3);
    CHECK(!camhealth_check_frame(&ch, &f, 2.0, msg, sizeof(msg)), "백화 1회 미확정");
    CHECK(!camhealth_check_frame(&ch, &f, 3.0, msg, sizeof(msg)), "백화 2회 미확정");
    CHECK(camhealth_check_frame(&ch, &f, 4.0, msg, sizeof(msg)) &&
          ch.state == CAM_WHITEOUT, "백화 3회 연속 확정 알림");
    CHECK(!camhealth_check_frame(&ch, &f, 5.0, msg, sizeof(msg)),
          "동일 상태 중복 알림 없음");

    /* 정상 복구 알림 */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t v = ((x / 8 + y / 8) % 2) ? 200 : 40;
            uint8_t *px = buf + (size_t)y * f.stride + (size_t)x * 3;
            px[0] = px[1] = px[2] = v;
        }
    CHECK(camhealth_check_frame(&ch, &f, 6.0, msg, sizeof(msg)) &&
          ch.state == CAM_OK, "복구 알림");

    /* 프레임 정지 감시 */
    camhealth_init(&ch, 10.0);
    camhealth_check_freeze(&ch, 5, 10.0, 5.0, msg, sizeof(msg));   /* seq 기록 */
    CHECK(!camhealth_check_freeze(&ch, 5, 12.0, 5.0, msg, sizeof(msg)),
          "5초 이내 정지는 미알림");
    CHECK(camhealth_check_freeze(&ch, 5, 16.0, 5.0, msg, sizeof(msg)) &&
          ch.state == CAM_FROZEN, "5초 초과 정지 알림");
    CHECK(camhealth_check_frame(&ch, &f, 17.0, msg, sizeof(msg)) &&
          ch.state == CAM_OK, "프레임 재개 시 정지 해제");

    free(buf);
}

/* ── 신규: lock-free 프레임 버스 (단일 스레드 시퀀스 검증) ── */
static void test_framebus(void)
{
    int w = 64, h = 48, stride = w * 3;
    uint8_t *src = malloc((size_t)h * stride);
    FrameBus *b = framebus_create();
    FrameView f;
    uint64_t seq = 0;

    CHECK(!framebus_acquire(b, &f, &seq), "발행 전 acquire 실패");

    memset(src, 11, (size_t)h * stride);
    framebus_publish(b, src, w, h, stride);
    CHECK(framebus_acquire(b, &f, &seq) && seq == 1 && f.data[0] == 11,
          "발행 1건 수신");
    CHECK(!framebus_acquire(b, &f, &seq), "중복 수신 없음");

    /* 소비자가 밀리면 최신본만 남고 이전 것은 드롭 집계 */
    memset(src, 22, (size_t)h * stride);
    framebus_publish(b, src, w, h, stride);
    memset(src, 33, (size_t)h * stride);
    framebus_publish(b, src, w, h, stride);
    CHECK(framebus_dropped(b) == 1, "미소비 프레임 드롭 집계");
    CHECK(framebus_acquire(b, &f, &seq) && seq == 3 && f.data[0] == 33,
          "항상 최신 프레임 수신");
    CHECK(framebus_published_seq(b) == 3, "공개 시퀀스 조회");

    framebus_destroy(b);
    free(src);
}

/* ── 신규: 점주 설정 JSON 왕복 + 오탐 피드백 보정 ── */
static void test_settings(const PipelineConfig *base)
{
    SettingsStore *s = settings_create(base, NULL);   /* 파일 없이 메모리만 */

    char json[2048];
    settings_to_json(s, json, sizeof(json));
    CHECK(strstr(json, "\"rule_fall\": true") != NULL, "기본값 직렬화");

    /* 점주 페이지 POST 반영: 룰 끄기 + 슬라이더 + 구역 드래그 */
    settings_apply_json(s,
        "{\"rule_announce\": false, \"announce_dwell_sec\": 600,"
        " \"table_zone_x1\": 50, \"table_zone_y1\": 60,"
        " \"table_zone_x2\": 400, \"table_zone_y2\": 700,"
        " \"fall_aspect_ratio\": 99}");               /* 99 → 상한 3.0으로 클램프 */
    StoreSettings st;
    settings_get(s, &st);
    CHECK(!st.rule_announce, "룰 토글 반영");
    CHECK(st.announce_dwell_sec == 600, "슬라이더 값 반영");
    CHECK(st.table_zone.x1 == 50 && st.table_zone.y2 == 700, "구역 반영");
    CHECK(st.fall_aspect_ratio <= 3.0f, "범위 밖 입력 클램프");

    /* cfg 반영 확인 */
    PipelineConfig cfg = *base;
    settings_apply_to_cfg(&st, &cfg);
    CHECK(!cfg.rule_announce && cfg.announce_dwell_sec == 600 &&
          cfg.table_zone.x1 == 50, "cfg 반영");

    /* 오탐 피드백 → 임계값 완화 */
    float before = st.fall_aspect_ratio;
    char desc[128];
    CHECK(!settings_feedback(s, "fall_alert", desc, sizeof(desc)),
          "상한 도달 시 보정 없음");                   /* 3.0 = 상한 */
    settings_apply_json(s, "{\"fall_aspect_ratio\": 1.4}");
    CHECK(settings_feedback(s, "fall_alert", desc, sizeof(desc)),
          "쓰러짐 오탐 피드백 보정");
    settings_get(s, &st);
    CHECK(st.fall_aspect_ratio > 1.4f && st.fall_aspect_ratio < before,
          "보정 방향(완화) 확인");

    settings_destroy(s);
}

/* ── 신규: 인식 범위 마스크 (셀 판정 + hex 직렬화 왕복) ── */
static void test_mask(void)
{
    uint8_t m[MASK_BYTES] = { 0 };
    CHECK(!mask_test(m, 0.5f, 0.5f), "빈 마스크는 전부 통과");

    mask_set_cell(m, mask_cell_index(0.5f, 0.5f));
    CHECK(mask_test(m, 0.5f, 0.5f), "칠한 셀은 제외 판정");
    CHECK(!mask_test(m, 0.1f, 0.1f), "다른 셀은 영향 없음");
    CHECK(mask_count(m) == 1, "마스크 셀 수 집계");

    /* hex 왕복 (설정 파일 저장 ↔ 점주 페이지 전송 형식) */
    char hex[MASK_HEX_LEN + 1];
    mask_to_hex(m, hex);
    CHECK((int)strlen(hex) == MASK_HEX_LEN, "hex 길이 144자");
    uint8_t m2[MASK_BYTES] = { 0xFF };                /* 쓰레기값으로 시작 */
    CHECK(mask_from_hex(hex, m2) && memcmp(m, m2, MASK_BYTES) == 0,
          "hex 왕복 일치");
    CHECK(!mask_from_hex("zz", m2), "잘못된 hex 거부");

    /* 경계 클램프: 프레임 밖 좌표도 죽지 않고 가장자리 셀로 */
    mask_set_cell(m, mask_cell_index(1.5f, -0.5f));
    CHECK(mask_test(m, 0.999f, 0.001f), "경계 밖 좌표 클램프");
}

/* ── 신규: 구매 후 허용량 초과 체류 룰 (1잔당 N시간) ── */
static double dwell_6000(void *c, int t, double n) { (void)c; (void)t; (void)n; return 6000.0; }

static void test_overstay(const PipelineConfig *base)
{
    PipelineConfig cfg = *base;
    cfg.rule_overstay = true;
    cfg.activity_exempt = false;      /* 활동 감지 영향 배제 (별도 테스트) */
    cfg.rule_announce = false;        /* 방송 룰 간섭 배제 */
    EventQueue *q = evq_create();
    StateMachine *m = fsm_create(&cfg, q, NULL);
    Event ev;

    TrackedPerson p = make_person(50, 100, 400, 200, 700);
    p.has_kpts = false;               /* 행동 분석 생략 경로 */
    PrecisionResult pr;
    memset(&pr, 0, sizeof(pr));
    pr.track_id = 50;

    /* 결제 1건 → 허용 5400초. 체류 6000초 = 초과 → 알림 1회 */
    fsm_mark_purchased(m, 50, 1);
    fsm_update(m, &p, 1, &pr, 1, NULL, dwell_6000, NULL, 10.0);
    CHECK(evq_pop(q, &ev, 0) && ev.kind == EV_OVERSTAY, "허용 체류 초과 알림");
    fsm_update(m, &p, 1, &pr, 1, NULL, dwell_6000, NULL, 11.0);
    CHECK(!evq_pop(q, &ev, 0), "초과 알림은 1회만");

    /* 추가 결제 → 허용 10800초로 연장, 6000초는 정상 범위 */
    fsm_mark_purchased(m, 50, 1);
    fsm_update(m, &p, 1, &pr, 1, NULL, dwell_6000, NULL, 12.0);
    CHECK(!evq_pop(q, &ev, 0), "추가 주문 시 허용 체류 연장");

    fsm_destroy(m);
    evq_destroy(q);
}

/* ── 신규: 행동 분석 — 공부/조립 중 손님은 체류 방송 제외 ── */
static void test_activity_exempt(const PipelineConfig *base)
{
    PipelineConfig cfg = *base;       /* activity_exempt=true (기본값) */
    cfg.announce_cooldown_sec = 10;   /* 테스트 시간 단축 */
    EventQueue *q = evq_create();
    StateMachine *m = fsm_create(&cfg, q, NULL);
    Event ev;

    TrackedPerson p = make_person(60, 100, 100, 300, 500);   /* bbox 높이 400 */
    PrecisionResult pr;
    memset(&pr, 0, sizeof(pr));
    pr.track_id = 60;

    /* 1) 체류 조건 없이 손목만 매 추론 20px씩 움직여 EMA를 먼저 쌓는다
     *    (400px 대비 5% > 임계 2% → '작업 중' 판정 진입) */
    for (int i = 0; i < 6; i++) {
        set_kpt(&p, KPT_L_WRIST, 150 + i * 20, 300);
        set_kpt(&p, KPT_R_WRIST, 250 - i * 20, 300);
        fsm_update(m, &p, 1, &pr, 1, NULL, zero_dwell, NULL, 100.0 + i * 20);
    }
    /* 2) 작업 중 상태에서 장기 체류 조건 부여 → 방송이 제외되어야 함 */
    fsm_update(m, &p, 1, &pr, 1, NULL, big_dwell, NULL, 250.0);
    CHECK(!evq_pop(q, &ev, 0), "작업 중(손 움직임) 손님은 방송 제외");

    /* 손이 멈추면 EMA 감쇠 → 비활동 정지로 복귀 → 방송 발행 */
    for (int i = 0; i < 12; i++)
        fsm_update(m, &p, 1, &pr, 1, NULL, big_dwell, NULL, 300.0 + i * 20);
    CHECK(evq_pop(q, &ev, 0) && ev.kind == EV_ANNOUNCE_DWELL,
          "비활동 정지 전환 후 방송 재개");

    fsm_destroy(m);
    evq_destroy(q);
}

/* ── 신규: 비품 구역 반복 접근 (시럽·빨대 어뷰징) ── */
static void test_supply_abuse(const PipelineConfig *base)
{
    PipelineConfig cfg = *base;
    cfg.rule_supply_abuse = true;     /* 기본 꺼짐 — 테스트에서 켬 */
    cfg.rule_announce = false;
    EventQueue *q = evq_create();
    StateMachine *m = fsm_create(&cfg, q, NULL);
    Event ev;

    TrackedPerson p = make_person(70, 100, 100, 200, 400);
    p.has_kpts = false;
    PrecisionResult pr;
    memset(&pr, 0, sizeof(pr));
    pr.track_id = 70;

    /* 방문 = 구역 밖→안 전환. 3회째 진입에서 알림 (기본 supply_abuse_visits=3) */
    ZoneFlags in_sup = { .in_supply = true };
    ZoneFlags out_sup = { .in_supply = false };
    for (int visit = 1; visit <= 2; visit++) {
        fsm_update(m, &p, 1, &pr, 1, &in_sup, zero_dwell, NULL, visit * 10.0);
        fsm_update(m, &p, 1, &pr, 1, &out_sup, zero_dwell, NULL, visit * 10.0 + 5);
    }
    CHECK(!evq_pop(q, &ev, 0), "비품 2회 방문까지 알림 없음");
    fsm_update(m, &p, 1, &pr, 1, &in_sup, zero_dwell, NULL, 30.0);
    CHECK(evq_pop(q, &ev, 0) && ev.kind == EV_SUPPLY_ABUSE, "3회째 방문 알림");
    /* 구역 안에 머무는 동안은 방문 수 증가 없음 (전환 기준) */
    fsm_update(m, &p, 1, &pr, 1, &in_sup, zero_dwell, NULL, 31.0);
    CHECK(!evq_pop(q, &ev, 0), "체류 중 중복 카운트 없음");

    fsm_destroy(m);
    evq_destroy(q);
}

/* ── 신규: 일행 수 vs 주문 수 불일치 ── */
static void test_group_mismatch(const PipelineConfig *base)
{
    PipelineConfig cfg = *base;
    cfg.rule_group_mismatch = true;
    cfg.rule_announce = false;
    cfg.rule_no_kiosk_sit = false;    /* 간섭 배제 */
    EventQueue *q = evq_create();
    StateMachine *m = fsm_create(&cfg, q, NULL);
    Event ev;

    /* 3명이 테이블에 유의미하게(유예 초과) 체류, 결제는 1잔 */
    TrackedPerson ppl[3];
    PrecisionResult prs[3];
    ZoneFlags fls[3];
    for (int i = 0; i < 3; i++) {
        ppl[i] = make_person(80 + i, 100, 100, 200, 400);
        ppl[i].has_kpts = false;
        memset(&prs[i], 0, sizeof(prs[i]));
        prs[i].track_id = 80 + i;
        fls[i] = (ZoneFlags){ .in_table = true };
    }
    fsm_update(m, ppl, 3, prs, 3, fls, mid_dwell, NULL, 10.0);   /* 결제 0잔 → 규칙 8 침묵 */
    CHECK(!evq_pop(q, &ev, 0), "결제 0잔이면 일행 대조 안 함 (다른 룰 담당)");

    fsm_mark_purchased(m, 80, 1);                                 /* 1잔 결제 */
    fsm_update(m, ppl, 3, prs, 3, fls, mid_dwell, NULL, 20.0);
    bool got = false;
    while (evq_pop(q, &ev, 0))
        if (ev.kind == EV_GROUP_MISMATCH) got = true;
    CHECK(got, "3명 체류 vs 1잔 결제 알림");

    fsm_update(m, ppl, 3, prs, 3, fls, mid_dwell, NULL, 30.0);
    CHECK(!evq_pop(q, &ev, 0), "쿨다운 내 중복 알림 없음");

    /* 3잔으로 채우면 정상 범위 */
    fsm_mark_purchased(m, 81, 2);
    fsm_update(m, ppl, 3, prs, 3, fls, mid_dwell, NULL, 700.0);   /* 쿨다운 경과 */
    CHECK(!evq_pop(q, &ev, 0), "인원 수만큼 결제되면 알림 없음");

    fsm_destroy(m);
    evq_destroy(q);
}

/* ── 신규: 노키즈존 나이 추정 표 집계 ── */
static void test_age_votes(const PipelineConfig *base)
{
    PipelineConfig cfg = *base;
    cfg.rule_no_kids = true;          /* 기본 꺼짐 — 테스트에서 켬 */
    cfg.rule_announce = false;
    EventQueue *q = evq_create();
    StateMachine *m = fsm_create(&cfg, q, NULL);
    Event ev;

    TrackedPerson p = make_person(90, 100, 100, 200, 400);
    p.has_kpts = false;
    PrecisionResult pr;
    memset(&pr, 0, sizeof(pr));
    pr.track_id = 90;

    CHECK(fsm_age_needs_vote(m, 90), "결론 전에는 표 필요");
    /* 8세 추정 5표 (기본 kids_confirm_votes=5, 임계 13세) → 미성년 의심 */
    for (int i = 0; i < 4; i++) fsm_note_age(m, 90, 8);
    fsm_update(m, &p, 1, &pr, 1, NULL, zero_dwell, NULL, 10.0);
    CHECK(!evq_pop(q, &ev, 0), "4표까지는 미확정");
    fsm_note_age(m, 90, 9);
    fsm_update(m, &p, 1, &pr, 1, NULL, zero_dwell, NULL, 11.0);
    CHECK(evq_pop(q, &ev, 0) && ev.kind == EV_MINOR_SUSPECT, "5표 확정 알림");
    CHECK(!fsm_age_needs_vote(m, 90), "결론 후 추가 추론 불필요");
    fsm_update(m, &p, 1, &pr, 1, NULL, zero_dwell, NULL, 12.0);
    CHECK(!evq_pop(q, &ev, 0), "미성년 알림은 트랙당 1회");

    /* 성인(30세) 표가 우세하면 알림 없이 결론 */
    TrackedPerson p2 = make_person(91, 100, 100, 200, 400);
    p2.has_kpts = false;
    PrecisionResult pr2;
    memset(&pr2, 0, sizeof(pr2));
    pr2.track_id = 91;
    for (int i = 0; i < 10; i++) fsm_note_age(m, 91, 30);
    fsm_update(m, &p2, 1, &pr2, 1, NULL, zero_dwell, NULL, 20.0);
    CHECK(!evq_pop(q, &ev, 0), "성인 우세 시 알림 없음");
    CHECK(!fsm_age_needs_vote(m, 91), "성인 결론 후 추론 중단");

    fsm_destroy(m);
    evq_destroy(q);
}

/* ── 신규: 제로샷 이상행동 (관절 이동 벡터) ──
 * 추론 주기 기반 속도 정규화를 쓰므로, 기본 detect_every_n=5 (샘플 간격
 * ≈ 1/6초) 기준으로 좌표를 움직여 시뮬레이션한다. */
#include "behavior.h"

/* 손목을 매 샘플 진폭 amp px로 좌우 진동시킨다 (스윙 시뮬레이션) */
static void swing_wrists(TrackedPerson *p, int step, float amp)
{
    float dir = (step % 2) ? amp : -amp;
    set_kpt(p, KPT_L_WRIST, 150 + dir, 300);
    set_kpt(p, KPT_R_WRIST, 250 - dir, 320);
}

static void test_behavior(const PipelineConfig *base)
{
    PipelineConfig cfg = *base;      /* violence/vandalism/loitering/tampering 기본 ON */
    cfg.rule_reco = true;
    EventQueue *q = evq_create();
    Event ev;
    const double DT = 1.0 / 6.0;     /* 추론 6회/초 (detect_every_n=5, 30fps) */

    /* 1) 폭력: 두 사람 근접 + 한쪽 고속 스윙 지속 */
    {
        Behavior *b = behavior_create(&cfg, q, NULL);
        TrackedPerson ppl[2] = {
            make_person(1, 100, 100, 300, 500),   /* H=400 */
            make_person(2, 350, 100, 550, 500),   /* 중심 거리 250px < 1.5H */
        };
        ZoneFlags fl[2] = { 0 };
        for (int s = 0; s < 8; s++) {
            swing_wrists(&ppl[0], s, 120);        /* 240px/샘플 ≈ 3.6H/s > 2.5 */
            behavior_update(b, ppl, 2, fl, 1.0 + s * DT);
        }
        CHECK(evq_pop(q, &ev, 0) && ev.kind == EV_VIOLENCE, "폭력 의심 감지");
        for (int s = 8; s < 12; s++) {            /* 쿨다운 내 반복 없음 */
            swing_wrists(&ppl[0], s, 120);
            behavior_update(b, ppl, 2, fl, 1.0 + s * DT);
        }
        CHECK(!evq_pop(q, &ev, 0), "폭력 쿨다운 내 중복 없음");
        behavior_destroy(b);
    }

    /* 2) 기물 파손: 단독 인원이 제자리 고속 스윙 반복 (더 긴 확정) */
    {
        Behavior *b = behavior_create(&cfg, q, NULL);
        TrackedPerson p = make_person(3, 100, 100, 300, 500);
        ZoneFlags fl = { 0 };
        for (int s = 0; s < 12; s++) {
            swing_wrists(&p, s, 120);
            behavior_update(b, &p, 1, &fl, 1.0 + s * DT);
        }
        CHECK(evq_pop(q, &ev, 0) && ev.kind == EV_VANDALISM, "기물 파손 의심 감지");
        behavior_destroy(b);
    }

    /* 3) 정상 활동(느린 손 움직임)은 어떤 이상행동도 아님 */
    {
        Behavior *b = behavior_create(&cfg, q, NULL);
        TrackedPerson p = make_person(4, 100, 100, 300, 500);
        ZoneFlags fl = { 0 };
        for (int s = 0; s < 12; s++) {
            swing_wrists(&p, s, 15);              /* 30px/샘플 ≈ 0.45H/s */
            behavior_update(b, &p, 1, &fl, 1.0 + s * DT);
        }
        CHECK(!evq_pop(q, &ev, 0), "정상 손 움직임은 미감지");
        behavior_destroy(b);
    }

    /* 4) 낙상 급강하: 중심이 빠르게 하강 + 종횡비 붕괴 */
    {
        Behavior *b = behavior_create(&cfg, q, NULL);
        TrackedPerson p = make_person(5, 100, 100, 300, 500);
        p.has_kpts = false;
        ZoneFlags fl = { 0 };
        behavior_update(b, &p, 1, &fl, 1.0);
        for (int s = 1; s <= 3; s++) {            /* 매 샘플 200px 하강 + 납작해짐 */
            p.bbox[1] += 200; p.bbox[3] += 140;   /* 높이도 감소 (넘어짐) */
            p.bbox[2] += 100;                     /* 가로로 퍼짐 → 종횡비 ↑ */
            behavior_update(b, &p, 1, &fl, 1.0 + s * DT);
        }
        CHECK(evq_pop(q, &ev, 0) && ev.kind == EV_FALL_ALERT,
              "낙상 급강하 빠른 경로 감지");
        behavior_destroy(b);
    }

    /* 5) 배회: 착석·구매 없이 loiter_sec 이상 계속 이동 */
    {
        PipelineConfig c2 = cfg;
        c2.loiter_sec = 10.0;                     /* 테스트 시간 단축 */
        Journey *j = journey_create();
        Behavior *b = behavior_create(&c2, q, j);
        TrackedPerson p = make_person(6, 100, 100, 300, 500);
        p.has_kpts = false;
        ZoneFlags fl = { 0 };
        for (int s = 0; s <= 70; s++) {           /* 12초간 매 샘플 40px 이동 */
            p.bbox[0] += 40; p.bbox[2] += 40;
            if (p.bbox[2] > 1200) { p.bbox[0] = 100; p.bbox[2] = 300; }
            behavior_update(b, &p, 1, &fl, 1.0 + s * DT);
        }
        CHECK(evq_pop(q, &ev, 0) && ev.kind == EV_LOITERING, "배회 감지");
        /* 착석 이력이 있으면 배회 아님 */
        TrackedPerson p2 = make_person(7, 100, 100, 300, 500);
        p2.has_kpts = false;
        journey_note(j, 7, STEP_SIT, 1.0);
        for (int s = 0; s <= 70; s++) {
            p2.bbox[0] += 40; p2.bbox[2] += 40;
            if (p2.bbox[2] > 1200) { p2.bbox[0] = 100; p2.bbox[2] = 300; }
            behavior_update(b, &p2, 1, &fl, 1.0 + s * DT);
        }
        CHECK(!evq_pop(q, &ev, 0), "착석 이력자는 배회 아님");
        behavior_destroy(b);
        journey_destroy(j);
    }

    /* 6) 무단 조작: 키오스크 연속 점유 tamper_sec 초과 + 추천 컨텍스트 1회 */
    {
        PipelineConfig c2 = cfg;
        c2.tamper_sec = 5.0;
        Behavior *b = behavior_create(&c2, q, NULL);
        TrackedPerson p = make_person(8, 100, 100, 300, 500);
        p.has_kpts = false;
        ZoneFlags fl = { .at_kiosk = true };
        behavior_update(b, &p, 1, &fl, 1.0);      /* 근접 전환 → 추천 발행 */
        CHECK(evq_pop(q, &ev, 0) && ev.kind == EV_KIOSK_RECO,
              "키오스크 추천 컨텍스트 발행");
        behavior_update(b, &p, 1, &fl, 3.0);
        CHECK(!evq_pop(q, &ev, 0), "점유 5초 전엔 조작 알림 없음");
        behavior_update(b, &p, 1, &fl, 7.0);
        CHECK(evq_pop(q, &ev, 0) && ev.kind == EV_TAMPERING,
              "키오스크 장시간 점유 알림");
        behavior_update(b, &p, 1, &fl, 8.0);
        CHECK(!evq_pop(q, &ev, 0), "조작 알림은 트랙당 1회");
        behavior_destroy(b);
    }

    evq_destroy(q);
}

int main(void)
{
    PipelineConfig cfg = CFG_DEFAULT;

    test_router(&cfg);
    test_fsm(&cfg);
    test_reid(&cfg);
    test_no_kiosk_sit(&cfg);
    test_journey();
    test_camhealth();
    test_framebus();
    test_settings(&cfg);
    test_mask();
    test_overstay(&cfg);
    test_activity_exempt(&cfg);
    test_supply_abuse(&cfg);
    test_group_mismatch(&cfg);
    test_age_votes(&cfg);
    test_behavior(&cfg);

    printf("%d passed, %d failed\n", n_pass, n_fail);
    return n_fail == 0 ? 0 : 1;
}
