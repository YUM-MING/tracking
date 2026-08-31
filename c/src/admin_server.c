#include "admin_server.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "os_compat.h"
#ifndef _WIN32
#include <netinet/in.h>
#include <sys/select.h>
#endif

#ifdef _WIN32
/* mingw에는 strcasestr가 없다 (POSIX 확장) */
static const char *win_strcasestr(const char *h, const char *n)
{
    size_t nl = strlen(n);
    for (; *h; h++)
        if (strncasecmp(h, n, nl) == 0) return h;
    return NULL;
}
#define strcasestr win_strcasestr
#endif

#include "camhealth.h"
#include "logger.h"
#include "resource.h"

#define EV_RING_CAP 64         /* 점주 페이지에서 열람 가능한 최근 이벤트 수 */
#define PURCHASE_CAP 32
#define REQ_MAX (16 * 1024)
#define RESP_MAX (192 * 1024)  /* HTML 페이지 + 이벤트 JSON 여유분 */

typedef struct {
    long id;
    char type[24];
    int track_id;
    char wall_ts[16];          /* "HH:MM:SS" */
    char message[192];
    char journey[224];
} AdminEvent;

typedef struct {
    int track_id;
    float x1, y1, x2, y2;      /* 0~1 정규화 좌표 */
    char reason[16];
} LivePerson;

struct AdminServer {
    const PipelineConfig *cfg;
    SettingsStore *settings;

    pthread_t thread;
    bool has_thread;
    _Atomic bool stop;
    sock_t listen_fd;

    pthread_mutex_t lock;      /* 아래 공유 스냅샷 전부 보호 */
    AdminStatus status;
    LivePerson people[MAX_PEOPLE];
    int n_people;
    int frame_w, frame_h;
    AdminEvent events[EV_RING_CAP];
    long ev_seq;               /* 발행 이벤트 총수 (ring 인덱스 = seq % CAP) */
    int purchases[PURCHASE_CAP];
    int purchase_items[PURCHASE_CAP];   /* 결제당 잔/개수 (일행 대조용) */
    int n_purchases;
    CalibRequest calib_req;    /* 캘리브레이션 메일박스 (1회성) */
    int calib_minutes;

    /* ── 오탐율 집계 + 목표치 자동 제어 (룰별) ──
     * total = 발행된 알림 수, fp = 점주가 '오탐 신고'한 수.
     * 표본이 충분해진 뒤(≥10) 오탐율이 목표(fp_target_pct)를 넘으면
     * 해당 룰 임계값을 자동으로 한 단계 보수화하고 창을 리셋한다. */
    long fp_total[EVK_COUNT];
    long fp_count[EVK_COUNT];

    char resp[RESP_MAX];       /* 단일 스레드 처리라 응답 버퍼는 1개면 충분 */
};

/* ── 공유 상태 갱신 API (파이프라인 쪽) ─────────────── */

void admin_update_status(AdminServer *a, const AdminStatus *st)
{
    if (!a) return;
    pthread_mutex_lock(&a->lock);
    a->status = *st;
    pthread_mutex_unlock(&a->lock);
}

void admin_update_people(AdminServer *a, const TrackedPerson *people, int n,
                         const PrecisionTarget *targets, int n_targets,
                         int frame_w, int frame_h)
{
    if (!a) return;
    float fw = frame_w > 0 ? (float)frame_w : 1.f;
    float fh = frame_h > 0 ? (float)frame_h : 1.f;
    pthread_mutex_lock(&a->lock);
    a->frame_w = frame_w;
    a->frame_h = frame_h;
    a->n_people = n > MAX_PEOPLE ? MAX_PEOPLE : n;
    for (int i = 0; i < a->n_people; i++) {
        LivePerson *lp = &a->people[i];
        lp->track_id = people[i].track_id;
        lp->x1 = people[i].bbox[0] / fw;
        lp->y1 = people[i].bbox[1] / fh;
        lp->x2 = people[i].bbox[2] / fw;
        lp->y2 = people[i].bbox[3] / fh;
        TriggerReason reason = REASON_NONE;
        for (int j = 0; j < n_targets; j++)
            if (targets[j].track_id == people[i].track_id) {
                reason = targets[j].reason;
                break;
            }
        snprintf(lp->reason, sizeof(lp->reason), "%s", reason_str(reason));
    }
    pthread_mutex_unlock(&a->lock);
}

