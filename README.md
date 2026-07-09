# YOLO + MediaPipe 하이브리드 캐스케이드 파이프라인

매장 전체 다중 객체 추적(전역)과 특정 구역 정밀 행동 분석(지역)을
저사양 엣지 기기 한 대에서 동시에 수행하는 4단계 파이프라인 구현입니다.

## 구조

| 단계 | 파일 | 역할 |
|---|---|---|
| 1. 전역 탐지 | `stage1_global_detector.py` | YOLO11n-pose(416px 저해상도) + ByteTrack으로 전원 추적, 고유 ID 부여 |
| 2. 동적 ROI | `stage2_roi_router.py` | 키오스크 진입 / 테이블 3분 정체 / 쓰러짐 징후 ID만 선별, numpy 뷰 기반 zero-copy 크롭 |
| 3. 정밀 분석 | `stage3_local_analyzer.py` | 크롭에만 MediaPipe Pose(33pt) + FaceMesh 적용 → 손 들기, 상체 수평, 시선 방향 추출 |
| 4. 상태 머신 | `stage4_state_machine.py` | 순수 조건문 로직. 300초 체류+미구매 → 안내방송, 쓰러짐 확정 → 긴급 이벤트를 비동기 큐로 발행 |

이미지 데이터는 3단계 분석 직후 참조 해제되며, 상태 머신에는
`(track_id, 상태 코드)` 형태의 비식별 메타데이터만 전달됩니다.

## 설치 및 실행

```bash
pip install -r requirements.txt
python main.py               # 웹캠
python main.py rtsp://IP/... # IP 카메라
```

최초 실행 시 `yolo11n-pose.pt` 가중치가 자동 다운로드됩니다.

## 커스터마이징

`config.py` 한 곳에서 조정합니다.

- `kiosk_zone`, `table_zone`: 카메라 화각에 맞춰 구역 좌표 수정
- `table_dwell_trigger_sec` / `announce_dwell_sec`: 정밀분석 트리거(180초) / 방송 트리거(300초)
- `max_precision_targets`: 프레임당 MediaPipe 처리 인원 상한 (연산 예산)
- `mp_pose_complexity`: 0(lite)~2(heavy), 엣지 기기는 0 권장

## 실전 연동 지점

- 구매 여부: 결제 웹훅에서 `StateMachine.mark_purchased(track_id)` 호출
- 오디오/알림: `stage4_state_machine.EventWorker.handle()` 내부에 aplay, MQTT 등 연결
- 코어 분리: 1단계를 전담 프로세스로 띄우려면 `multiprocessing` + 공유 메모리 프레임 버퍼로 확장
