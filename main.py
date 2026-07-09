"""
메인 파이프라인 오케스트레이터
[카메라] → 1단계 전역 탐지 → 2단계 동적 크롭 → 3단계 정밀 분석 → 4단계 상태 머신

실행:
    python main.py                # 기본 웹캠
    python main.py rtsp://...     # IP 카메라
"""
from __future__ import annotations

import logging
import queue
import sys
import time

import cv2

from config import CFG
from stage1_global_detector import GlobalDetector
from stage2_roi_router import DynamicROIRouter
from stage3_local_analyzer import LocalPrecisionAnalyzer
from stage4_state_machine import EventWorker, StateMachine

logging.basicConfig(
    level=getattr(logging, CFG.log_level),
    format="%(asctime)s %(name)-16s %(levelname)-7s %(message)s",
)
log = logging.getLogger("main")

REASON_COLOR = {
    "fall_suspect": (0, 0, 255),
    "kiosk_enter": (0, 200, 255),
    "table_dwell": (255, 150, 0),
}


def draw_debug(frame, people, targets, router):
    """디버그 오버레이 (구역, BBox, 트리거 상태)."""
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
    return frame


def main():
    source = sys.argv[1] if len(sys.argv) > 1 else CFG.camera_source

    # ── 파이프라인 구성 ─────────────────────────────────────
    detector = GlobalDetector(CFG)          # 1단계
    router = DynamicROIRouter(CFG)          # 2단계
    analyzer = LocalPrecisionAnalyzer(CFG)  # 3단계
    ev_q: "queue.Queue" = queue.Queue()
    fsm = StateMachine(CFG, ev_q)           # 4단계
    worker = EventWorker(ev_q)
    worker.start()

    cap = cv2.VideoCapture(source)
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, CFG.frame_width)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, CFG.frame_height)
    if not cap.isOpened():
        log.error("카메라를 열 수 없습니다: %s", source)
        return

    log.info("파이프라인 시작 (종료: q)")
    fps_t0, fps_n = time.monotonic(), 0

    try:
        while True:
            ok, frame = cap.read()
            if not ok:
                log.warning("프레임 수신 실패 — 종료")
                break

            # 1단계: 전역 탐지 + 추적
            people = detector.detect(frame)

            # 2단계: 트리거 대상 선별 + zero-copy 크롭
            targets = router.route(frame, people)

            # 3단계: 선별된 크롭만 MediaPipe 정밀 분석
            precision = [analyzer.analyze(t) for t in targets]

            # 4단계: 결정론적 상태 머신 → 이벤트 큐
            fsm.update(people, precision, router.table_dwell_seconds)

            # ── 디버그 시각화 및 FPS ────────────────────────
            fps_n += 1
            if CFG.show_window:
                vis = draw_debug(frame, people, targets, router)
                elapsed = time.monotonic() - fps_t0
                if elapsed > 0:
                    cv2.putText(vis, f"FPS {fps_n / elapsed:.1f}", (10, 25),
                                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 2)
                cv2.imshow("Hybrid Cascade Pipeline", vis)
                if cv2.waitKey(1) & 0xFF == ord("q"):
                    break
            if fps_n % 300 == 0:
                fps_t0, fps_n = time.monotonic(), 0
    finally:
        cap.release()
        cv2.destroyAllWindows()
        analyzer.close()
        worker.stop()
        log.info("파이프라인 종료")


if __name__ == "__main__":
    main()