void admin_push_event(AdminServer *a, const Event *ev)
{
    if (!a) return;
    time_t now = time(NULL);
    struct tm tm_buf;
    os_localtime(&now, &tm_buf);

    pthread_mutex_lock(&a->lock);
    AdminEvent *e = &a->events[a->ev_seq % EV_RING_CAP];
    e->id = ++a->ev_seq;
    snprintf(e->type, sizeof(e->type), "%s", event_kind_str(ev->kind));
    e->track_id = ev->track_id;
    strftime(e->wall_ts, sizeof(e->wall_ts), "%H:%M:%S", &tm_buf);
    snprintf(e->message, sizeof(e->message), "%s", ev->message);
    snprintf(e->journey, sizeof(e->journey), "%s", ev->journey);
    if ((int)ev->kind < (int)EVK_COUNT)
        a->fp_total[ev->kind]++;               /* 오탐율 분모 집계 */
    pthread_mutex_unlock(&a->lock);
}

CalibRequest admin_take_calib(AdminServer *a, int *minutes)
{
    if (!a) return CALIB_REQ_NONE;
    pthread_mutex_lock(&a->lock);
    CalibRequest req = a->calib_req;
    *minutes = a->calib_minutes;
    a->calib_req = CALIB_REQ_NONE;
    pthread_mutex_unlock(&a->lock);
    return req;
}

int admin_take_purchases(AdminServer *a, int *track_ids, int *items, int max)
{
    if (!a) return 0;
    pthread_mutex_lock(&a->lock);
    int n = a->n_purchases < max ? a->n_purchases : max;
    memcpy(track_ids, a->purchases, (size_t)n * sizeof(int));
    memcpy(items, a->purchase_items, (size_t)n * sizeof(int));
    if (n < a->n_purchases) {
        memmove(a->purchases, a->purchases + n,
                (size_t)(a->n_purchases - n) * sizeof(int));
        memmove(a->purchase_items, a->purchase_items + n,
                (size_t)(a->n_purchases - n) * sizeof(int));
    }
    a->n_purchases -= n;
    pthread_mutex_unlock(&a->lock);
    return n;
}

/* ── JSON 헬퍼 ────────────────────────────────────────── */

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

/* ── 라우트별 본문 생성 (a->lock 없이 호출, 내부에서 잠깐씩만 잠근다) ── */

static int body_status(AdminServer *a, char *buf, size_t len)
{
    pthread_mutex_lock(&a->lock);
    AdminStatus st = a->status;
    pthread_mutex_unlock(&a->lock);
    return snprintf(buf, len,
        "{\"uptime\":%.0f,\"infer_fps\":%.1f,\"people\":%d,"
        "\"cpu\":%.1f,\"rss\":%.0f,\"cam_state\":%d,\"cam_state_str\":\"%s\","
        "\"mode\":\"%s\",\"calib_pct\":%d,"
        "\"dropped\":%llu,\"ws\":%s,\"settings_version\":%ld}",
        st.uptime_sec, st.infer_fps, st.n_people, st.cpu_pct, st.rss_mb,
        st.cam_state, st.cam_state_str ? st.cam_state_str : "정상",
        st.mode ? st.mode : "정상", st.calib_pct,
        (unsigned long long)st.frames_dropped,
        st.ws_enabled ? "true" : "false", st.settings_version);
}

static int body_events(AdminServer *a, long since, char *buf, size_t len)
{
    size_t o = 0;
    pthread_mutex_lock(&a->lock);
    o += (size_t)snprintf(buf + o, len - o, "{\"last_id\":%ld,\"events\":[",
                          a->ev_seq);
    long lo = a->ev_seq - EV_RING_CAP + 1;
    if (lo < 1) lo = 1;
    if (since + 1 > lo) lo = since + 1;
    bool first = true;
    for (long id = lo; id <= a->ev_seq && o + 900 < len; id++) {
        const AdminEvent *e = &a->events[(id - 1) % EV_RING_CAP];
        if (e->id != id) continue;               /* 덮어써진 슬롯 방어 */
        char em[384], ej[448];
        esc_json(e->message, em, sizeof(em));
        esc_json(e->journey, ej, sizeof(ej));
        o += (size_t)snprintf(buf + o, len - o,
            "%s{\"id\":%ld,\"type\":\"%s\",\"track_id\":%d,"
            "\"ts\":\"%s\",\"message\":\"%s\",\"journey\":\"%s\"}",
            first ? "" : ",", e->id, e->type, e->track_id,
            e->wall_ts, em, ej);
        first = false;
    }
    pthread_mutex_unlock(&a->lock);
    o += (size_t)snprintf(buf + o, len - o, "]}");
    return (int)o;
}

