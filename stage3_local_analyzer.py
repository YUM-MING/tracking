"""
3단계: 지역 정밀 분석 (Local Precision Analysis)
- 2단계에서 넘어온 크롭 이미지에 대해서만 MediaPipe Pose(33포인트) + FaceMesh 실행
- 입력이 작을수록 MediaPipe 연산이 급격히 빨라지는 특성을 활용
- 분석 직후 크롭/리사이즈 버퍼는 스코프 종료와 함께 해제 (비식별 메타데이터만 반환)
"""
from __future__ import annotations

import logging
from dataclasses import dataclass, field
from typing import Dict, Optional

import cv2
import mediapipe as mp
import numpy as np

from config import PipelineConfig
from stage2_roi_router import PrecisionTarget

log = logging.getLogger("stage3.precision")

mp_pose = mp.solutions.pose
mp_face = mp.solutions.face_mesh


@dataclass
class PrecisionResult:
    """상태 머신으로 전달되는 비식별 분석 결과 (이미지 데이터 없음)"""
    track_id: int
    reason: str
    pose_detected: bool = False
    hand_raised: bool = False          # 손목이 어깨보다 위 (키오스크 조작/도움 요청)
    torso_horizontal: bool = False     # 상체 수평 (쓰러짐 확정 신호)
    face_detected: bool = False
    gaze_forward: bool = False         # 얼굴이 카메라(키오스크) 방향
    extra: Dict = field(default_factory=dict)


class LocalPrecisionAnalyzer:
    """
    track_id별로 MediaPipe 인스턴스를 재사용하면 시계열 안정성이 좋아지지만
    엣지 메모리를 아끼기 위해 여기서는 공용 인스턴스 + static_image_mode로 처리.
    """

    def __init__(self, cfg: PipelineConfig):
        self.cfg = cfg
        self.pose = mp_pose.Pose(
            static_image_mode=True,
            model_complexity=cfg.mp_pose_complexity,
            min_detection_confidence=0.5,
        )
        self.face: Optional[mp_face.FaceMesh] = None
        if cfg.mp_face_enabled:
            self.face = mp_face.FaceMesh(
                static_image_mode=True,
                max_num_faces=1,
                refine_landmarks=False,
                min_detection_confidence=0.5,
            )

    def analyze(self, target: PrecisionTarget) -> PrecisionResult:
        res = PrecisionResult(track_id=target.track_id, reason=target.reason)

        # 크롭을 고정 크기로 리사이즈 → MediaPipe 입력
        small = cv2.resize(target.crop_view, self.cfg.mp_input_size)
        rgb = cv2.cvtColor(small, cv2.COLOR_BGR2RGB)
        rgb.flags.writeable = False

        # ── Pose (33 landmarks) ────────────────────────────
        pose_out = self.pose.process(rgb)
        if pose_out.pose_landmarks:
            res.pose_detected = True
            lm = pose_out.pose_landmarks.landmark
            L = mp_pose.PoseLandmark

            l_sh, r_sh = lm[L.LEFT_SHOULDER], lm[L.RIGHT_SHOULDER]
            l_wr, r_wr = lm[L.LEFT_WRIST], lm[L.RIGHT_WRIST]
            l_hip, r_hip = lm[L.LEFT_HIP], lm[L.RIGHT_HIP]

            # 손 들어올림: 어느 한쪽 손목 y가 어깨 y보다 위 (이미지 좌표는 위가 작음)
            sh_y = min(l_sh.y, r_sh.y)
            res.hand_raised = (l_wr.y < sh_y) or (r_wr.y < sh_y)

            # 상체 수평: 어깨 중심과 엉덩이 중심의 y 차이가 x 차이보다 작으면 수평
            sh_cx, sh_cy = (l_sh.x + r_sh.x) / 2, (l_sh.y + r_sh.y) / 2
            hp_cx, hp_cy = (l_hip.x + r_hip.x) / 2, (l_hip.y + r_hip.y) / 2
            res.torso_horizontal = abs(sh_cy - hp_cy) < abs(sh_cx - hp_cx)

        # ── FaceMesh (시선/얼굴 방향) ──────────────────────
        if self.face is not None:
            face_out = self.face.process(rgb)
            if face_out.multi_face_landmarks:
                res.face_detected = True
                flm = face_out.multi_face_landmarks[0].landmark
                # 코끝(1)이 좌우 광대(234, 454)의 x 중앙 부근이면 정면 응시로 근사
                nose_x = flm[1].x
                left_x, right_x = flm[234].x, flm[454].x
                mid = (left_x + right_x) / 2
                span = max(abs(right_x - left_x), 1e-6)
                res.gaze_forward = abs(nose_x - mid) / span < 0.15

        # rgb/small은 함수 종료와 함께 참조 해제 → 이미지 데이터 비잔류
        return res

    def close(self):
        self.pose.close()
        if self.face is not None:
            self.face.close()
