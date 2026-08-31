#!/usr/bin/env python3
"""매장 특화 파인튜닝 스크립트 (행동분석/확장 감지 모델 공용).

배경
----
저장소에 포함된 모델은 대규모 공개 데이터(COCO)로 사전학습된 가중치다.
일반 매장에서는 그대로 잘 동작하지만, 조명·화각·유니폼 등이 특수한 매장은
현장 데이터로 파인튜닝하면 정확도가 오른다. 이 스크립트가 그 과정을 자동화한다.

데이터 준비 (ultralytics YOLO 형식)
----------------------------------
  dataset/
    images/train/*.jpg     # 매장 CCTV 캡처 (kiosk_tracking 실행 중 스크린샷 등)
    images/val/*.jpg
    labels/train/*.txt     # 라벨링 도구: labelImg, CVAT, Roboflow 등
    labels/val/*.txt
    data.yaml              # names/클래스 정의

사용
----
  # 사람 pose 모델 파인튜닝 (행동분석 정밀도 강화)
  python3 tools/finetune.py --base yolo11n-pose.pt --data dataset/data.yaml \
      --imgsz 416 --epochs 60 --out models/yolo11n-pose.onnx

  # 확장 검출 모델 파인튜닝 (반려동물/음식 — 매장 오탐 보정)
  python3 tools/finetune.py --base yolo11n.pt --data dataset/data.yaml \
      --imgsz 320 --epochs 60 --out models/yolo11n.onnx

파인튜닝 후 models/의 onnx를 교체하고 재빌드 없이 재실행만 하면 된다
(C 파이프라인은 실행 폴더의 onnx를 로드한다 — 코드 수정 불필요).

엣지 사양(4200U급) 유지 규칙
---------------------------
- base 모델은 n(nano) 크기를 벗어나지 않는다 (s/m은 CPU 예산 초과).
- imgsz는 배포 해상도와 동일하게 (pose 416 / 검출 320).
- 학습은 GPU 머신에서, 배포는 ONNX만 엣지로.
"""
import argparse
from pathlib import Path


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", required=True, help="사전학습 가중치 (yolo11n.pt 등)")
    ap.add_argument("--data", required=True, help="dataset/data.yaml 경로")
    ap.add_argument("--imgsz", type=int, default=320, help="학습/배포 입력 크기")
    ap.add_argument("--epochs", type=int, default=60)
    ap.add_argument("--batch", type=int, default=16)
    ap.add_argument("--out", required=True, help="내보낼 ONNX 경로")
    args = ap.parse_args()

    from ultralytics import YOLO   # 학습 머신에만 필요 (엣지에는 불필요)

    model = YOLO(args.base)
    # 사전학습 가중치에서 시작하는 전이학습 — 소량(수백 장) 데이터로도 효과
    model.train(data=args.data, imgsz=args.imgsz, epochs=args.epochs,
                batch=args.batch, patience=15)

    # 배포용 ONNX (동적 배치, 그래프 단순화 — ORT C API 로드 형식과 동일)
    exported = model.export(format="onnx", dynamic=True,
                            imgsz=args.imgsz, simplify=True)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    Path(exported).rename(out)
    print(f"완료: {out} — 실행 폴더에 복사 후 kiosk_tracking 재시작")


if __name__ == "__main__":
    main()
