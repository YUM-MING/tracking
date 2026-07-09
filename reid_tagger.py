"""
비율 기반 태깅/재식별 (Appearance Tagger)
- 입장 시 옷 색상·채도 + 스켈레톤 신체 **비율**로 시그니처를 만들어
  ByteTrack ID가 바뀌어도(가림, 잠깐 퇴장 후 재진입) 동일인에게
  안정 ID(stable_id)를 유지한다.
- 길이(px) 기반은 거리·화각에 따라 오차가 크므로 반드시 비율만 사용한다.
- 시그니처는 스칼라 몇 개(색상 2쌍 + 비율 2개)로, 원본 이미지는 저장하지 않는다.
"""
from __future__ import annotations

import logging
import time
from dataclasses import dataclass
from typing import Dict, List, Optional

import cv2
import numpy as np

from config import PipelineConfig
from stage1_global_detector import TrackedPerson

log = logging.getLogger("stage1.reid")

# COCO 키포인트 인덱스
L_SHOULDER, R_SHOULDER = 5, 6
L_HIP, R_HIP = 11, 12
L_ANKLE, R_ANKLE = 15, 16


@dataclass
class Signature:
    """비식별 외형 시그니처 (색상은 HSV 평균, 비율은 무차원)"""
    upper_hue: float          # 상의 색상 (0~180, 순환값)
    upper_sat: float          # 상의 채도 (0~255)
    lower_hue: float          # 하의 색상
    lower_sat: float          # 하의 채도
    shoulder_torso: Optional[float]   # 어깨너비 / 상체길이
    leg_torso: Optional[float]        # 다리길이 / 상체길이


def _hue_dist(a: float, b: float) -> float:
    """OpenCV Hue(0~180)는 순환값이므로 원형 거리로 계산 후 0~1 정규화."""
    d = abs(a - b)
    return min(d, 180 - d) / 90.0


def _sig_distance(a: Signature, b: Signature) -> float:
    """가용한 성분만 골라 가중 평균 거리 (0에 가까울수록 동일인)."""
    parts: List[float] = [
        _hue_dist(a.upper_hue, b.upper_hue),
        abs(a.upper_sat - b.upper_sat) / 255.0,
        _hue_dist(a.lower_hue, b.lower_hue),
        abs(a.lower_sat - b.lower_sat) / 255.0,
    ]
    if a.shoulder_torso is not None and b.shoulder_torso is not None:
        parts.append(min(abs(a.shoulder_torso - b.shoulder_torso), 1.0))
    if a.leg_torso is not None and b.leg_torso is not None:
        parts.append(min(abs(a.leg_torso - b.leg_torso) / 2.0, 1.0))
    return float(np.mean(parts))


def _ema(old: Signature, new: Signature, alpha: float = 0.3) -> Signature:
    """살아있는 트랙의 시그니처를 지수이동평균으로 갱신 (조명 변화 완충)."""
    def mix(o, n):
        if o is None:
            return n
        if n is None:
            return o
        return o * (1 - alpha) + n * alpha
    return Signature(
        upper_hue=mix(old.upper_hue, new.upper_hue),
        upper_sat=mix(old.upper_sat, new.upper_sat),
        lower_hue=mix(old.lower_hue, new.lower_hue),
        lower_sat=mix(old.lower_sat, new.lower_sat),
        shoulder_torso=mix(old.shoulder_torso, new.shoulder_torso),
        leg_torso=mix(old.leg_torso, new.leg_torso),
    )


