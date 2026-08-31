/*
 * 리소스 가드 (resource_guard.py 포팅)
 * - 제품 CPU(4200U급)는 순간 100% 피크도 허용되지 않으며 24시간 가동된다.
 * - 스레드 상한은 ORT 세션 옵션에서 적용(yolo_pose.c), 여기서는
 *   CPU 친화도 지정 + CPU/RAM 상시 모니터링을 담당한다.
 * - psutil 대신 macOS API 직접 호출:
 *   getrusage / host_statistics(HOST_CPU_LOAD_INFO) / task_info(MACH_TASK_BASIC_INFO)
 *   CPU affinity는 macOS가 프로세스 단위 고정을 지원하지 않아 무시(경고만 출력).
 */
#ifndef RESOURCE_H
#define RESOURCE_H

#include <stdbool.h>
#include "config.h"

/* CPU 친화도 지정 (cfg->cpu_affinity_mask가 0이면 미지정) */
void apply_cpu_affinity(const PipelineConfig *cfg);

/*
 * 스레드 역할별 우선순위 (8/9 보고 'Thread Affinity' 계획의 macOS 구현)
 * macOS는 코어 고정(bind)을 지원하지 않는 대신 QoS 클래스로 스케줄러에
 * 역할을 알린다 — 캡처/추론은 성능 코어, 통신/모니터는 효율 코어로 유도.
 * 각 스레드 시작 직후 자기 스레드에서 호출한다.
 */
typedef enum {
    THREAD_ROLE_CAPTURE,    /* 생산자: 카메라 스트리밍 */
    THREAD_ROLE_INFER,      /* 소비자: 추론 파이프라인 */
    THREAD_ROLE_IO,         /* 이벤트 워커 / 웹소켓 / 점주 페이지 */
    THREAD_ROLE_MONITOR,    /* 리소스 모니터 (최하위) */
} ThreadRole;
void apply_thread_role(ThreadRole role);

typedef struct ResourceMonitor ResourceMonitor;

ResourceMonitor *monitor_create(const PipelineConfig *cfg);
void monitor_start(ResourceMonitor *m);
void monitor_stop(ResourceMonitor *m);   /* 정지 후 피크값은 유효 */
double monitor_peak_cpu(const ResourceMonitor *m);
double monitor_peak_rss_mb(const ResourceMonitor *m);
/* 최근 측정치 (점주 페이지 상태 표시용 — 스레드 세이프) */
void monitor_last(ResourceMonitor *m, double *sys_cpu_pct, double *rss_mb);
void monitor_destroy(ResourceMonitor *m);

#endif /* RESOURCE_H */