static int body_live(AdminServer *a, char *buf, size_t len)
{
    size_t o = 0;
    pthread_mutex_lock(&a->lock);
    o += (size_t)snprintf(buf + o, len - o, "{\"w\":%d,\"h\":%d,\"people\":[",
                          a->frame_w, a->frame_h);
    for (int i = 0; i < a->n_people && o + 200 < len; i++) {
        const LivePerson *p = &a->people[i];
        o += (size_t)snprintf(buf + o, len - o,
            "%s{\"id\":%d,\"x1\":%.4f,\"y1\":%.4f,\"x2\":%.4f,\"y2\":%.4f,"
            "\"reason\":\"%s\"}",
            i ? "," : "", p->track_id, (double)p->x1, (double)p->y1,
            (double)p->x2, (double)p->y2, p->reason);
    }
    pthread_mutex_unlock(&a->lock);
    o += (size_t)snprintf(buf + o, len - o, "]}");
    return (int)o;
}

/* owner_page.html은 실행 파일 옆에서 매 요청 시 읽는다 (재빌드 없이 수정 가능) */
static int body_page(char *buf, size_t len)
{
    FILE *f = fopen("owner_page.html", "r");
    if (!f) {
        return snprintf(buf, len,
            "<meta charset=utf-8><h3>owner_page.html 없음</h3>"
            "<p>실행 파일과 같은 폴더에 owner_page.html이 필요합니다 "
            "(빌드 시 자동 복사됨).</p>");
    }
    size_t n = fread(buf, 1, len - 1, f);
    fclose(f);
    buf[n] = 0;
    return (int)n;
}

/* ── HTTP 처리 ───────────────────────────────────────── */

static void send_response(int fd, int code, const char *status,
                          const char *ctype, const char *body, int body_len)
{
    char hdr[256];
    int hl = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        code, status, ctype, body_len);
    send(fd, hdr, (size_t)hl, 0);
    int off = 0;
    while (off < body_len) {
        int n = send(fd, body + off, (size_t)(body_len - off), 0);
        if (n <= 0) return;
        off += (int)n;
    }
}

static void send_json(int fd, int code, const char *body, int body_len)
{
    send_response(fd, code, code == 200 ? "OK" : "Bad Request",
                  "application/json", body, body_len);
}

/* 본문에서 "track_id": N 같은 정수 하나 뽑기 */
static bool body_int(const char *body, const char *key, long *out)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ':' || *p == ' ') p++;
    char *end;
    long v = strtol(p, &end, 10);
    if (end == p) return false;
    *out = v;
    return true;
}

static bool body_str(const char *body, const char *key, char *out, size_t out_len)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), '"');
    if (!p) return false;
    p++;
    const char *e = strchr(p, '"');
    if (!e || (size_t)(e - p) >= out_len) return false;
    memcpy(out, p, (size_t)(e - p));
    out[e - p] = 0;
    return true;
}

