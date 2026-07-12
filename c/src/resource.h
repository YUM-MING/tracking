/*
 * 리소스 가드 (resource_guard.py 포팅)
 * - 제품 CPU(4200U급)는 순간 100% 피크도 허용되지 않으며 24시간 가동된다.
 * - 스레드 상한은 ORT 세션 옵션에서 적용(yolo_pose.c), 여기서는
 *   CPU 친화도 지정 + CPU/RAM 상시 모니터링을 담당한다.
 * - psutil 대신 Win32 API 직접 호출:
 *   GetProcessTimes / GetSystemTimes / GetProcessMemoryInfo
 */
#ifndef RESOURCE_H
#define RESOURCE_H

#include <stdbool.h>
#include "config.h"

/* CPU 친화도 지정 (cfg->cpu_affinity_mask가 0이면 미지정) */
void apply_cpu_affinity(const PipelineConfig *cfg);

typedef struct ResourceMonitor ResourceMonitor;

ResourceMonitor *monitor_create(const PipelineConfig *cfg);
void monitor_start(ResourceMonitor *m);
void monitor_stop(ResourceMonitor *m);   /* 정지 후 피크값은 유효 */
double monitor_peak_cpu(const ResourceMonitor *m);
double monitor_peak_rss_mb(const ResourceMonitor *m);
void monitor_destroy(ResourceMonitor *m);

#endif /* RESOURCE_H */
