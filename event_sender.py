"""
서버 전송: WebSocket 이벤트 발행 (재연결 포함)
- 원본 영상이 아닌 추상화된 이벤트 메타데이터(JSON)만 전송한다.
- 회사 솔루션과 동일하게 웹소켓 상시 연결 + 끊김 시 지수 백오프 재연결.
- 서버 인터페이스 스키마:
    {
      "type":     "announce_dwell" | "fall_alert" | "kiosk_assist",
      "track_id": 안정 ID (정수, 비식별),
      "ts":       유닉스 타임스탬프 (float, 서버 기록용),
      "message":  사람이 읽는 설명 문자열,
      "meta":     부가 정보 (선택)
    }
- config.ws_url이 비어 있으면 비활성 (로컬 로그만).
"""
from __future__ import annotations

import json
import logging
import queue
import threading
import time
from typing import Optional

from config import PipelineConfig

log = logging.getLogger("sender.ws")

try:
    import websocket  # websocket-client
except ImportError:
    websocket = None


class WebSocketSender(threading.Thread):
    """전송 전용 워커. send()는 큐 적재만 하므로 핫패스를 막지 않는다."""
    daemon = True
    MAX_BACKLOG = 500  # 서버 장기 다운 시 오래된 이벤트부터 폐기

    def __init__(self, cfg: PipelineConfig):
        super().__init__(name="ws-sender")
        self.cfg = cfg
        self._q: "queue.Queue[dict]" = queue.Queue()
        self._stop = threading.Event()
        self._ws = None
        self._backoff = cfg.ws_reconnect_min_sec

    @property
    def enabled(self) -> bool:
        return bool(self.cfg.ws_url) and websocket is not None

    def send(self, payload: dict):
        """비차단 적재. 백로그 초과 시 가장 오래된 것부터 버린다."""
        if not self.enabled:
            return
        if self._q.qsize() >= self.MAX_BACKLOG:
            try:
                dropped = self._q.get_nowait()
                log.warning("백로그 초과 — 이벤트 폐기: %s", dropped.get("type"))
            except queue.Empty:
                pass
        self._q.put(payload)

    # ── 내부 ──────────────────────────────────────────────
    def _connect(self) -> bool:
        try:
            self._ws = websocket.create_connection(self.cfg.ws_url, timeout=5)
            self._backoff = self.cfg.ws_reconnect_min_sec
            log.info("웹소켓 연결됨: %s", self.cfg.ws_url)
            return True
        except Exception as e:
            self._ws = None
            log.warning("웹소켓 연결 실패(%s) — %.1f초 후 재시도", e, self._backoff)
            self._stop.wait(self._backoff)
            self._backoff = min(self._backoff * 2, self.cfg.ws_reconnect_max_sec)
            return False

    def run(self):
        if not self.cfg.ws_url:
            log.info("ws_url 미설정 — 서버 전송 비활성 (로컬 로그만)")
            return
        if websocket is None:
            log.warning("websocket-client 미설치 — 서버 전송 비활성 (pip install websocket-client)")
            return

        while not self._stop.is_set():
            try:
                payload = self._q.get(timeout=0.5)
            except queue.Empty:
                continue

            while not self._stop.is_set():
                if self._ws is None and not self._connect():
                    continue
                try:
                    self._ws.send(json.dumps(payload, ensure_ascii=False))
                    break
                except Exception as e:
                    log.warning("전송 실패(%s) — 재연결 후 재시도", e)
                    self._close_ws()

        self._close_ws()

    def _close_ws(self):
        if self._ws is not None:
            try:
                self._ws.close()
            except Exception:
                pass
            self._ws = None

    def stop(self):
        self._stop.set()
