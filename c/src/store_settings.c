#include "store_settings.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "logger.h"

struct SettingsStore {
    pthread_mutex_t lock;
    StoreSettings cur;
    char path[512];
};

/* ── 기본값: PipelineConfig에서 추출 ─────────────────── */
static void from_cfg(const PipelineConfig *cfg, StoreSettings *st)
{
    memset(st, 0, sizeof(*st));
    st->rule_announce = cfg->rule_announce;
    st->rule_fall = cfg->rule_fall;
    st->rule_assist = cfg->rule_assist;
    st->rule_no_kiosk_sit = cfg->rule_no_kiosk_sit;
    st->rule_hw_alert = cfg->rule_hw_alert;
    st->rule_overstay = cfg->rule_overstay;
    st->rule_pet = cfg->rule_pet;
    st->rule_no_kids = cfg->rule_no_kids;
    st->rule_outside_food = cfg->rule_outside_food;
    st->rule_supply_abuse = cfg->rule_supply_abuse;
    st->rule_group_mismatch = cfg->rule_group_mismatch;
    st->rule_violence = cfg->rule_violence;
    st->rule_vandalism = cfg->rule_vandalism;
    st->rule_loitering = cfg->rule_loitering;
    st->rule_tampering = cfg->rule_tampering;
    st->rule_clean = cfg->rule_clean;
    st->rule_reco = cfg->rule_reco;
    st->activity_exempt = cfg->activity_exempt;
    st->eco_mode = cfg->eco_mode;
    st->hours_enabled = cfg->hours_enabled;
    st->verbose_log = cfg->verbose_log;
    st->table_dwell_trigger_sec = cfg->table_dwell_trigger_sec;
    st->announce_dwell_sec = cfg->announce_dwell_sec;
    st->announce_cooldown_sec = cfg->announce_cooldown_sec;
    st->no_kiosk_sit_grace_sec = cfg->no_kiosk_sit_grace_sec;
    st->stay_per_purchase_sec = cfg->stay_per_purchase_sec;
    st->eco_idle_sec = cfg->eco_idle_sec;
    st->fall_aspect_ratio = cfg->fall_aspect_ratio;
    st->kiosk_face_w_frac = cfg->kiosk_face_w_frac;
    st->kiosk_body_h_frac = cfg->kiosk_body_h_frac;
    st->yolo_conf = cfg->yolo_conf;
    st->activity_min_move = cfg->activity_min_move;
    st->detect_every_n = cfg->detect_every_n;
    st->fall_confirm_frames = cfg->fall_confirm_frames;
    st->open_min = cfg->open_min;
    st->close_min = cfg->close_min;
    st->obj_conf = cfg->obj_conf;
    st->obj_every_k = cfg->obj_every_k;
    st->kids_age_limit = cfg->kids_age_limit;
    st->supply_abuse_visits = cfg->supply_abuse_visits;
    st->behavior_sense = cfg->behavior_sense;
    st->loiter_sec = cfg->loiter_sec;
    st->tamper_sec = cfg->tamper_sec;
    st->fp_target_pct = cfg->fp_target_pct;
    st->kiosk_zone = cfg->kiosk_zone;
    st->table_zone = cfg->table_zone;
    st->supply_zone = cfg->supply_zone;
    st->kiosk_trigger_mode = (int)cfg->kiosk_trigger_mode;
    /* detect_mask는 memset으로 전부 0(마스크 없음)에서 시작 */
    st->version = 1;
}

