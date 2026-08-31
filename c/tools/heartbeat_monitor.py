#!/usr/bin/env python3
"""하트비트 수신 모니터 (필드 테스트 회의: "1분마다 신호 — 안 오면 나한테 알림")

엣지 파이프라인(`--ws=ws://<이 컴퓨터 IP>:8080/events`)이 보내는 이벤트와
하트비트를 받는 WebSocket 서버. 표준 라이브러리만 사용 — pip 설치 불필요.

기능
  - 이벤트/하트비트를 received/ 폴더에 날짜별 JSONL로 기록
  - 하트비트가 N초(기본 180초 = 3회 결손) 이상 끊기면 알림
    (터미널 경고 + macOS 알림센터 + 선택적 웹훅 POST)
  - 오래된 수신 파일 자동 정리 (기본 7일 — 회의의 "쌓이면 클리어" 정책)

사용
  python3 tools/heartbeat_monitor.py --port 8080
  python3 tools/heartbeat_monitor.py --port 8080 --alert-after 180 \
      --webhook https://hooks.slack.com/...   # 슬랙 등으로 알림
"""
import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.request

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

state = {
    "last_hb": None,        # 마지막 하트비트 수신 시각 (None = 아직 없음)
    "last_hb_body": None,   # 마지막 하트비트 내용 (상태 표시용)
    "connected": False,
    "alerted": False,       # 결손 알림 중복 방지 (복구 시 해제)
    "lock": threading.Lock(),
}


def log(msg):
    print(f"[{time.strftime('%m-%d %H:%M:%S')}] {msg}", flush=True)


def notify(title, body, webhook):
    """알림: 터미널 + macOS 알림센터 + 웹훅 (가능한 것만, 실패 무시)"""
    log(f"🚨 {title} — {body}")
    if sys.platform == "darwin":
        try:
            subprocess.run(
                ["osascript", "-e",
                 f'display notification "{body}" with title "{title}"'],
                timeout=5, capture_output=True)
        except Exception:
            pass
    if webhook:
        try:
            req = urllib.request.Request(
                webhook,
                data=json.dumps({"text": f"{title}: {body}"}).encode(),
                headers={"Content-Type": "application/json"})
            urllib.request.urlopen(req, timeout=10)
        except Exception as e:
            log(f"웹훅 전송 실패: {e}")


def watchdog(alert_after, webhook):
    """하트비트 결손 감시 (10초 주기)"""
    while True:
        time.sleep(10)
        with state["lock"]:
            last = state["last_hb"]
            alerted = state["alerted"]
        if last is None:
            continue                     # 첫 하트비트 전에는 판단하지 않음
        gap = time.time() - last
        if gap > alert_after and not alerted:
            with state["lock"]:
                state["alerted"] = True
            notify("파이프라인 생존 신호 끊김",
                   f"{int(gap)}초째 하트비트 없음 — 엣지 확인 필요 (크래시/네트워크)",
                   webhook)
        elif gap <= alert_after and alerted:
            with state["lock"]:
                state["alerted"] = False
            notify("파이프라인 복구", f"하트비트 재개 (결손 {int(gap)}초)", webhook)


def cleanup_loop(out_dir, keep_days):
    """오래된 수신 파일 정리 (1시간 주기)"""
    while True:
        cutoff = time.time() - keep_days * 86400
        try:
            for name in os.listdir(out_dir):
                path = os.path.join(out_dir, name)
                if os.path.isfile(path) and os.path.getmtime(path) < cutoff:
                    os.remove(path)
                    log(f"오래된 수신 파일 삭제: {name} ({keep_days}일 경과)")
        except FileNotFoundError:
            pass
        time.sleep(3600)


def append_jsonl(out_dir, line):
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, f"received_{time.strftime('%Y-%m-%d')}.jsonl")
    with open(path, "a") as f:
        f.write(line + "\n")


