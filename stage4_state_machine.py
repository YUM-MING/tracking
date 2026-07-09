"""
4단계: 경량 상태 머신 (Deterministic Logic)
- 딥러닝 연산이 전혀 없는 순수 조건문 영역
- [테이블 존 체류 300초 경과] + [구매 여부 False] → 안내방송 이벤트 발행
- [쓰러짐 확정] → 긴급 이벤트 즉시 발행
- 이벤트는 비동기 큐에 적재되고, 별도 워커 스레드가 소비 (오디오 재생 등)
"""
from __future__ import annotations

import logging
import queue
import threading
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional

from config import PipelineConfig
from stage1_global_detector import TrackedPerson
from stage3_local_analyzer import PrecisionResult

log = logging.getLogger("stage4.fsm")


@dataclass
class Event:
    kind: str            # "announce_dwell" | "fall_alert" | "kiosk_assist"
    track_id: int
    message: str
    ts: float = field(default_factory=time.monotonic)


@dataclass
class PersonState:
    track_id: int
    purchased: bool = False              # POS 연동 시 갱신되는 구매 플래그
    last_announce_ts: float = -1e9
    fall_frames: int = 0                 # 쓰러짐 신호 연속 프레임 수 (디바운스)
    last_seen: float = field(default_factory=time.monotonic)


class StateMachine:
    FALL_CONFIRM_FRAMES = 5  # N회 연속 수평 자세일 때만 확정
                             # (프레임 스킵 적용 시 "추론 프레임" 기준 — 30fps·스킵5면 약 1초)

    def __init__(self, cfg: PipelineConfig, event_queue: "queue.Queue[Event]"):
        self.cfg = cfg
        self.q = event_queue
        self.states: Dict[int, PersonState] = {}

    def _state(self, tid: int) -> PersonState:
        st = self.states.get(tid)
        if st is None:
            st = self.states[tid] = PersonState(track_id=tid)
        st.last_seen = time.monotonic()
        return st

    def mark_purchased(self, track_id: int):
        """POS/키오스크 결제 완료 웹훅에서 호출."""
        self._state(track_id).purchased = True

    def update(
        self,
        people: List[TrackedPerson],
        precision: List[PrecisionResult],
        table_dwell_lookup,   # Callable[[int], float]
    ):
        now = time.monotonic()
        prec_by_id = {r.track_id: r for r in precision}

        for p in people:
            st = self._state(p.track_id)
            pr: Optional[PrecisionResult] = prec_by_id.get(p.track_id)

            # ── 규칙 1: 쓰러짐 확정 (디바운스 후 즉시 발행) ──
            fall_signal = pr is not None and pr.pose_detected and pr.torso_horizontal
            st.fall_frames = st.fall_frames + 1 if fall_signal else 0
            if st.fall_frames == self.FALL_CONFIRM_FRAMES:
                self.q.put(Event(
                    kind="fall_alert",
                    track_id=p.track_id,
                    message=f"[긴급] ID {p.track_id} 쓰러짐 감지 — 직원 확인 요망",
                ))

            # ── 규칙 2: 장기 체류 + 미구매 → 안내방송 ──────
            dwell = table_dwell_lookup(p.track_id)
            if (
                dwell >= self.cfg.announce_dwell_sec
                and not st.purchased
                and now - st.last_announce_ts >= self.cfg.announce_cooldown_sec
            ):
                st.last_announce_ts = now
                self.q.put(Event(
                    kind="announce_dwell",
                    track_id=p.track_id,
                    message=f"ID {p.track_id} 테이블 {int(dwell)}초 체류(미구매) — 안내방송 재생",
                ))

            # ── 규칙 3: 키오스크 앞 손 들어올림 → 도움 요청 ──
            if pr is not None and pr.reason == "kiosk_enter" and pr.hand_raised:
                if now - st.last_announce_ts >= self.cfg.announce_cooldown_sec:
                    st.last_announce_ts = now
                    self.q.put(Event(
                        kind="kiosk_assist",
                        track_id=p.track_id,
                        message=f"ID {p.track_id} 키오스크 앞 도움 요청 제스처 감지",
                    ))

        self._gc(now)

    def _gc(self, now: float):
        ttl = self.cfg.stale_track_ttl_sec
        for tid in [t for t, s in self.states.items() if now - s.last_seen > ttl]:
            del self.states[tid]


class EventWorker(threading.Thread):
    """
    이벤트 큐 소비자. 로컬 처리(오디오 재생 등)와 서버 전송(웹소켓)을 담당.
    실제 배포에서는 handle()에서 오디오 재생(playsound / GPIO 앰프),
    안내방송 트리거 등을 수행한다.
    """
    daemon = True

    def __init__(self, event_queue: "queue.Queue[Event]", sender=None):
        super().__init__(name="event-worker")
        self.q = event_queue
        self.sender = sender          # event_sender.WebSocketSender (없으면 로그만)
        self._stop = threading.Event()

    def run(self):
        while not self._stop.is_set():
            try:
                ev = self.q.get(timeout=0.5)
            except queue.Empty:
                continue
            self.handle(ev)
            self.q.task_done()

    def handle(self, ev: Event):
        # TODO: 실제 오디오/알림 연동 지점
        # 예) subprocess.Popen(["aplay", AUDIO_MAP[ev.kind]])
        log.warning("[EVENT] %s | %s", ev.kind, ev.message)

        # 서버로는 비식별 메타데이터(JSON)만 전송
        if self.sender is not None:
            self.sender.send({
                "type": ev.kind,
                "track_id": ev.track_id,
                "ts": time.time(),        # 서버 기록용 벽시계 시각
                "message": ev.message,
                "meta": {},
            })

    def stop(self):
        self._stop.set()