void settings_apply_to_cfg(const StoreSettings *st, PipelineConfig *cfg)
{
    cfg->rule_announce = st->rule_announce;
    cfg->rule_fall = st->rule_fall;
    cfg->rule_assist = st->rule_assist;
    cfg->rule_no_kiosk_sit = st->rule_no_kiosk_sit;
    cfg->rule_hw_alert = st->rule_hw_alert;
    cfg->rule_overstay = st->rule_overstay;
    cfg->rule_pet = st->rule_pet;
    cfg->rule_no_kids = st->rule_no_kids;
    cfg->rule_outside_food = st->rule_outside_food;
    cfg->rule_supply_abuse = st->rule_supply_abuse;
    cfg->rule_group_mismatch = st->rule_group_mismatch;
    cfg->rule_violence = st->rule_violence;
    cfg->rule_vandalism = st->rule_vandalism;
    cfg->rule_loitering = st->rule_loitering;
    cfg->rule_tampering = st->rule_tampering;
    cfg->rule_clean = st->rule_clean;
    cfg->rule_reco = st->rule_reco;
    cfg->activity_exempt = st->activity_exempt;
    cfg->eco_mode = st->eco_mode;
    cfg->hours_enabled = st->hours_enabled;
    cfg->verbose_log = st->verbose_log;
    cfg->table_dwell_trigger_sec = st->table_dwell_trigger_sec;
    cfg->announce_dwell_sec = st->announce_dwell_sec;
    cfg->announce_cooldown_sec = st->announce_cooldown_sec;
    cfg->no_kiosk_sit_grace_sec = st->no_kiosk_sit_grace_sec;
    cfg->stay_per_purchase_sec = st->stay_per_purchase_sec;
    cfg->eco_idle_sec = st->eco_idle_sec;
    cfg->fall_aspect_ratio = st->fall_aspect_ratio;
    cfg->kiosk_face_w_frac = st->kiosk_face_w_frac;
    cfg->kiosk_body_h_frac = st->kiosk_body_h_frac;
    cfg->yolo_conf = st->yolo_conf;
    cfg->activity_min_move = st->activity_min_move;
    cfg->detect_every_n = st->detect_every_n;
    cfg->fall_confirm_frames = st->fall_confirm_frames;
    cfg->open_min = st->open_min;
    cfg->close_min = st->close_min;
    cfg->obj_conf = st->obj_conf;
    cfg->obj_every_k = st->obj_every_k;
    cfg->kids_age_limit = st->kids_age_limit;
    cfg->supply_abuse_visits = st->supply_abuse_visits;
    cfg->behavior_sense = st->behavior_sense;
    cfg->loiter_sec = st->loiter_sec;
    cfg->tamper_sec = st->tamper_sec;
    cfg->fp_target_pct = st->fp_target_pct;
    /* Zone 이름 포인터는 정적 문자열 유지 */
    cfg->kiosk_zone.x1 = st->kiosk_zone.x1; cfg->kiosk_zone.y1 = st->kiosk_zone.y1;
    cfg->kiosk_zone.x2 = st->kiosk_zone.x2; cfg->kiosk_zone.y2 = st->kiosk_zone.y2;
    cfg->table_zone.x1 = st->table_zone.x1; cfg->table_zone.y1 = st->table_zone.y1;
    cfg->table_zone.x2 = st->table_zone.x2; cfg->table_zone.y2 = st->table_zone.y2;
    cfg->supply_zone.x1 = st->supply_zone.x1; cfg->supply_zone.y1 = st->supply_zone.y1;
    cfg->supply_zone.x2 = st->supply_zone.x2; cfg->supply_zone.y2 = st->supply_zone.y2;
    cfg->kiosk_trigger_mode = st->kiosk_trigger_mode == 1
        ? KIOSK_TRIGGER_ZONE : KIOSK_TRIGGER_NEAR;
}

/* ── 평면 JSON 스캐너 ─────────────────────────────────
 * 설정 JSON은 {"key": 숫자|true|false, ...} 형태로 고정 (중첩 없음).
 * 라이브러리 없이 필요한 만큼만 직접 구현한다. */
static bool json_find(const char *json, const char *key, const char **val_out)
{
    char pat[80];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p && (*p == ':' || isspace((unsigned char)*p))) p++;
    if (!*p) return false;
    *val_out = p;
    return true;
}

static bool json_num(const char *json, const char *key, double *out)
{
    const char *v;
    if (!json_find(json, key, &v)) return false;
    char *end;
    double d = strtod(v, &end);
    if (end == v) return false;
    *out = d;
    return true;
}

static bool json_bool(const char *json, const char *key, bool *out)
{
    const char *v;
    if (!json_find(json, key, &v)) return false;
    if (strncmp(v, "true", 4) == 0)  { *out = true;  return true; }
    if (strncmp(v, "false", 5) == 0) { *out = false; return true; }
    return false;
}

#define CLAMPD(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

/* 파싱 결과를 안전 범위로 강제 — 점주 페이지 입력값 검증.
 * 슬라이더를 극단으로 밀어도 파이프라인이 죽거나 룰이 무의미해지지 않게 한다. */
