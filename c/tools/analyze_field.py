#!/usr/bin/env python3
"""현장 수집 데이터 진단 리포트 (표준 라이브러리만)

사용: python3 tools/analyze_field.py <data 폴더>   (events_*.jsonl / journeys_*.jsonl)

9월 사내 카페 테스트 분석을 재현 가능하게 만든 도구. 다음을 출력한다:
  - 이벤트 유형별/일별 집계 (알림 남발 룰 식별)
  - 여정 수·체류 분포 (ID 파편화 지표)
  - 시간대별 여정 (무인 시간대 발생 = 고정 오탐원 → 캘리브레이션 필요 신호)
  - 동선 패턴 상위 (존 설정 이상 식별: 전원 착석 등)
"""
import json, glob, sys, os, collections

d = sys.argv[1] if len(sys.argv) > 1 else "data"

evs, js = [], []
for f in sorted(glob.glob(os.path.join(d, "events_*.jsonl"))):
    for line in open(f):
        try:
            e = json.loads(line); e["day"] = os.path.basename(f)[7:17]; evs.append(e)
        except json.JSONDecodeError:
            pass
for f in sorted(glob.glob(os.path.join(d, "journeys_*.jsonl"))):
    for line in open(f):
        try:
            j = json.loads(line); j["day"] = os.path.basename(f)[9:19]; js.append(j)
        except json.JSONDecodeError:
            pass

days = len({e["day"] for e in evs} | {j["day"] for j in js}) or 1
print(f"■ 기간 {days}일 / 이벤트 {len(evs)}건 (일평균 {len(evs)/days:.0f}) / "
      f"여정 {len(js)}건 (일평균 {len(js)/days:.0f})")

print("\n■ 이벤트 유형별 (일평균 5건 넘는 룰은 오탐/설정 점검 대상):")
for t, c in collections.Counter(e["type"] for e in evs).most_common():
    flag = "  ← 점검" if c / days > 5 else ""
    print(f"  {t:16s} {c:5d} ({c/days:.1f}/일){flag}")

if js:
    dur = sorted(j["duration_sec"] for j in js)
    n = len(dur)
    short = sum(1 for x in dur if x < 30) / n
    print(f"\n■ 체류: 중앙값 {dur[n//2]:.0f}s / 90% {dur[int(n*.9)]:.0f}s / "
          f"30초 미만 {short*100:.0f}%")
    if short > 0.5:
        print("  ⇒ 30초 미만이 절반 이상 = ID 파편화/고정 오탐원 의심 — 자동 캘리브레이션(마스킹) 필요")
    k = sum(j["visited_kiosk"] for j in js) / n
    s = sum(j["sat"] for j in js) / n
    p = sum(j["purchased"] for j in js) / n
    print(f"■ 키오스크 방문 {k*100:.0f}% / 착석 {s*100:.0f}% / 결제 {p*100:.0f}%")
    if s > 0.6:
        print("  ⇒ 착석율 과다 = 테이블 존이 실제 배치와 불일치 의심 — 점주 페이지에서 존 조정")
    if k < 0.1:
        print("  ⇒ 키오스크 방문율 과소 = 트리거 모드(근접/구역)와 카메라 배치 불일치 의심")

    hours = collections.Counter(int(j["enter"][11:13]) for j in js)
    night = sum(hours[h] for h in [0, 1, 2, 3, 4, 23])
    print("\n■ 시간대별 여정:")
    peak = max(hours.values()) if hours else 1
    for h in range(24):
        print(f"  {h:02d}시 {'█' * int(hours[h] * 40 / peak)}{hours[h]}")
    if night / max(n, 1) > 0.03:
        print(f"  ⇒ 심야(23~04시) 여정 {night}건 — 무인 시간 트랙 발생 = 고정 오탐원"
              f"(유리 반사·포스터 등). 자동 캘리브레이션 + 영업시간 설정 필요")

    pat = collections.Counter("→".join(s["step"] for s in j["steps"]) for j in js)
    print("\n■ 동선 패턴 상위 5:")
    for pt, c in pat.most_common(5):
        print(f"  {c:5d}  {pt}")
