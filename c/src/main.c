/*
 * 메인 파이프라인 오케스트레이터 (main.py 포팅 → 멀티스레드 개편)
 *
 * 스레드 구성 (8/9 보고 '스레드 분배 계획'의 구현):
 *   [1] 캡처 (생산자)     : 카메라 → lock-free 트리플 버퍼 (framebus)
 *   [2] 추론 (소비자=메인) : YOLO → 추적 → 재식별 → ROI → 정밀분석 → 상태머신
 *   [3] 이벤트 워커        : 큐 소비 → 로그/점주 페이지/웹소켓 전달
 *   [4] 웹소켓 전송        : 서버 전송 (재연결 백오프)
 *   [5] 점주 페이지 HTTP   : 설정/상태/이벤트 API + 웹 UI
 *   [6] 리소스 모니터      : CPU/RSS 상시 계측
 * 스레드 간 공유는 전부 큐/스냅샷 복사 — 공유 프레임 없음.
 * 힙 할당은 초기화 시 1회, 메인 루프 안에서는 malloc 0회.
 *
 * 실행:
 *   kiosk_tracking                 # 기본 웹캠
 *   kiosk_tracking rtsp://...      # IP 카메라
 *   kiosk_tracking video.mp4       # 파일 재생 (오프라인 테스트)
 *   kiosk_tracking --no-window --ws=ws://host:8080/events --admin-port=8765
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <termios.h>
#include <sys/select.h>

#include "mask.h"

#include "admin_server.h"
#include "age_est.h"
#include "analyzer.h"
#include "behavior.h"
#include "camhealth.h"
#include "objdet.h"
#include "config.h"
#include "cv_shim.h"
#include "datastore.h"
#include "framebus.h"
#include "fsm.h"
#include "imgproc.h"
#include "journey.h"
#include "logger.h"
#include "profiler.h"
#include "reid.h"
#include "resource.h"
#include "roi_router.h"
#include "store_settings.h"
#include "tracker.h"
#include "types.h"
#include "ws_sender.h"
#include "yolo_pose.h"

/* ── 터미널 q 종료 (Windows _kbhit/_getch 대응) ──
 * cbreak 모드(ICANON/ECHO 끔, VMIN=0)로 전환해 Enter 없이 즉시 키 감지.
 * select()로 non-blocking 조회 후 read()로 1바이트만 소비한다. */
static struct termios g_orig_termios;
static bool g_termios_saved = false;

static void restore_terminal(void)
{
    if (g_termios_saved) tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
}

static bool enable_raw_stdin(void)
{
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) != 0) return false;
    g_termios_saved = true;
    atexit(restore_terminal);
    struct termios raw = g_orig_termios;
    raw.c_lflag &= (unsigned)~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
}

static bool stdin_has_key(void)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = { 0, 0 };
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

static int read_key(void)
{
    unsigned char ch;
    return read(STDIN_FILENO, &ch, 1) == 1 ? ch : -1;
}

/* ── 캡처 스레드 (생산자) ─────────────────────────────
 * 카메라에서 프레임을 읽어 framebus에 공개하는 일만 한다.
 * 읽기 실패가 이어지면 재연결을 시도하고, 원인은 통합 로그에 남긴다. */
typedef struct {
    const PipelineConfig *cfg;
    const char *source;
    FrameBus *bus;
    _Atomic bool stop;
    _Atomic bool eof;          /* 파일 재생 종료 등 — 소비자에게 종료 전파 */
    _Atomic bool low_power;    /* 영업시간 외: 2초 1프레임 (발열 보호) */
} CaptureCtx;

static void *capture_main(void *arg)
{
    CaptureCtx *c = arg;
    apply_thread_role(THREAD_ROLE_CAPTURE);

    CvsCapture *cap = cvs_open(c->source, c->cfg->frame_width, c->cfg->frame_height);
    if (!cap) {
        LOGE("캡처", "카메라를 열 수 없습니다: %s", c->source);
        atomic_store(&c->eof, true);
        return NULL;
    }
    LOGI("캡처", "카메라 스트리밍 시작: %s", c->source);

    int fail_streak = 0;
    while (!atomic_load(&c->stop)) {
        uint8_t *data;
        int w, h, stride;
        if (cvs_read(cap, &data, &w, &h, &stride)) {
            fail_streak = 0;
            framebus_publish(c->bus, data, w, h, stride);
            /* 영업시간 외 저전력: 프레임 사이를 크게 띄워 카메라 ISP·엣지 CPU
             * 발열을 낮춘다. 하트비트 프레임은 유지 → 야간에도 자가 진단 동작. */
            if (atomic_load(&c->low_power)) usleep(2000 * 1000);
            continue;
        }
        /* 읽기 실패: 순간 끊김(스트림)일 수도, 파일 끝일 수도 있다 */
        fail_streak++;
        if (fail_streak == 1)
            LOGW("캡처", "프레임 수신 실패 — 재시도");
        if (fail_streak >= 30) {                     /* 약 3초 연속 실패 → 재연결 */
            LOGE("캡처", "프레임 수신 30회 연속 실패 — 카메라 재연결 시도");
            cvs_close(cap);
            cap = cvs_open(c->source, c->cfg->frame_width, c->cfg->frame_height);
            if (!cap) {
                LOGE("캡처", "재연결 실패 — 캡처 종료 (하드웨어 점검 필요)");
                break;
            }
            fail_streak = 0;
        }
        usleep(100 * 1000);
    }
    if (cap) cvs_close(cap);
    atomic_store(&c->eof, true);
    LOGI("캡처", "캡처 스레드 종료");
    return NULL;
}

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

