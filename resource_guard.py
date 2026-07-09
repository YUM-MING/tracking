"""
리소스 가드: 코어/스레드 제한 + CPU/RAM 상시 모니터링
- 제품 CPU(4200U급)는 순간 100% 피크도 허용되지 않으며 24시간 가동된다.
- 기존 솔루션이 1~2코어를 점유하므로, 이 파이프라인은 스레드 상한과
  (선택) CPU 친화도 지정으로 코어 충돌(오버행)을 피한다.
- 테스트 수칙: 실행 중 항상 CPU/메모리 사용량을 로깅해 피크를 확인한다.

주의: apply_thread_limits()는 반드시 ultralytics/torch를 import하기 **전에**
호출해야 OpenMP/MKL 스레드 수 환경변수가 적용된다.
"""
from __future__ import annotations

import logging
import os
import threading

from config import PipelineConfig

log = logging.getLogger("resource")


def apply_thread_limits(cfg: PipelineConfig):
    """스레드 풀 상한 적용. torch/cv2 import 전에 호출할 것."""
    n = str(cfg.max_threads)
    # OpenMP/MKL/BLAS 계열은 import 시점에 환경변수를 읽으므로 가장 먼저 설정
    for var in ("OMP_NUM_THREADS", "MKL_NUM_THREADS",
                "OPENBLAS_NUM_THREADS", "NUMEXPR_NUM_THREADS"):
        os.environ.setdefault(var, n)

    import cv2
    cv2.setNumThreads(cfg.max_threads)

    try:
        import torch
        torch.set_num_threads(cfg.max_threads)
        torch.set_num_interop_threads(1)
    except Exception:
        pass  # torch 미설치 또는 이미 초기화됨

    if cfg.cpu_affinity:
        try:
            import psutil
            psutil.Process().cpu_affinity(cfg.cpu_affinity)
            log.info("CPU 친화도 지정: cores=%s", cfg.cpu_affinity)
        except Exception as e:
            log.warning("CPU 친화도 지정 실패: %s", e)

    log.info("스레드 상한 적용: %d", cfg.max_threads)


class ResourceMonitor(threading.Thread):
    """주기적으로 프로세스/시스템 CPU와 RSS 메모리를 로깅하고 피크를 경고."""
    daemon = True

    def __init__(self, cfg: PipelineConfig):
        super().__init__(name="resource-monitor")
        self.cfg = cfg
        self._stop = threading.Event()
        self.peak_cpu = 0.0
        self.peak_rss_mb = 0.0

    def run(self):
        try:
            import psutil
        except ImportError:
            log.warning("psutil 미설치 — 리소스 모니터링 비활성 (pip install psutil)")
            return

        proc = psutil.Process()
        proc.cpu_percent()  # 첫 호출은 기준점 설정

        while not self._stop.wait(self.cfg.monitor_interval_sec):
            proc_cpu = proc.cpu_percent()          # 1코어 기준 % (스레드 수만큼 100 초과 가능)
            rss_mb = proc.memory_info().rss / (1024 * 1024)
            sys_cpu = psutil.cpu_percent()         # 기기 전체 %
            self.peak_cpu = max(self.peak_cpu, sys_cpu)
            self.peak_rss_mb = max(self.peak_rss_mb, rss_mb)

            msg = "proc CPU %.0f%%(1코어 기준) | sys CPU %.1f%% | RSS %.0fMB (sys peak %.1f%% / %.0fMB)"
            args = (proc_cpu, sys_cpu, rss_mb, self.peak_cpu, self.peak_rss_mb)
            # 기기 전체 CPU가 임계값을 넘으면 순간 피크로 간주하고 경고
            if sys_cpu >= self.cfg.cpu_alert_pct:
                log.warning("[피크 경고] " + msg, *args)
            else:
                log.info(msg, *args)

    def stop(self):
        self._stop.set()