def ws_handshake(conn):
    """HTTP Upgrade 요청 수신 → 101 응답. 실패 시 False."""
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(2048)
        if not chunk:
            return False
        data += chunk
        if len(data) > 16384:
            return False
    key = None
    for line in data.split(b"\r\n"):
        if line.lower().startswith(b"sec-websocket-key:"):
            key = line.split(b":", 1)[1].strip().decode()
    if not key:
        return False
    accept = base64.b64encode(
        hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
    conn.sendall((
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
    return True


def recv_exact(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def read_frame(conn):
    """클라이언트 프레임 1개 파싱 → (opcode, payload) 또는 None(연결 종료)"""
    hdr = recv_exact(conn, 2)
    if hdr is None:
        return None
    opcode = hdr[0] & 0x0F
    masked = hdr[1] & 0x80
    length = hdr[1] & 0x7F
    if length == 126:
        ext = recv_exact(conn, 2)
        if ext is None:
            return None
        length = struct.unpack(">H", ext)[0]
    elif length == 127:
        ext = recv_exact(conn, 8)
        if ext is None:
            return None
        length = struct.unpack(">Q", ext)[0]
    mask = recv_exact(conn, 4) if masked else b"\x00" * 4
    if mask is None:
        return None
    payload = recv_exact(conn, length) if length else b""
    if payload is None:
        return None
    if masked:
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return opcode, payload


def handle_client(conn, addr, out_dir, webhook):
    try:
        if not ws_handshake(conn):
            return
        log(f"엣지 연결됨: {addr[0]}")
        with state["lock"]:
            state["connected"] = True
        while True:
            frame = read_frame(conn)
            if frame is None:
                break
            opcode, payload = frame
            if opcode == 0x8:            # close
                break
            if opcode == 0x9:            # ping → pong
                conn.sendall(b"\x8a" + bytes([len(payload)]) + payload)
                continue
            if opcode != 0x1:            # 텍스트만 처리
                continue
            text = payload.decode("utf-8", errors="replace")
            append_jsonl(out_dir, text)
            try:
                msg = json.loads(text)
            except json.JSONDecodeError:
                log(f"JSON 아님(기록만): {text[:80]}")
                continue
            if msg.get("type") == "heartbeat":
                with state["lock"]:
                    state["last_hb"] = time.time()
                    state["last_hb_body"] = msg
                log(f"💓 하트비트 — 가동 {int(msg.get('uptime_sec', 0))}초, "
                    f"CPU {msg.get('cpu_pct', 0)}%, RSS {msg.get('rss_mb', 0)}MB, "
                    f"인원 {msg.get('n_people', 0)}, FPS {msg.get('infer_fps', 0)}")
            else:
                log(f"이벤트: {msg.get('type')} (ID {msg.get('track_id')}) "
                    f"{msg.get('message', '')}")
    finally:
        conn.close()
        with state["lock"]:
            state["connected"] = False
        log(f"엣지 연결 끊김: {addr[0]} (하트비트 결손 감시는 계속)")


def main():
    ap = argparse.ArgumentParser(description="하트비트/이벤트 수신 모니터")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--alert-after", type=int, default=180,
                    help="이 시간(초) 이상 하트비트 없으면 알림 (기본 180)")
    ap.add_argument("--dir", default="received", help="수신 기록 폴더")
    ap.add_argument("--keep-days", type=int, default=7,
                    help="수신 파일 보관 일수 (기본 7일 후 자동 삭제)")
    ap.add_argument("--webhook", default="",
                    help="알림 웹훅 URL (슬랙 등, 비우면 로컬 알림만)")
    args = ap.parse_args()

    threading.Thread(target=watchdog, args=(args.alert_after, args.webhook),
                     daemon=True).start()
    threading.Thread(target=cleanup_loop, args=(args.dir, args.keep_days),
                     daemon=True).start()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(4)
    log(f"수신 대기: ws://0.0.0.0:{args.port} — 엣지는 --ws=ws://<이IP>:{args.port}/events")
    log(f"기록: {args.dir}/received_YYYY-MM-DD.jsonl (보관 {args.keep_days}일), "
        f"알림 기준: 하트비트 {args.alert_after}초 결손")
    while True:
        conn, addr = srv.accept()
        threading.Thread(target=handle_client,
                         args=(conn, addr, args.dir, args.webhook),
                         daemon=True).start()


if __name__ == "__main__":
    main()