/* ── 비식별 표시 모드 (8월 말 회의 ③ 카메라 거부감 해소) ──
 * 16px 블록 평균으로 디테일을 뭉개 인물 식별을 불가능하게 만들고,
 * 밝기를 열화상풍 의사색(어두움=파랑 → 밝음=노랑/흰색)으로 입힌다.
 * 위에 그려지는 박스/스켈레톤/라벨은 그대로 또렷이 보인다. */
static void privacy_mosaic(FrameView *f)
{
    const int B = 16;                       /* 블록 크기 (px) */
    for (int by = 0; by < f->h; by += B) {
        int bh = by + B > f->h ? f->h - by : B;
        for (int bx = 0; bx < f->w; bx += B) {
            int bw = bx + B > f->w ? f->w - bx : B;
            /* 블록 평균 밝기 */
            unsigned long sum = 0;
            for (int y = by; y < by + bh; y++) {
                const uint8_t *p = f->data + (size_t)y * f->stride + (size_t)bx * 3;
                for (int x = 0; x < bw; x++, p += 3)
                    sum += (unsigned)(p[0] * 29 + p[1] * 150 + p[2] * 77) >> 8;
            }
            int y8 = (int)(sum / (unsigned long)(bw * bh));
            /* 의사 열화상 팔레트 (BGR) */
            int r = y8 < 128 ? y8 * 2 : 255;
            int g = y8 < 128 ? 0 : (y8 - 128) * 2;
            int b = y8 < 64 ? 128 + y8 * 2 : (y8 < 160 ? 255 - (y8 - 64) * 2 : 0);
            for (int y = by; y < by + bh; y++) {
                uint8_t *p = f->data + (size_t)y * f->stride + (size_t)bx * 3;
                for (int x = 0; x < bw; x++, p += 3) {
                    p[0] = (uint8_t)b;
                    p[1] = (uint8_t)g;
                    p[2] = (uint8_t)r;
                }
            }
        }
    }
}

/* 상태 머신 → 라우터 체류 시간 조회 어댑터 */
static double dwell_adapter(void *ctx, int track_id, double now)
{
    return router_table_dwell_seconds((const RoiRouter *)ctx, track_id, now);
}

/* 영업시간 판정 (자정 기준 분). open > close면 심야 영업(예: 22시~새벽 2시). */
static bool within_hours(const PipelineConfig *cfg)
{
    if (!cfg->hours_enabled) return true;
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    int m = tmv.tm_hour * 60 + tmv.tm_min;
    if (cfg->open_min <= cfg->close_min)
        return m >= cfg->open_min && m < cfg->close_min;
    return m >= cfg->open_min || m < cfg->close_min;
}

/* ── 자동 캘리브레이션(세팅) 세션 (8/3 회의 ① 영점 맞추기) ──
 * 점주/엔지니어가 매장을 비운 상태로 점주 페이지 [세팅 시작하기]를 누르면
 * N분(기본 60분) 동안 '사람'으로 잡히는 검출을 셀 단위로 집계한다.
 * 빈 매장에서 잡히는 사람은 전부 오탐원(거울·포스터·TV·창밖)이므로,
 * 반복 검출된 셀을 인식 제외 마스크로 만들어 설정에 병합·저장한다. */
#define CALIB_MIN_HITS 12          /* 이 횟수 이상 잡힌 셀만 마스킹 (우연 방지) */

typedef struct {
    bool active;
    double t0, dur_sec;
    uint16_t counts[MASK_CELLS];   /* 셀별 오탐 검출 횟수 */
} CalibSession;

/* 세션 종료: 집계 → 마스크 생성 → 설정에 병합 저장 */
static void calib_finish(CalibSession *cs, SettingsStore *settings, double now)
{
    uint8_t new_mask[MASK_BYTES] = { 0 };
    int n_cells = 0;
    for (int i = 0; i < MASK_CELLS; i++) {
        if (cs->counts[i] >= CALIB_MIN_HITS) {
            mask_set_cell(new_mask, i);
            n_cells++;
        }
    }
    LOGI("캘리브", "세팅 종료 (%.0f분 진행) — 오탐 셀 %d개 발견 → 마스크 병합",
         (now - cs->t0) / 60.0, n_cells);
    if (n_cells > 0) settings_set_mask(settings, new_mask, true);
    cs->active = false;
}

