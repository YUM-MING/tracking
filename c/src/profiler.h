/*
 * 단계별 연산 시간 프로파일러 (stage_profiler.py 포팅)
 * - YOLO / 재식별 / ROI / 크롭 재추론 각 단계가 추론 1회당 몇 ms 먹는지 주기 로깅.
 * - 목적: 실측 → 4200U 이식 시 어느 단계부터 줄일지 판단하는 근거 데이터.
 */
#ifndef PROFILER_H
#define PROFILER_H

#define PROF_MAX_STAGES 8

typedef struct {
    const char *name;
    double total_ms, max_ms;
    int calls, items;
} ProfStage;

typedef struct {
    double interval_sec;
    double t0;
    ProfStage stages[PROF_MAX_STAGES];
    int n_stages;
} StageProfiler;

void prof_init(StageProfiler *p, double report_interval_sec);
void prof_add(StageProfiler *p, const char *name, double sec, int items);
void prof_maybe_report(StageProfiler *p, double now);

/* 고해상도 단조 시각 (초) — time.monotonic / perf_counter 대응 */
double mono_now(void);

#endif /* PROFILER_H */
