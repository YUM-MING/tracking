#include "resource.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include <sys/resource.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <pthread/qos.h>

void apply_cpu_affinity(const PipelineConfig *cfg)
{
    if (cfg->cpu_affinity_mask == 0) return;
    fprintf(stderr, "[리소스] macOS는 프로세스 CPU affinity 고정을 지원하지 않음 — 무시됨\n");
}

/* macOS: 코어 고정 대신 QoS 클래스로 성능/효율 코어 배치를 유도한다.
 * (4200U/리눅스 이식 시 이 함수만 pthread_setaffinity_np로 교체하면 된다) */
void apply_thread_role(ThreadRole role)
{
    qos_class_t qos;
    switch (role) {
    case THREAD_ROLE_CAPTURE: qos = QOS_CLASS_USER_INTERACTIVE; break;
    case THREAD_ROLE_INFER:   qos = QOS_CLASS_USER_INITIATED;   break;
    case THREAD_ROLE_IO:      qos = QOS_CLASS_UTILITY;          break;
    default:                  qos = QOS_CLASS_BACKGROUND;       break;
    }
    pthread_set_qos_class_self_np(qos, 0);
}

struct ResourceMonitor {
    const PipelineConfig *cfg;
    pthread_t thread;
    bool has_thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool stop;
    double peak_cpu;
    double peak_rss_mb;
    double last_sys_cpu;       /* 점주 페이지 상태 표시용 최근 측정치 */
    double last_rss_mb;
    int n_cores;
};

static double mono_seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* 프로세스 누적 CPU 시간(초, user+system) — GetProcessTimes 대응 */
static double proc_cpu_seconds(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6
         + (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6;
}

/* 프로세스 RSS(MB) — GetProcessMemoryInfo 대응 */
static double proc_rss_mb(void)
{
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) != KERN_SUCCESS)
        return 0;
    return (double)info.resident_size / (1024.0 * 1024.0);
}

/* 기기 전체 CPU 틱(비유휴/전체) — GetSystemTimes 대응 */
static void host_cpu_ticks(unsigned long long *busy, unsigned long long *total)
{
    host_cpu_load_info_data_t info;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                        (host_info_t)&info, &count) != KERN_SUCCESS) {
        *busy = 0; *total = 0;
        return;
    }
    unsigned long long user = info.cpu_ticks[CPU_STATE_USER];
    unsigned long long sys  = info.cpu_ticks[CPU_STATE_SYSTEM];
    unsigned long long nice = info.cpu_ticks[CPU_STATE_NICE];
    unsigned long long idle = info.cpu_ticks[CPU_STATE_IDLE];
    *busy = user + sys + nice;
    *total = *busy + idle;
}

static void *monitor_main(void *arg)
{
    ResourceMonitor *m = arg;
    apply_thread_role(THREAD_ROLE_MONITOR);

    double proc0 = proc_cpu_seconds();
    double wall0 = mono_seconds();
    unsigned long long busy0, total0;
    host_cpu_ticks(&busy0, &total0);

    double interval = m->cfg->monitor_interval_sec;

    pthread_mutex_lock(&m->lock);
    for (;;) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += (time_t)interval;
        deadline.tv_nsec += (long)((interval - (long)interval) * 1e9);
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_nsec -= 1000000000L;
            deadline.tv_sec++;
        }

        int rc = 0;
        while (!m->stop && rc == 0)
            rc = pthread_cond_timedwait(&m->cond, &m->lock, &deadline);
        if (m->stop) break;                 /* 타임아웃(rc!=0)일 때만 측정 진행 */
        pthread_mutex_unlock(&m->lock);

        double proc1 = proc_cpu_seconds();
        double wall1 = mono_seconds();
        unsigned long long busy1, total1;
        host_cpu_ticks(&busy1, &total1);

        double wall_sec = wall1 - wall0;
        if (wall_sec <= 0) wall_sec = 1e-3;

        double proc_cpu = (proc1 - proc0) / wall_sec * 100.0;   /* 1코어 기준 % */
        unsigned long long dt_busy = busy1 - busy0, dt_total = total1 - total0;
        double sys_cpu = dt_total > 0 ? (double)dt_busy / (double)dt_total * 100.0 : 0;
        double rss_mb = proc_rss_mb();

        if (sys_cpu > m->peak_cpu) m->peak_cpu = sys_cpu;
        if (rss_mb > m->peak_rss_mb) m->peak_rss_mb = rss_mb;
        pthread_mutex_lock(&m->lock);
        m->last_sys_cpu = sys_cpu;
        m->last_rss_mb = rss_mb;
        pthread_mutex_unlock(&m->lock);

        const char *tag = sys_cpu >= m->cfg->cpu_alert_pct ? "[피크 경고] " : "";
        fprintf(stderr,
                "[리소스] %sproc CPU %.0f%%(1코어 기준) | sys CPU %.1f%% | "
                "RSS %.0fMB (sys peak %.1f%% / %.0fMB)\n",
                tag, proc_cpu, sys_cpu, rss_mb, m->peak_cpu, m->peak_rss_mb);

        proc0 = proc1; wall0 = wall1; busy0 = busy1; total0 = total1;
        pthread_mutex_lock(&m->lock);
    }
    pthread_mutex_unlock(&m->lock);
    return NULL;
}

ResourceMonitor *monitor_create(const PipelineConfig *cfg)
{
    ResourceMonitor *m = calloc(1, sizeof(ResourceMonitor));
    m->cfg = cfg;
    pthread_mutex_init(&m->lock, NULL);
    pthread_cond_init(&m->cond, NULL);
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    m->n_cores = n > 0 ? (int)n : 1;
    return m;
}

void monitor_start(ResourceMonitor *m)
{
    if (pthread_create(&m->thread, NULL, monitor_main, m) == 0)
        m->has_thread = true;
}

void monitor_stop(ResourceMonitor *m)
{
    if (!m->has_thread) return;
    pthread_mutex_lock(&m->lock);
    m->stop = true;
    pthread_mutex_unlock(&m->lock);
    pthread_cond_broadcast(&m->cond);
    pthread_join(m->thread, NULL);
    m->has_thread = false;
}

double monitor_peak_cpu(const ResourceMonitor *m) { return m->peak_cpu; }
double monitor_peak_rss_mb(const ResourceMonitor *m) { return m->peak_rss_mb; }

void monitor_last(ResourceMonitor *m, double *sys_cpu_pct, double *rss_mb)
{
    pthread_mutex_lock(&m->lock);
    *sys_cpu_pct = m->last_sys_cpu;
    *rss_mb = m->last_rss_mb;
    pthread_mutex_unlock(&m->lock);
}

void monitor_destroy(ResourceMonitor *m)
{
    if (!m) return;
    pthread_mutex_destroy(&m->lock);
    pthread_cond_destroy(&m->cond);
    free(m);
}