/* ── 이벤트 워커 스레드: 큐 소비 → 로그 + 점주 페이지 + 서버 전송 ── */
typedef struct {
    EventQueue *q;
    WsSender *sender;
    AdminServer *admin;
} WorkerCtx;

static void *event_worker_main(void *arg)
{
    WorkerCtx *ctx = arg;
    apply_thread_role(THREAD_ROLE_IO);
    Event ev;
    for (;;) {
        if (evq_pop(ctx->q, &ev, 500)) {
            /* TODO: 실제 오디오/알림 연동 지점 (예: 사운드 재생) */
            LogLevel lv = (ev.kind == EV_FALL_ALERT || ev.kind == EV_HW_FAULT)
                          ? LOGL_WARN : LOGL_INFO;
            log_msg(lv, "이벤트", "%s | %s%s%s", event_kind_str(ev.kind),
                    ev.message, ev.journey[0] ? " | 동선: " : "", ev.journey);
            admin_push_event(ctx->admin, &ev);
            /* 파일럿 데이터 축적: 이벤트 1건 = JSONL 1줄 (사후 분석용) */
            datastore_event(event_kind_str(ev.kind), ev.track_id,
                            ev.message, ev.journey);
            if (ctx->sender) ws_send_event(ctx->sender, &ev);
        } else if (evq_is_closed(ctx->q)) {
            return NULL;    /* 종료 신호 + 큐 비움 완료 */
        }
    }
}

/* 비인물(전역) 이벤트 발행 헬퍼 — 하드웨어/반려동물/외부음식 등 */
static void emit_global_event(EventQueue *q, EventKind kind,
                              const char *msg, double now)
{
    Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = kind;
    ev.track_id = -1;
    ev.ts = now;
    snprintf(ev.message, sizeof(ev.message), "%s", msg);
    evq_push(q, &ev);
}

/* 하드웨어 진단 이벤트 발행 (룰 토글이 꺼져 있으면 로그만) */
static void emit_hw_event(EventQueue *q, const PipelineConfig *cfg,
                          const char *msg, double now)
{
    if (!cfg->rule_hw_alert) {
        LOGW("카메라", "%s (알림 룰 꺼짐 — 로그만 기록)", msg);
        return;
    }
    emit_global_event(q, EV_HW_FAULT, msg, now);
}

/* ── 확장 감지 (반려동물/외부음식) 결과 → 이벤트 ──
 * 같은 종류 알림의 반복을 막기 위해 종류별 쿨다운을 둔다. */
#define PET_COOLDOWN_SEC 120.0
#define FOOD_COOLDOWN_SEC 300.0

typedef struct {
    double last_pet_ts, last_food_ts;
} ObjRuleState;

static void process_obj_detections(const PipelineConfig *cfg, EventQueue *q,
                                   ObjRuleState *st, const ObjDetection *objs,
                                   int n_objs, int frame_w, int frame_h,
                                   double now)
{
    (void)frame_w;
    (void)frame_h;
    for (int i = 0; i < n_objs; i++) {
        const ObjDetection *o = &objs[i];
        /* 구역 판정 기준점: 객체 바닥 중앙 (사람 발밑 판정과 동일) */
        float fx = (o->bbox[0] + o->bbox[2]) / 2;
        float fy = o->bbox[3];
        bool in_table = zone_contains(&cfg->table_zone, fx, fy);

        if (o->category == OBJ_PET && cfg->rule_pet) {
            LOGD("판정", "반려동물 감지: %s conf %.2f%s",
                 objdet_class_str(o->coco_class), (double)o->conf,
                 in_table ? " (테이블 구역 안)" : "");
            if (now - st->last_pet_ts >= PET_COOLDOWN_SEC) {
                st->last_pet_ts = now;
                char msg[192];
                snprintf(msg, sizeof(msg),
                         "반려동물(%s) 감지%s — 매장 정책 확인 요망",
                         objdet_class_str(o->coco_class),
                         in_table ? " (테이블/좌석 구역)" : "");
                emit_global_event(q, EV_PET, msg, now);
            }
        } else if (o->category == OBJ_FOOD && cfg->rule_outside_food) {
            /* 외부 음식은 테이블 구역에 놓였을 때만 의미 (계산대 오탐 방지) */
            LOGD("판정", "음식류 감지: %s conf %.2f%s",
                 objdet_class_str(o->coco_class), (double)o->conf,
                 in_table ? " (테이블 구역 안)" : " (구역 밖 — 무시)");
            if (in_table && now - st->last_food_ts >= FOOD_COOLDOWN_SEC) {
                st->last_food_ts = now;
                char msg[192];
                snprintf(msg, sizeof(msg),
                         "테이블에서 외부 음식 의심 품목(%s) 감지 — 확인 요망",
                         objdet_class_str(o->coco_class));
                emit_global_event(q, EV_OUTSIDE_FOOD, msg, now);
            }
        }
    }
}

