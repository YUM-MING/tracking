#include "resource.h"

#include <stdio.h>
#include <stdlib.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

void apply_cpu_affinity(const PipelineConfig *cfg)
{
    if (cfg->cpu_affinity_mask == 0) return;
    if (SetProcessAffinityMask(GetCurrentProcess(),
                               (DWORD_PTR)cfg->cpu_affinity_mask))
        fprintf(stderr, "[리소스] CPU 친화도 지정: mask=0x%llx\n",
                cfg->cpu_affinity_mask);
    else
        fprintf(stderr, "[리소스] CPU 친화도 지정 실패\n");
}

struct ResourceMonitor {
    const PipelineConfig *cfg;
    HANDLE thread;
    HANDLE stop_event;
    double peak_cpu;           /* 기기 전체 CPU 피크 % */
    double peak_rss_mb;
    int n_cores;
};

static ULONGLONG ft_to_u64(FILETIME ft)
{
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static DWORD WINAPI monitor_main(LPVOID arg)
{
    ResourceMonitor *m = arg;
    HANDLE proc = GetCurrentProcess();

    FILETIME dummy_c, dummy_e, k0, u0, sys_idle0, sys_k0, sys_u0;
    GetProcessTimes(proc, &dummy_c, &dummy_e, &k0, &u0);
    GetSystemTimes(&sys_idle0, &sys_k0, &sys_u0);
    ULONGLONG proc0 = ft_to_u64(k0) + ft_to_u64(u0);
    ULONGLONG idle0 = ft_to_u64(sys_idle0);
    ULONGLONG sys0 = ft_to_u64(sys_k0) + ft_to_u64(sys_u0);
    double wall0 = 0;
    {
        LARGE_INTEGER f, t;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t);
        wall0 = (double)t.QuadPart / f.QuadPart;
    }

    DWORD interval = (DWORD)(m->cfg->monitor_interval_sec * 1000);
    while (WaitForSingleObject(m->stop_event, interval) == WAIT_TIMEOUT) {
        FILETIME k1, u1, sys_idle1, sys_k1, sys_u1;
        GetProcessTimes(proc, &dummy_c, &dummy_e, &k1, &u1);
        GetSystemTimes(&sys_idle1, &sys_k1, &sys_u1);
        ULONGLONG proc1 = ft_to_u64(k1) + ft_to_u64(u1);
        ULONGLONG idle1 = ft_to_u64(sys_idle1);
        ULONGLONG sys1 = ft_to_u64(sys_k1) + ft_to_u64(sys_u1);

        LARGE_INTEGER f, t;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t);
        double wall1 = (double)t.QuadPart / f.QuadPart;
        double wall_sec = wall1 - wall0;
        if (wall_sec <= 0) wall_sec = 1e-3;

        /* 프로세스 CPU: 1코어 기준 % (psutil.Process().cpu_percent 대응) */
        double proc_cpu = (double)(proc1 - proc0) / 1e7 / wall_sec * 100.0;
        /* 기기 전체 CPU: 시스템 시간 중 비유휴 비율 */
        ULONGLONG sys_delta = sys1 - sys0;        /* 커널+유저 (유휴 포함) */
        ULONGLONG idle_delta = idle1 - idle0;
        double sys_cpu = sys_delta > 0
            ? (double)(sys_delta - idle_delta) / (double)sys_delta * 100.0 : 0;

        PROCESS_MEMORY_COUNTERS pmc;
        double rss_mb = 0;
        if (GetProcessMemoryInfo(proc, &pmc, sizeof(pmc)))
            rss_mb = (double)pmc.WorkingSetSize / (1024.0 * 1024.0);

        if (sys_cpu > m->peak_cpu) m->peak_cpu = sys_cpu;
        if (rss_mb > m->peak_rss_mb) m->peak_rss_mb = rss_mb;

        const char *tag = sys_cpu >= m->cfg->cpu_alert_pct ? "[피크 경고] " : "";
        fprintf(stderr,
                "[리소스] %sproc CPU %.0f%%(1코어 기준) | sys CPU %.1f%% | "
                "RSS %.0fMB (sys peak %.1f%% / %.0fMB)\n",
                tag, proc_cpu, sys_cpu, rss_mb, m->peak_cpu, m->peak_rss_mb);

        proc0 = proc1; idle0 = idle1; sys0 = sys1; wall0 = wall1;
    }
    return 0;
}

ResourceMonitor *monitor_create(const PipelineConfig *cfg)
{
    ResourceMonitor *m = calloc(1, sizeof(ResourceMonitor));
    m->cfg = cfg;
    m->stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    m->n_cores = (int)si.dwNumberOfProcessors;
    return m;
}

void monitor_start(ResourceMonitor *m)
{
    m->thread = CreateThread(NULL, 0, monitor_main, m, 0, NULL);
}

void monitor_stop(ResourceMonitor *m)
{
    if (!m->thread) return;
    SetEvent(m->stop_event);
    WaitForSingleObject(m->thread, 2000);
    CloseHandle(m->thread);
    m->thread = NULL;
}

double monitor_peak_cpu(const ResourceMonitor *m) { return m->peak_cpu; }
double monitor_peak_rss_mb(const ResourceMonitor *m) { return m->peak_rss_mb; }

void monitor_destroy(ResourceMonitor *m)
{
    if (!m) return;
    if (m->stop_event) CloseHandle(m->stop_event);
    free(m);
}
