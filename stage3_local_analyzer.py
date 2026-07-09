"""
3단계: 지역 정밀 분석 (Local Precision Analysis)
- 2단계에서 넘어온 크롭 이미지에 대해서만 MediaPipe Pose(33포인트) 실행
- 얼굴은 6키포인트 FaceDetection 사용 — FaceMesh(468점 폴리곤)는
  "폴리곤 방식 금지" 원칙(리소스 감당 불가)에 따라 배제하고,
  얼굴 분석은 **비율 기반**(눈 간격/눈-입 거리)으로 접근한다.
- 거리별 모델 전환: 크롭이 작은(원거리) 대상은 스켈레톤만 보고,
  근거리(키오스크 접근 등)에서만 얼굴 분석을 추가한다.
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
mp_facedet = mp.solutions.face_detection


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
        self.face: Optional[mp_facedet.FaceDetection] = None
        if cfg.mp_face_enabled:
            # model_selection=0: 2m 이내 근거리 모델 (키오스크 접근 상황에 부합)
            self.face = mp_facedet.FaceDetection(
                model_selection=0,
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

        # ── FaceDetection 6키포인트 (시선/얼굴 방향, 비율 기반) ──
        # 거리 게이트: 크롭이 화면 대비 작으면(원거리) 얼굴 분석을 생략해 연산 절약
        crop_frac = target.crop_view.shape[0] / max(target.frame_h, 1)
        if self.face is not None and crop_frac >= self.cfg.face_min_crop_frac:
            face_out = self.face.process(rgb)
            if face_out.detections:
                res.face_detected = True
                kp = face_out.detections[0].location_data.relative_keypoints
                r_eye, l_eye, nose, mouth = kp[0], kp[1], kp[2], kp[3]
                eye_mid_x = (r_eye.x + l_eye.x) / 2
                eye_dist = max(abs(l_eye.x - r_eye.x), 1e-6)
                # 코끝이 양눈 x 중앙 부근이면 정면 응시로 근사 (눈 간격 대비 비율)
                res.gaze_forward = abs(nose.x - eye_mid_x) / eye_dist < 0.35
                # 얼굴 비율 시그니처 (비식별 스칼라): 눈 간격 / 눈-입 세로 거리
                eye_mid_y = (r_eye.y + l_eye.y) / 2
                v = abs(mouth.y - eye_mid_y)
                if v > 1e-6:
                    res.extra["face_ratio"] = round(eye_dist / v, 3)

        # rgb/small은 함수 종료와 함께 참조 해제 → 이미지 데이터 비잔류
        return res

    def close(self):
        self.pose.close()
        if self.face is not None:
            self.face.close()
