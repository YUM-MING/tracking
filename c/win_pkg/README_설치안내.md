# 무인매장 트래킹 파이프라인 — 윈도우 설치 안내

압축을 푸는 것으로 설치가 끝납니다. 별도 프로그램 설치가 필요 없습니다.

## 요구 사항

- Windows 10 / 11 (64비트)
- 카메라: USB 웹캠 또는 RTSP IP 카메라

## 실행 (3단계)

1. **압축을 아무 폴더에 풉니다** (예: `C:\kiosk_tracking`)
2. **`START.bat` 더블클릭**
   - 점주 페이지(`http://localhost:8765`)가 브라우저로 자동 열립니다
   - 첫 실행 시 Windows 방화벽 창이 뜨면 **[허용]** 을 눌러주세요
   - 프로그램이 비정상 종료되면 3초 후 자동 재시작됩니다 (`restarts.log`에 이력)
3. 점주 페이지에서 실시간 인원·상태 확인, 룰/민감도/구역 설정

종료: 검은 콘솔 창에서 `q` 입력 (또는 콘솔 창 닫기).

## 카메라 지정

기본은 **첫 번째 웹캠**입니다. 다른 카메라를 쓰려면 `START.bat`를
메모장으로 열어 `kiosk_tracking.exe` 줄 끝에 인자를 추가하세요:

```
kiosk_tracking.exe --no-window --log=pipeline.log 1                ← 두 번째 웹캠
kiosk_tracking.exe --no-window --log=pipeline.log rtsp://주소      ← IP 카메라
kiosk_tracking.exe --no-window --log=pipeline.log "video=카메라이름" ← 장치 이름 직접 지정
kiosk_tracking.exe --no-window --log=pipeline.log 영상파일.mp4      ← 파일 재생 (오프라인 테스트)
```

웹캠이 안 잡히면 콘솔에 사용 가능한 장치 이름이 표시됩니다.

## 쌓이는 데이터 (분석용)

| 파일 | 내용 |
|---|---|
| `data\tracking.db` | **SQLite DB — 이 파일 하나만 복사해 가면 전체 데이터를 볼 수 있습니다** (events/journeys 테이블) |
| `data\events_날짜.jsonl` | 알림 이벤트 (텍스트, 1건 = 1줄) |
| `data\journeys_날짜.jsonl` | 손님 1명당 동선 요약 (입장~퇴장, 키오스크·착석·결제 여부) |
| `pipeline.log` | 통합 로그 (문제 발생 시 원인 확인) |
| `store_settings.json` | 점주 페이지에서 저장한 설정 (재시작 후 유지) |

이미지·영상·개인정보는 일절 저장/전송하지 않습니다.

## 부팅 시 자동 시작 (선택)

작업 스케줄러(Win+R → `taskschd.msc`) → 작업 만들기 →
트리거 "시스템 시작 시" → 동작에 `run_forever.bat` 지정
(START.bat와 동일하나 브라우저를 자동으로 열지 않는 버전).

## 원격 모니터링 연동 (선택)

관리자 컴퓨터에서 하트비트 수신 서버를 켜두면, 매장 프로그램이 꺼졌을 때
알림을 받을 수 있습니다. `START.bat`의 실행 줄에 추가:

```
--ws=ws://관리자컴퓨터IP:8080/events
```

이러면 1분마다 생존 신호(CPU/메모리/인원 포함)가 전송됩니다.

## 문제 해결

| 증상 | 확인 |
|---|---|
| 카메라가 안 열림 | 콘솔의 장치 목록 확인 → `"video=이름"` 으로 지정. 다른 프로그램이 카메라 점유 중인지 확인 |
| 점주 페이지 접속 안 됨 | 방화벽 허용 여부, 콘솔에 "점주 페이지: http://..." 로그 확인 |
| 자꾸 재시작됨 | `pipeline.log` 마지막 부분과 `restarts.log`를 개발자에게 전달 |
| 모델 로드 실패 | 압축 해제가 완전한지 확인 (`yolo11n-pose.onnx` 등 3개 파일 존재) |

문의: 로그 파일(`pipeline.log`) 첨부해서 개발자에게 전달해 주세요.
