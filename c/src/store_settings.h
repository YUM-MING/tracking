/*
 * 점주 맞춤 설정 저장소 (8/3 회의 ① 캘리브레이션 / 8/18 회의 ⑤ 체크박스 UI)
 * - 디폴트 제공값만으로는 모든 매장을 못 맞춘다 → 점주가 룰 ON/OFF와
 *   민감도(임계값), 구역(Zone)을 직접 조정하고 JSON 파일로 영속화한다.
 * - 파이프라인(소비자 스레드)은 매 사이클 스냅샷을 복사해 자기 cfg에 반영
 *   → 락 구간은 작은 구조체 복사 1회뿐, 핫패스와 HTTP 스레드가 분리된다.
 * - 오탐 피드백 루프 (8/6 기획): 점주가 알림에 "오탐" 버튼을 누르면
 *   해당 룰의 임계값을 한 단계 완화하고 즉시 저장 — 쓸수록 매장에 맞춰진다.
 */
#ifndef STORE_SETTINGS_H
#define STORE_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "mask.h"

typedef struct {
    /* 룰 ON/OFF (체크박스) */
    bool rule_announce, rule_fall, rule_assist, rule_no_kiosk_sit, rule_hw_alert;
    bool rule_overstay;               /* 구매 후 허용량 초과 체류 */
    bool rule_pet;                    /* 반려동물 감지 (확장 모델) */
    bool rule_no_kids;                /* 노키즈존 나이 추정 (확장 모델) */
    bool rule_outside_food;           /* 외부 음식 반입 (확장 모델) */
    bool rule_supply_abuse;           /* 비품 구역 반복 접근 */
    bool rule_group_mismatch;         /* 일행 수 vs 주문 수 (POS 잔수) */
    bool rule_violence;               /* 폭력 의심 (제로샷 관절 벡터) */
    bool rule_vandalism;              /* 기물 파손 의심 */
    bool rule_loitering;              /* 장시간 배회 */
    bool rule_tampering;              /* 키오스크 무단/장시간 조작 */
    bool rule_clean;                  /* 스마트 청소 알림 */
    bool rule_reco;                   /* 키오스크 동적 추천 컨텍스트 */
    bool activity_exempt;             /* 공부·작업 중 손님은 체류 알림 제외 */
    bool eco_mode;                    /* 무인 시 절전 (발열 보호) */
    bool hours_enabled;               /* 영업시간 밖 저전력 모드 */
    bool verbose_log;                 /* 상세 판단 로그 (DEBUG 레벨) */
    /* 민감도 (슬라이더) */
    double table_dwell_trigger_sec;   /* 테이블 정체 감시 시작 */
    double announce_dwell_sec;        /* 미구매 안내방송 기준 */
    double announce_cooldown_sec;     /* 동일 인원 재방송 쿨다운 */
    double no_kiosk_sit_grace_sec;    /* 미방문 착석 유예 */
    double stay_per_purchase_sec;     /* 결제 1건당 허용 체류 */
    double eco_idle_sec;              /* 무인 판정 시간 */
    float fall_aspect_ratio;          /* 쓰러짐 민감도 (클수록 둔감) */
    float kiosk_face_w_frac;          /* 키오스크 근접 민감도 */
    float yolo_conf;                  /* 검출 신뢰도 (감지 민감도의 역방향) */
    float activity_min_move;          /* 작업 판정 손목 움직임 임계 */
    int detect_every_n;               /* 분석 주기 (N프레임당 1회 추론) */
    int fall_confirm_frames;          /* 쓰러짐 확정 연속 횟수 */
    int open_min, close_min;          /* 영업 시작/종료 (자정 기준 분) */
    float obj_conf;                   /* 확장 감지(반려동물/음식) 민감도 */
    int obj_every_k;                  /* 확장 감지 주기 (사람 추론 K회당 1회) */
    int kids_age_limit;               /* 미성년 의심 기준 나이 */
    int supply_abuse_visits;          /* 비품 어뷰징 판정 방문 횟수 */
    float behavior_sense;             /* 이상행동 민감도 배율 (0.5~2.0) */
    double loiter_sec;                /* 배회 판정 시간 */
    double tamper_sec;                /* 키오스크 무단 점유 판정 시간 */
    double fp_target_pct;             /* 목표 오탐율 (%) — 자동 보정 기준 */
    /* 구역 (드래그 편집) */
    Zone kiosk_zone, table_zone, supply_zone;
    int kiosk_trigger_mode;           /* KioskTriggerMode 값 */
    /* 인식 범위 마스크 (자동 캘리브레이션 + 수동 칠하기) */
    uint8_t detect_mask[MASK_BYTES];
    /* 변경 세대 — 파이프라인이 변경 감지 로그를 남길 때 사용 */
    long version;
} StoreSettings;

typedef struct SettingsStore SettingsStore;

/* defaults(CFG_DEFAULT)로 초기화 후 json_path 파일이 있으면 덮어 적용 */
SettingsStore *settings_create(const PipelineConfig *defaults, const char *json_path);
void settings_destroy(SettingsStore *s);

void settings_get(SettingsStore *s, StoreSettings *out);
void settings_set(SettingsStore *s, const StoreSettings *in);   /* 적용 + 파일 저장 */

/* 점주 페이지 연동: JSON 본문 파싱/직렬화 (평면 key:value) */
bool settings_apply_json(SettingsStore *s, const char *json);
int  settings_to_json(SettingsStore *s, char *buf, size_t len);

/* 오탐 피드백: kind = 이벤트 종류 문자열. 보정이 일어났으면 true. */
bool settings_feedback(SettingsStore *s, const char *kind, char *desc, size_t desc_len);

/* 자동 캘리브레이션 결과 반영: 기존 마스크에 OR 병합(merge=true) 또는 교체 */
void settings_set_mask(SettingsStore *s, const uint8_t *mask, bool merge);

/* 스냅샷을 파이프라인 설정에 반영 (소비자 스레드 전용) */
void settings_apply_to_cfg(const StoreSettings *st, PipelineConfig *cfg);

#endif /* STORE_SETTINGS_H */
