/*
 * 하이브리드 캐스케이드 파이프라인 전역 설정 (config.py 1:1 포팅)
 * - 구역(Zone) 정의, 트리거 임계값, 모델 파라미터를 한곳에서 관리
 * - 파이썬 dataclass 대신 정적 초기화 구조체 사용
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <wchar.h>     /* C에서 wchar_t는 키워드가 아님 (ORT Windows 경로용) */

/* 매장 내 관심 구역 (프레임 좌표 기준 단순 사각형) */
typedef struct {
    const char *name;
    int x1, y1, x2, y2;
} Zone;

static inline bool zone_contains(const Zone *z, float cx, float cy) {
    return z->x1 <= cx && cx <= z->x2 && z->y1 <= cy && cy <= z->y2;
}

typedef enum {
    KIOSK_TRIGGER_NEAR,   /* 근접(키오스크 부착 카메라): 얼굴 크기 비율 */
    KIOSK_TRIGGER_ZONE,   /* 화면 구역(천장/벽 카메라) */
} KioskTriggerMode;

typedef struct {
    /* ── 카메라 ────────────────────────────────────────── */
    const char *camera_source;    /* "0" = 웹캠, 또는 RTSP URL */
    int frame_width;
    int frame_height;

    /* ── 프레임 샘플링 (리소스 핵심 제약) ─────────────── */
    /* 매 프레임 추론하면 4200U급 CPU를 감당할 수 없다.
     * 30fps 입력 기준 5 → 초당 6회 추론. */
    int detect_every_n;

    /* ── 리소스 가드 ──────────────────────────────────── */
    int max_threads;              /* ORT intra-op 스레드 상한 (1~2코어 정책) */
    unsigned long long cpu_affinity_mask;  /* 0 = 미지정, 예: 0xC = 코어 2,3 */
    bool monitor_enabled;
    double monitor_interval_sec;
    double cpu_alert_pct;

    /* ── 1단계: 전역 탐지 (YOLO11n-pose ONNX + ByteTrack식 IoU 추적) ── */
    const wchar_t *yolo_model;    /* ONNX 경로 (ORT Windows는 wchar 경로) */
    int yolo_imgsz;               /* 저해상도 추론 (핫패스 경량화) */
    float yolo_conf;
    float yolo_nms_iou;
    /* ByteTrack식 2단계 매칭 파라미터 */
    float track_low_conf;         /* 저신뢰 검출 하한 (2차 매칭용) */
    float track_match_iou;        /* 트랙-검출 매칭 최소 IoU */
    int   track_max_misses;       /* 이 횟수(추론 프레임) 이상 소실 시 트랙 폐기 */
    int   track_min_hits;         /* 신규 트랙 확정에 필요한 연속 매칭 수 */

    /* ── 비율 기반 태깅/재식별 ────────────────────────── */
    bool  reid_enabled;
    float reid_match_threshold;   /* 시그니처 거리 임계값 (작을수록 엄격) */
    double reid_gallery_ttl_sec;  /* 사라진 트랙 시그니처 보관 시간 */

    /* ── 2단계: 동적 ROI 트리거 조건 ──────────────────── */
    KioskTriggerMode kiosk_trigger_mode;
    float kiosk_face_w_frac;      /* 머리 폭 ≥ 화면 가로 비율 → 키오스크 사용 중 */
    Zone kiosk_zone;              /* zone 모드에서만 사용 */
    Zone table_zone;
    double table_dwell_trigger_sec;
    float fall_aspect_ratio;      /* BBox 가로/세로 비율 (쓰러짐 징후) */
    float crop_padding;           /* 크롭 시 BBox 여유 비율 */
    int   max_precision_targets;  /* 프레임당 정밀 분석 최대 인원 */

    /* ── 3단계: 지역 정밀 분석 (크롭 YOLO-pose 재추론) ──
     * MediaPipe는 C API가 없어 동일 모델을 크롭에 고해상도로 재추론한다.
     * 크롭 기준 입력이 커지므로 원거리 전역 추론보다 키포인트가 정밀해진다. */
    int   crop_imgsz;             /* 크롭 재추론 letterbox 크기 (32 배수) */
    float crop_conf;              /* 크롭 내 사람 검출 최소 신뢰도 */
    bool  face_enabled;
    float face_min_crop_frac;     /* 크롭 세로/화면 세로 비율이 이보다 작으면(원거리) 얼굴 분석 생략 */
    float kpt_valid_conf;         /* 키포인트 유효 판정 신뢰도 하한 */

    /* ── 4단계: 상태 머신 ─────────────────────────────── */
    double announce_dwell_sec;    /* 테이블 N초 + 미구매 → 안내방송 */
    double announce_cooldown_sec; /* 동일 ID 재방송 쿨다운 */
    double stale_track_ttl_sec;   /* 트랙 소실 후 상태 유지 시간 */

    /* ── 서버 전송 (WebSocket) ────────────────────────── */
    const char *ws_url;           /* 비우면 로컬 로그만. 예: "ws://192.168.0.10:8080/events" */
    double ws_reconnect_min_sec;
    double ws_reconnect_max_sec;

    /* ── 기타 ─────────────────────────────────────────── */
    bool show_window;
    int  display_every_n;         /* 화면 갱신 주기 (imshow 자체가 CPU를 상당히 먹는다) */
} PipelineConfig;

/* 전역 기본 설정 — config.py의 CFG = PipelineConfig() 에 해당 */
static const PipelineConfig CFG_DEFAULT = {
    .camera_source = "0",
    .frame_width = 1280,
    .frame_height = 720,

    .detect_every_n = 5,

    .max_threads = 2,
    .cpu_affinity_mask = 0,
    .monitor_enabled = true,
    .monitor_interval_sec = 5.0,
    .cpu_alert_pct = 85.0,

    .yolo_model = L"yolo11n-pose.onnx",
    .yolo_imgsz = 416,
    .yolo_conf = 0.35f,
    .yolo_nms_iou = 0.65f,
    .track_low_conf = 0.10f,
    .track_match_iou = 0.20f,
    .track_max_misses = 30,
    .track_min_hits = 2,

    .reid_enabled = true,
    .reid_match_threshold = 0.22f,
    .reid_gallery_ttl_sec = 60.0,

    .kiosk_trigger_mode = KIOSK_TRIGGER_NEAR,
    .kiosk_face_w_frac = 0.13f,
    .kiosk_zone = { "kiosk", 900, 100, 1280, 600 },
    .table_zone = { "table", 0, 300, 700, 720 },
    .table_dwell_trigger_sec = 180.0,
    .fall_aspect_ratio = 1.4f,
    .crop_padding = 0.15f,
    .max_precision_targets = 3,

    .crop_imgsz = 256,
    .crop_conf = 0.50f,
    .face_enabled = true,
    .face_min_crop_frac = 0.35f,
    .kpt_valid_conf = 0.30f,

    .announce_dwell_sec = 300.0,
    .announce_cooldown_sec = 120.0,
    .stale_track_ttl_sec = 5.0,

    .ws_url = "",
    .ws_reconnect_min_sec = 1.0,
    .ws_reconnect_max_sec = 30.0,

    .show_window = true,
    .display_every_n = 2,
};

#endif /* CONFIG_H */
