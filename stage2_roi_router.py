"""
2단계: 동적 관심 구역(Dynamic ROI) 필터링 및 라우팅
- 모든 사람을 MediaPipe로 넘기지 않고, 트리거 조건에 해당하는 ID만 선별
- numpy 슬라이싱은 메모리 복사가 아닌 뷰(view)이므로 포인터 기반 가상 크롭에 해당
- 트리거 조건:
    1) 키오스크 구역 진입
    2) 테이블 구역 N분 이상 정체
    3) 스켈레톤 붕괴(쓰러짐 징후: BBox 종횡비 급변)
"""
from __future__ import annotations

import logging
import time
from dataclasses import dataclass
from typing import Dict, List, Optional

import numpy as np

from config import PipelineConfig
from stage1_global_detector import TrackedPerson

log = logging.getLogger("stage2.roi")


@dataclass
class PrecisionTarget:
    """3단계로 전달되는 정밀 분석 대상"""
    track_id: int
    reason: str                  # "kiosk_enter" | "table_dwell" | "fall_suspect"
    crop_view: np.ndarray        # 원본 프레임의 뷰 (zero-copy)
    crop_origin: tuple           # 크롭의 원본 좌표 (x1, y1) — 좌표 역변환용
    person: TrackedPerson
    frame_h: int = 720           # 원본 프레임 세로 — 크롭 크기를 '비율'로 판정하기 위함


class DynamicROIRouter:
    def __init__(self, cfg: PipelineConfig):
        self.cfg = cfg
        # track_id → 해당 구역 최초 진입 시각
        self._table_enter_ts: Dict[int, float] = {}
        self._last_seen: Dict[int, float] = {}

    # ── 내부 유틸 ──────────────────────────────────────────
    def _crop_view(self, frame: np.ndarray, person: TrackedPerson) -> tuple:
        """BBox에 패딩을 더해 프레임 경계로 클램프한 뒤 numpy 뷰 반환."""
        h, w = frame.shape[:2]
        x1, y1, x2, y2 = person.bbox
        pw = (x2 - x1) * self.cfg.crop_padding
        ph = (y2 - y1) * self.cfg.crop_padding
        cx1 = int(max(0, x1 - pw))
        cy1 = int(max(0, y1 - ph))
        cx2 = int(min(w, x2 + pw))
        cy2 = int(min(h, y2 + ph))
        # 복사 없는 슬라이스 뷰
        return frame[cy1:cy2, cx1:cx2], (cx1, cy1)

    def _check_fall(self, person: TrackedPerson) -> bool:
        """종횡비 기반 1차 판정 + 키포인트 보조 판정."""
        if person.aspect_ratio >= self.cfg.fall_aspect_ratio:
            return True
        kp = person.keypoints17
        if kp is not None and len(kp) >= 13:
            # 코(0)와 엉덩이(11,12)의 y 차이가 거의 없으면 수평 자세 의심
            nose_y = kp[0][1]
            hip_y = (kp[11][1] + kp[12][1]) / 2
            bbox_h = person.bbox[3] - person.bbox[1]
            if bbox_h > 0 and abs(hip_y - nose_y) / bbox_h < 0.15 and nose_y > 0:
                return True
        return False

    def is_kiosk_near(self, p: TrackedPerson, frame_w: int) -> bool:
        """
        얼굴(머리 폭)이 화면 가로 대비 일정 비율 이상 = 키오스크 사용 중.
        몸통 BBox는 앉은 자세/상반신만 잡혀도 커져서 애매하므로 쓰지 않는다.
        키오스크 사용자는 화면(카메라)을 향해 얼굴을 들이밀기 때문에
        얼굴 크기가 가장 확실한 근접 신호다.
        """
        fw = p.face_width()
        if fw is None:
            return False
        return fw / max(frame_w, 1) >= self.cfg.kiosk_face_w_frac

    # ── 메인 라우팅 ────────────────────────────────────────
    def route(self, frame: np.ndarray, people: List[TrackedPerson]) -> List[PrecisionTarget]:
        now = time.monotonic()
        frame_h, frame_w = frame.shape[:2]
        targets: List[PrecisionTarget] = []

        for p in people:
            self._last_seen[p.track_id] = now
            cx, cy = p.center
            reason: Optional[str] = None

            # 키오스크 판정: near 모드(부착 카메라)는 얼굴 크기 비율, zone 모드는 화면 구역
            if self.cfg.kiosk_trigger_mode == "near":
                at_kiosk = self.is_kiosk_near(p, frame_w)
            else:
                at_kiosk = self.cfg.kiosk_zone.contains(cx, cy)

            # 테이블 체류 타이머는 트리거 우선순위와 무관하게 구역 기준으로만 관리
            # (키오스크로 이동해도 타이머가 남는 오탐 방지)
            in_table = self.cfg.table_zone.contains(cx, cy) and not at_kiosk
            if in_table:
                enter = self._table_enter_ts.setdefault(p.track_id, now)
            else:
                self._table_enter_ts.pop(p.track_id, None)

            # 조건 3: 쓰러짐 징후 (최우선)
            # 단, 근접 상태는 제외 — 카메라 앞에 가까이 오면 상반신만 잡혀
            # BBox가 가로로 넓어지고 종횡비 기준이 서있어도 '쓰러짐'으로 오탐한다.
            if not at_kiosk and self._check_fall(p):
                reason = "fall_suspect"

            # 조건 1: 키오스크 근접/진입
            elif at_kiosk:
                reason = "kiosk_enter"

            # 조건 2: 테이블 구역 정체
            elif in_table and now - enter >= self.cfg.table_dwell_trigger_sec:
                reason = "table_dwell"

            if reason:
                crop, origin = self._crop_view(frame, p)
                if crop.size > 0:
                    targets.append(
                        PrecisionTarget(
                            track_id=p.track_id,
                            reason=reason,
                            crop_view=crop,
                            crop_origin=origin,
                            person=p,
                            frame_h=frame_h,
                        )
                    )

        # 연산 예산 보호: 프레임당 정밀 분석 인원 상한 (쓰러짐 의심 우선)
        targets.sort(key=lambda t: 0 if t.reason == "fall_suspect" else 1)
        targets = targets[: self.cfg.max_precision_targets]

        self._gc(now)
        return targets

    def _gc(self, now: float):
        """오래 사라진 트랙의 타이머 정리."""
        ttl = self.cfg.stale_track_ttl_sec
        dead = [tid for tid, ts in self._last_seen.items() if now - ts > ttl]
        for tid in dead:
            self._last_seen.pop(tid, None)
            self._table_enter_ts.pop(tid, None)

    def table_dwell_seconds(self, track_id: int) -> float:
        """상태 머신에서 참조하는 테이블 체류 시간."""
        ts = self._table_enter_ts.get(track_id)
        return 0.0 if ts is None else time.monotonic() - ts
