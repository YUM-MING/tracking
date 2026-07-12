/*
 * 메인 파이프라인 오케스트레이터 (main.py 포팅)
 * [카메라] → (프레임 샘플링) → 1단계 전역 탐지(YOLO ONNX + 추적)
 *         → 1.5단계 비율 태깅/재식별 → 2단계 동적 크롭 선별
 *         → 3단계 크롭 정밀 재추론 → 4단계 상태 머신 → 웹소켓 전송
 *
 * 실행:
 *   kiosk_tracking.exe                # 기본 웹캠
 *   kiosk_tracking.exe rtsp://...     # IP 카메라
 *   kiosk_tracking.exe 영상파일.mp4    # 파일 재생 (오프라인 테스트)
 */
#include <conio.h>
#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "analyzer.h"
#include "config.h"
#include "cv_shim.h"
#include "fsm.h"
#include "imgproc.h"
#include "profiler.h"
#include "reid.h"
#include "resource.h"
#include "roi_router.h"
#include "tracker.h"
#include "types.h"
#include "ws_sender.h"
#include "yolo_pose.h"

/* COCO 17 키포인트 연결선 (스켈레톤 시각화용) */
static const int SKELETON_EDGES[][2] = {
    { 0, 1 }, { 0, 2 }, { 1, 3 }, { 2, 4 },              /* 얼굴 (코-눈-귀) */
    { 5, 6 }, { 5, 7 }, { 7, 9 }, { 6, 8 }, { 8, 10 },   /* 어깨-팔 */
    { 5, 11 }, { 6, 12 }, { 11, 12 },                    /* 몸통 */
    { 11, 13 }, { 13, 15 }, { 12, 14 }, { 14, 16 },      /* 다리 */
};

/* reason별 BBox 색 (B, G, R) */
static void reason_color(TriggerReason r, int *b, int *g, int *rr)
{
    switch (r) {
    case REASON_FALL_SUSPECT: *b = 0;   *g = 0;   *rr = 255; break;
    case REASON_KIOSK_ENTER:  *b = 0;   *g = 200; *rr = 255; break;
    case REASON_TABLE_DWELL:  *b = 255; *g = 150; *rr = 0;   break;
    default:                  *b = 0;   *g = 255; *rr = 0;   break;
    }
}

/* YOLO 17키포인트 뼈대 그리기. 무효 포인트는 건너뛴다. */
static void draw_skeleton(FrameView *f, const TrackedPerson *p, float min_conf)
{
    for (int k = 0; k < KPT_COUNT; k++) {
        if (!kpt_valid(p, k, min_conf)) continue;
        cvs_circle(f->data, f->w, f->h, f->stride,
                   (int)p->kpts[k].x, (int)p->kpts[k].y, 3, 0, 255, 255, -1);
    }
    for (int e = 0; e < (int)(sizeof(SKELETON_EDGES) / sizeof(SKELETON_EDGES[0])); e++) {
        int a = SKELETON_EDGES[e][0], b = SKELETON_EDGES[e][1];
        if (!kpt_valid(p, a, min_conf) || !kpt_valid(p, b, min_conf)) continue;
        cvs_line(f->data, f->w, f->h, f->stride,
                 (int)p->kpts[a].x, (int)p->kpts[a].y,
                 (int)p->kpts[b].x, (int)p->kpts[b].y, 0, 255, 255, 1);
    }
}

