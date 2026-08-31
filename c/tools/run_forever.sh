#!/bin/sh
# 크래시 자동 재시작 워치독 (필드 테스트 회의: "꺼지면 안 된다")
# - 비정상 종료 시 3초 후 재시작하고 restarts.log에 이력(시각/종료 코드)을 남긴다.
# - 정상 종료(q, 종료 코드 0)는 재시작하지 않는다.
# - 연속 크래시 폭주 방지: 60초 안에 다시 죽으면 대기 시간을 2배로 늘린다(최대 60초).
#
# 사용: tools/run_forever.sh [kiosk_tracking 인자들...]
#   예: tools/run_forever.sh --no-window --ws=ws://192.168.0.10:8080/events

DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$DIR/build/kiosk_tracking"
LOG="$DIR/restarts.log"
DELAY=3

if [ ! -x "$BIN" ]; then
    echo "실행 파일 없음: $BIN — 먼저 빌드하세요 (cmake --build build -j)" >&2
    exit 1
fi

while :; do
    START=$(date +%s)
    "$BIN" "$@"
    CODE=$?
    [ "$CODE" -eq 0 ] && exit 0          # 정상 종료 — 재시작 안 함

    RUNTIME=$(( $(date +%s) - START ))
    if [ "$RUNTIME" -lt 60 ]; then       # 금방 또 죽음 — 백오프 증가
        DELAY=$(( DELAY * 2 )); [ "$DELAY" -gt 60 ] && DELAY=60
    else
        DELAY=3
    fi
    echo "$(date '+%Y-%m-%d %H:%M:%S') 비정상 종료 (code=$CODE, 가동 ${RUNTIME}초) — ${DELAY}초 후 재시작" | tee -a "$LOG" >&2
    sleep "$DELAY"
done
