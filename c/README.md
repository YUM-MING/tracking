# 무인매장 키오스크 트래킹 파이프라인 (C / macOS)

파이썬 프로토타입을 순수 C로 포팅한 엣지 비전 AI 파이프라인.
"라이브러리 전체를 불러오지 말고, 원리를 파악해서 필요한 것만 직접 구현한다"는
방향에 따라 알고리즘 경로는 전부 직접 구현했다.

8월 회의(최적화·행동 필터·점주 관점 기능) 반영으로 다음이 추가되었다:
- **생산자-소비자 멀티스레드**: 캡처와 추론을 lock-free 트리플 버퍼(`framebus.c`)로 분리
- **점주 페이지**: 내장 HTTP 서버(`admin_server.c`) + 웹 UI — 룰 체크박스 ON/OFF,
  민감도 슬라이더, 구역 드래그 설정, 실시간 알림/오탐 피드백, POS 결제 웹훅
- **설명 가능한 로깅**: 고객 동선 기록(`journey.c`)이 모든 알림에 증거로 첨부
  ("14:00 입장 → 14:02 키오스크 → 14:05 착석 …")
- **행동 패턴 필터**: 키오스크 미방문 착석, 구매 후 허용량(1잔당 N시간) 초과 체류
  (룰 기반 우선, AI 최소화)
- **행동 분석 — 정지 상태 맥락 파악**: 손목 키포인트 이동량(EMA)으로
  '작업 중(공부·노트북·레고 조립)'을 분류 — 몸이 정지해도 손이 움직이면 정상 이용으로
  보고 체류 알림에서 제외 (점주 선택, 별도 모델 없이 기존 스켈레톤 재사용)
- **자동 캘리브레이션(세팅)**: 점주 페이지 [세팅 시작하기] → 매장을 비운 채 N분(기본 60분)
  가동 → 그동안 '사람'으로 잡힌 위치(거울·포스터·TV·창밖)를 인식 제외 마스크로 자동 생성
  (`mask.h`, 32×18 셀). 수동 칠하기/지우기도 지원 — **오류 보완은 코드 수정이 아니라
  점주 페이지 설정으로** 해결하는 구조
- **판단 로그**: 모든 판정(구역 진입/이탈, 트리거 전환, 룰 발행/억제와 그 근거)이
  DEBUG 레벨로 기록 — 점주 페이지 '상세 판단 로그' 토글 또는 설정으로 활성화
- **발열 보호**: ① 절전 모드 — 무인 N초 지속 시 추론 주기 4배 확장,
  ② 영업시간 모드 — 시간 밖에는 캡처를 2초/1프레임으로 낮추고 추론 중단 (자가진단 유지)
- **카메라 자가 진단**(`camhealth.c`): 백화현상·블랙아웃·초점 상실·프레임 정지 알림
- **통합 로깅**(`logger.c`): [시각][레벨][모듈] 형식, 점주 페이지에서 원격 열람
- **점주 설정 영속화**(`store_settings.c`): JSON 저장 + 오탐 피드백 자동 캘리브레이션
- **확장 감지 모델 2종** (전부 저장소 포함 — 클론만 하면 동작):
  - `objdet.c` + `models/yolo11n.onnx` (11MB, COCO): **반려동물**(강아지/고양이)과
    **외부 음식**(피자·샌드위치·케이크류 10종) 감지. 320px + 사람 추론 K회당
    1회(기본 ~1Hz) + 관심 클래스 12종만 디코드 — 실측 31ms/회 (환산 +6ms/추론)
  - `age_est.c` + `models/genderage.onnx` (1.3MB, InsightFace): **노키즈존** 나이 추정.
    키오스크 근접자만, 트랙당 표가 확정될 때까지만 추론 (성인/미성년 결론 후 자동 중단)
- **추가 룰 3종**: 비품 어뷰징(비품 구역 반복 방문 — 모델 불필요),
  일행 수 vs 주문 수 불일치(POS 잔 수 웹훅), 반려동물/외부음식/노키즈존 알림