static void sanitize(StoreSettings *st)
{
    st->table_dwell_trigger_sec = CLAMPD(st->table_dwell_trigger_sec, 10, 3600);
    st->announce_dwell_sec = CLAMPD(st->announce_dwell_sec, 30, 21600);
    st->announce_cooldown_sec = CLAMPD(st->announce_cooldown_sec, 10, 3600);
    st->no_kiosk_sit_grace_sec = CLAMPD(st->no_kiosk_sit_grace_sec, 5, 600);
    st->stay_per_purchase_sec = CLAMPD(st->stay_per_purchase_sec, 600, 21600);
    st->eco_idle_sec = CLAMPD(st->eco_idle_sec, 30, 3600);
    st->fall_aspect_ratio = (float)CLAMPD(st->fall_aspect_ratio, 1.0, 3.0);
    st->kiosk_face_w_frac = (float)CLAMPD(st->kiosk_face_w_frac, 0.03, 0.5);
    st->kiosk_body_h_frac = (float)CLAMPD(st->kiosk_body_h_frac, 0.3, 1.0);
    st->yolo_conf = (float)CLAMPD(st->yolo_conf, 0.10, 0.90);
    st->activity_min_move = (float)CLAMPD(st->activity_min_move, 0.005, 0.10);
    st->detect_every_n = (int)CLAMPD(st->detect_every_n, 1, 30);
    st->fall_confirm_frames = (int)CLAMPD(st->fall_confirm_frames, 2, 30);
    st->open_min = (int)CLAMPD(st->open_min, 0, 1440);
    st->close_min = (int)CLAMPD(st->close_min, 0, 1440);
    st->obj_conf = (float)CLAMPD(st->obj_conf, 0.20, 0.90);
    st->obj_every_k = (int)CLAMPD(st->obj_every_k, 1, 30);
    st->kids_age_limit = (int)CLAMPD(st->kids_age_limit, 5, 19);
    st->supply_abuse_visits = (int)CLAMPD(st->supply_abuse_visits, 2, 20);
    st->behavior_sense = (float)CLAMPD(st->behavior_sense, 0.5, 2.0);
    st->loiter_sec = CLAMPD(st->loiter_sec, 60, 7200);
    st->tamper_sec = CLAMPD(st->tamper_sec, 60, 3600);
    st->fp_target_pct = CLAMPD(st->fp_target_pct, 1, 50);
    if (st->kiosk_trigger_mode != 1) st->kiosk_trigger_mode = 0;
}

static void parse_zone(const char *json, const char *prefix, Zone *z)
{
    char key[64];
    double d;
    snprintf(key, sizeof(key), "%s_x1", prefix);
    if (json_num(json, key, &d)) z->x1 = (int)d;
    snprintf(key, sizeof(key), "%s_y1", prefix);
    if (json_num(json, key, &d)) z->y1 = (int)d;
    snprintf(key, sizeof(key), "%s_x2", prefix);
    if (json_num(json, key, &d)) z->x2 = (int)d;
    snprintf(key, sizeof(key), "%s_y2", prefix);
    if (json_num(json, key, &d)) z->y2 = (int)d;
}

