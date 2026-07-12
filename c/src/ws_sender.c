#include "ws_sender.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#define MAX_BACKLOG 500        /* 서버 장기 다운 시 오래된 이벤트부터 폐기 */

/* ── 내부 큐 (fsm.c의 이벤트 큐와 동일 패턴, JSON 문자열 보관) ── */
typedef struct {
    char json[512];
} WsPayload;

struct WsSender {
    const PipelineConfig *cfg;
    bool enabled;

    WsPayload buf[MAX_BACKLOG];
    int head, tail, count;
    bool closed;
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE not_empty;

    HANDLE thread;
    SOCKET sock;
    double backoff;
};

/* ── URL 파싱: ws://host:port/path ── */
static bool parse_ws_url(const char *url, char *host, size_t host_len,
                         char *port, size_t port_len, char *path, size_t path_len)
{
    if (strncmp(url, "ws://", 5) != 0) return false;   /* wss(TLS)는 미지원 */
    const char *p = url + 5;
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        size_t hl = (size_t)(colon - p);
        if (hl >= host_len) return false;
        memcpy(host, p, hl); host[hl] = 0;
        const char *pe = slash ? slash : p + strlen(p);
        size_t pl = (size_t)(pe - colon - 1);
        if (pl >= port_len || pl == 0) return false;
        memcpy(port, colon + 1, pl); port[pl] = 0;
    } else {
        const char *he = slash ? slash : p + strlen(p);
        size_t hl = (size_t)(he - p);
        if (hl >= host_len || hl == 0) return false;
        memcpy(host, p, hl); host[hl] = 0;
        snprintf(port, port_len, "80");
    }
    snprintf(path, path_len, "%s", slash ? slash : "/");
    return true;
}

/* ── base64 (핸드셰이크 키 생성용) ── */
static void base64_16(const unsigned char *in, char *out /* 25바이트 이상 */)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < 15; i += 3) {
        unsigned v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = tbl[(v >> 6) & 63];
        out[o++] = tbl[v & 63];
    }
    /* 남은 1바이트 (16 = 3*5 + 1) */
    unsigned v = in[15] << 16;
    out[o++] = tbl[(v >> 18) & 63];
    out[o++] = tbl[(v >> 12) & 63];
    out[o++] = '=';
    out[o++] = '=';
    out[o] = 0;
}

/* ── RFC 6455 클라이언트 핸드셰이크 ── */
static bool ws_handshake(SOCKET s, const char *host, const char *port, const char *path)
{
    unsigned char key_raw[16];
    for (int i = 0; i < 16; i++) key_raw[i] = (unsigned char)(rand() & 0xFF);
    char key_b64[32];
    base64_16(key_raw, key_b64);

    char req[512];
    int len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        path, host, port, key_b64);
    if (send(s, req, len, 0) != len) return false;

    /* 응답 헤더 끝(\r\n\r\n)까지 수신 후 101 확인 */
    char resp[2048];
    int total = 0;
    while (total < (int)sizeof(resp) - 1) {
        int n = recv(s, resp + total, (int)sizeof(resp) - 1 - total, 0);
        if (n <= 0) return false;
        total += n;
        resp[total] = 0;
        if (strstr(resp, "\r\n\r\n")) break;
    }
    return strncmp(resp, "HTTP/1.1 101", 12) == 0;
}

/* ── 마스킹된 텍스트 프레임 전송 ── */
static bool ws_send_text(SOCKET s, const char *text)
{
    size_t len = strlen(text);
    unsigned char hdr[14];
    int hl = 0;
    hdr[hl++] = 0x81;                          /* FIN + opcode text */
    unsigned char mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (unsigned char)(rand() & 0xFF);

    if (len < 126) {
        hdr[hl++] = 0x80 | (unsigned char)len; /* MASK 비트 (클라이언트 필수) */
    } else if (len < 65536) {
        hdr[hl++] = 0x80 | 126;
        hdr[hl++] = (unsigned char)(len >> 8);
        hdr[hl++] = (unsigned char)(len & 0xFF);
    } else {
        return false;                          /* 이벤트 JSON은 이 크기를 넘지 않는다 */
    }
    memcpy(hdr + hl, mask, 4);
    hl += 4;

    if (send(s, (char *)hdr, hl, 0) != hl) return false;

    char chunk[512];
    size_t off = 0;
    while (off < len) {
        size_t n = len - off > sizeof(chunk) ? sizeof(chunk) : len - off;
        for (size_t i = 0; i < n; i++)
            chunk[i] = text[off + i] ^ (char)mask[(off + i) % 4];
        if (send(s, chunk, (int)n, 0) != (int)n) return false;
        off += n;
    }
    return true;
}

static void ws_close_sock(WsSender *s)
{
    if (s->sock != INVALID_SOCKET) {
        closesocket(s->sock);
        s->sock = INVALID_SOCKET;
    }
}