int main(int argc, char **argv)
{
    PipelineConfig cfg = CFG_DEFAULT;
    const char *source = cfg.camera_source;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-window") == 0)
            cfg.show_window = false;   /* 운영/헤드리스 실행 (표시 비용 절약) */
        else if (strncmp(argv[i], "--ws=", 5) == 0)
            cfg.ws_url = argv[i] + 5;  /* 예: --ws=ws://192.168.0.10:8080/events */
        else if (strncmp(argv[i], "--admin-port=", 13) == 0)
            cfg.admin_port = atoi(argv[i] + 13);   /* 0이면 점주 페이지 끔 */
        else if (strncmp(argv[i], "--log=", 6) == 0)
            cfg.log_path = argv[i] + 6;
        else if (strncmp(argv[i], "--data-dir=", 11) == 0)
            cfg.data_dir = argv[i] + 11;   /* 빈 값(--data-dir=)이면 축적 끔 */
        else if (strcmp(argv[i], "--privacy") == 0)
            cfg.privacy_view = true;       /* 열화상풍 비식별 표시 (현장 데모용) */
        else
            source = argv[i];
    }

    log_init(LOGL_INFO, cfg.log_path);
    datastore_init(cfg.data_dir);          /* 이벤트/여정 JSONL 축적 */
    apply_cpu_affinity(&cfg);
    apply_thread_role(THREAD_ROLE_INFER);          /* 메인 = 추론(소비자) */

    /* ── 점주 설정: 저장 파일 로드 → 파이프라인 설정에 반영 ── */
    SettingsStore *settings = settings_create(&cfg, cfg.settings_path);
    StoreSettings snap;
    settings_get(settings, &snap);
    settings_apply_to_cfg(&snap, &cfg);
    long settings_version = snap.version;

    /* 인식 제외 마스크는 소비자 스레드 로컬 사본으로 판정 (락 없는 핫패스) */
    uint8_t detect_mask[MASK_BYTES];
    memcpy(detect_mask, snap.detect_mask, MASK_BYTES);
    log_set_level(cfg.verbose_log ? LOGL_DEBUG : LOGL_INFO);

    /* ── 파이프라인 구성 ───────────────────────────────── */
    YoloPose *yolo = yolo_create(&cfg);            /* 1단계 (3단계와 공유) */
    if (!yolo) {
        LOGE("메인", "모델 로드 실패 — yolo11n-pose.onnx 확인");
        return 1;
    }
    Tracker *tracker = tracker_create(&cfg);
    Reid *reid = cfg.reid_enabled ? reid_create(&cfg) : NULL;
    RoiRouter *router = router_create(&cfg);       /* 2단계 */
    Analyzer *analyzer = analyzer_create(&cfg, yolo); /* 3단계 */
    EventQueue *evq = evq_create();
    Journey *journey = journey_create();           /* 설명 가능한 동선 기록 */
    StateMachine *fsm = fsm_create(&cfg, evq, journey); /* 4단계 */

    /* 확장 모델 (파일이 없으면 NULL — 해당 룰만 비활성, 파이프라인은 정상) */
    ObjDet *objdet = objdet_create(&cfg);          /* 반려동물/외부음식/잔여물 (COCO) */
    AgeEst *age = age_create(&cfg);                /* 노키즈존 나이 추정 */

    /* 제로샷 이상행동 분석기 — 폭력/파손/낙상/배회/무단조작 (모델 불필요) */
    Behavior *behavior = behavior_create(&cfg, evq, journey);

    WsSender *sender = ws_create(&cfg);            /* 서버 전송 */
    ws_start(sender);

    AdminServer *admin = admin_create(&cfg, settings);  /* 점주 페이지 */
    admin_start(admin);

    WorkerCtx worker_ctx = { evq, sender, admin };
    pthread_t worker;
    bool has_worker = pthread_create(&worker, NULL, event_worker_main, &worker_ctx) == 0;

    ResourceMonitor *monitor = NULL;
    if (cfg.monitor_enabled) {                     /* 테스트 수칙: 리소스 모니터 상시 가동 */
        monitor = monitor_create(&cfg);
        monitor_start(monitor);
    }

    /* ── 캡처 스레드 (생산자) 기동 ─────────────────────── */
    FrameBus *bus = framebus_create();
    CaptureCtx cap_ctx = { .cfg = &cfg, .source = source, .bus = bus };
    atomic_init(&cap_ctx.stop, false);
    atomic_init(&cap_ctx.eof, false);
    pthread_t cap_thread;
    if (pthread_create(&cap_thread, NULL, capture_main, &cap_ctx) != 0) {
        LOGE("메인", "캡처 스레드 생성 실패");
        return 1;
    }

    LOGI("메인", "파이프라인 시작 (종료: 영상 창 또는 터미널에서 q)");

    /* 터미널 q 종료는 stdin이 진짜 tty일 때만 활성.
     * 파이프/리다이렉트 환경에서는 raw 모드 전환 자체가 무의미하다. */
    bool stdin_is_console = isatty(STDIN_FILENO) && enable_raw_stdin();

    double start_ts = mono_now();
    long frame_idx = 0;
    double infer_t0 = mono_now(), infer_fps = 0;
    int infer_n = 0;
    double last_status_ts = 0;
    double last_heartbeat_ts = 0;          /* 첫 하트비트는 기동 직후 상태 갱신 때 */

    TrackedPerson people[MAX_PEOPLE];
    int n_people = 0;                              /* 스킵 프레임에서는 직전 결과 재사용 */
    ZoneFlags zone_flags[MAX_PEOPLE];
    PrecisionTarget targets[MAX_TARGETS];
    int n_targets = 0;
    PrecisionResult precision[MAX_TARGETS];
    Detection dets[MAX_DETECTIONS];
    int purchase_ids[16], purchase_items[16];
    ObjDetection objs[MAX_OBJ_DETECTIONS];
    ObjRuleState obj_state = { -1e9, -1e9 };
    int obj_tick = 0;                              /* 확장 감지 주기 카운터 */

    /* 스마트 청소 알림: 테이블이 비워진 직후 1회만 잔여물 검사 */
    int prev_table_count = 0;
    bool clean_pending = false;
    double clean_due_ts = 0;

    CamHealth camhealth;
    camhealth_init(&camhealth, mono_now());
    char hw_msg[192];

    /* 운영 모드 상태 (발열 보호): 절전 / 영업시간 외 */
    CalibSession calib;
    memset(&calib, 0, sizeof(calib));
    double last_person_ts = mono_now();
    bool eco_active = false;
    bool hours_off = false;

    StageProfiler prof;
    prof_init(&prof, 10.0);

    for (;;) {
        /* 터미널에서 q 입력으로 종료 (영상 창 포커스 없이도 동작) */
        if (stdin_is_console && stdin_has_key()) {
            int ch = read_key();
            if (ch == 'q' || ch == 'Q') break;
        }

        double now = mono_now();

        /* ── 점주 설정 변경 폴링 (락 구간 = 구조체 복사 1회) ── */
        settings_get(settings, &snap);
        if (snap.version != settings_version) {
            bool was_verbose = cfg.verbose_log;
            settings_apply_to_cfg(&snap, &cfg);
            memcpy(detect_mask, snap.detect_mask, MASK_BYTES);
            if (cfg.verbose_log != was_verbose)
                log_set_level(cfg.verbose_log ? LOGL_DEBUG : LOGL_INFO);
            LOGI("설정", "점주 설정 v%ld 적용 (이전 v%ld)", snap.version,
                 settings_version);
            settings_version = snap.version;
        }

        /* ── POS 결제 웹훅 처리 (점주 페이지 → 메일박스 → 여기서만 반영) ── */
        int n_purchase = admin_take_purchases(admin, purchase_ids, purchase_items, 16);
        for (int i = 0; i < n_purchase; i++) {
            fsm_mark_purchased(fsm, purchase_ids[i], purchase_items[i]);
            journey_note(journey, purchase_ids[i], STEP_PURCHASE, now);
            LOGI("POS", "ID %d 결제 확인 (%d잔) — 미구매 판정 해제",
                 purchase_ids[i], purchase_items[i]);
        }

        /* ── 캘리브레이션(세팅) 요청 처리 ── */
        int calib_min = 0;
        CalibRequest creq = admin_take_calib(admin, &calib_min);
        if (creq == CALIB_REQ_START) {
            memset(&calib, 0, sizeof(calib));
            calib.active = true;
            calib.t0 = now;
            calib.dur_sec = calib_min * 60.0;
            LOGI("캘리브", "세팅 시작 — %d분간 매장을 비워주세요. "
                 "이 동안 잡히는 인식은 전부 오탐으로 학습됩니다", calib_min);
        } else if (creq == CALIB_REQ_STOP && calib.active) {
            calib_finish(&calib, settings, now);
        } else if (creq == CALIB_REQ_CLEAR) {
            uint8_t zero[MASK_BYTES] = { 0 };
            settings_set_mask(settings, zero, false);
            LOGI("캘리브", "인식 범위 마스크 전체 해제");
        }
        if (calib.active && now - calib.t0 >= calib.dur_sec)
            calib_finish(&calib, settings, now);

        /* ── 영업시간 모드 (발열 보호 1단계: 캡처 저속 + 추론 중단) ── */
        bool in_hours = within_hours(&cfg);
        if (hours_off == in_hours) {              /* 상태 전환 감지 */
            hours_off = !in_hours;
            atomic_store(&cap_ctx.low_power, hours_off);
            LOGI("모드", "%s", hours_off
                 ? "영업시간 외 — 저전력 전환 (캡처 2초/1프레임, 추론 중단)"
                 : "영업시간 시작 — 정상 가동 복귀");
        }

        /* ── 프레임 수신 (없으면 프리즈 감시 후 대기) ── */
        FrameView frame;
        uint64_t seq;
        if (!framebus_acquire(bus, &frame, &seq)) {
            if (camhealth_check_freeze(&camhealth, framebus_published_seq(bus),
                                       now, cfg.freeze_alert_sec,
                                       hw_msg, sizeof(hw_msg)))
                emit_hw_event(evq, &cfg, hw_msg, now);
            if (atomic_load(&cap_ctx.eof)) {
                LOGI("메인", "입력 종료(EOF) — 파이프라인 종료");
                break;
            }
            usleep(2 * 1000);                      /* 다음 프레임 대기 */
            continue;
        }
        frame_idx++;

        /* ── 영업시간 외: 자가 진단만 유지하고 분석은 전부 생략 ── */
        if (hours_off) {
            if (camhealth_check_frame(&camhealth, &frame, now,
                                      hw_msg, sizeof(hw_msg)))
                emit_hw_event(evq, &cfg, hw_msg, now);
            n_people = 0;
            n_targets = 0;
            usleep(200 * 1000);                    /* 소비자도 저속 순환 */
            goto status_update;                    /* 상태 갱신은 계속 */
        }

        /* ── 절전 모드 (발열 보호 2단계): 무인이면 추론 주기 4배 ──
         * 사람이 다시 잡히면 다음 추론에서 즉시 해제된다. */
        {
            bool want_eco = cfg.eco_mode && !calib.active &&
                            now - last_person_ts > cfg.eco_idle_sec;
            if (want_eco != eco_active) {
                eco_active = want_eco;
                LOGI("모드", "%s", eco_active
                     ? "무인 상태 지속 — 절전 전환 (추론 주기 4배, 발열 감소)"
                     : "활동 감지 — 절전 해제, 정상 주기 복귀");
            }
        }
        int every_n = cfg.detect_every_n * (eco_active ? 4 : 1);

        /* ── 프레임 샘플링: N프레임마다 1회만 추론 (CPU 예산 보호) ── */
        if (frame_idx % every_n == 0) {
            now = mono_now();

            /* 카메라 자가 진단 (성긴 샘플링 — 백화/블랙아웃/초점 상실) */
            if (camhealth_check_frame(&camhealth, &frame, now,
                                      hw_msg, sizeof(hw_msg)))
                emit_hw_event(evq, &cfg, hw_msg, now);

            /* ── 캘리브레이션 모드: 검출만 돌려 오탐 셀 집계 ──
             * (추적/재식별/룰 판정은 전부 생략 — 빈 매장 전제) */
            if (calib.active) {
                int n_raw = yolo_infer(yolo, &frame, 0, 0, frame.w, frame.h,
                                       cfg.yolo_imgsz, cfg.yolo_conf,
                                       dets, MAX_DETECTIONS);
                for (int j = 0; j < n_raw; j++) {
                    float nx = (dets[j].bbox[0] + dets[j].bbox[2]) / 2 / (float)frame.w;
                    float ny = dets[j].bbox[3] / (float)frame.h;   /* 발밑 기준 */
                    int cell = mask_cell_index(nx, ny);
                    if (calib.counts[cell] < 65535) calib.counts[cell]++;
                    LOGD("캘리브", "오탐 후보: 셀(%d,%d) conf %.2f (%d회째)",
                         cell % MASK_W, cell / MASK_W, (double)dets[j].conf,
                         calib.counts[cell]);
                }
                n_people = 0;
                n_targets = 0;
                goto status_update;
            }

            /* 1단계: 전역 탐지 + 추적 (저신뢰 검출도 트래커 2차 매칭에 필요) */
            double t0 = mono_now();
            int n_dets = yolo_infer(yolo, &frame, 0, 0, frame.w, frame.h,
                                    cfg.yolo_imgsz, cfg.track_low_conf,
                                    dets, MAX_DETECTIONS);

            /* 인식 범위 마스크: 제외 구역(거울·포스터·창밖)의 검출을
             * 추적에 들어가기 전에 버린다 — 오탐이 트랙 ID를 받지 못하게 */
            int kept = 0, mask_dropped = 0;
            for (int j = 0; j < n_dets; j++) {
                float nx = (dets[j].bbox[0] + dets[j].bbox[2]) / 2 / (float)frame.w;
                float ny = dets[j].bbox[3] / (float)frame.h;
                if (mask_test(detect_mask, nx, ny)) mask_dropped++;
                else dets[kept++] = dets[j];
            }
            n_dets = kept;
            if (mask_dropped > 0)
                LOGD("판정", "인식 제외 구역 검출 %d건 무시 (마스크)", mask_dropped);

            n_people = tracker_update(tracker, dets, n_dets, people, MAX_PEOPLE);
            if (n_people > 0) last_person_ts = now;   /* 절전 판정 기준 */
            double t1 = mono_now();
            prof_add(&prof, "yolo+track", t1 - t0, n_people);

            /* 1.5단계: 옷 색상·채도 + 신체 비율 시그니처로 안정 ID 부여 */
            if (reid) reid_assign(reid, &frame, people, n_people, now);
            double t2 = mono_now();
            prof_add(&prof, "reid", t2 - t1, 1);

            /* 2단계: 트리거 대상 선별 (크롭은 사각형만 — zero-copy) */
            n_targets = router_route(router, &frame, people, n_people, now,
                                     targets, MAX_TARGETS, zone_flags);
            double t3 = mono_now();
            prof_add(&prof, "roi", t3 - t2, 1);

            /* 동선 기록: 입장/키오스크/착석/이탈 전환을 남긴다 (증거 로그) */
            int table_count = 0;
            for (int i = 0; i < n_people; i++) {
                int tid = people[i].track_id;
                if (zone_flags[i].in_table) table_count++;
                if (zone_flags[i].at_kiosk)
                    journey_note(journey, tid, STEP_KIOSK, now);
                else if (zone_flags[i].in_table)
                    journey_note(journey, tid, STEP_SIT, now);
                else if (journey_last_step(journey, tid) == STEP_SIT)
                    journey_note(journey, tid, STEP_LEAVE_TABLE, now);
                else
                    journey_touch(journey, tid, now);
            }

            /* 스마트 청소 알림 (8/6 기획 '테이블 회전 감지'):
             * 테이블 구역 인원이 0이 되는 전환 = 퇴석 → 잠시 뒤 잔여물 검사 */
            if (cfg.rule_clean && prev_table_count > 0 && table_count == 0) {
                clean_pending = true;
                clean_due_ts = now + 5.0;          /* 완전히 벗어난 뒤 촬영 */
                LOGD("판정", "테이블 비워짐 감지 — 5초 후 잔여물 검사 예약");
            }
            prev_table_count = table_count;

            /* 3단계: 선별된 크롭만 고해상도 재추론 */
            for (int i = 0; i < n_targets; i++)
                analyzer_analyze(analyzer, &frame, &targets[i], &precision[i]);
            double t4 = mono_now();
            prof_add(&prof, "crop-pose", t4 - t3, n_targets);

            /* 4단계: 결정론적 상태 머신 → 이벤트 큐 */
            fsm_update(fsm, people, n_people, precision, n_targets, zone_flags,
                       dwell_adapter, router, now);

            /* 4.5단계: 제로샷 이상행동 분석 (관절 이동 벡터 기반)
             * 폭력/기물파손/낙상급강하/배회/무단조작 + 키오스크 추천 컨텍스트 */
            behavior_update(behavior, people, n_people, zone_flags, now);

            journey_gc(journey, now, cfg.journey_ttl_sec);

            /* ── 확장 감지: 반려동물/외부음식/잔여물 (사람 추론 K회당 1회) ──
             * COCO 모델 320px — 주기와 민감도는 점주 페이지에서 조정 */
            bool clean_check_due = clean_pending && now >= clean_due_ts;
            if (objdet &&
                (cfg.rule_pet || cfg.rule_outside_food || clean_check_due) &&
                ++obj_tick >= cfg.obj_every_k) {
                obj_tick = 0;
                double to0 = mono_now();
                int n_objs = objdet_infer(objdet, &frame, cfg.obj_conf,
                                          objs, MAX_OBJ_DETECTIONS);
                prof_add(&prof, "objdet", mono_now() - to0, n_objs);
                process_obj_detections(&cfg, evq, &obj_state, objs, n_objs,
                                       frame.w, frame.h, now);

                /* 퇴석 직후 1회: 테이블 구역에 잔여물(컵/병/그릇/음식) 확인 */
                if (clean_check_due) {
                    clean_pending = false;
                    int leftovers = 0;
                    const char *first_item = NULL;
                    for (int i = 0; i < n_objs; i++) {
                        if (objs[i].category != OBJ_TABLEWARE &&
                            objs[i].category != OBJ_FOOD) continue;
                        float fx = (objs[i].bbox[0] + objs[i].bbox[2]) / 2;
                        float fy = objs[i].bbox[3];
                        if (!zone_contains(&cfg.table_zone, fx, fy)) continue;
                        leftovers++;
                        if (!first_item)
                            first_item = objdet_class_str(objs[i].coco_class);
                    }
                    if (leftovers > 0) {
                        char msg[192];
                        snprintf(msg, sizeof(msg),
                                 "퇴석한 테이블에 잔여물 %d개 감지(%s 등) — 정리 방문 필요",
                                 leftovers, first_item);
                        emit_global_event(evq, EV_CLEAN_NEEDED, msg, now);
                    } else {
                        LOGD("판정", "퇴석 후 테이블 잔여물 없음 — 청소 알림 생략");
                    }
                }
            }

            /* ── 노키즈존: 키오스크 근접자만, 트랙당 결론이 날 때까지만 ──
             * 96px 초경량 모델 + 사이클당 2명 예산 — 상시 부하 없음 */
            if (age && cfg.rule_no_kids) {
                int budget = 2;
                for (int i = 0; i < n_people && budget > 0; i++) {
                    if (!zone_flags[i].at_kiosk) continue;
                    if (!fsm_age_needs_vote(fsm, people[i].track_id)) continue;
                    int fx1, fy1, fx2, fy2;
                    if (!age_face_box(&people[i], cfg.kpt_valid_conf,
                                      frame.w, frame.h, &fx1, &fy1, &fx2, &fy2))
                        continue;
                    int est = age_estimate(age, &frame, fx1, fy1, fx2, fy2);
                    if (est >= 0) fsm_note_age(fsm, people[i].track_id, est);
                    budget--;
                }
            }

            prof_maybe_report(&prof, now);

            /* 점주 페이지 실시간 오버레이용 스냅샷 */
            admin_update_people(admin, people, n_people, targets, n_targets,
                                frame.w, frame.h);

            infer_n++;
            double elapsed = mono_now() - infer_t0;
            if (elapsed >= 2.0) {
                infer_fps = infer_n / elapsed;
                infer_t0 = mono_now();
                infer_n = 0;
            }
        }

        /* ── 점주 페이지 상태 갱신 (1초 주기) ── */
status_update:
        if (now - last_status_ts >= 1.0) {
            last_status_ts = now;
            AdminStatus st = { 0 };
            st.uptime_sec = now - start_ts;
            st.infer_fps = infer_fps;
            st.n_people = n_people;
            if (monitor) monitor_last(monitor, &st.cpu_pct, &st.rss_mb);
            st.cam_state = (int)camhealth.state;
            st.cam_state_str = camhealth_str(camhealth.state);
            /* 운영 모드: 점주가 현재 시스템이 뭘 하고 있는지 한눈에 알 수 있게 */
            st.mode = calib.active ? "캘리브레이션"
                    : hours_off    ? "영업시간외"
                    : eco_active   ? "절전"
                    :                "정상";
            st.calib_pct = calib.active
                ? (int)((now - calib.t0) * 100.0 /
                        (calib.dur_sec > 0 ? calib.dur_sec : 1))
                : -1;
            st.frames_dropped = framebus_dropped(bus);
            st.ws_enabled = cfg.ws_url && cfg.ws_url[0];
            st.settings_version = settings_version;
            admin_update_status(admin, &st);

            /* 생존 신호: N초마다 상태 요약을 서버로 (수신 측이 결손 감지) */
            if (cfg.heartbeat_sec > 0 &&
                now - last_heartbeat_ts >= cfg.heartbeat_sec) {
                last_heartbeat_ts = now;
                ws_send_heartbeat(sender, st.uptime_sec, st.cpu_pct,
                                  st.rss_mb, n_people, infer_fps);
            }
        }

        /* ── 디버그 시각화 (표시 자체도 CPU 비용 — 주기 조절 가능) ──
         * frame은 소비자 소유 슬롯이라 오버레이를 그려도 안전하다. */
        if (cfg.show_window && !hours_off && frame_idx % cfg.display_every_n == 0) {
            if (cfg.privacy_view) privacy_mosaic(&frame);
            draw_debug(&cfg, &frame, people, n_people, targets, n_targets,
                       router, mono_now(), infer_fps);
            cvs_show("Hybrid Cascade Pipeline (C)", frame.data,
                     frame.w, frame.h, frame.stride);
            int key = cvs_waitkey(1);
            if ((key & 0xFF) == 'q') break;
        }
    }

    /* ── 정리 ──────────────────────────────────────────── */
    atomic_store(&cap_ctx.stop, true);
    pthread_join(cap_thread, NULL);
    framebus_destroy(bus);
    cvs_destroy_all_windows();
    admin_stop(admin);
    evq_close(evq);
    if (has_worker) pthread_join(worker, NULL);
    ws_stop(sender);
    ws_destroy(sender);
    if (monitor) {
        monitor_stop(monitor);
        LOGI("리소스", "피크: sys CPU %.1f%% / RSS %.0fMB",
             monitor_peak_cpu(monitor), monitor_peak_rss_mb(monitor));
        monitor_destroy(monitor);
    }
    admin_destroy(admin);
    behavior_destroy(behavior);
    age_destroy(age);
    objdet_destroy(objdet);
    fsm_destroy(fsm);
    journey_gc(journey, mono_now(), -1.0); /* 잔여 트랙도 여정 레코드로 마감 */
    journey_destroy(journey);
    evq_destroy(evq);
    analyzer_destroy(analyzer);
    router_destroy(router);
    if (reid) reid_destroy(reid);
    tracker_destroy(tracker);
    yolo_destroy(yolo);
    settings_destroy(settings);
    LOGI("메인", "파이프라인 종료");
    datastore_shutdown();
    log_shutdown();
    return 0;
}
