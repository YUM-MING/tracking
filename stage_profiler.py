"""
단계별 연산 시간 프로파일러
- YOLO / 재식별 / ROI / MediaPipe 각 단계가 추론 1회당 몇 ms 먹는지 주기 로깅.
- 목적: 맥북 실측 → 4200U 이식 시 어느 단계부터 줄일지(프레임 스킵, imgsz,
  정밀분석 인원 상한) 판단하는 근거 데이터 확보.
- 오버헤드는 perf_counter 호출 몇 번 수준으로 무시 가능.
"""
from __future__ import annotations

import logging
import time
from typing import Dict, List

log = logging.getLogger("profiler")


class StageProfiler:
    def __init__(self, report_interval_sec: float = 10.0):
        self.interval = report_interval_sec
        self._t0 = time.monotonic()
        # name → [누적 ms, 최대 ms, 호출 수, 처리 건수(크롭 등)]
        self._stats: Dict[str, List[float]] = {}
        self._order: List[str] = []

    def add(self, name: str, sec: float, items: int = 1):
        if name not in self._stats:
            self._stats[name] = [0.0, 0.0, 0, 0]
            self._order.append(name)
        s = self._stats[name]
        ms = sec * 1000.0
        s[0] += ms
        s[1] = max(s[1], ms)
        s[2] += 1
        s[3] += items

    def maybe_report(self):
        now = time.monotonic()
        if now - self._t0 < self.interval or not self._stats:
            return
        parts = []
        total_avg = 0.0
        for name in self._order:
            tot, mx, calls, items = self._stats[name]
            avg = tot / max(calls, 1)
            total_avg += avg
            p = f"{name} {avg:.1f}ms(max {mx:.0f})"
            if items != calls and calls > 0:
                p += f" {items / calls:.1f}건/회"
            parts.append(p)
        log.info("[단계별 추론시간] %s | 추론 1회 합계 %.1fms",
                 " | ".join(parts), total_avg)
        self._stats.clear()
        self._order.clear()
        self._t0 = now
