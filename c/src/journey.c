#include "journey.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "datastore.h"
#include "logger.h"
#include "os_compat.h"

#define JOURNEY_TRACKS 128     /* 동시 추적 동선 상한 (정적 할당) */
#define JOURNEY_STEPS  16      /* 트랙당 기록 단계 상한 (초과 시 마지막 갱신) */

typedef struct {
    JourneyStep step;
    double ts;                 /* 단조 시각 */
} StepRec;

typedef struct {
    bool used;
    int track_id;
    double last_seen;
    int n_steps;
    StepRec steps[JOURNEY_STEPS];
} TrackJourney;

struct Journey {
    TrackJourney tracks[JOURNEY_TRACKS];
    double wall_offset;        /* time(NULL) - mono 시각 → 표시용 벽시계 변환 */
};

static const char *STEP_STR[] = {
    "입장", "키오스크", "착석", "테이블이탈", "결제", "퇴장",
};

/* JSONL 여정 레코드용 기계 키 (분석 스크립트에서 한글 파싱 불필요) */
static const char *STEP_KEY[] = {
    "enter", "kiosk", "sit", "leave_table", "purchase", "exit",
};

static double mono_seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

Journey *journey_create(void)
{
    Journey *j = calloc(1, sizeof(Journey));
    j->wall_offset = (double)time(NULL) - mono_seconds();
    return j;
}

void journey_destroy(Journey *j) { free(j); }

static TrackJourney *slot_find(Journey *j, int track_id)
{
    for (int i = 0; i < JOURNEY_TRACKS; i++)
        if (j->tracks[i].used && j->tracks[i].track_id == track_id)
            return &j->tracks[i];
    return NULL;
}

static TrackJourney *slot_get(Journey *j, int track_id, double now)
{
    TrackJourney *t = slot_find(j, track_id);
    if (t) return t;
    for (int i = 0; i < JOURNEY_TRACKS; i++) {
        if (!j->tracks[i].used) {
            t = &j->tracks[i];
            memset(t, 0, sizeof(*t));
            t->used = true;
            t->track_id = track_id;
            t->last_seen = now;
            return t;
        }
    }
    return NULL;                                /* 슬롯 고갈 — 기록 생략 */
}

static void step_append(TrackJourney *t, JourneyStep step, double now)
{
    if (t->n_steps > 0 && t->steps[t->n_steps - 1].step == step)
        return;                                 /* 연속 중복 억제 */
    if (t->n_steps == JOURNEY_STEPS) {          /* 가득 참 — 마지막 칸 갱신 */
        t->steps[JOURNEY_STEPS - 1].step = step;
        t->steps[JOURNEY_STEPS - 1].ts = now;
        return;
    }
    t->steps[t->n_steps].step = step;
    t->steps[t->n_steps].ts = now;
    t->n_steps++;
}

void journey_note(Journey *j, int track_id, JourneyStep step, double now)
{
    if (!j) return;
    TrackJourney *t = slot_get(j, track_id, now);
    if (!t) return;
    t->last_seen = now;
    if (t->n_steps == 0 && step != STEP_ENTER)  /* 첫 기록은 항상 입장부터 */
        step_append(t, STEP_ENTER, now);
    step_append(t, step, now);
}

void journey_touch(Journey *j, int track_id, double now)
{
    if (!j) return;
    TrackJourney *t = slot_get(j, track_id, now);
    if (!t) return;
    t->last_seen = now;
    if (t->n_steps == 0) step_append(t, STEP_ENTER, now);
}

int journey_last_step(const Journey *j, int track_id)
{
    if (!j) return -1;
    const TrackJourney *t = slot_find((Journey *)j, track_id);
    if (!t || t->n_steps == 0) return -1;
    return (int)t->steps[t->n_steps - 1].step;
}

bool journey_visited(const Journey *j, int track_id, JourneyStep step)
{
    if (!j) return false;
    const TrackJourney *t = slot_find((Journey *)j, track_id);
    if (!t) return false;
    for (int i = 0; i < t->n_steps; i++)
        if (t->steps[i].step == step) return true;
    return false;
}

static void format_ts(const Journey *j, double mono_ts, char *out, size_t len)
{
    time_t wall = (time_t)(mono_ts + j->wall_offset);
    struct tm tm_buf;
    os_localtime(&wall, &tm_buf);
    strftime(out, len, "%H:%M:%S", &tm_buf);
}

void journey_format(const Journey *j, int track_id, char *buf, size_t len)
{
    buf[0] = 0;
    if (!j) return;
    const TrackJourney *t = slot_find((Journey *)j, track_id);
    if (!t || t->n_steps == 0) return;
    size_t o = 0;
    for (int i = 0; i < t->n_steps && o + 32 < len; i++) {
        char ts[16];
        format_ts(j, t->steps[i].ts, ts, sizeof(ts));
        o += (size_t)snprintf(buf + o, len - o, "%s%s %s",
                              i ? " → " : "", ts, STEP_STR[t->steps[i].step]);
    }
}

/* 퇴장 트랙 1명 = 레코드 1건 (SQLite + JSONL). 방문·전환율 분석용. */
static void journey_store_record(const Journey *j, const TrackJourney *t)
{
    if (!datastore_enabled() || t->n_steps == 0) return;

    double enter_ts = t->steps[0].ts + j->wall_offset;
    double exit_ts = t->steps[t->n_steps - 1].ts + j->wall_offset;
    time_t enter_wall = (time_t)enter_ts;
    struct tm tm_buf;
    os_localtime(&enter_wall, &tm_buf);
    char enter_str[24];
    strftime(enter_str, sizeof(enter_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

    bool kiosk = false, sat = false, purchased = false;
    for (int i = 0; i < t->n_steps; i++) {
        if (t->steps[i].step == STEP_KIOSK) kiosk = true;
        if (t->steps[i].step == STEP_SIT) sat = true;
        if (t->steps[i].step == STEP_PURCHASE) purchased = true;
    }

    char steps[768];
    size_t o = 0;
    o += (size_t)snprintf(steps + o, sizeof(steps) - o, "[");
    for (int i = 0; i < t->n_steps && o + 64 < sizeof(steps); i++) {
        char hms[16];
        format_ts(j, t->steps[i].ts, hms, sizeof(hms));
        o += (size_t)snprintf(steps + o, sizeof(steps) - o,
                              "%s{\"step\":\"%s\",\"ts\":%.3f,\"time\":\"%s\"}",
                              i ? "," : "", STEP_KEY[t->steps[i].step],
                              t->steps[i].ts + j->wall_offset, hms);
    }
    snprintf(steps + o, sizeof(steps) - o, "]");
    datastore_journey(t->track_id, enter_str, enter_ts, exit_ts,
                      kiosk, sat, purchased, steps);
}

void journey_gc(Journey *j, double now, double ttl)
{
    if (!j) return;
    for (int i = 0; i < JOURNEY_TRACKS; i++) {
        TrackJourney *t = &j->tracks[i];
        if (!t->used || now - t->last_seen <= ttl) continue;
        step_append(t, STEP_EXIT, now);
        char line[512];
        journey_format(j, t->track_id, line, sizeof(line));
        LOGI("동선", "ID %d: %s", t->track_id, line);
        journey_store_record(j, t);
        t->used = false;
    }
}
