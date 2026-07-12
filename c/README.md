# 키오스크 트래킹 파이프라인 — C 포팅

파이썬 파이프라인(`../*.py`)을 순수 C로 포팅한 버전.
"라이브러리 전체를 불러오지 말고, 원리를 파악해서 필요한 것만 직접 구현한다"는
방향에 따라 알고리즘 경로는 전부 직접 구현했다.

## 파이썬판과의 대응 관계

| 파이썬 | C | 비고 |
|---|---|---|
| `config.py` | `src/config.h` | 값 동일 |
| `stage1_global_detector.py` (ultralytics YOLO) | `src/yolo_pose.c` | ONNX Runtime **C API** + 직접 구현한 letterbox/디코드/NMS |
| ultralytics 내장 ByteTrack | `src/tracker.c` | ByteTrack 핵심(고/저신뢰 2단계 매칭)만 직접 구현, 칼만 대신 등속 예측 |
| `reid_tagger.py` | `src/reid.c` | HSV 변환도 직접 구현 (`imgproc.c`) |
| `stage2_roi_router.py` | `src/roi_router.c` | numpy 뷰 → "프레임 포인터+사각형" (zero-copy 동일) |
| `stage3_local_analyzer.py` (**MediaPipe**) | `src/analyzer.c` | ⚠️ 대체: 아래 참고 |
| `stage4_state_machine.py` | `src/fsm.c` | 이벤트 큐는 Win32 조건변수 링버퍼 |
| `event_sender.py` (websocket-client) | `src/ws_sender.c` | Winsock 위에 RFC 6455 직접 구현 (ws:// 전용, wss 미지원) |
| `resource_guard.py` (psutil) | `src/resource.c` | Win32 API 직접 호출 |
| `stage_profiler.py` | `src/profiler.c` | 동일 |
| cv2 (전체) | `src/cv_shim.cpp` + `src/imgproc.c` | OpenCV는 캡처/디버그 창에만 격리, 리사이즈·HSV는 순수 C |

### MediaPipe 대체 (중요)
MediaPipe는 C API를 제공하지 않는다. 3단계 정밀 분석은 **같은 YOLO11n-pose
모델을 크롭 영역에만 256px로 재추론**하는 방식으로 대체했다:
- 전역 추론(416px, 프레임 전체)에서는 사람 1명당 유효 해상도가 낮지만,
  크롭 재추론은 사람 1명이 입력 전체를 차지해 키포인트가 정밀해진다.
- 손들기/상체수평/정면응시 판정 로직은 파이썬판과 동일한 비율 기반.
- 모델 1개를 두 단계가 공유 → 메모리 추가 부담 없음.
- 33포인트가 아닌 17포인트라서 얼굴 비율 시그니처는
  (눈 간격/눈-입) 대신 (눈 간격/눈-코)로 대체됨.

## 빌드

필요: Visual Studio 2022 (C++ 워크로드), CMake

```
# 1) 의존성 (third_party/ — git 미포함, 최초 1회)
#    - https://github.com/microsoft/onnxruntime/releases → onnxruntime-win-x64-1.20.1 → third_party/
#    - https://github.com/opencv/opencv/releases → opencv-4.10.0-windows.exe → third_party/opencv 로 압축 해제
# 2) 모델 (models/yolo11n-pose.onnx — git 미포함, 최초 1회)
pip install ultralytics
python -c "from ultralytics import YOLO; YOLO('yolo11n-pose.pt').export(format='onnx', dynamic=True, imgsz=416, simplify=True)"

# 3) 빌드
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## 실행

```
build\Release\kiosk_tracking.exe                # 기본 웹캠
build\Release\kiosk_tracking.exe rtsp://...     # IP 카메라
build\Release\kiosk_tracking.exe video.mp4      # 파일 (오프라인 테스트)
```

종료: 영상 창 또는 터미널에서 `q`.

## 구조 원칙
- **순수 C 핫패스**: 추론 전후처리, 추적, 재식별, 라우팅, 상태머신, 소켓 전송
  전부 라이브러리 없이 C로 직접 구현. 힙 할당은 초기화 시 1회, 루프 안에서는
  고정 버퍼 재사용 (24시간 가동 시 단편화 방지).
- **C++ 격리**: OpenCV(C++ 전용)는 `cv_shim.cpp` 하나로 격리하고 `extern "C"`
  인터페이스만 노출. 운영 배포(`show_window=false`)에서는 캡처만 쓴다.
- **스레드**: 메인(캡처+추론), 이벤트 워커, 웹소켓 전송, 리소스 모니터 —
  4개 스레드가 조건변수 큐로만 통신 (공유 프레임 없음).