/* 디버그 오버레이 (구역, BBox, 트리거 상태, 추론 FPS) */
static void draw_debug(const PipelineConfig *cfg, FrameView *f,
                       const TrackedPerson *people, int n_people,
                       const PrecisionTarget *targets, int n_targets,
                       const RoiRouter *router, double now, double infer_fps)
{
    /* 구역 */
    const Zone *zones[2];
    int zb[2][3];
    int nz = 0;
    zones[nz] = &cfg->table_zone;
    zb[nz][0] = 255; zb[nz][1] = 150; zb[nz][2] = 0;
    nz++;
    if (cfg->kiosk_trigger_mode == KIOSK_TRIGGER_ZONE) {  /* near 모드엔 구역 개념 없음 */
        zones[nz] = &cfg->kiosk_zone;
        zb[nz][0] = 0; zb[nz][1] = 200; zb[nz][2] = 255;
        nz++;
    }
    for (int i = 0; i < nz; i++) {
        cvs_rect(f->data, f->w, f->h, f->stride, zones[i]->x1, zones[i]->y1,
                 zones[i]->x2, zones[i]->y2, zb[i][0], zb[i][1], zb[i][2], 1);
        cvs_text(f->data, f->w, f->h, f->stride, zones[i]->name,
                 zones[i]->x1 + 4, zones[i]->y1 + 18, 0.5,
                 zb[i][0], zb[i][1], zb[i][2], 1);
    }

    for (int i = 0; i < n_people; i++) {
        const TrackedPerson *p = &people[i];
        TriggerReason reason = REASON_NONE;
        for (int j = 0; j < n_targets; j++) {
            if (targets[j].track_id == p->track_id) { reason = targets[j].reason; break; }
        }
        int b, g, r;
        reason_color(reason, &b, &g, &r);
        int x1 = (int)p->bbox[0], y1 = (int)p->bbox[1];
        int x2 = (int)p->bbox[2], y2 = (int)p->bbox[3];
        cvs_rect(f->data, f->w, f->h, f->stride, x1, y1, x2, y2, b, g, r, 2);

        /* 스켈레톤이 실제로 잡히는지 눈으로 확인할 수 있게 뼈대 표시 */
        if (p->has_kpts) draw_skeleton(f, p, cfg->kpt_valid_conf);

        char label[128];
        int off = snprintf(label, sizeof(label), "ID%d", p->track_id);
        if (reason != REASON_NONE)
            off += snprintf(label + off, sizeof(label) - off, " [%s]",
                            reason_str(reason));
        double dwell = router_table_dwell_seconds(router, p->track_id, now);
        if (dwell > 0)
            off += snprintf(label + off, sizeof(label) - off, " %ds", (int)dwell);
        /* 키오스크 근접 판정에 쓰는 얼굴 비율 표시 (임계값 튜닝용) */
        float fw = person_face_width(p, cfg->kpt_valid_conf);
        if (fw >= 0)
            snprintf(label + off, sizeof(label) - off, " face%.2f", fw / f->w);
        int ty = y1 - 6 > 12 ? y1 - 6 : 12;
        cvs_text(f->data, f->w, f->h, f->stride, label, x1, ty, 0.55, b, g, r, 2);
    }

    char fps_label[64];
    snprintf(fps_label, sizeof(fps_label), "infer FPS %.1f (skip %d)",
             infer_fps, cfg->detect_every_n);
    cvs_text(f->data, f->w, f->h, f->stride, fps_label, 10, 25, 0.7, 255, 255, 255, 2);
}

/* 상태 머신 → 라우터 체류 시간 조회 어댑터 */
static double dwell_adapter(void *ctx, int track_id, double now)
{
    return router_table_dwell_seconds((const RoiRouter *)ctx, track_id, now);
}

/* ── 이벤트 워커 스레드: 큐 소비 → 로컬 처리 + 서버 전송 ── */
typedef struct {
    EventQueue *q;
    WsSender *sender;
} WorkerCtx;

static DWORD WINAPI event_worker_main(LPVOID arg)
{
    WorkerCtx *ctx = arg;
    Event ev;
    for (;;) {
        if (evq_pop(ctx->q, &ev, 500)) {
            /* TODO: 실제 오디오/알림 연동 지점 (예: PlaySound) */
            fprintf(stderr, "[EVENT] %s | %s\n", event_kind_str(ev.kind), ev.message);
            if (ctx->sender) ws_send_event(ctx->sender, &ev);
        } else if (evq_is_closed(ctx->q)) {
            return 0;      /* 종료 신호 + 큐 비움 완료 */
        }
    }
}