class AppearanceTagger:
    """
    사용법: 1단계 detect() 직후 assign(frame, people) 호출.
    각 TrackedPerson.track_id가 안정 ID로 치환되어 이후 단계(ROI, 상태 머신)의
    체류 타이머·구매 플래그가 ID 스위칭에도 끊기지 않는다.
    """

    def __init__(self, cfg: PipelineConfig):
        self.cfg = cfg
        self._next_stable = 1
        self._raw_to_stable: Dict[int, int] = {}       # ByteTrack ID → 안정 ID
        self._live_sig: Dict[int, Signature] = {}      # 안정 ID → 시그니처(EMA)
        self._raw_last_seen: Dict[int, float] = {}
        self._lost: Dict[int, tuple] = {}              # 안정 ID → (시그니처, 소실 시각)

    # ── 시그니처 추출 ──────────────────────────────────────
    def _signature(self, frame: np.ndarray, p: TrackedPerson) -> Signature:
        h, w = frame.shape[:2]
        x1, y1, x2, y2 = [int(v) for v in p.bbox]
        x1, y1 = max(0, x1), max(0, y1)
        x2, y2 = min(w, x2), min(h, y2)
        bh = max(y2 - y1, 1)

        def region_hs(fy1: float, fy2: float) -> tuple:
            """BBox 세로 구간(비율)의 HSV 평균. 뷰 기반이라 복사 없음."""
            ry1 = y1 + int(bh * fy1)
            ry2 = y1 + int(bh * fy2)
            roi = frame[ry1:ry2, x1:x2]
            if roi.size == 0:
                return 0.0, 0.0
            hsv = cv2.cvtColor(roi, cv2.COLOR_BGR2HSV)
            return float(np.mean(hsv[:, :, 0])), float(np.mean(hsv[:, :, 1]))

        u_hue, u_sat = region_hs(0.20, 0.50)   # 상의(어깨~허리 부근)
        l_hue, l_sat = region_hs(0.55, 0.85)   # 하의(허리~무릎 아래)

        # 신체 비율 (키포인트가 잡힌 경우에만)
        st = lt = None
        kp = p.keypoints17
        if kp is not None and len(kp) >= 17:
            def pt(i):
                x, y = kp[i]
                return None if (x <= 0 and y <= 0) else np.array([x, y])
            ls, rs = pt(L_SHOULDER), pt(R_SHOULDER)
            lh, rh = pt(L_HIP), pt(R_HIP)
            la, ra = pt(L_ANKLE), pt(R_ANKLE)
            if ls is not None and rs is not None and lh is not None and rh is not None:
                sh_mid, hip_mid = (ls + rs) / 2, (lh + rh) / 2
                torso = float(np.linalg.norm(sh_mid - hip_mid))
                if torso > 1e-3:
                    st = float(np.linalg.norm(ls - rs)) / torso
                    if la is not None and ra is not None:
                        ank_mid = (la + ra) / 2
                        lt = float(np.linalg.norm(hip_mid - ank_mid)) / torso

        return Signature(u_hue, u_sat, l_hue, l_sat, st, lt)

    # ── 메인 할당 ──────────────────────────────────────────
    def assign(self, frame: np.ndarray, people: List[TrackedPerson]):
        now = time.monotonic()

        for p in people:
            raw_id = p.track_id
            self._raw_last_seen[raw_id] = now
            sig = self._signature(frame, p)

            sid = self._raw_to_stable.get(raw_id)
            if sid is None:
                sid = self._match_lost(sig, now)
                if sid is None:
                    sid = self._next_stable
                    self._next_stable += 1
                else:
                    log.info("재식별: raw %d → 안정 ID %d 복원", raw_id, sid)
                self._raw_to_stable[raw_id] = sid

            old = self._live_sig.get(sid)
            self._live_sig[sid] = _ema(old, sig) if old else sig
            p.track_id = sid  # 이후 파이프라인은 안정 ID 사용

        self._gc(now)

    def _match_lost(self, sig: Signature, now: float) -> Optional[int]:
        """최근 소실된 시그니처 중 가장 가까운 것을 복원 (임계값 이내일 때만)."""
        best_sid, best_d = None, self.cfg.reid_match_threshold
        for sid, (lost_sig, _) in self._lost.items():
            d = _sig_distance(sig, lost_sig)
            if d < best_d:
                best_sid, best_d = sid, d
        if best_sid is not None:
            self._lost.pop(best_sid, None)
        return best_sid

    def _gc(self, now: float):
        # 사라진 raw 트랙 → 소실 갤러리로 이동
        ttl = self.cfg.stale_track_ttl_sec
        for raw_id in [r for r, ts in self._raw_last_seen.items() if now - ts > ttl]:
            sid = self._raw_to_stable.pop(raw_id, None)
            self._raw_last_seen.pop(raw_id, None)
            if sid is not None and sid in self._live_sig:
                self._lost[sid] = (self._live_sig.pop(sid), now)
        # 보관 시간이 지난 소실 시그니처 폐기
        gttl = self.cfg.reid_gallery_ttl_sec
        for sid in [s for s, (_, ts) in self._lost.items() if now - ts > gttl]:
            del self._lost[sid]
