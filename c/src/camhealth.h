/*
 * 카메라 하드웨어 자가 진단 (8/18 회의 ② 백화현상·장애 알림)
 * - 렌즈 백화/초점 상실이 오면 AI 디텍팅 자체가 무용지물이 되고,
 *   이때는 동선 로그로도 설명이 불가능하다 → 시스템이 스스로 감지해
 *   점주에게 하드웨어 이상 알람을 보내야 한다.
 * - 프레임 픽셀을 성기게 샘플링(8px 간격)해 밝기 평균/대비/그래디언트만
 *   계산하므로 추론 대비 비용은 무시 가능한 수준.
 * - 판정은 N회 연속 이상일 때만 확정(디바운스), 상태 전환 시 1회만 알림.
 */
#ifndef CAMHEALTH_H
#define CAMHEALTH_H

#include <stdbool.h>
#include <stdint.h>

#include "types.h"

typedef enum {
    CAM_OK = 0,
    CAM_WHITEOUT,              /* 백화: 화면 전체가 밝고 디테일 없음 */
    CAM_BLACKOUT,              /* 신호 이상/가림: 화면 전체가 어둡고 디테일 없음 */
    CAM_LOW_CONTRAST,          /* 초점 상실/김서림: 밝기는 정상, 디테일 급감 */
    CAM_FROZEN,                /* 프레임 정지: 새 프레임이 오지 않음 */
} CamHealthState;

typedef struct {
    CamHealthState state;
    int abnormal_streak;       /* 연속 이상 판정 수 (디바운스) */
    int ok_streak;             /* 연속 정상 판정 수 (복구도 디바운스 — 플래핑 방지) */
    double last_alert_ts;      /* 마지막 이상 알림 시각 (재알림 쿨다운) */
    bool alerted;              /* 이상 알림을 실제로 발행했는가 (복구 알림 짝 맞춤) */
    CamHealthState pending;    /* 연속 집계 중인 이상 종류 */
    double last_frame_ts;      /* 마지막 새 프레임 수신 시각 */
    uint64_t last_seq;
    /* 최근 측정치 (점주 페이지 표시용) */
    double mean_luma, grad;
} CamHealth;

void camhealth_init(CamHealth *ch, double now);

/* 새 프레임 검사. 상태가 바뀌면 true를 반환하고 msg에 알림 문구를 채운다. */
bool camhealth_check_frame(CamHealth *ch, const FrameView *f, double now,
                           char *msg, int msg_len);

/* 프레임이 안 올 때 호출하는 정지 감시. freeze_sec 초과 시 상태 전환. */
bool camhealth_check_freeze(CamHealth *ch, uint64_t published_seq, double now,
                            double freeze_sec, char *msg, int msg_len);

const char *camhealth_str(CamHealthState s);

#endif /* CAMHEALTH_H */
