"""
메인 파이프라인 오케스트레이터
[카메라] → (프레임 샘플링) → 1단계 전역 탐지 → 비율 태깅/재식별
        → 2단계 동적 크롭 → 3단계 정밀 분석 → 4단계 상태 머신 → 웹소켓 전송

실행:
    python main.py                # 기본 웹캠
    python main.py rtsp://...     # IP 카메라
"""
from __future__ import annotations

import logging
import queue
import sys
import time

from config import CFG

logging.basicConfig(
    level=getattr(logging, CFG.log_level),
    format="%(asctime)s %(name)-16s %(levelname)-7s %(message)s",
)
log = logging.getLogger("main")

# 스레드/코어 제한은 torch·ultralytics가 import되기 전에 적용해야 한다.
from resource_guard import ResourceMonitor, apply_thread_limits  # noqa: E402
apply_thread_limits(CFG)

import cv2  # noqa: E402

from event_sender import WebSocketSender                 # noqa: E402
from reid_tagger import AppearanceTagger                 # noqa: E402
from stage_profiler import StageProfiler                 # noqa: E402
from stage1_global_detector import GlobalDetector        # noqa: E402
from stage2_roi_router import DynamicROIRouter           # noqa: E402
from stage3_local_analyzer import LocalPrecisionAnalyzer # noqa: E402
from stage4_state_machine import EventWorker, StateMachine  # noqa: E402

REASON_COLOR = {
    "fall_suspect": (0, 0, 255),
    "kiosk_enter": (0, 200, 255),
    "table_dwell": (255, 150, 0),
}


def draw_debug(frame, people, targets, router, infer_fps):
    """디버그 오버레이 (구역, BBox, 트리거 상태, 추론 FPS)."""
    for z, color in ((CFG.kiosk_zone, (0, 200, 255)), (CFG.table_zone, (255, 150, 0))):
        cv2.rectangle(frame, (z.x1, z.y1), (z.x2, z.y2), color, 1)
        cv2.putText(frame, z.name, (z.x1 + 4, z.y1 + 18),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 1)

    target_ids = {t.track_id: t.reason for t in targets}
    for p in people:
        x1, y1, x2, y2 = map(int, p.bbox)
        reason = target_ids.get(p.track_id)
        color = REASON_COLOR.get(reason, (0, 255, 0))
        cv2.rectangle(frame, (x1, y1), (x2, y2), color, 2)
        label = f"ID{p.track_id}"
        if reason:
            label += f" [{reason}]"
        dwell = router.table_dwell_seconds(p.track_id)
        if dwell > 0:
            label += f" {int(dwell)}s"
        cv2.putText(frame, label, (x1, max(y1 - 6, 12)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.55, color, 2)

    cv2.putText(frame, f"infer FPS {infer_fps:.1f} (skip {CFG.detect_every_n})",
                (10, 25), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 2)
    return frame


def main():
    source = sys.argv[1] if len(sys.argv) > 1 else CFG.camera_source

    # ── 파이프라인 구성 ─────────────────────────────────────
    detector = GlobalDetector(CFG)          # 1단계
    tagger = AppearanceTagger(CFG) if CFG.reid_enabled else None
    router = DynamicROIRouter(CFG)          # 2단계
    analyzer = LocalPrecisionAnalyzer(CFG)  # 3단계
    ev_q: "queue.Queue" = queue.Queue()
    fsm = StateMachine(CFG, ev_q)           # 4단계

    sender = WebSocketSender(CFG)           # 서버 전송 (ws_url 없으면 로컬 로그만)
    sender.start()
    worker = EventWorker(ev_q, sender=sender)
    worker.start()

    monitor = None
    if CFG.monitor_enabled:                 # 테스트 수칙: 리소스 모니터 상시 가동
        monitor = ResourceMonitor(CFG)
        monitor.start()

    cap = cv2.VideoCapture(source)
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, CFG.frame_width)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, CFG.frame_height)
    if not cap.isOpened():
        log.error("카메라를 열 수 없습니다: %s", source)
        return

    log.info("파이프라인 시작 (종료: q, 프레임 스킵: %d)", CFG.detect_every_n)
    frame_idx = 0
    infer_t0, infer_n, infer_fps = time.monotonic(), 0, 0.0
    people, targets = [], []                # 스킵 프레임에서는 직전 결과 재사용
    prof = StageProfiler(report_interval_sec=10.0)

    try:
        while True:
            ok, frame = cap.read()
            if not ok:
                log.warning("프레임 수신 실패 — 종료")
                break
            frame_idx += 1

            # ── 프레임 샘플링: N프레임마다 1회만 추론 (CPU 예산 보호) ──
            if frame_idx % CFG.detect_every_n == 0:
                # 1단계: 전역 탐지 + 추적
                t0 = time.perf_counter()
                people = detector.detect(frame)
                t1 = time.perf_counter()
                prof.add("yolo+track", t1 - t0, items=len(people))

                # 1.5단계: 옷 색상·채도 + 신체 비율 시그니처로 안정 ID 부여
                if tagger is not None:
                    tagger.assign(frame, people)
                t2 = time.perf_counter()
                prof.add("reid", t2 - t1)

                # 2단계: 트리거 대상 선별 + zero-copy 크롭
                targets = router.route(frame, people)
                t3 = time.perf_counter()
                prof.add("roi", t3 - t2)

                # 3단계: 선별된 크롭만 MediaPipe 정밀 분석
                precision = [analyzer.analyze(t) for t in targets]
                t4 = time.perf_counter()
                prof.add("mediapipe", t4 - t3, items=len(targets))

                # 4단계: 결정론적 상태 머신 → 이벤트 큐
                fsm.update(people, precision, router.table_dwell_seconds)
                prof.maybe_report()

                infer_n += 1
                elapsed = time.monotonic() - infer_t0
                if elapsed >= 2.0:
                    infer_fps = infer_n / elapsed
                    infer_t0, infer_n = time.monotonic(), 0

            # ── 디버그 시각화 (매 프레임, 직전 추론 결과 오버레이) ──
            if CFG.show_window:
                vis = draw_debug(frame, people, targets, router, infer_fps)
                cv2.imshow("Hybrid Cascade Pipeline", vis)
                if cv2.waitKey(1) & 0xFF == ord("q"):
                    break
    finally:
        cap.release()
        cv2.destroyAllWindows()
        analyzer.close()
        worker.stop()
        sender.stop()
        if monitor is not None:
            monitor.stop()
            log.info("리소스 피크: sys CPU %.1f%% / RSS %.0fMB",
                     monitor.peak_cpu, monitor.peak_rss_mb)
        log.info("파이프라인 종료")


if __name__ == "__main__":
    main()