static void handle_request(AdminServer *a, int fd, const char *method,
                           const char *path, const char *body)
{
    char *resp = a->resp;

    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/") == 0 || strncmp(path, "/index", 6) == 0) {
            int n = body_page(resp, RESP_MAX);
            send_response(fd, 200, "OK", "text/html", resp, n);
            return;
        }
        if (strcmp(path, "/api/status") == 0) {
            send_json(fd, 200, resp, body_status(a, resp, RESP_MAX));
            return;
        }
        if (strcmp(path, "/api/settings") == 0) {
            send_json(fd, 200, resp, settings_to_json(a->settings, resp, RESP_MAX));
            return;
        }
        if (strncmp(path, "/api/events", 11) == 0) {
            long since = 0;
            const char *q = strstr(path, "since=");
            if (q) since = strtol(q + 6, NULL, 10);
            send_json(fd, 200, resp, body_events(a, since, resp, RESP_MAX));
            return;
        }
        if (strcmp(path, "/api/live") == 0) {
            send_json(fd, 200, resp, body_live(a, resp, RESP_MAX));
            return;
        }
        if (strcmp(path, "/api/logs") == 0) {
            send_json(fd, 200, resp, log_recent_json(resp, RESP_MAX));
            return;
        }
        if (strcmp(path, "/api/fpstats") == 0) {
            /* 룰별 (알림 수, 오탐 수, 오탐율) — 점주 페이지 신뢰도 표 */
            size_t o = 0;
            pthread_mutex_lock(&a->lock);
            o += (size_t)snprintf(resp + o, RESP_MAX - o, "[");
            bool first = true;
            for (int k = 0; k < (int)EVK_COUNT; k++) {
                if (a->fp_total[k] == 0) continue;
                o += (size_t)snprintf(resp + o, RESP_MAX - o,
                    "%s{\"kind\":\"%s\",\"total\":%ld,\"fp\":%ld,\"rate\":%.1f}",
                    first ? "" : ",", event_kind_str((EventKind)k),
                    a->fp_total[k], a->fp_count[k],
                    100.0 * (double)a->fp_count[k] / (double)a->fp_total[k]);
                first = false;
            }
            pthread_mutex_unlock(&a->lock);
            o += (size_t)snprintf(resp + o, RESP_MAX - o, "]");
            send_json(fd, 200, resp, (int)o);
            return;
        }
    } else if (strcmp(method, "POST") == 0) {
        if (strcmp(path, "/api/settings") == 0) {
            settings_apply_json(a->settings, body);
            send_json(fd, 200, resp, settings_to_json(a->settings, resp, RESP_MAX));
            return;
        }
        if (strcmp(path, "/api/purchase") == 0) {
            long tid;
            if (!body_int(body, "track_id", &tid)) {
                send_json(fd, 400, "{\"ok\":false,\"error\":\"track_id 필요\"}", 36);
                return;
            }
            long items = 1;                      /* 잔/개수 — 없으면 1건 */
            body_int(body, "items", &items);
            if (items < 1) items = 1;
            if (items > 20) items = 20;
            pthread_mutex_lock(&a->lock);
            if (a->n_purchases < PURCHASE_CAP) {
                a->purchases[a->n_purchases] = (int)tid;
                a->purchase_items[a->n_purchases] = (int)items;
                a->n_purchases++;
            }
            pthread_mutex_unlock(&a->lock);
            LOGI("POS", "결제 웹훅 수신: track_id=%ld, %ld잔", tid, items);
            send_json(fd, 200, "{\"ok\":true}", 11);
            return;
        }
        /* 캘리브레이션(세팅) 제어: 점주/엔지니어가 매장을 비운 상태로
         * [세팅 시작하기] → N분간 잡히는 '사람'은 전부 오탐원 → 자동 마스킹 */
        if (strcmp(path, "/api/calibrate") == 0) {
            char action[16];
            if (!body_str(body, "action", action, sizeof(action))) {
                send_json(fd, 400, "{\"ok\":false,\"error\":\"action 필요\"}", 35);
                return;
            }
            long minutes = 60;                   /* 기본 1시간 (회의 요구사항) */
            body_int(body, "minutes", &minutes);
            if (minutes < 1) minutes = 1;
            if (minutes > 240) minutes = 240;

            CalibRequest req = CALIB_REQ_NONE;
            if (strcmp(action, "start") == 0)      req = CALIB_REQ_START;
            else if (strcmp(action, "stop") == 0)  req = CALIB_REQ_STOP;
            else if (strcmp(action, "clear") == 0) req = CALIB_REQ_CLEAR;
            if (req == CALIB_REQ_NONE) {
                send_json(fd, 400, "{\"ok\":false,\"error\":\"알 수 없는 action\"}", 44);
                return;
            }
            pthread_mutex_lock(&a->lock);
            a->calib_req = req;
            a->calib_minutes = (int)minutes;
            pthread_mutex_unlock(&a->lock);
            LOGI("점주", "캘리브레이션 요청: %s (%ld분)", action, minutes);
            send_json(fd, 200, "{\"ok\":true}", 11);
            return;
        }
        if (strcmp(path, "/api/feedback") == 0) {
            char kind[32], desc[128];
            if (!body_str(body, "kind", kind, sizeof(kind))) {
                send_json(fd, 400, "{\"ok\":false,\"error\":\"kind 필요\"}", 33);
                return;
            }
            bool ok = settings_feedback(a->settings, kind, desc, sizeof(desc));

            /* ── 오탐율 폐루프: 신고 집계 → 목표 초과 시 추가 자동 보정 ── */
            EventKind ek = event_kind_from_str(kind);
            if (ek != EVK_COUNT) {
                StoreSettings st;
                settings_get(a->settings, &st);
                pthread_mutex_lock(&a->lock);
                a->fp_count[ek]++;
                long total = a->fp_total[ek], fp = a->fp_count[ek];
                bool over = total >= 10 &&
                            (double)fp * 100.0 > st.fp_target_pct * (double)total;
                if (over) {                     /* 새 창에서 다시 측정 */
                    a->fp_total[ek] = 0;
                    a->fp_count[ek] = 0;
                }
                pthread_mutex_unlock(&a->lock);
                if (over) {
                    char extra[128];
                    settings_feedback(a->settings, kind, extra, sizeof(extra));
                    LOGW("오탐율", "%s 오탐율 %.0f%% > 목표 %.0f%% — 추가 자동 보정: %s",
                         kind, 100.0 * fp / total, st.fp_target_pct, extra);
                }
            }

            char em[256];
            esc_json(desc, em, sizeof(em));
            int n = snprintf(resp, RESP_MAX, "{\"ok\":%s,\"desc\":\"%s\"}",
                             ok ? "true" : "false", em);
            send_json(fd, 200, resp, n);
            return;
        }
    }
    send_response(fd, 404, "Not Found", "text/plain", "not found", 9);
}

