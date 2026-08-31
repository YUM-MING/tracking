#include "profiler.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

double mono_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

void prof_init(StageProfiler *p, double report_interval_sec)
{
    memset(p, 0, sizeof(*p));
    p->interval_sec = report_interval_sec;
    p->t0 = mono_now();
}

void prof_add(StageProfiler *p, const char *name, double sec, int items)
{
    ProfStage *s = NULL;
    for (int i = 0; i < p->n_stages; i++) {
        if (strcmp(p->stages[i].name, name) == 0) { s = &p->stages[i]; break; }
    }
    if (!s) {
        if (p->n_stages == PROF_MAX_STAGES) return;
        s = &p->stages[p->n_stages++];
        s->name = name;             /* 리터럴 전제 (수명 걱정 없음) */
    }
    double ms = sec * 1000.0;
    s->total_ms += ms;
    if (ms > s->max_ms) s->max_ms = ms;
    s->calls++;
    s->items += items;
}

void prof_maybe_report(StageProfiler *p, double now)
{
    if (now - p->t0 < p->interval_sec || p->n_stages == 0) return;

    char line[512];
    int off = 0;
    double total_avg = 0;
    for (int i = 0; i < p->n_stages; i++) {
        ProfStage *s = &p->stages[i];
        double avg = s->total_ms / (s->calls > 0 ? s->calls : 1);
        total_avg += avg;
        off += snprintf(line + off, sizeof(line) - off, "%s%s %.1fms(max %.0f)",
                        i ? " | " : "", s->name, avg, s->max_ms);
        if (s->items != s->calls && s->calls > 0)
            off += snprintf(line + off, sizeof(line) - off, " %.1f건/회",
                            (double)s->items / s->calls);
        if (off >= (int)sizeof(line)) break;
    }
    fprintf(stderr, "[프로파일] %s | 추론 1회 합계 %.1fms\n", line, total_avg);

    p->n_stages = 0;
    p->t0 = now;
}