- **제로샷 이상행동 분석**(`behavior.c`): 별도 행동인식 모델 없이 ByteTrack식
  추적 좌표·관절 이동 벡터의 기구학 특징(손목 스윙 속도, 몸 이동/급강하 속도,
  근접도 — 전부 BBox 높이·시간 정규화)만으로 판정:
  **폭력**(2인 근접+고속 스윙), **기물 파손**(단독 제자리 고속 스윙),
  **낙상 빠른 경로**(급강하+자세 붕괴), **장시간 배회**(착석·구매 없는 이동),
  **키오스크 무단 조작**(장시간 연속 점유). 민감도는 다이얼 1개로 통합 제어
- **스마트 청소 알림**: 테이블 구역 인원이 0이 되는 전환(퇴석) 5초 후
  확장 모델로 잔여물(컵/병/그릇/음식) 확인 → 있을 때만 정리 알림
- **키오스크 동적 추천 컨텍스트**: 근접 전환 시 동행 수+시간대를 이벤트로 발행
  (팝업 표출은 키오스크 SW/서버 담당 — 8/6 기획의 엣지 측 구현)
- **오탐율 폐루프 제어**: 룰별 (알림 수, 오탐 신고 수)를 집계해 오탐율이
  목표(기본 5%, 점주 조정)를 넘으면 해당 룰 기준을 자동 보수화.
  구조적 억제(디바운스+다수결+쿨다운) 위에 얹힌 2차 방어선
- **파인튜닝 파이프라인**(`tools/finetune.py`): 매장 데이터 축적 후 현장 특화
  파인튜닝 → ONNX 교체만으로 반영 (코드 수정·재빌드 불필요)

## 파이썬판과의 대응 관계