static void parse_into(const char *json, StoreSettings *st)
{
    double d;
    json_bool(json, "rule_announce", &st->rule_announce);
    json_bool(json, "rule_fall", &st->rule_fall);
    json_bool(json, "rule_assist", &st->rule_assist);
    json_bool(json, "rule_no_kiosk_sit", &st->rule_no_kiosk_sit);
    json_bool(json, "rule_hw_alert", &st->rule_hw_alert);
    json_bool(json, "rule_overstay", &st->rule_overstay);
    json_bool(json, "rule_pet", &st->rule_pet);
    json_bool(json, "rule_no_kids", &st->rule_no_kids);
    json_bool(json, "rule_outside_food", &st->rule_outside_food);
    json_bool(json, "rule_supply_abuse", &st->rule_supply_abuse);
    json_bool(json, "rule_group_mismatch", &st->rule_group_mismatch);
    json_bool(json, "rule_violence", &st->rule_violence);
    json_bool(json, "rule_vandalism", &st->rule_vandalism);
    json_bool(json, "rule_loitering", &st->rule_loitering);
    json_bool(json, "rule_tampering", &st->rule_tampering);
    json_bool(json, "rule_clean", &st->rule_clean);
    json_bool(json, "rule_reco", &st->rule_reco);
    json_bool(json, "activity_exempt", &st->activity_exempt);
    json_bool(json, "eco_mode", &st->eco_mode);
    json_bool(json, "hours_enabled", &st->hours_enabled);
    json_bool(json, "verbose_log", &st->verbose_log);
    if (json_num(json, "table_dwell_trigger_sec", &d)) st->table_dwell_trigger_sec = d;
    if (json_num(json, "announce_dwell_sec", &d)) st->announce_dwell_sec = d;
    if (json_num(json, "announce_cooldown_sec", &d)) st->announce_cooldown_sec = d;
    if (json_num(json, "no_kiosk_sit_grace_sec", &d)) st->no_kiosk_sit_grace_sec = d;
    if (json_num(json, "stay_per_purchase_sec", &d)) st->stay_per_purchase_sec = d;
    if (json_num(json, "eco_idle_sec", &d)) st->eco_idle_sec = d;
    if (json_num(json, "fall_aspect_ratio", &d)) st->fall_aspect_ratio = (float)d;
    if (json_num(json, "kiosk_face_w_frac", &d)) st->kiosk_face_w_frac = (float)d;
    if (json_num(json, "kiosk_body_h_frac", &d)) st->kiosk_body_h_frac = (float)d;
    if (json_num(json, "yolo_conf", &d)) st->yolo_conf = (float)d;
    if (json_num(json, "activity_min_move", &d)) st->activity_min_move = (float)d;
    if (json_num(json, "detect_every_n", &d)) st->detect_every_n = (int)d;
    if (json_num(json, "fall_confirm_frames", &d)) st->fall_confirm_frames = (int)d;
    if (json_num(json, "open_min", &d)) st->open_min = (int)d;
    if (json_num(json, "close_min", &d)) st->close_min = (int)d;
    if (json_num(json, "obj_conf", &d)) st->obj_conf = (float)d;
    if (json_num(json, "obj_every_k", &d)) st->obj_every_k = (int)d;
    if (json_num(json, "kids_age_limit", &d)) st->kids_age_limit = (int)d;
    if (json_num(json, "supply_abuse_visits", &d)) st->supply_abuse_visits = (int)d;
    if (json_num(json, "behavior_sense", &d)) st->behavior_sense = (float)d;
    if (json_num(json, "loiter_sec", &d)) st->loiter_sec = d;
    if (json_num(json, "tamper_sec", &d)) st->tamper_sec = d;
    if (json_num(json, "fp_target_pct", &d)) st->fp_target_pct = d;
    if (json_num(json, "kiosk_trigger_mode", &d)) st->kiosk_trigger_mode = (int)d;
    parse_zone(json, "kiosk_zone", &st->kiosk_zone);
    parse_zone(json, "table_zone", &st->table_zone);
    parse_zone(json, "supply_zone", &st->supply_zone);
    /* 인식 범위 마스크: 144자 hex 문자열 (형식 오류면 기존 값 유지) */
    const char *mv;
    if (json_find(json, "mask_hex", &mv) && *mv == '"')
        mask_from_hex(mv + 1, st->detect_mask);
    sanitize(st);
}