int main(int argc, char **argv)
{
    /* 콘솔 UTF-8 출력 (한글 로그) */
    SetConsoleOutputCP(CP_UTF8);

    PipelineConfig cfg = CFG_DEFAULT;
    const char *source = cfg.camera_source;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-window") == 0)
            cfg.show_window = false;   /* 운영/헤드리스 실행 (표시 비용 절약) */
        else if (strncmp(argv[i], "--ws=", 5) == 0)
            cfg.ws_url = argv[i] + 5;  /* 예: --ws=ws://192.168.0.10:8080/events */
        else
            source = argv[i];
    }

    apply_cpu_affinity(&cfg);

    /* ── 파이프라인 구성 ───────────────────────────────── */
    YoloPose *yolo = yolo_create(&cfg);            /* 1단계 (3단계와 공유) */
    if (!yolo) {
        fprintf(stderr, "모델 로드 실패 — models\\yolo11n-pose.onnx 확인\n");
        return 1;
    }
    Tracker *tracker = tracker_create(&cfg);
    Reid *reid = cfg.reid_enabled ? reid_create(&cfg) : NULL;
    RoiRouter *router = router_create(&cfg);       /* 2단계 */
    Analyzer *analyzer = analyzer_create(&cfg, yolo); /* 3단계 */
    EventQueue *evq = evq_create();
    StateMachine *fsm = fsm_create(&cfg, evq);     /* 4단계 */

    WsSender *sender = ws_create(&cfg);            /* 서버 전송 */
    ws_start(sender);
    WorkerCtx worker_ctx = { evq, sender };
    HANDLE worker = CreateThread(NULL, 0, event_worker_main, &worker_ctx, 0, NULL);

    ResourceMonitor *monitor = NULL;
    if (cfg.monitor_enabled) {                     /* 테스트 수칙: 리소스 모니터 상시 가동 */
        monitor = monitor_create(&cfg);
        monitor_start(monitor);
    }

    CvsCapture *cap = cvs_open(source, cfg.frame_width, cfg.frame_height);
    if (!cap) {
        fprintf(stderr, "카메라를 열 수 없습니다: %s\n", source);
        return 1;
    }

    fprintf(stderr, "파이프라인 시작 (종료: 영상 창 또는 터미널에서 q)\n");

    /* 터미널 q 종료는 stdin이 진짜 콘솔일 때만 활성.
     * 파이프/리다이렉트 환경에서 _kbhit는 신뢰할 수 없고 _getch가 블로킹된다. */
    DWORD con_mode;
    bool stdin_is_console =
        GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &con_mode) != 0;

    long frame_idx = 0;
    double infer_t0 = mono_now(), infer_fps = 0;
    int infer_n = 0;

    TrackedPerson people[MAX_PEOPLE];
    int n_people = 0;                              /* 스킵 프레임에서는 직전 결과 재사용 */
    PrecisionTarget targets[MAX_TARGETS];
    int n_targets = 0;
    PrecisionResult precision[MAX_TARGETS];
    Detection dets[MAX_DETECTIONS];

    StageProfiler prof;
    prof_init(&prof, 10.0);

    for (;;) {
        /* 터미널에서 q 입력으로 종료 (영상 창 포커스 없이도 동작) */
        if (stdin_is_console && _kbhit()) {
            int ch = _getch();
            if (ch == 'q' || ch == 'Q') break;
        }

        FrameView frame;
        if (!cvs_read(cap, &frame.data, &frame.w, &frame.h, &frame.stride)) {
            fprintf(stderr, "프레임 수신 실패 — 종료\n");
            break;
        }
        frame_idx++;

        /* ── 프레임 샘플링: N프레임마다 1회만 추론 (CPU 예산 보호) ── */
        if (frame_idx % cfg.detect_every_n == 0) {
            double now = mono_now();

            /* 1단계: 전역 탐지 + 추적 (저신뢰 검출도 트래커 2차 매칭에 필요) */
            double t0 = mono_now();
            int n_dets = yolo_infer(yolo, &frame, 0, 0, frame.w, frame.h,
                                    cfg.yolo_imgsz, cfg.track_low_conf,
                                    dets, MAX_DETECTIONS);
            n_people = tracker_update(tracker, dets, n_dets, people, MAX_PEOPLE);
            double t1 = mono_now();
            prof_add(&prof, "yolo+track", t1 - t0, n_people);

            /* 1.5단계: 옷 색상·채도 + 신체 비율 시그니처로 안정 ID 부여 */
            if (reid) reid_assign(reid, &frame, people, n_people, now);
            double t2 = mono_now();
            prof_add(&prof, "reid", t2 - t1, 1);

            /* 2단계: 트리거 대상 선별 (크롭은 사각형만 — zero-copy) */
            n_targets = router_route(router, &frame, people, n_people, now,
                                     targets, MAX_TARGETS);
            double t3 = mono_now();
            prof_add(&prof, "roi", t3 - t2, 1);

            /* 3단계: 선별된 크롭만 고해상도 재추론 */
            for (int i = 0; i < n_targets; i++)
                analyzer_analyze(analyzer, &frame, &targets[i], &precision[i]);
            double t4 = mono_now();
            prof_add(&prof, "crop-pose", t4 - t3, n_targets);

            /* 4단계: 결정론적 상태 머신 → 이벤트 큐 */
            fsm_update(fsm, people, n_people, precision, n_targets,
                       dwell_adapter, router, now);
            prof_maybe_report(&prof, now);

            infer_n++;
            double elapsed = mono_now() - infer_t0;
            if (elapsed >= 2.0) {
                infer_fps = infer_n / elapsed;
                infer_t0 = mono_now();
                infer_n = 0;
            }
        }

        /* ── 디버그 시각화 (표시 자체도 CPU 비용 — 주기 조절 가능) ── */
        if (cfg.show_window && frame_idx % cfg.display_every_n == 0) {
            draw_debug(&cfg, &frame, people, n_people, targets, n_targets,
                       router, mono_now(), infer_fps);
            cvs_show("Hybrid Cascade Pipeline (C)", frame.data,
                     frame.w, frame.h, frame.stride);
            int key = cvs_waitkey(1);
            if ((key & 0xFF) == 'q') break;
        }
    }

    /* ── 정리 ──────────────────────────────────────────── */
    cvs_close(cap);
    cvs_destroy_all_windows();
    evq_close(evq);
    if (worker) {
        WaitForSingleObject(worker, 2000);
        CloseHandle(worker);
    }
    ws_stop(sender);
    ws_destroy(sender);
    if (monitor) {
        monitor_stop(monitor);
        fprintf(stderr, "리소스 피크: sys CPU %.1f%% / RSS %.0fMB\n",
                monitor_peak_cpu(monitor), monitor_peak_rss_mb(monitor));
        monitor_destroy(monitor);
    }
    fsm_destroy(fsm);
    evq_destroy(evq);
    analyzer_destroy(analyzer);
    router_destroy(router);
    if (reid) reid_destroy(reid);
    tracker_destroy(tracker);
    yolo_destroy(yolo);
    fprintf(stderr, "파이프라인 종료\n");
    return 0;
}
