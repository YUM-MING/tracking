#include "logger.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define LOG_RING_CAP 128       /* 점주 페이지에서 열람 가능한 최근 로그 수 */
#define LOG_LINE_MAX 240

typedef struct {
    long   seq;
    char   ts[16];             /* "HH:MM:SS" */
    char   level;              /* D/I/W/E */
    char   tag[16];
    char   text[LOG_LINE_MAX];
} LogEntry;

static struct {
    pthread_mutex_t lock;
    LogLevel min_level;
    FILE *file;
    LogEntry ring[LOG_RING_CAP];
    long seq;                  /* 전체 발행 수 (ring 인덱스 = seq % CAP) */
} g_log = { .lock = PTHREAD_MUTEX_INITIALIZER, .min_level = LOGL_INFO };

static const char LEVEL_CH[] = { 'D', 'I', 'W', 'E' };

void log_init(LogLevel min_level, const char *file_path)
{
    pthread_mutex_lock(&g_log.lock);
    g_log.min_level = min_level;
    if (file_path && !g_log.file) {
        g_log.file = fopen(file_path, "a");
        if (!g_log.file)
            fprintf(stderr, "[logger] 로그 파일 열기 실패: %s\n", file_path);
    }
    pthread_mutex_unlock(&g_log.lock);
}

void log_set_level(LogLevel min_level)
{
    pthread_mutex_lock(&g_log.lock);
    g_log.min_level = min_level;
    pthread_mutex_unlock(&g_log.lock);
}

void log_shutdown(void)
{
    pthread_mutex_lock(&g_log.lock);
    if (g_log.file) {
        fclose(g_log.file);
        g_log.file = NULL;
    }
    pthread_mutex_unlock(&g_log.lock);
}

void log_msg(LogLevel lv, const char *tag, const char *fmt, ...)
{
    if (lv < g_log.min_level) return;

    char text[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char ts[16];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm_buf);
    /* 파일 로그에는 날짜 포함 (여러 날 파일럿 시 며칠째인지 구분) */
    char ts_full[24];
    strftime(ts_full, sizeof(ts_full), "%Y-%m-%d %H:%M:%S", &tm_buf);

    pthread_mutex_lock(&g_log.lock);
    LogEntry *e = &g_log.ring[g_log.seq % LOG_RING_CAP];
    e->seq = g_log.seq++;
    snprintf(e->ts, sizeof(e->ts), "%s", ts);
    e->level = LEVEL_CH[lv];
    snprintf(e->tag, sizeof(e->tag), "%s", tag);
    snprintf(e->text, sizeof(e->text), "%s", text);

    fprintf(stderr, "[%s][%c][%s] %s\n", ts, e->level, tag, text);
    if (g_log.file) {
        fprintf(g_log.file, "[%s][%c][%s] %s\n", ts_full, e->level, tag, text);
        fflush(g_log.file);
    }
    pthread_mutex_unlock(&g_log.lock);
}

/* JSON 문자열 이스케이프 (따옴표/역슬래시/제어문자만, UTF-8은 그대로) */
static size_t esc_json(const char *in, char *out, size_t out_len)
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
    return o;
}

int log_recent_json(char *buf, size_t len)
{
    pthread_mutex_lock(&g_log.lock);
    long total = g_log.seq;
    long n = total < LOG_RING_CAP ? total : LOG_RING_CAP;
    size_t o = 0;
    o += (size_t)snprintf(buf + o, len - o, "[");
    for (long i = total - n; i < total && o + 400 < len; i++) {
        const LogEntry *e = &g_log.ring[i % LOG_RING_CAP];
        char esc[LOG_LINE_MAX * 2];
        esc_json(e->text, esc, sizeof(esc));
        o += (size_t)snprintf(buf + o, len - o,
                              "%s{\"ts\":\"%s\",\"lv\":\"%c\",\"tag\":\"%s\",\"msg\":\"%s\"}",
                              i == total - n ? "" : ",", e->ts, e->level, e->tag, esc);
    }
    o += (size_t)snprintf(buf + o, len - o, "]");
    pthread_mutex_unlock(&g_log.lock);
    return (int)o;
}