/* 요청 1건 수신·파싱 (요청 라인 + Content-Length 본문) */
static void serve_client(AdminServer *a, int fd)
{
    sock_set_timeout_ms((sock_t)fd, 2000);

    static char req[REQ_MAX];                    /* 단일 스레드 처리 전용 */
    int total = 0;
    char *body = NULL;
    long content_len = 0;

    while (total < REQ_MAX - 1) {
        int n = recv(fd, req + total, (size_t)(REQ_MAX - 1 - total), 0);
        if (n <= 0) return;
        total += (int)n;
        req[total] = 0;
        char *hdr_end = strstr(req, "\r\n\r\n");
        if (!hdr_end) continue;
        const char *cl = strcasestr(req, "Content-Length:");
        if (cl) content_len = strtol(cl + 15, NULL, 10);
        if (content_len > REQ_MAX - (hdr_end + 4 - req) - 1) return;
        body = hdr_end + 4;
        if ((long)strlen(body) >= content_len) break;   /* 본문 수신 완료 */
    }
    if (!body) return;

    char method[8] = "", path[512] = "";
    sscanf(req, "%7s %511s", method, path);
    handle_request(a, fd, method, path, body);
}

static void *admin_thread_main(void *arg)
{
    AdminServer *a = arg;
    apply_thread_role(THREAD_ROLE_IO);

    while (!atomic_load(&a->stop)) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(a->listen_fd, &fds);
        struct timeval tv = { 0, 500 * 1000 };
        if (select(a->listen_fd + 1, &fds, NULL, NULL, &tv) <= 0) continue;
        sock_t cfd = accept(a->listen_fd, NULL, NULL);
        if (cfd == SOCK_INVALID) continue;
        serve_client(a, (int)cfd);       /* 윈도우 SOCKET 값도 int 범위 안 */
        sock_close(cfd);
    }
    return NULL;
}

/* ── 생성/시작/종료 ─────────────────────────────────── */

AdminServer *admin_create(const PipelineConfig *cfg, SettingsStore *settings)
{
    AdminServer *a = calloc(1, sizeof(AdminServer));
    a->cfg = cfg;
    a->settings = settings;
    a->listen_fd = SOCK_INVALID;
    pthread_mutex_init(&a->lock, NULL);
    return a;
}

void admin_start(AdminServer *a)
{
    if (a->cfg->admin_port <= 0) {
        LOGI("점주", "admin_port=0 — 점주 페이지 비활성");
        return;
    }
    sock_global_init();
    sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == SOCK_INVALID) {
        LOGE("점주", "소켓 생성 실패");
        return;
    }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)a->cfg->admin_port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 8) != 0) {
        LOGE("점주", "포트 %d 바인드 실패 (%s)", a->cfg->admin_port,
             strerror(errno));
        sock_close(fd);
        return;
    }
    a->listen_fd = fd;
    if (pthread_create(&a->thread, NULL, admin_thread_main, a) == 0) {
        a->has_thread = true;
        LOGI("점주", "점주 페이지: http://localhost:%d", a->cfg->admin_port);
    }
}

void admin_stop(AdminServer *a)
{
    if (!a) return;
    atomic_store(&a->stop, true);
    if (a->has_thread) {
        pthread_join(a->thread, NULL);
        a->has_thread = false;
    }
    if (a->listen_fd != SOCK_INVALID) {
        sock_close(a->listen_fd);
        a->listen_fd = SOCK_INVALID;
    }
}

void admin_destroy(AdminServer *a)
{
    if (!a) return;
    pthread_mutex_destroy(&a->lock);
    free(a);
}