static int serialize(const StoreSettings *st, char *buf, size_t len)
{
    char mask_hex[MASK_HEX_LEN + 1];
    mask_to_hex(st->detect_mask, mask_hex);
    return snprintf(buf, len,
        "{\n"
        "  \"rule_announce\": %s,\n"
        "  \"rule_fall\": %s,\n"
        "  \"rule_assist\": %s,\n"
        "  \"rule_no_kiosk_sit\": %s,\n"
        "  \"rule_hw_alert\": %s,\n"
        "  \"rule_overstay\": %s,\n"
        "  \"rule_pet\": %s,\n"
        "  \"rule_no_kids\": %s,\n"
        "  \"rule_outside_food\": %s,\n"
        "  \"rule_supply_abuse\": %s,\n"
        "  \"rule_group_mismatch\": %s,\n"
        "  \"rule_violence\": %s,\n"
        "  \"rule_vandalism\": %s,\n"
        "  \"rule_loitering\": %s,\n"
        "  \"rule_tampering\": %s,\n"
        "  \"rule_clean\": %s,\n"
        "  \"rule_reco\": %s,\n"
        "  \"activity_exempt\": %s,\n"
        "  \"eco_mode\": %s,\n"
        "  \"hours_enabled\": %s,\n"
        "  \"verbose_log\": %s,\n"
        "  \"table_dwell_trigger_sec\": %.0f,\n"
        "  \"announce_dwell_sec\": %.0f,\n"
        "  \"announce_cooldown_sec\": %.0f,\n"
        "  \"no_kiosk_sit_grace_sec\": %.0f,\n"
        "  \"stay_per_purchase_sec\": %.0f,\n"
        "  \"eco_idle_sec\": %.0f,\n"
        "  \"fall_aspect_ratio\": %.2f,\n"
        "  \"kiosk_face_w_frac\": %.3f,\n"
        "  \"kiosk_body_h_frac\": %.3f,\n"
        "  \"yolo_conf\": %.2f,\n"
        "  \"activity_min_move\": %.3f,\n"
        "  \"detect_every_n\": %d,\n"
        "  \"fall_confirm_frames\": %d,\n"
        "  \"open_min\": %d,\n"
        "  \"close_min\": %d,\n"
        "  \"obj_conf\": %.2f,\n"
        "  \"obj_every_k\": %d,\n"
        "  \"kids_age_limit\": %d,\n"
        "  \"supply_abuse_visits\": %d,\n"
        "  \"behavior_sense\": %.2f,\n"
        "  \"loiter_sec\": %.0f,\n"
        "  \"tamper_sec\": %.0f,\n"
        "  \"fp_target_pct\": %.1f,\n"
        "  \"kiosk_trigger_mode\": %d,\n"
        "  \"kiosk_zone_x1\": %d, \"kiosk_zone_y1\": %d,"
        " \"kiosk_zone_x2\": %d, \"kiosk_zone_y2\": %d,\n"
        "  \"table_zone_x1\": %d, \"table_zone_y1\": %d,"
        " \"table_zone_x2\": %d, \"table_zone_y2\": %d,\n"
        "  \"supply_zone_x1\": %d, \"supply_zone_y1\": %d,"
        " \"supply_zone_x2\": %d, \"supply_zone_y2\": %d,\n"
        "  \"mask_hex\": \"%s\",\n"
        "  \"version\": %ld\n"
        "}\n",
        st->rule_announce ? "true" : "false",
        st->rule_fall ? "true" : "false",
        st->rule_assist ? "true" : "false",
        st->rule_no_kiosk_sit ? "true" : "false",
        st->rule_hw_alert ? "true" : "false",
        st->rule_overstay ? "true" : "false",
        st->rule_pet ? "true" : "false",
        st->rule_no_kids ? "true" : "false",
        st->rule_outside_food ? "true" : "false",
        st->rule_supply_abuse ? "true" : "false",
        st->rule_group_mismatch ? "true" : "false",
        st->rule_violence ? "true" : "false",
        st->rule_vandalism ? "true" : "false",
        st->rule_loitering ? "true" : "false",
        st->rule_tampering ? "true" : "false",
        st->rule_clean ? "true" : "false",
        st->rule_reco ? "true" : "false",
        st->activity_exempt ? "true" : "false",
        st->eco_mode ? "true" : "false",
        st->hours_enabled ? "true" : "false",
        st->verbose_log ? "true" : "false",
        st->table_dwell_trigger_sec, st->announce_dwell_sec,
        st->announce_cooldown_sec, st->no_kiosk_sit_grace_sec,
        st->stay_per_purchase_sec, st->eco_idle_sec,
        (double)st->fall_aspect_ratio, (double)st->kiosk_face_w_frac,
        (double)st->kiosk_body_h_frac,
        (double)st->yolo_conf, (double)st->activity_min_move,
        st->detect_every_n, st->fall_confirm_frames,
        st->open_min, st->close_min,
        (double)st->obj_conf, st->obj_every_k,
        st->kids_age_limit, st->supply_abuse_visits,
        (double)st->behavior_sense, st->loiter_sec, st->tamper_sec,
        st->fp_target_pct,
        st->kiosk_trigger_mode,
        st->kiosk_zone.x1, st->kiosk_zone.y1, st->kiosk_zone.x2, st->kiosk_zone.y2,
        st->table_zone.x1, st->table_zone.y1, st->table_zone.x2, st->table_zone.y2,
        st->supply_zone.x1, st->supply_zone.y1, st->supply_zone.x2, st->supply_zone.y2,
        mask_hex, st->version);
}

