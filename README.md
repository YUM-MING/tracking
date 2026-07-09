# YOLO + MediaPipe 하이브리드 캐스케이드 파이프라인

매장 전체 다중 객체 추적(전역)과 특정 구역 정밀 행동 분석(지역)을
저사양 엣지 기기(4200U급, 24시간 가동) 한 대에서 동시에 수행하는 파이프라인입니다.

## 구조

| 단계 | 파일 | 역할 |
|---|---|---|
| 0. 리소스 가드 | `resource_guard.py` | 스레드/코어 제한(1~2코어 정책), CPU/RAM 상시 모니터링 및 피크 경고 |
| 1. 전역 탐지 | `stage1_global_detector.py` | YOLO11n-pose(416px 저해상도) + ByteTrack으로 전원 추적, 고유 ID 부여 |
| 1.5 태깅/재식별 | `reid_tagger.py` | 옷 색상·채도 + 신체 **비율** 시그니처로 안정 ID 유지 (ID 스위칭/재입장 대응) |
| 2. 동적 ROI | `stage2_roi_router.py` | 키오스크 진입 / 테이블 3분 정체 / 쓰러짐 징후 ID만 선별, numpy 뷰 기반 zero-copy 크롭 |
| 3. 정밀 분석 | `stage3_local_analyzer.py` | 크롭에만 MediaPipe Pose(33pt) + 6점 FaceDetection 적용 → 손 들기, 상체 수평, 시선 방향 |
| 4. 상태 머신 | `stage4_state_machine.py` | 순수 조건문 로직. 300초 체류+미구매 → 안내방송, 쓰러짐 확정 → 긴급 이벤트를 비동기 큐로 발행 |
| 5. 서버 전송 | `event_sender.py` | 웹소켓(재연결 백오프 포함)으로 이벤트 JSON만 전송. 원본 영상은 절대 전송하지 않음 |

## 설계 원칙 (미팅 합의 반영)

- **프레임 스킵**: 매 프레임 추론하지 않고 `detect_every_n`(기본 5)프레임마다 1회 추론.
  스킵 프레임에서는 직전 결과를 재사용한다.
- **코어/스레드 제한**: OpenCV·torch·OpenMP 스레드를 `max_threads`(기본 2)로 제한하고,
  필요 시 `cpu_affinity`로 유휴 코어에 지정 할당해 기존 솔루션과의 충돌을 피한다.
- **테스트 수칙**: 실행 중 항상 CPU/RAM 사용량이 로깅되며(`ResourceMonitor`),
  기기 전체 CPU가 임계값(기본 85%)을 넘으면 피크 경고를 남긴다.
- **폴리곤 금지 / 비율 기반**: 468점 FaceMesh 대신 6키포인트 FaceDetection을 사용하고,
  얼굴·신체 식별은 길이(px)가 아닌 **비율**로만 계산한다.
- **거리별 모델 전환**: 원거리(작은 크롭)는 스켈레톤만, 근거리(`face_min_crop_h` 이상)에서만
  얼굴 분석을 추가한다.
- **개인정보 최소화**: 이미지 데이터는 3단계 분석 직후 참조 해제되고, 상태 머신·서버에는
  `(track_id, 상태 코드)` 형태의 비식별 메타데이터만 전달된다. 재식별 시그니처도
  색상 평균과 비율 스칼라 몇 개일 뿐 원본 이미지·얼굴 특징 벡터를 저장하지 않는다.

## 설치 및 실행

```bash
pip install -r requirements.txt
python main.py               # 웹캠
python main.py rtsp://IP/... # IP 카메라
```

최초 실행 시 `yolo11n-pose.pt` 가중치가 자동 다운로드됩니다.

## 커스터마이징

`config.py` 한 곳에서 조정합니다.

- `detect_every_n`: 프레임 스킵 간격 (부하 ↔ 반응성 트레이드오프)
- `max_threads` / `cpu_affinity`: 코어 사용 정책
- `kiosk_zone`, `table_zone`: 카메라 화각에 맞춰 구역 좌표 수정 (5평/20평 등 매장별 세팅)
- `table_dwell_trigger_sec` / `announce_dwell_sec`: 정밀분석 트리거(180초) / 방송 트리거(300초)
- `max_precision_targets`: 추론 프레임당 MediaPipe 처리 인원 상한 (연산 예산)
- `mp_pose_complexity`: 0(lite)~2(heavy), 엣지 기기는 0 권장
- `face_min_crop_h`: 얼굴 분석을 켜는 근거리 기준 (크롭 세로 px)
- `reid_*`: 비율 기반 재식별 on/off 및 매칭 민감도
- `ws_url`: 서버 웹소켓 주소. 비우면 로컬 로그만 남김

## 서버 인터페이스 (웹소켓 이벤트 스키마)

```json
{
  "type": "announce_dwell | fall_alert | kiosk_assist",
  "track_id": 3,
  "ts": 1751856000.0,
  "message": "ID 3 테이블 320초 체류(미구매) — 안내방송 재생",
  "meta": {}
}
```

## 실전 연동 지점

- 구매 여부: 결제 웹훅에서 `StateMachine.mark_purchased(track_id)` 호출
- 오디오/알림: `stage4_state_machine.EventWorker.handle()` 내부에 aplay, MQTT 등 연결
- 코어 분리: 1단계를 전담 프로세스로 띄우려면 `multiprocessing` + 공유 메모리 프레임 버퍼로 확장
- (로드맵 10월) 키오스크 근접 시 연령·성별 통계: 3단계 근거리 게이트 통과 시점에 분류기 1회 호출로 확장