/* 연결 시도. 실패 시 백오프 대기 후 false. */
static bool ws_connect(WsSender *s)
{
    char host[256], port[16], path[256];
    if (!parse_ws_url(s->cfg->ws_url, host, sizeof(host),
                      port, sizeof(port), path, sizeof(path))) {
        fprintf(stderr, "[ws] URL 파싱 실패: %s\n", s->cfg->ws_url);
        Sleep(5000);
        return false;
    }

    struct addrinfo hints = { 0 }, *ai = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    bool ok = false;
    if (getaddrinfo(host, port, &hints, &ai) == 0) {
        SOCKET sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock != INVALID_SOCKET) {
            DWORD tmo = 5000;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&tmo, sizeof(tmo));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&tmo, sizeof(tmo));
            if (connect(sock, ai->ai_addr, (int)ai->ai_addrlen) == 0 &&
                ws_handshake(sock, host, port, path)) {
                s->sock = sock;
                ok = true;
            } else {
                closesocket(sock);
            }
        }
        freeaddrinfo(ai);
    }

    if (ok) {
        s->backoff = s->cfg->ws_reconnect_min_sec;
        fprintf(stderr, "[ws] 웹소켓 연결됨: %s\n", s->cfg->ws_url);
    } else {
        fprintf(stderr, "[ws] 연결 실패 — %.1f초 후 재시도\n", s->backoff);
        Sleep((DWORD)(s->backoff * 1000));
        s->backoff *= 2;
        if (s->backoff > s->cfg->ws_reconnect_max_sec)
            s->backoff = s->cfg->ws_reconnect_max_sec;
    }
    return ok;
}

/* ── 워커 스레드: 큐 소비 → (재)연결 → 전송 ── */
static DWORD WINAPI ws_thread_main(LPVOID arg)
{
    WsSender *s = arg;

    for (;;) {
        /* 페이로드 대기 */
        WsPayload payload;
        bool got = false;
        EnterCriticalSection(&s->lock);
        while (s->count == 0 && !s->closed)
            SleepConditionVariableCS(&s->not_empty, &s->lock, 500);
        if (s->count > 0) {
            payload = s->buf[s->head];
            s->head = (s->head + 1) % MAX_BACKLOG;
            s->count--;
            got = true;
        }
        bool closed = s->closed;
        LeaveCriticalSection(&s->lock);
        if (!got) {
            if (closed) break;
            continue;
        }

        /* 전송 (실패 시 재연결 후 재시도) */
        for (;;) {
            EnterCriticalSection(&s->lock);
            closed = s->closed;
            LeaveCriticalSection(&s->lock);
            if (closed) break;

            if (s->sock == INVALID_SOCKET && !ws_connect(s)) continue;
            if (ws_send_text(s->sock, payload.json)) break;
            fprintf(stderr, "[ws] 전송 실패 — 재연결 후 재시도\n");
            ws_close_sock(s);
        }
        if (closed) break;
    }

    ws_close_sock(s);
    return 0;
}

/* ── 공개 API ── */
WsSender *ws_create(const PipelineConfig *cfg)
{
    WsSender *s = calloc(1, sizeof(WsSender));
    s->cfg = cfg;
    s->sock = INVALID_SOCKET;
    s->backoff = cfg->ws_reconnect_min_sec;
    s->enabled = cfg->ws_url && cfg->ws_url[0];
    InitializeCriticalSection(&s->lock);
    InitializeConditionVariable(&s->not_empty);
    return s;
}

void ws_start(WsSender *s)
{
    if (!s->enabled) {
        fprintf(stderr, "[ws] ws_url 미설정 — 서버 전송 비활성 (로컬 로그만)\n");
        return;
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[ws] WSAStartup 실패 — 서버 전송 비활성\n");
        s->enabled = false;
        return;
    }
    srand((unsigned)time(NULL) ^ GetCurrentProcessId());
    s->thread = CreateThread(NULL, 0, ws_thread_main, s, 0, NULL);
}

/*
 * JSON 직렬화: 스키마는 파이썬판과 동일.
 * { "type", "track_id", "ts"(유닉스 초), "message", "meta": {} }
 * message의 한글은 UTF-8 그대로 내보내고, 제어문자/따옴표만 이스케이프.
 */
static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 7 < out_len; p++) {
        if (*p == '"' || *p == '\\') {
            out[o++] = '\\';
            out[o++] = *p;
        } else if (*p < 0x20) {
            o += snprintf(out + o, out_len - o, "\\u%04x", *p);
        } else {
            out[o++] = *p;
        }
    }
    out[o] = 0;
}

void ws_send_event(WsSender *s, const Event *ev)
{
    if (!s->enabled) return;

    char esc[384];
    json_escape(ev->message, esc, sizeof(esc));
    WsPayload payload;
    snprintf(payload.json, sizeof(payload.json),
             "{\"type\":\"%s\",\"track_id\":%d,\"ts\":%.3f,"
             "\"message\":\"%s\",\"meta\":{}}",
             event_kind_str(ev->kind), ev->track_id,
             (double)time(NULL), esc);

    EnterCriticalSection(&s->lock);
    if (s->count >= MAX_BACKLOG) {           /* 백로그 초과 — 오래된 것 폐기 */
        s->head = (s->head + 1) % MAX_BACKLOG;
        s->count--;
        fprintf(stderr, "[ws] 백로그 초과 — 이벤트 폐기\n");
    }
    s->buf[s->tail] = payload;
    s->tail = (s->tail + 1) % MAX_BACKLOG;
    s->count++;
    LeaveCriticalSection(&s->lock);
    WakeConditionVariable(&s->not_empty);
}

void ws_stop(WsSender *s)
{
    EnterCriticalSection(&s->lock);
    s->closed = true;
    LeaveCriticalSection(&s->lock);
    WakeAllConditionVariable(&s->not_empty);
    if (s->thread) {
        WaitForSingleObject(s->thread, 3000);
        CloseHandle(s->thread);
        s->thread = NULL;
    }
    if (s->enabled) WSACleanup();
}

void ws_destroy(WsSender *s)
{
    if (!s) return;
    DeleteCriticalSection(&s->lock);
    free(s);
}