/* 파일 저장은 호출자가 lock을 쥔 상태에서만 부른다 */
static void save_locked(SettingsStore *s)
{
    if (!s->path[0]) return;
    char buf[2048];
    int n = serialize(&s->cur, buf, sizeof(buf));
    FILE *f = fopen(s->path, "w");
    if (!f) {
        LOGW("설정", "저장 실패: %s", s->path);
        return;
    }
    fwrite(buf, 1, (size_t)n, f);
    fclose(f);
}

SettingsStore *settings_create(const PipelineConfig *defaults, const char *json_path)
{
    SettingsStore *s = calloc(1, sizeof(SettingsStore));
    pthread_mutex_init(&s->lock, NULL);
    from_cfg(defaults, &s->cur);
    if (json_path) snprintf(s->path, sizeof(s->path), "%s", json_path);

    if (s->path[0]) {
        FILE *f = fopen(s->path, "r");
        if (f) {
            char buf[4096];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = 0;
            fclose(f);
            parse_into(buf, &s->cur);
            LOGI("설정", "점주 설정 로드: %s (v%ld)", s->path, s->cur.version);
        } else {
            LOGI("설정", "저장된 설정 없음 — 기본값 사용 (%s)", s->path);
        }
    }
    return s;
}

void settings_destroy(SettingsStore *s)
{
    if (!s) return;
    pthread_mutex_destroy(&s->lock);
    free(s);
}

void settings_get(SettingsStore *s, StoreSettings *out)
{
    pthread_mutex_lock(&s->lock);
    *out = s->cur;
    pthread_mutex_unlock(&s->lock);
}

void settings_set(SettingsStore *s, const StoreSettings *in)
{
    pthread_mutex_lock(&s->lock);
    long v = s->cur.version;
    s->cur = *in;
    sanitize(&s->cur);
    s->cur.version = v + 1;
    save_locked(s);
    pthread_mutex_unlock(&s->lock);
}

bool settings_apply_json(SettingsStore *s, const char *json)
{
    pthread_mutex_lock(&s->lock);
    parse_into(json, &s->cur);
    s->cur.version++;
    save_locked(s);
    long v = s->cur.version;
    pthread_mutex_unlock(&s->lock);
    LOGI("설정", "점주 설정 변경 적용 (v%ld)", v);
    return true;
}

int settings_to_json(SettingsStore *s, char *buf, size_t len)
{
    pthread_mutex_lock(&s->lock);
    int n = serialize(&s->cur, buf, len);
    pthread_mutex_unlock(&s->lock);
    return n;
}

/*
 * 오탐 피드백 자동 캘리브레이션 (8/6 기획 'AI 상황 추론 피드백 루프')
 * - 점주가 "오탐" 버튼을 누른 이벤트 종류의 임계값을 한 단계 완화한다.
 * - 안전 상한이 있어 반복 피드백으로도 룰이 완전히 죽지는 않는다.
 */
