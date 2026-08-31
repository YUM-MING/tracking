#include "datastore.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "os_compat.h"
#ifdef _WIN32
#include <direct.h>
#define os_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define os_mkdir(p) mkdir((p), 0755)
#endif

#include "sqlite3.h"

#define DS_DIR_MAX 512
#define DS_DATE_MAX 12         /* "YYYY-MM-DD" */
#define DS_LINE_MAX 1536

typedef enum { DS_EVENTS = 0, DS_JOURNEYS, DS_STREAM_COUNT } DsStream;

static const char *STREAM_PREFIX[DS_STREAM_COUNT] = { "events", "journeys" };

static struct {
    pthread_mutex_t lock;
    bool enabled;
    char dir[DS_DIR_MAX];
    FILE *file[DS_STREAM_COUNT];
    char date[DS_STREAM_COUNT][DS_DATE_MAX];   /* 열려 있는 JSONL 파일의 날짜 */
    sqlite3 *db;
    sqlite3_stmt *ins_event;
    sqlite3_stmt *ins_journey;
} g_ds = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void local_date(char *out, size_t len)
{
    time_t now = time(NULL);
    struct tm tm_buf;
    os_localtime(&now, &tm_buf);
    strftime(out, len, "%Y-%m-%d", &tm_buf);
}

/* ── SQLite (인계용 원본 DB — 파일 하나 복사로 전체 데이터 전달) ── */
static const char *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS events ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts REAL, time TEXT, type TEXT, track_id INTEGER,"
    "  message TEXT, journey TEXT);"
    "CREATE TABLE IF NOT EXISTS journeys ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  track_id INTEGER, enter TEXT, enter_ts REAL, exit_ts REAL,"
    "  duration_sec REAL, visited_kiosk INTEGER, sat INTEGER,"
    "  purchased INTEGER, steps TEXT);"
    "CREATE INDEX IF NOT EXISTS idx_events_ts ON events(ts);"
    "CREATE INDEX IF NOT EXISTS idx_journeys_enter ON journeys(enter_ts);";

/* 락 보유 상태에서 호출 */
static bool db_open(void)
{
    char path[DS_DIR_MAX + 32];
    snprintf(path, sizeof(path), "%s/tracking.db", g_ds.dir);
    if (sqlite3_open(path, &g_ds.db) != SQLITE_OK) {
        fprintf(stderr, "[데이터] SQLite 열기 실패: %s\n",
                g_ds.db ? sqlite3_errmsg(g_ds.db) : path);
        if (g_ds.db) { sqlite3_close(g_ds.db); g_ds.db = NULL; }
        return false;
    }
    sqlite3_busy_timeout(g_ds.db, 2000);
    /* WAL 대신 기본 저널: "파일 하나만 복사하면 그대로 보인다" 요구 충족 */
    sqlite3_exec(g_ds.db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    char *err = NULL;
    if (sqlite3_exec(g_ds.db, SCHEMA_SQL, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "[데이터] 스키마 생성 실패: %s\n", err ? err : "?");
        sqlite3_free(err);
        sqlite3_close(g_ds.db);
        g_ds.db = NULL;
        return false;
    }
    sqlite3_prepare_v2(g_ds.db,
        "INSERT INTO events(ts,time,type,track_id,message,journey)"
        " VALUES(?,?,?,?,?,?)", -1, &g_ds.ins_event, NULL);
    sqlite3_prepare_v2(g_ds.db,
        "INSERT INTO journeys(track_id,enter,enter_ts,exit_ts,duration_sec,"
        " visited_kiosk,sat,purchased,steps) VALUES(?,?,?,?,?,?,?,?,?)",
        -1, &g_ds.ins_journey, NULL);
    return g_ds.ins_event && g_ds.ins_journey;
}

void datastore_init(const char *dir)
{
    if (!dir || !dir[0]) return;
    pthread_mutex_lock(&g_ds.lock);
    snprintf(g_ds.dir, sizeof(g_ds.dir), "%s", dir);
    if (os_mkdir(g_ds.dir) != 0 && errno != EEXIST) {
        fprintf(stderr, "[데이터] 디렉터리 생성 실패: %s — 축적 비활성\n", g_ds.dir);
        pthread_mutex_unlock(&g_ds.lock);
        return;
    }
    g_ds.enabled = db_open();
    pthread_mutex_unlock(&g_ds.lock);
    if (g_ds.enabled)
        fprintf(stderr, "[데이터] 축적 시작: %s/tracking.db (+ 날짜별 JSONL)\n", dir);
}

void datastore_shutdown(void)
{
    pthread_mutex_lock(&g_ds.lock);
    for (int i = 0; i < DS_STREAM_COUNT; i++) {
        if (g_ds.file[i]) {
            fclose(g_ds.file[i]);
            g_ds.file[i] = NULL;
        }
    }
    if (g_ds.ins_event) { sqlite3_finalize(g_ds.ins_event); g_ds.ins_event = NULL; }
    if (g_ds.ins_journey) { sqlite3_finalize(g_ds.ins_journey); g_ds.ins_journey = NULL; }
    if (g_ds.db) { sqlite3_close(g_ds.db); g_ds.db = NULL; }
    g_ds.enabled = false;
    pthread_mutex_unlock(&g_ds.lock);
}

bool datastore_enabled(void)
{
    return g_ds.enabled;
}

/* ── JSONL 병행 기록 (grep/pandas 즉석 분석용) ── */

/* 락 보유 상태에서 호출. 날짜가 바뀌었으면 파일을 교체(자정 롤오버). */
static FILE *stream_file(DsStream st)
{
    char today[DS_DATE_MAX];
    local_date(today, sizeof(today));
    if (g_ds.file[st] && strcmp(g_ds.date[st], today) == 0)
        return g_ds.file[st];

    if (g_ds.file[st]) fclose(g_ds.file[st]);
    char path[DS_DIR_MAX + 64];
    snprintf(path, sizeof(path), "%s/%s_%s.jsonl",
             g_ds.dir, STREAM_PREFIX[st], today);
    g_ds.file[st] = fopen(path, "a");
    if (!g_ds.file[st]) {
        fprintf(stderr, "[데이터] 파일 열기 실패: %s\n", path);
        return NULL;
    }
    snprintf(g_ds.date[st], sizeof(g_ds.date[st]), "%s", today);
    return g_ds.file[st];
}

/* 락 보유 상태에서 호출 */
static void jsonl_append(DsStream st, const char *json_line)
{
    FILE *f = stream_file(st);
    if (f) {
        fputs(json_line, f);
        fputc('\n', f);
        fflush(f);
    }
}

/* JSON 문자열 이스케이프 (따옴표/역슬래시/제어문자만, UTF-8은 그대로) */
static void esc_json(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in;
         *p && o + 7 < out_len; p++) {
        if (*p == '"' || *p == '\\') {
            out[o++] = '\\';
            out[o++] = (char)*p;
        } else if (*p < 0x20) {
            o += (size_t)snprintf(out + o, out_len - o, "\\u%04x", *p);
        } else {
            out[o++] = (char)*p;
        }
    }
    out[o] = 0;
}

