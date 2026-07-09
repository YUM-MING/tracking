"""
하이브리드 캐스케이드 파이프라인 전역 설정
- 구역(Zone) 정의, 트리거 임계값, 모델 파라미터를 한곳에서 관리
"""
from dataclasses import dataclass, field
from typing import List, Optional, Tuple


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

    # ── 프레임 샘플링 (리소스 핵심 제약) ───────────────────
    # 매 프레임 추론하면 4200U급 CPU를 감당할 수 없다.
    # 30fps 입력 기준 5 → 초당 6회 추론. 정확도-부하 트레이드오프 조정용.
    detect_every_n: int = 5

    # ── 리소스 가드 (코어/스레드 제한 + 모니터링) ──────────
    max_threads: int = 2                  # OpenCV/torch/OpenMP 스레드 상한 (1~2코어 정책)
    cpu_affinity: Optional[List[int]] = None  # 예: [2, 3] — 솔루션과 코어 충돌 회피용, None이면 미지정
    monitor_enabled: bool = True          # CPU/RAM 사용량 주기 로깅 (테스트 수칙)
    monitor_interval_sec: float = 5.0
    cpu_alert_pct: float = 85.0           # 프로세스 CPU가 이 값을 넘으면 경고 (순간 피크 감시)

    # ── 1단계: 전역 탐지 (YOLO11n-pose + ByteTrack) ────────
    yolo_model: str = "yolo11n-pose.pt"
    yolo_imgsz: int = 416                 # 저해상도 추론 (핫패스 경량화)
    yolo_conf: float = 0.35
    tracker_cfg: str = "bytetrack.yaml"

    # ── 비율 기반 태깅/재식별 ───────────────────────────────
    # ByteTrack ID는 가림/재입장 시 바뀌므로, 옷 색상·채도 + 신체 비율
    # 시그니처로 안정 ID를 유지한다. (길이가 아닌 비율 기반)
    reid_enabled: bool = True
    reid_match_threshold: float = 0.22    # 시그니처 거리 임계값 (작을수록 엄격)
    reid_gallery_ttl_sec: float = 60.0    # 사라진 트랙 시그니처 보관 시간

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
    face_min_crop_h: int = 160            # 크롭 세로 px가 이보다 작으면(원거리) 얼굴 분석 생략
                                          # → 거리별 모델 전환: 원거리는 스켈레톤만, 근거리에서만 얼굴

    # ── 4단계: 상태 머신 ────────────────────────────────────
    announce_dwell_sec: float = 300.0     # 테이블 300초 + 미구매 → 안내방송
    announce_cooldown_sec: float = 120.0  # 동일 ID 재방송 쿨다운
    stale_track_ttl_sec: float = 5.0      # 트랙 소실 후 상태 유지 시간

    # ── 서버 전송 (WebSocket) ───────────────────────────────
    # 비워두면 로컬 로그만 남긴다. 회사 솔루션은 웹소켓 연결(재연결 포함) 전제.
    ws_url: str = ""                      # 예: "ws://192.168.0.10:8080/events"
    ws_reconnect_min_sec: float = 1.0     # 재연결 백오프 시작값
    ws_reconnect_max_sec: float = 30.0    # 재연결 백오프 상한

    # ── 기타 ────────────────────────────────────────────────
    show_window: bool = True              # 디버그 시각화 창
    log_level: str = "INFO"


CFG = PipelineConfig()