| 파이썬 | C | 비고 |
|---|---|---|
| `config.py` | `src/config.h` | 값 동일 |
| `stage1_global_detector.py` (ultralytics YOLO) | `src/yolo_pose.c` | ONNX Runtime **C API** + 직접 구현한 letterbox/디코드/NMS |
| ultralytics 내장 ByteTrack | `src/tracker.c` | ByteTrack 핵심(고/저신뢰 2단계 매칭)만 직접 구현, 칼만 대신 등속 예측 |
| `reid_tagger.py` | `src/reid.c` | HSV 변환도 직접 구현 (`imgproc.c`) |
| `stage2_roi_router.py` | `src/roi_router.c` | numpy 뷰 → "프레임 포인터+사각형" (zero-copy 동일) |
| `stage3_local_analyzer.py` (**MediaPipe**) | `src/analyzer.c` | ⚠️ 대체: 아래 참고 |
| `stage4_state_machine.py` | `src/fsm.c` | 이벤트 큐는 pthread 조건변수 링버퍼 |
| `event_sender.py` (websocket-client) | `src/ws_sender.c` | BSD 소켓 위에 RFC 6455 직접 구현 (ws:// 전용, wss 미지원) |
| `resource_guard.py` (psutil) | `src/resource.c` | Mach(`task_info`/`host_statistics`) + `getrusage` 직접 호출. CPU affinity 고정은 macOS 미지원(무시) |
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

## 빌드 (macOS)

원래 Windows(MSVC + Winsock)로 작성됐으나, 지금은 macOS(Apple Silicon/Intel) 기준으로
포팅되어 있다. 스레딩(pthread), 소켓(BSD socket), 리소스 모니터링(Mach/getrusage),
콘솔 입력(termios)이 전부 POSIX 구현으로 교체되었다. Windows 빌드가 다시 필요하면
git 히스토리의 이전 CMakeLists.txt/소스를 참고.

필요: Xcode Command Line Tools, Homebrew, CMake

**모델 3종은 저장소에 포함되어 있어 클론 후 바로 빌드·실행된다.**

```
git clone https://github.com/YUM-MING/hunik-kiosk-tracking-mac-ver.git
cd hunik-kiosk-tracking-mac-ver

# 1) 의존성
brew install onnxruntime opencv cmake

# 2) 빌드 (모델은 자동으로 실행 폴더에 복사됨)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 3) 실행
build/kiosk_tracking
```

### 포함 모델 (사양 최적화 구성)

| 파일 | 역할 | 크기 | 입력 | 실행 조건 |
|---|---|---|---|---|
| `yolo11n-pose.onnx` | 사람 검출+스켈레톤 (주 모델) | 12MB | 416px (크롭 256px) | 매 추론 |
| `yolo11n.onnx` | 반려동물·외부음식 (COCO) | 11MB | 320px | 사람 추론 K회당 1회, 룰 켜졌을 때만 |
| `genderage.onnx` | 나이 추정 (노키즈존) | 1.3MB | 96px 얼굴 크롭 | 키오스크 근접자만, 결론까지만 |

모델 파일이 없어도 빌드·실행은 되며 해당 룰만 자동 비활성된다.

**라이선스 주의**: yolo11n 계열 가중치는 Ultralytics **AGPL-3.0**,
genderage.onnx(InsightFace buffalo)는 **비상업 연구용**으로 배포된 모델이다.
상용 판매 전에 Ultralytics 상용 라이선스와 나이 추정 모델의 상용 대체
(자체 학습 또는 상용 라이선스 모델)를 검토해야 한다.

### 파인튜닝 (매장 특화)

포함 모델은 대규모 공개 데이터(COCO 등) 사전학습 가중치다. 특수한 조명/화각의
매장은 현장 캡처를 라벨링해 `tools/finetune.py`로 전이학습하면 정확도가 오른다:

```
python3 tools/finetune.py --base yolo11n.pt --data dataset/data.yaml \
    --imgsz 320 --epochs 60 --out models/yolo11n.onnx
```

학습은 GPU 머신에서 하고 엣지에는 ONNX만 교체한다 — 재빌드 불필요.
일상적인 오탐 보정은 파인튜닝 전에 점주 페이지의 오탐 신고/캘리브레이션으로
해결되도록 설계되어 있다 (코드·모델 수정은 최후 수단).

## 실행

```
build/kiosk_tracking                # 기본 웹캠 (최초 실행 시 macOS가 카메라 권한을 물어봄)
build/kiosk_tracking rtsp://...     # IP 카메라
build/kiosk_tracking video.mp4      # 파일 (오프라인 테스트)
build/kiosk_tracking --no-window    # 헤드리스 (운영 배포)
build/kiosk_tracking --ws=ws://192.168.0.10:8080/events   # 서버 전송
build/kiosk_tracking --admin-port=8765   # 점주 페이지 포트 (0 = 끔, 기본 8765)
build/kiosk_tracking --log=pipeline.log  # 통합 로그 파일 기록 (기본 stderr만)
build/kiosk_tracking --data-dir=data     # 파일럿 데이터 JSONL 축적 폴더 (기본 data, --data-dir= 로 끔)
build/kiosk_tracking --privacy           # 비식별 표시 — 영상을 열화상풍으로 뭉개고 박스/뼈대만 표시
build/kiosk_tracking --imgsz=320         # 전역 추론 해상도 A/B (기본 416 — 320이면 연산 약 40%↓)
```

리소스 절감 구조 (9/22 회의 "합산 20~30% 목표" 대응):
- **디코딩 다이어트**: 헤드리스(`--no-window`) 운영 시 추론에 쓸 프레임만 색 변환·게시
  (30fps 입력·초당 6추론 기준 변환 작업 80% 제거, 판정 품질 무손실)
- **시작 스파이크 제거**: ONNX 그래프 최적화 결과를 `<모델>.opt.onnx`로 캐싱 —
  최초 1회만 최적화하고 이후 기동은 즉시 로드 (실측 5.4초 → 0.9초)
- 분석 주기(점주 페이지 슬라이더)·`--imgsz`·절전/영업시간 모드로 추가 조절

운영(필드 테스트) 도구 — [필드 테스트 계획서](docs/field_test_plan.md) 참고:

```
tools/run_forever.sh --no-window ...       # 크래시 자동 재시작 + restarts.log 이력
python3 tools/heartbeat_monitor.py --port 8080   # 수신 서버: 하트비트 3분 결손 시 알림
```

`--ws=` 설정 시 1분마다 생존 신호(uptime/CPU/RSS/인원/FPS)가 자동 전송된다.

종료: 영상 창 또는 터미널에서 `q` (터미널이 실제 tty일 때만 동작).
모델(`yolo11n-pose.onnx`)과 점주 페이지(`owner_page.html`)는 실행 파일과 같은
폴더에서 찾는다 (빌드 시 자동 복사).
onnxruntime/OpenCV는 Homebrew가 설치한 dylib을 `@rpath`로 그대로 참조한다
(별도 복사 불필요 — Windows판의 DLL 복사 단계에 해당).

## 파일럿 데이터 축적 (SQLite + JSONL)

실행 중 발생한 데이터가 `--data-dir`(기본 `data/`)에 쌓인다.
이미지·개인정보는 저장하지 않는다 (동선 단계·시각·메시지만).

| 파일 | 내용 |
|---|---|
| `tracking.db` | **SQLite 원본 DB (8/31 회의 채택)** — events/journeys 테이블. 파일 하나만 복사하면 전체 데이터가 그대로 보인다 |
| `events_YYYY-MM-DD.jsonl` | 이벤트 1건 = 1줄 (grep/pandas 즉석 분석용 병행 기록) |
| `journeys_YYYY-MM-DD.jsonl` | 퇴장 손님 1명 = 1줄: 입장/퇴장 시각, 체류 시간, 키오스크 방문·착석·결제 여부, 단계별 `steps[]` |

SQLite 조회 예: `sqlite3 data/tracking.db "SELECT time,type,message FROM events ORDER BY ts DESC LIMIT 20"` 

분석 예 (시간대별 방문 수, 키오스크 전환율, 평균 체류):

```python
import pandas as pd, json
j = pd.DataFrame(json.loads(l) for l in open("data/journeys_2026-08-31.jsonl"))
print(len(j), "명 방문 /", j.visited_kiosk.mean(), "키오스크 전환율 /",
      j.duration_sec.mean(), "초 평균 체류")
```

## 윈도우 패키징 (필드 테스트 배포 — 8/31 회의)

맥에서 윈도우용 배포 zip을 크로스 빌드로 만든다 (윈도우 PC에서는 압축 해제 후
`START.bat` 더블클릭이 전부):

```
brew install mingw-w64
# third_party/win/ 에 onnxruntime win-x64 zip과 ffmpeg win64 shared zip 압축 해제
tools/package_win.sh          # → zip(포터블) + setup.exe(설치형, NSIS) 동시 생성
```

- 캡처는 OpenCV 대신 FFmpeg C API(`cv_shim_ffmpeg.c`) — 웹캠(dshow)/RTSP/파일 지원
- 윈도우판은 디버그 창 미지원(헤드리스) — 확인은 점주 페이지 오버레이로
- 스레드는 winpthreads로 pthread 그대로, 소켓·시간·리소스 계측은 `os_compat.h`에 격리
- 동봉물: exe + onnxruntime/FFmpeg DLL + **MSVC 런타임(vcruntime140·msvcp140 계열)** +
  모델 3종 + 점주 페이지 + 자동재시작 bat + 설치 안내서 — 풀릴리즈(클린 PC에서 무설치 실행)
- 설치형: `win_pkg/installer.nsi` (NSIS) — 사용자 폴더 설치라 관리자 권한 불필요,
  바로가기·제거 프로그램 등록, 제거 시 수집 데이터 보존

## 점주 페이지

실행 후 `http://<엣지IP>:8765` 접속 (매장 LAN 전용 — 인증 없음, 외부 노출 금지).

| 기능 | 설명 |
|---|---|
| 매장 상태 | 현재 인원, 추론 FPS, CPU/RSS, 카메라 자가 진단, 운영 모드(정상/절전/영업시간외/캘리브레이션) |
| 룰 체크박스 | 쓰러짐/안내방송/미방문 착석/도움 요청/하드웨어 알림/초과 체류/작업 중 배려/절전/상세 로그 ON·OFF — 시스템이 강제하지 않고 점주가 선택 |
| 민감도 슬라이더 | 체류 기준·쿨다운·쓰러짐 종횡비·키오스크 근접 비율·검출 신뢰도·분석 주기·쓰러짐 확정 횟수·결제당 허용 체류·작업 판정 민감도·절전 대기 |
| 구역 · 인식 범위 | 캔버스 드래그로 테이블/키오스크 구역 지정 + 인식 제외 셀 칠하기/지우기, 실시간 인원 오버레이 |
| 자동 세팅 | [세팅 시작하기] → 빈 매장 N분 가동 → 오탐 위치 자동 마스킹 (진행률 표시, 중지/전체 해제) |
| 영업시간 | 시간대 지정 — 시간 밖에는 저전력 모드 (발열 보호) |
| 실시간 알림 | 이벤트 + 동선 증거 표시. **오탐 신고** 버튼 → 해당 임계값 자동 완화 후 저장 |
| POS 웹훅 | `POST /api/purchase {"track_id":N}` — 결제 내역과 체류/미구매 판정 크로스체크 |

설정은 `store_settings.json`(실행 폴더)에 저장되어 재시작 후에도 유지된다.
회의에서 나온 확장 룰(반려동물/노키즈존/외부음식/비품 어뷰징/일행 수 대조)도
전부 체크박스로 동작한다 — 룰 21종 + 슬라이더 20종 + 구역 3종 + 인식 제외 마스크. 이상행동(폭력·파손·낙상·배회·무단조작)과 청소 알림·추천 컨텍스트, 오탐율 목표(기본 5%) 자동 제어까지 전부 점주 페이지에서 켜고 끄고 조정한다.

## 스레드 구조 (8/9 최적화 보고 계획의 구현)

```
[캡처(생산자)] ──lock-free 트리플 버퍼──▶ [추론(소비자=메인)]
                                            │ 이벤트 큐(조건변수)
                             ┌──────────────┼──────────────┐
                        [이벤트 워커]   [웹소켓 전송]   [점주 HTTP]      [리소스 모니터]
```

- 캡처/추론 분리로 카메라 I/O가 추론을 막지 않는다. 소비자가 밀리면 낡은 프레임은
  자동 폐기(드롭 카운트는 점주 페이지 status에 표시) — 항상 최신 프레임만 처리.
- 트리플 버퍼는 뮤텍스 없이 C11 `atomic_exchange` 하나로 동작 (`framebus.c`).
- macOS는 코어 고정(affinity)을 지원하지 않아 스레드 역할별 QoS 클래스로 대체
  (`apply_thread_role`): 캡처/추론 → 성능 코어 유도, 통신/모니터 → 효율 코어.
  리눅스(4200U) 이식 시 이 함수만 `pthread_setaffinity_np`로 교체하면 된다.
- 힙 할당은 초기화 시 1회 — 메인 루프 안 malloc 0회 원칙 유지.

### 스레드/리소스 현황 (macOS 실측)

| # | 스레드 | 역할 | 부하 특성 |
|---|---|---|---|
| 1 | 캡처 (생산자) | 카메라 → 트리플 버퍼 복사 | I/O 대기 위주 |
| 2 | 추론 (메인=소비자) | YOLO·추적·재식별·ROI·정밀분석·룰 | 유일한 무거운 연산 |
| 3 | 이벤트 워커 | 큐 소비 → 로그/점주 페이지/WS 전달 | 거의 유휴 |
| 4 | 웹소켓 전송 | 서버 전송 + 재연결 백오프 | 거의 유휴 |
| 5 | 점주 페이지 HTTP | 설정/상태/이벤트 API | 요청 시에만 |
| 6 | 리소스 모니터 | CPU/RSS 5초 주기 계측 | 무시 가능 |

- OS 스레드 실측(`ps -M`): **총 ~21개** = 설계 스레드 6(위 표)
  + ORT 내부 스레드풀(세션 3개: pose 2 / objdet 1 / age 1, 추론 중에만 활동)
  + OpenCV·AVFoundation 캡처 헬퍼 + macOS dispatch 큐.
  무거운 연산은 추론 스레드 1곳에 격리되어 있어 8/9 보고의
  "6스레드 분배 + 시스템 예비 여유" 계획과 일치한다.
- 실측(M-series Mac, 1280×720, skip 5): proc CPU ≈ 47% (1코어 기준, 8코어 환산 ≈ 6%),
  sys CPU 10~14%, RSS ≈ 180~250MB. KPI였던 "시스템 점유 5~7%대" 범위.
  확장 감지(objdet)는 K회당 1회라 환산 +6ms/추론, 이상행동 분석(behavior.c)은
  순수 산술이라 1ms 미만.
- 추가 감축 수단(전부 점주 페이지에서 조절): 분석 주기 슬라이더(skip↑),
  절전 모드(무인 시 주기 4배), 영업시간 모드(야간 추론 중단), `--no-window`.

### 오탐율 제어 (목표 5%)

오탐은 3중 구조로 억제하고, 잔여 오탐은 폐루프가 목표치로 수렴시킨다:
1. **구조적 억제**: 모든 룰이 연속 샘플 디바운스(쓰러짐 N회·스윙 스트릭·나이 표 80%)
   + 쿨다운/트랙당 1회 + 구역 게이트를 거친다.
2. **점주 피드백**: 알림 카드 [오탐 신고] → 해당 룰 임계값 1단계 완화 + 저장.
3. **자동 거버너**: 룰별 오탐율(신고/알림)이 목표(기본 5%)를 넘으면
   추가 보수화를 자동 적용하고 측정 창을 리셋. 현황은 점주 페이지
   '알림 신뢰도' 표와 `/api/fpstats`로 확인.

## 문서

- `docs/camera_install_guide.md` — 하드웨어 엔지니어용 카메라 화각/설치 조건 (8/23 액션아이템)
- `docs/age_estimation_research.md` — 노키즈존용 경량 나이 추정 모델 리서치 (8/23 액션아이템)
- `docs/feature_status.md` — 회의록 대비 기능 현황 전수 체크리스트 (구현/자리만/미착수 + 사유)

## 테스트 도구

| 도구 | 용도 |
|---|---|
| `test_logic` | 라우터/상태머신/재식별 + 동선/자가진단/프레임버스/점주설정 단위 테스트 (모델 불필요, 95 케이스) |
| `test_detect <이미지\|영상> [imgsz] [conf]` | 검출 결과 출력 — 파이썬 ultralytics와 파리티 비교용 |
| `test_ws ws://...` | 웹소켓 클라이언트 실통신 검증 (이벤트 3건 전송, 동선 메타 포함) |

검증 이력 (2026-07-13, macOS/Apple Silicon 포팅 후):
- bus.jpg 기준 C 검출 결과가 ultralytics(동일 ONNX)와 박스 ~2-8px, conf ~0.01-0.02 이내 일치
- test_logic 22/22 통과
- 파이썬 websockets 서버로 JSON 스키마·UTF-8·이스케이프 수신 확인
- mp4 파이프라인 완주(헤드리스), 리소스 모니터(Mach API) CPU/RSS 정상 측정 확인

## 구조 원칙
- **순수 C 핫패스**: 추론 전후처리, 추적, 재식별, 라우팅, 상태머신, 소켓 전송
  전부 라이브러리 없이 C로 직접 구현. 힙 할당은 초기화 시 1회, 루프 안에서는
  고정 버퍼 재사용 (24시간 가동 시 단편화 방지).
- **C++ 격리**: OpenCV(C++ 전용)는 `cv_shim.cpp` 하나로 격리하고 `extern "C"`
  인터페이스만 노출. 운영 배포(`show_window=false`)에서는 캡처만 쓴다.
- **스레드**: 캡처(생산자), 추론(소비자=메인), 이벤트 워커, 웹소켓 전송,
  점주 페이지 HTTP, 리소스 모니터 — 6개 스레드가 lock-free 버퍼/조건변수 큐/
  락 걸린 스냅샷 복사로만 통신 (공유 프레임 없음).