void datastore_event(const char *type, int track_id,
                     const char *message, const char *journey)
{
    if (!g_ds.enabled) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double unix_ts = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    time_t sec = (time_t)ts.tv_sec;
    struct tm tm_buf;
    os_localtime(&sec, &tm_buf);
    char when[24];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm_buf);

    const char *msg = message ? message : "";
    const char *jny = journey ? journey : "";

    char esc_msg[400], esc_journey[480];
    esc_json(msg, esc_msg, sizeof(esc_msg));
    esc_json(jny, esc_journey, sizeof(esc_journey));

    char line[DS_LINE_MAX];
    snprintf(line, sizeof(line),
             "{\"ts\":%.3f,\"time\":\"%s\",\"type\":\"%s\",\"track_id\":%d,"
             "\"message\":\"%s\",\"journey\":\"%s\"}",
             unix_ts, when, type, track_id, esc_msg, esc_journey);

    pthread_mutex_lock(&g_ds.lock);
    jsonl_append(DS_EVENTS, line);
    if (g_ds.ins_event) {
        sqlite3_bind_double(g_ds.ins_event, 1, unix_ts);
        sqlite3_bind_text(g_ds.ins_event, 2, when, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(g_ds.ins_event, 3, type, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(g_ds.ins_event, 4, track_id);
        sqlite3_bind_text(g_ds.ins_event, 5, msg, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(g_ds.ins_event, 6, jny, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(g_ds.ins_event) != SQLITE_DONE)
            fprintf(stderr, "[데이터] events INSERT 실패: %s\n",
                    sqlite3_errmsg(g_ds.db));
        sqlite3_reset(g_ds.ins_event);
    }
    pthread_mutex_unlock(&g_ds.lock);
}

void datastore_journey(int track_id, const char *enter_str,
                       double enter_ts, double exit_ts,
                       bool visited_kiosk, bool sat, bool purchased,
                       const char *steps_json)
{
    if (!g_ds.enabled) return;

    char line[DS_LINE_MAX];
    snprintf(line, sizeof(line),
             "{\"track_id\":%d,\"enter\":\"%s\",\"enter_ts\":%.3f,"
             "\"exit_ts\":%.3f,\"duration_sec\":%.1f,\"visited_kiosk\":%s,"
             "\"sat\":%s,\"purchased\":%s,\"steps\":%s}",
             track_id, enter_str, enter_ts, exit_ts, exit_ts - enter_ts,
             visited_kiosk ? "true" : "false", sat ? "true" : "false",
             purchased ? "true" : "false", steps_json);

    pthread_mutex_lock(&g_ds.lock);
    jsonl_append(DS_JOURNEYS, line);
    if (g_ds.ins_journey) {
        sqlite3_bind_int(g_ds.ins_journey, 1, track_id);
        sqlite3_bind_text(g_ds.ins_journey, 2, enter_str, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(g_ds.ins_journey, 3, enter_ts);
        sqlite3_bind_double(g_ds.ins_journey, 4, exit_ts);
        sqlite3_bind_double(g_ds.ins_journey, 5, exit_ts - enter_ts);
        sqlite3_bind_int(g_ds.ins_journey, 6, visited_kiosk ? 1 : 0);
        sqlite3_bind_int(g_ds.ins_journey, 7, sat ? 1 : 0);
        sqlite3_bind_int(g_ds.ins_journey, 8, purchased ? 1 : 0);
        sqlite3_bind_text(g_ds.ins_journey, 9, steps_json, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(g_ds.ins_journey) != SQLITE_DONE)
            fprintf(stderr, "[데이터] journeys INSERT 실패: %s\n",
                    sqlite3_errmsg(g_ds.db));
        sqlite3_reset(g_ds.ins_journey);
    }
    pthread_mutex_unlock(&g_ds.lock);
}
