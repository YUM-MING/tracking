/*
 * OS 호환 계층 (macOS/리눅스 ↔ Windows)
 * - 윈도우 패키징(8/31 회의)을 위해 소켓/시간/디렉터리 API 차이를 한곳에 격리.
 * - 스레드는 winpthreads(mingw-w64) 덕에 pthread 그대로 사용한다.
 */
#ifndef OS_COMPAT_H
#define OS_COMPAT_H

#include <time.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>          /* windows.h보다 먼저 */
#include <ws2tcpip.h>
#include <windows.h>

typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
#define sock_close closesocket

/* Winsock은 프로세스당 1회 초기화 필요 (여러 번 불러도 무해) */
static inline void sock_global_init(void)
{
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
}

/* SO_RCVTIMEO/SO_SNDTIMEO: 윈도우는 timeval이 아니라 밀리초 DWORD */
static inline void sock_set_timeout_ms(sock_t s, int ms)
{
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof(t));
}

#else /* POSIX */

#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef int sock_t;
#define SOCK_INVALID (-1)
#define sock_close close

static inline void sock_global_init(void) {}

static inline void sock_set_timeout_ms(sock_t s, int ms)
{
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

#endif /* _WIN32 */

/* localtime_r: 윈도우는 localtime_s(인자 순서 반대) — 래퍼로 통일 */
static inline void os_localtime(const time_t *t, struct tm *out)
{
#ifdef _WIN32
    localtime_s(out, t);
#else
    localtime_r(t, out);
#endif
}

#endif /* OS_COMPAT_H */
