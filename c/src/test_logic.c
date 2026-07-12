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

#include "config.h"
#include "fsm.h"
#include "reid.h"
#include "roi_router.h"
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
        int n = router_route(r, &frame, &p, 1, 0.0, targets, MAX_TARGETS);
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
        int n = router_route(r, &frame, &p, 1, 0.0, targets, MAX_TARGETS);
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
        int n = router_route(r, &frame, &p, 1, 0.0, targets, MAX_TARGETS);
        CHECK(n == 1 && targets[0].reason == REASON_KIOSK_ENTER,
              "눈 간격 근사 키오스크 트리거");
        router_destroy(r);
    }

    /* 4) 테이블 구역 180초 정체 → table_dwell (그 전에는 트리거 없음) */
    {
        RoiRouter *r = router_create(cfg);
        TrackedPerson p = make_person(4, 300, 400, 400, 700);   /* 발밑 (350,700) = table 구역 */
        int n = router_route(r, &frame, &p, 1, 0.0, targets, MAX_TARGETS);
        CHECK(n == 0, "정체 시간 미달 시 트리거 없음");
        n = router_route(r, &frame, &p, 1, 100.0, targets, MAX_TARGETS);
        CHECK(n == 0, "100초 시점에도 트리거 없음");
        n = router_route(r, &frame, &p, 1, 181.0, targets, MAX_TARGETS);
        CHECK(n == 1 && targets[0].reason == REASON_TABLE_DWELL,
              "테이블 181초 정체 트리거");
        CHECK(router_table_dwell_seconds(r, 4, 181.0) > 180.0, "체류 시간 조회");
        /* 구역 이탈 → 타이머 리셋 */
        TrackedPerson out_p = make_person(4, 900, 100, 1000, 400);
        router_route(r, &frame, &out_p, 1, 182.0, targets, MAX_TARGETS);
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
        int n = router_route(r, &frame, &p, 1, 0.0, targets, MAX_TARGETS);
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
    StateMachine *m = fsm_create(cfg, q);
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
        fsm_update(m, &p, 1, &pr, 1, zero_dwell, NULL, (double)i);
    CHECK(!evq_pop(q, &ev, 0), "쓰러짐 4회까지 이벤트 없음");
    fsm_update(m, &p, 1, &pr, 1, zero_dwell, NULL, 5.0);
    CHECK(evq_pop(q, &ev, 0) && ev.kind == EV_FALL_ALERT, "5회째 쓰러짐 확정");
    fsm_update(m, &p, 1, &pr, 1, zero_dwell, NULL, 6.0);
    CHECK(!evq_pop(q, &ev, 0), "6회째 중복 발행 없음");

    /* 신호 끊기면 카운터 리셋 */
    pr.torso_horizontal = false;
    fsm_update(m, &p, 1, &pr, 1, zero_dwell, NULL, 7.0);
    pr.torso_horizontal = true;
    for (int i = 8; i <= 11; i++)
        fsm_update(m, &p, 1, &pr, 1, zero_dwell, NULL, (double)i);
    CHECK(!evq_pop(q, &ev, 0), "리셋 후 4회까지 이벤트 없음");

    /* 장기 체류 안내방송 + 쿨다운 */
    memset(&pr, 0, sizeof(pr));
    pr.track_id = 10;
    fsm_update(m, &p, 1, &pr, 1, big_dwell, NULL, 100.0);
    CHECK(evq_pop(q, &ev, 0) && ev.kind == EV_ANNOUNCE_DWELL, "장기 체류 안내방송");
    fsm_update(m, &p, 1, &pr, 1, big_dwell, NULL, 150.0);
    CHECK(!evq_pop(q, &ev, 0), "쿨다운(120초) 내 재방송 없음");
    fsm_update(m, &p, 1, &pr, 1, big_dwell, NULL, 221.0);
    CHECK(evq_pop(q, &ev, 0) && ev.kind == EV_ANNOUNCE_DWELL, "쿨다운 후 재방송");

    /* 구매 완료 후에는 방송 없음 */
    fsm_mark_purchased(m, 10);
    fsm_update(m, &p, 1, &pr, 1, big_dwell, NULL, 400.0);
    CHECK(!evq_pop(q, &ev, 0), "구매 후 방송 없음");

    /* 키오스크 앞 손들기 → 도움 요청 */
    TrackedPerson p2 = make_person(20, 100, 100, 200, 400);
    PrecisionResult pr2;
    memset(&pr2, 0, sizeof(pr2));
    pr2.track_id = 20;
    pr2.reason = REASON_KIOSK_ENTER;
    pr2.pose_detected = true;
    pr2.hand_raised = true;
    fsm_update(m, &p2, 1, &pr2, 1, zero_dwell, NULL, 500.0);
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

int main(void)
{
    PipelineConfig cfg = CFG_DEFAULT;

    test_router(&cfg);
    test_fsm(&cfg);
    test_reid(&cfg);

    printf("%d passed, %d failed\n", n_pass, n_fail);
    return n_fail == 0 ? 0 : 1;
}
