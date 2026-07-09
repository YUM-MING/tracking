"""
하이브리드 캐스케이드 파이프라인 전역 설정
- 구역(Zone) 정의, 트리거 임계값, 모델 파라미터를 한곳에서 관리
"""
from dataclasses import dataclass, field
from typing import Tuple, List


@dataclass(frozen=True)
class Zone:
    """매장 내 관심 구역 (프레임 좌표 기준 다각형 대신 단순 사각형 사용)"""
    name: str
    x1: int
    y1: int
    x2: int
    y2: int

    def contains(self, cx: float, cy: float) -> bool:
        return self.x1 <= cx <= self.x2 and self.y1 <= cy <= self.y2


@dataclass
class PipelineConfig:
    # ── 카메라 ──────────────────────────────────────────────
    camera_source: int | str = 0          # 0 = 웹캠, 또는 RTSP URL
    frame_width: int = 1280
    frame_height: int = 720

    # ── 1단계: 전역 탐지 (YOLO11n-pose + ByteTrack) ────────
    yolo_model: str = "yolo11n-pose.pt"
    yolo_imgsz: int = 416                 # 저해상도 추론 (핫패스 경량화)
    yolo_conf: float = 0.35
    tracker_cfg: str = "bytetrack.yaml"

    # ── 2단계: 동적 ROI 트리거 조건 ────────────────────────
    kiosk_zone: Zone = field(default_factory=lambda: Zone("kiosk", 900, 100, 1280, 600))
    table_zone: Zone = field(default_factory=lambda: Zone("table", 0, 300, 700, 720))
    table_dwell_trigger_sec: float = 180.0    # 테이블 구역 3분 정체 → 정밀 분석
    fall_aspect_ratio: float = 1.4            # BBox 가로/세로 비율 (쓰러짐 징후)
    crop_padding: float = 0.15                # 크롭 시 BBox 여유 비율
    max_precision_targets: int = 3            # 프레임당 MediaPipe 최대 처리 인원

    # ── 3단계: 지역 정밀 분석 (MediaPipe) ──────────────────
    mp_input_size: Tuple[int, int] = (256, 256)   # 크롭 리사이즈 크기
    mp_pose_complexity: int = 0                    # 0 = lite (엣지 최적화)
    mp_face_enabled: bool = True

    # ── 4단계: 상태 머신 ────────────────────────────────────
    announce_dwell_sec: float = 300.0     # 테이블 300초 + 미구매 → 안내방송
    announce_cooldown_sec: float = 120.0  # 동일 ID 재방송 쿨다운
    stale_track_ttl_sec: float = 5.0      # 트랙 소실 후 상태 유지 시간

    # ── 기타 ────────────────────────────────────────────────
    show_window: bool = True              # 디버그 시각화 창
    log_level: str = "INFO"


CFG = PipelineConfig()
