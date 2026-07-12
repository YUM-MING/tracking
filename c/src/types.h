/*
 * 파이프라인 단계 간에 오가는 공용 타입
 * - stage1_global_detector.py의 TrackedPerson
 * - stage2_roi_router.py의 PrecisionTarget
 * - stage3_local_analyzer.py의 PrecisionResult
 * 를 C 구조체로 포팅. 이미지 데이터는 항상 "원본 프레임 포인터 + 사각형"으로만
 * 전달한다 (numpy 뷰에 해당하는 zero-copy).
 */
#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stdint.h>

#define KPT_COUNT 17           /* COCO 17 키포인트 */
#define MAX_PEOPLE 64          /* 프레임당 추적 인원 상한 */
#define MAX_TARGETS 8          /* 프레임당 정밀 분석 대상 상한(설정값 이상) */

/* COCO 키포인트 인덱스 (reid_tagger.py / stage2와 공유) */
enum {
    KPT_NOSE = 0,
    KPT_L_EYE = 1, KPT_R_EYE = 2,
    KPT_L_EAR = 3, KPT_R_EAR = 4,
    KPT_L_SHOULDER = 5, KPT_R_SHOULDER = 6,
    KPT_L_ELBOW = 7, KPT_R_ELBOW = 8,
    KPT_L_WRIST = 9, KPT_R_WRIST = 10,
    KPT_L_HIP = 11, KPT_R_HIP = 12,
    KPT_L_KNEE = 13, KPT_R_KNEE = 14,
    KPT_L_ANKLE = 15, KPT_R_ANKLE = 16,
};

typedef struct {
    float x, y;
    float conf;                /* 키포인트 신뢰도. 임계 미만이면 무효로 취급 */
} Keypoint;

/* BGR 8bit 프레임 참조 (소유하지 않음) */
typedef struct {
    uint8_t *data;
    int w, h;
    int stride;                /* 행 바이트 수 (w*3과 다를 수 있음) */
} FrameView;

/* 전역 탐지 결과 1인분 (다음 단계로 전달되는 최소 데이터) */
typedef struct {
    int   track_id;
    float bbox[4];             /* x1, y1, x2, y2 (원본 프레임 좌표) */
    float conf;
    bool  has_kpts;
    Keypoint kpts[KPT_COUNT];
    float center_x, center_y;  /* 발밑 기준점(구역 판정용): 가로 중앙, 세로 하단 */
} TrackedPerson;

/* 가로/세로 비율. 서 있으면 < 1, 쓰러지면 > 1로 커진다. */
static inline float person_aspect_ratio(const TrackedPerson *p) {
    float h = p->bbox[3] - p->bbox[1];
    if (h < 1e-6f) h = 1e-6f;
    return (p->bbox[2] - p->bbox[0]) / h;
}

/* 키포인트 유효 여부 (미검출이면 conf가 낮게 나온다) */
static inline bool kpt_valid(const TrackedPerson *p, int i, float min_conf) {
    return p->has_kpts && p->kpts[i].conf >= min_conf &&
           (p->kpts[i].x > 0.f || p->kpts[i].y > 0.f);
}

/*
 * 머리 폭(px): 양귀 간격, 귀가 안 잡히면 양눈 간격×2로 근사.
 * 얼굴 키포인트 미검출(뒤돌아 있음 등)이면 음수 반환.
 * 키오스크 근접 판정은 이 값을 화면 가로로 나눈 '비율'로 한다.
 */
static inline float person_face_width(const TrackedPerson *p, float min_conf) {
    if (!p->has_kpts) return -1.f;
    if (kpt_valid(p, KPT_L_EAR, min_conf) && kpt_valid(p, KPT_R_EAR, min_conf)) {
        float d = p->kpts[KPT_L_EAR].x - p->kpts[KPT_R_EAR].x;
        return d < 0 ? -d : d;
    }
    if (kpt_valid(p, KPT_L_EYE, min_conf) && kpt_valid(p, KPT_R_EYE, min_conf)) {
        float d = p->kpts[KPT_L_EYE].x - p->kpts[KPT_R_EYE].x;
        return (d < 0 ? -d : d) * 2.0f;
    }
    return -1.f;
}

typedef enum {
    REASON_NONE = 0,
    REASON_FALL_SUSPECT,       /* 스켈레톤 붕괴(쓰러짐 징후) */
    REASON_KIOSK_ENTER,        /* 키오스크 근접/진입 */
    REASON_TABLE_DWELL,        /* 테이블 구역 정체 */
} TriggerReason;

const char *reason_str(TriggerReason r);

/* 3단계로 전달되는 정밀 분석 대상 (크롭은 원본 프레임 내 사각형 = zero-copy) */
typedef struct {
    int track_id;
    TriggerReason reason;
    int cx1, cy1, cx2, cy2;    /* 패딩 포함, 프레임 경계로 클램프된 크롭 사각형 */
    const TrackedPerson *person;
    int frame_h;               /* 크롭 크기를 '비율'로 판정하기 위한 원본 세로 */
} PrecisionTarget;

/* 상태 머신으로 전달되는 비식별 분석 결과 (이미지 데이터 없음) */
typedef struct {
    int track_id;
    TriggerReason reason;
    bool pose_detected;
    bool hand_raised;          /* 손목이 어깨보다 위 (키오스크 조작/도움 요청) */
    bool torso_horizontal;     /* 상체 수평 (쓰러짐 확정 신호) */
    bool face_detected;
    bool gaze_forward;         /* 얼굴이 카메라(키오스크) 방향 */
    float face_ratio;          /* 눈 간격 / 눈-코 세로 거리 (비식별 스칼라, <0 = 없음) */
} PrecisionResult;

#endif /* TYPES_H */
