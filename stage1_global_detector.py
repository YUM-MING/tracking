"""
1단계: 전역 탐지 (Global Detection)
- YOLO11n-pose를 저해상도로 추론하여 매장 전체의 사람 BBox + 17포인트 뼈대 확보
- ByteTrack으로 프레임 간 고유 ID 유지
- 이 단계는 핫패스(Hot-path)이므로 최대한 가볍게 유지한다.
"""
from __future__ import annotations

import logging
from dataclasses import dataclass, field
from typing import List, Optional

import numpy as np
from ultralytics import YOLO

from config import PipelineConfig

log = logging.getLogger("stage1.global")


@dataclass
class TrackedPerson:
    """전역 탐지 결과 1인분 (다음 단계로 전달되는 최소 데이터)"""
    track_id: int
    bbox: np.ndarray                    # [x1, y1, x2, y2] (원본 프레임 좌표)
    conf: float
    keypoints17: Optional[np.ndarray]   # (17, 2) COCO 키포인트, 없으면 None
    center: tuple = field(init=False)

    def __post_init__(self):
        x1, y1, x2, y2 = self.bbox
        # 발밑 기준점(구역 판정용): 가로 중앙, 세로 하단
        self.center = (float((x1 + x2) / 2), float(y2))

    @property
    def aspect_ratio(self) -> float:
        """가로/세로 비율. 서 있으면 < 1, 쓰러지면 > 1로 커진다."""
        x1, y1, x2, y2 = self.bbox
        h = max(y2 - y1, 1e-6)
        return float((x2 - x1) / h)


class GlobalDetector:
    def __init__(self, cfg: PipelineConfig):
        self.cfg = cfg
        log.info("YOLO 모델 로드 중: %s", cfg.yolo_model)
        self.model = YOLO(cfg.yolo_model)

    def detect(self, frame: np.ndarray) -> List[TrackedPerson]:
        """
        원본 프레임을 받아 추적 결과 리스트 반환.
        persist=True 로 내부 트래커 상태를 프레임 간 유지한다.
        """
        results = self.model.track(
            frame,
            imgsz=self.cfg.yolo_imgsz,
            conf=self.cfg.yolo_conf,
            classes=[0],                 # person만
            tracker=self.cfg.tracker_cfg,
            persist=True,
            verbose=False,
        )

        people: List[TrackedPerson] = []
        r = results[0]
        if r.boxes is None or r.boxes.id is None:
            return people

        boxes = r.boxes.xyxy.cpu().numpy()
        ids = r.boxes.id.cpu().numpy().astype(int)
        confs = r.boxes.conf.cpu().numpy()

        kpts_all = None
        if r.keypoints is not None and r.keypoints.xy is not None:
            kpts_all = r.keypoints.xy.cpu().numpy()  # (N, 17, 2)

        for i, tid in enumerate(ids):
            kp = kpts_all[i] if kpts_all is not None and i < len(kpts_all) else None
            people.append(
                TrackedPerson(
                    track_id=int(tid),
                    bbox=boxes[i],
                    conf=float(confs[i]),
                    keypoints17=kp,
                )
            )
        return people
