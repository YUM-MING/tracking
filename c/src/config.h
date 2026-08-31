/*
 * 하이브리드 캐스케이드 파이프라인 전역 설정 (config.py 1:1 포팅)
 * - 구역(Zone) 정의, 트리거 임계값, 모델 파라미터를 한곳에서 관리
 * - 파이썬 dataclass 대신 정적 초기화 구조체 사용
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stddef.h>

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
    const char *yolo_model;       /* ONNX 경로 (ORT의 ORTCHAR_T — macOS/Linux는 char) */
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

    /* ── 룰 토글 (점주 페이지 체크박스 — 8/18 회의 ⑤) ──
     * 시스템이 규칙을 강제하지 않는다. 점주가 매장 정책에 맞춰 켜고 끈다. */
    bool rule_announce;           /* 장기 체류 미구매 안내방송 */
    bool rule_fall;               /* 쓰러짐 긴급 알림 */
    bool rule_assist;             /* 키오스크 앞 도움 요청 */
    bool rule_no_kiosk_sit;       /* 키오스크 미방문 착석 (구매 목적 아님 필터) */
    bool rule_hw_alert;           /* 카메라 하드웨어 이상 알림 */
    bool rule_overstay;           /* 구매 후 허용량 초과 체류 (1잔당 N시간 룰) */
    bool rule_pet;                /* 반려동물 출입 감지 (확장 모델 필요) */
    bool rule_no_kids;            /* 노키즈존 — 얼굴 나이 추정 (확장 모델 필요) */
    bool rule_outside_food;       /* 외부 음식 반입 감지 (확장 모델 필요) */
    bool rule_supply_abuse;       /* 비품 구역 반복 접근 (시럽·빨대 어뷰징) */
    bool rule_group_mismatch;     /* 일행 수 대비 주문 수 부족 (POS 잔수 연동) */
    bool rule_violence;           /* 폭력 의심 (2인 근접 + 고속 스윙) */
    bool rule_vandalism;          /* 기물 파손 의심 (단독 + 고속 스윙 반복) */
    bool rule_loitering;          /* 장시간 배회 (착석/구매 없이 장시간 이동) */
    bool rule_tampering;          /* 키오스크 무단/장시간 조작 */
    bool rule_clean;              /* 스마트 청소 알림 (퇴석 후 테이블 잔여물) */
    bool rule_reco;               /* 키오스크 동적 추천 (동행 수+시간대 컨텍스트) */
    double no_kiosk_sit_grace_sec; /* 미방문 착석 판정 유예 (착석 후 N초) */
    double stay_per_purchase_sec; /* 결제 1건당 허용 체류 시간 (8/18 회의 ③) */

    /* ── 제로샷 이상행동 분석 (behavior.c) ──
     * 학습 없이 추적 좌표·관절 이동 벡터의 기구학 특징만으로 판정한다.
     * behavior_sense는 모든 움직임 임계값의 공용 배율 (1.0 기준,
     * 올리면 민감·내리면 둔감 — 점주 다이얼 1개로 통합). */
    float behavior_sense;
    double loiter_sec;            /* 배회 판정: 착석/구매 없이 매장 체류 시간 */
    double tamper_sec;            /* 키오스크 연속 점유 → 무단 조작 의심 시간 */

    /* ── 오탐율 제어 (피드백 기반 폐루프) ──
     * 룰별 (알림 수, 오탐 신고 수)를 집계해 오탐율이 목표를 넘으면
     * 해당 룰 임계값을 자동으로 한 단계 보수화한다. */
    double fp_target_pct;         /* 목표 오탐율 (기본 5%) */

    /* ── 확장 객체 검출 (반려동물/외부음식 — COCO YOLO11n) ──
     * 사람 pose 모델과 별도 세션. 룰이 둘 다 꺼져 있으면 로드하지 않는다. */
    const char *obj_model;        /* yolo11n.onnx (COCO 80클래스) */
    int   obj_imgsz;              /* 320 — 사람 검출(416)보다 작게 (경량) */
    float obj_conf;               /* 확장 감지 민감도 (점주 조정) */
    int   obj_every_k;            /* 사람 추론 K회당 1회만 실행 (기본 5 ≈ 1Hz) */

    /* ── 나이 추정 (노키즈존 — InsightFace genderage 1.3MB) ── */
    const char *age_model;
    int kids_age_limit;           /* 이 나이 이하 추정 시 미성년 의심 (점주 조정) */
    int kids_confirm_votes;       /* 확정에 필요한 누적 표 (오탐 방지) */

    /* ── 비품 어뷰징 (구역 방문 횟수 기반 — 모델 불필요) ── */
    Zone supply_zone;             /* 시럽/빨대/컵홀더 비품대 구역 */
    int supply_abuse_visits;      /* 같은 사람이 N회 이상 방문 시 알림 */

    /* ── 일행 수 vs 주문 수 (POS 잔수 웹훅 연동) ── */
    double group_mismatch_cooldown_sec;   /* 매장 단위 알림 쿨다운 */

    /* ── 행동 분석: 정지 상태 맥락 파악 (8/3 회의 ②) ──
     * 손님이 가만히 있어도 손이 움직이면(공부·노트북·레고 조립 등)
     * '작업 중'으로 분류해 체류 알림에서 제외할 수 있다 (점주 선택). */
    bool activity_exempt;         /* 작업 중인 손님은 체류 알림 제외 */
    float activity_min_move;      /* 작업 판정 손목 움직임 임계 (BBox 높이 비율) */

    /* ── 발열 보호 (엣지·카메라 24시간 가동 대비) ──
     * 무인 시간대에 추론을 계속 돌리면 CPU 발열이 누적된다.
     * 절전: 사람이 N초 이상 없으면 추론 주기를 4배로 늘림 (사람 재등장 시 즉시 복귀).
     * 영업시간: 시간 밖에는 캡처 자체를 저속(2초 1프레임)으로 낮추고 추론 중단. */
    bool eco_mode;
    double eco_idle_sec;          /* 무인 판정까지의 시간 */
    bool hours_enabled;
    int open_min, close_min;      /* 영업 시작/종료 (자정 기준 분, 예: 540 = 09:00) */

    /* ── 판단 로그 (모든 판정 근거를 남기는 상세 로그) ── */
    bool verbose_log;             /* true = DEBUG 레벨 (판정 하나하나 기록) */

    /* ── 상태 머신 세부 (점주 페이지에서 조정 가능) ── */
    int fall_confirm_frames;      /* 쓰러짐 확정에 필요한 연속 감지 횟수 */

    /* ── 하드웨어 자가 진단 ───────────────────────────── */
    double freeze_alert_sec;      /* 새 프레임 없음 N초 → 프레임 정지 알림 */

    /* ── 동선 기록 (설명 가능한 로깅 — 8/18 회의 ①) ──── */
    double journey_ttl_sec;       /* 트랙 소실 후 동선 보관 시간 (퇴장 판정) */

    /* ── 점주 페이지 (내장 HTTP 서버) ─────────────────── */
    int admin_port;               /* 0 = 비활성 */
    const char *settings_path;    /* 점주 설정 저장 파일 (JSON) */
    const char *log_path;         /* 통합 로그 파일 (NULL = stderr만) */
    const char *data_dir;         /* 파일럿 데이터 JSONL 축적 폴더 (빈 문자열 = 끔) */

    /* ── 서버 전송 (WebSocket) ────────────────────────── */
    const char *ws_url;           /* 비우면 로컬 로그만. 예: "ws://192.168.0.10:8080/events" */
    double ws_reconnect_min_sec;
    double ws_reconnect_max_sec;
    double heartbeat_sec;         /* 생존 신호 주기 (0 = 끔). 수신 측이 결손 시 알림 */

    /* ── 기타 ─────────────────────────────────────────── */
    bool show_window;
    int  display_every_n;         /* 화면 갱신 주기 (imshow 자체가 CPU를 상당히 먹는다) */
    bool privacy_view;            /* 비식별 표시: 영상을 열화상풍으로 뭉개고 박스/뼈대만 또렷이 */
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

    .yolo_model = "yolo11n-pose.onnx",
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

    .rule_announce = true,
    .rule_fall = true,
    .rule_assist = true,
    .rule_no_kiosk_sit = true,
    .rule_hw_alert = true,
    .rule_overstay = false,       /* POS 연동 매장에서만 의미 있어 기본 꺼짐 */
    .rule_pet = true,
    .rule_no_kids = false,        /* 매장 정책에 따라 다름 — 기본 꺼짐 */
    .rule_outside_food = false,   /* 판매 품목과 겹칠 수 있어 기본 꺼짐 */
    .rule_supply_abuse = false,   /* 비품 구역을 그린 뒤에 켜는 기능 */
    .rule_group_mismatch = false, /* POS 잔수 연동 매장에서만 의미 있음 */
    .rule_violence = true,
    .rule_vandalism = true,
    .rule_loitering = true,
    .rule_tampering = true,
    .rule_clean = false,          /* 확장 모델 + 테이블 구역 설정 후 켜는 기능 */
    .rule_reco = false,           /* 키오스크 SW 연동 매장에서만 의미 있음 */
    .no_kiosk_sit_grace_sec = 30.0,
    .stay_per_purchase_sec = 5400.0,   /* 1잔당 1.5시간 (8/18 회의 기본 룰) */

    .behavior_sense = 1.0f,
    .loiter_sec = 600.0,          /* 10분 배회 */
    .tamper_sec = 300.0,          /* 정상 주문은 1~3분 — 5분 초과 시 의심 */

    .fp_target_pct = 5.0,

    .obj_model = "yolo11n.onnx",
    .obj_imgsz = 320,
    .obj_conf = 0.45f,
    .obj_every_k = 5,

    .age_model = "genderage.onnx",
    .kids_age_limit = 13,
    .kids_confirm_votes = 5,

    .supply_zone = { "supply", 700, 300, 900, 500 },
    .supply_abuse_visits = 3,

    .group_mismatch_cooldown_sec = 600.0,

    .activity_exempt = true,
    .activity_min_move = 0.02f,   /* 손목 이동량 ≥ BBox 높이의 2%/추론 → 작업 중 */

    .eco_mode = true,
    .eco_idle_sec = 180.0,
    .hours_enabled = false,
    .open_min = 540,              /* 09:00 */
    .close_min = 1320,            /* 22:00 */

    .verbose_log = false,

    .fall_confirm_frames = 5,

    .freeze_alert_sec = 5.0,

    .journey_ttl_sec = 10.0,

    .admin_port = 8765,
    .settings_path = "store_settings.json",
    .log_path = NULL,
    .data_dir = "data",           /* 이벤트/여정 JSONL 기본 축적 (--data-dir= 로 끔) */

    .ws_url = "",
    .ws_reconnect_min_sec = 1.0,
    .ws_reconnect_max_sec = 30.0,
    .heartbeat_sec = 60.0,        /* 필드 테스트 회의: 1분마다 생존 신호 */

    .show_window = true,
    .display_every_n = 2,
    .privacy_view = false,
};

#endif /* CONFIG_H */