bool settings_feedback(SettingsStore *s, const char *kind, char *desc, size_t desc_len)
{
    bool changed = false;
    pthread_mutex_lock(&s->lock);
    StoreSettings *st = &s->cur;
    if (strcmp(kind, "fall_alert") == 0 && st->fall_aspect_ratio < 2.2f) {
        st->fall_aspect_ratio += 0.05f;
        snprintf(desc, desc_len, "쓰러짐 민감도 완화 → 종횡비 %.2f",
                 (double)st->fall_aspect_ratio);
        changed = true;
    } else if (strcmp(kind, "announce_dwell") == 0 && st->announce_dwell_sec < 1800) {
        st->announce_dwell_sec += 60;
        snprintf(desc, desc_len, "안내방송 기준 완화 → %.0f초",
                 st->announce_dwell_sec);
        changed = true;
    } else if (strcmp(kind, "kiosk_assist") == 0 && st->kiosk_face_w_frac < 0.4f) {
        st->kiosk_face_w_frac += 0.01f;
        snprintf(desc, desc_len, "키오스크 근접 기준 강화 → %.3f",
                 (double)st->kiosk_face_w_frac);
        changed = true;
    } else if (strcmp(kind, "no_kiosk_sit") == 0 && st->no_kiosk_sit_grace_sec < 600) {
        st->no_kiosk_sit_grace_sec += 30;
        snprintf(desc, desc_len, "미방문 착석 유예 연장 → %.0f초",
                 st->no_kiosk_sit_grace_sec);
        changed = true;
    } else if (strcmp(kind, "overstay") == 0 && st->stay_per_purchase_sec < 21600) {
        st->stay_per_purchase_sec += 600;
        snprintf(desc, desc_len, "결제당 허용 체류 연장 → %.0f분",
                 st->stay_per_purchase_sec / 60);
        changed = true;
    } else if ((strcmp(kind, "pet") == 0 || strcmp(kind, "outside_food") == 0) &&
               st->obj_conf < 0.9f) {
        st->obj_conf += 0.05f;
        snprintf(desc, desc_len, "확장 감지 기준 강화 → 신뢰도 %.2f",
                 (double)st->obj_conf);
        changed = true;
    } else if (strcmp(kind, "minor_suspect") == 0 && st->kids_age_limit > 5) {
        st->kids_age_limit -= 1;
        snprintf(desc, desc_len, "미성년 의심 기준 하향 → %d세 이하",
                 st->kids_age_limit);
        changed = true;
    } else if (strcmp(kind, "supply_abuse") == 0 && st->supply_abuse_visits < 20) {
        st->supply_abuse_visits += 1;
        snprintf(desc, desc_len, "비품 어뷰징 기준 완화 → %d회 이상",
                 st->supply_abuse_visits);
        changed = true;
    } else if ((strcmp(kind, "violence") == 0 || strcmp(kind, "vandalism") == 0) &&
               st->behavior_sense > 0.5f) {
        st->behavior_sense -= 0.1f;
        snprintf(desc, desc_len, "이상행동 민감도 하향 → %.1f",
                 (double)st->behavior_sense);
        changed = true;
    } else if (strcmp(kind, "loitering") == 0 && st->loiter_sec < 7200) {
        st->loiter_sec += 120;
        snprintf(desc, desc_len, "배회 판정 시간 연장 → %.0f분",
                 st->loiter_sec / 60);
        changed = true;
    } else if (strcmp(kind, "tampering") == 0 && st->tamper_sec < 3600) {
        st->tamper_sec += 60;
        snprintf(desc, desc_len, "무단 조작 판정 시간 연장 → %.0f분",
                 st->tamper_sec / 60);
        changed = true;
    } else if (strcmp(kind, "clean_needed") == 0 && st->obj_conf < 0.9f) {
        st->obj_conf += 0.05f;
        snprintf(desc, desc_len, "잔여물 감지 기준 강화 → 신뢰도 %.2f",
                 (double)st->obj_conf);
        changed = true;
    }
    if (changed) {
        st->version++;
        save_locked(s);
    }
    pthread_mutex_unlock(&s->lock);
    if (changed) LOGI("설정", "오탐 피드백 반영(%s): %s", kind, desc);
    else snprintf(desc, desc_len, "보정 대상 아님 또는 한계 도달");
    return changed;
}

/*
 * 자동 캘리브레이션(세팅 세션) 결과 반영.
 * merge=true: 기존 마스크 위에 새로 발견된 오탐 셀을 추가 (일반적인 경우).
 * merge=false: 통째로 교체 (마스크 초기화 후 재캘리브레이션).
 */
void settings_set_mask(SettingsStore *s, const uint8_t *mask, bool merge)
{
    pthread_mutex_lock(&s->lock);
    if (merge) {
        for (int i = 0; i < MASK_BYTES; i++)
            s->cur.detect_mask[i] |= mask[i];
    } else {
        memcpy(s->cur.detect_mask, mask, MASK_BYTES);
    }
    s->cur.version++;
    save_locked(s);
    int n = mask_count(s->cur.detect_mask);
    pthread_mutex_unlock(&s->lock);
    LOGI("설정", "인식 범위 마스크 갱신 — 제외 셀 %d/%d (%.0f%%)",
         n, MASK_CELLS, 100.0 * n / MASK_CELLS);
}
