#include "datastore.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>

#define DS_DIR_MAX 512
#define DS_DATE_MAX 12         /* "YYYY-MM-DD" */
#define DS_LINE_MAX 1536

static const char *STREAM_PREFIX[DS_STREAM_COUNT] = { "events", "journeys" };

static struct {
    pthread_mutex_t lock;
    bool enabled;
    char dir[DS_DIR_MAX];
    FILE *file[DS_STREAM_COUNT];
    char date[DS_STREAM_COUNT][DS_DATE_MAX];   /* 열려 있는 파일의 날짜 */
} g_ds = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void local_date(char *out, size_t len)
{
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    strftime(out, len, "%Y-%m-%d", &tm_buf);
}

void datastore_init(const char *dir)
{
    if (!dir || !dir[0]) return;
    pthread_mutex_lock(&g_ds.lock);
    snprintf(g_ds.dir, sizeof(g_ds.dir), "%s", dir);
    if (mkdir(g_ds.dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "[데이터] 디렉터리 생성 실패: %s — 축적 비활성\n", g_ds.dir);
        pthread_mutex_unlock(&g_ds.lock);
        return;
    }
    g_ds.enabled = true;
    pthread_mutex_unlock(&g_ds.lock);
    fprintf(stderr, "[데이터] JSONL 축적 시작: %s/{events,journeys}_YYYY-MM-DD.jsonl\n",
            dir);
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
    g_ds.enabled = false;
    pthread_mutex_unlock(&g_ds.lock);
}

bool datastore_enabled(void)
{
    return g_ds.enabled;
}

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

void datastore_append(DsStream st, const char *json_line)
{
    if (!g_ds.enabled || st < 0 || st >= DS_STREAM_COUNT) return;
    pthread_mutex_lock(&g_ds.lock);
    FILE *f = stream_file(st);
    if (f) {
        fputs(json_line, f);
        fputc('\n', f);
        fflush(f);
    }
    pthread_mutex_unlock(&g_ds.lock);
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
    struct tm tm_buf;
    localtime_r(&ts.tv_sec, &tm_buf);
    char when[24];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm_buf);

    char esc_msg[400], esc_journey[480];
    esc_json(message ? message : "", esc_msg, sizeof(esc_msg));
    esc_json(journey ? journey : "", esc_journey, sizeof(esc_journey));

    char line[DS_LINE_MAX];
    snprintf(line, sizeof(line),
             "{\"ts\":%.3f,\"time\":\"%s\",\"type\":\"%s\",\"track_id\":%d,"
             "\"message\":\"%s\",\"journey\":\"%s\"}",
             unix_ts, when, type, track_id, esc_msg, esc_journey);
    datastore_append(DS_EVENTS, line);
}
