/*
 * 검출 파리티 테스트 도구
 * - 이미지/영상 1프레임에 yolo_infer를 돌려 검출 결과를 그대로 출력한다.
 * - 파이썬 ultralytics가 같은 ONNX로 낸 결과와 비교해 전처리(letterbox)/
 *   디코드/NMS 직접 구현이 맞는지 검증하는 용도.
 *
 * 사용: test_detect.exe <이미지|영상> [imgsz] [conf]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "cv_shim.h"
#include "types.h"
#include "yolo_pose.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "사용법: %s <이미지|영상> [imgsz] [conf]\n", argv[0]);
        return 1;
    }
    PipelineConfig cfg = CFG_DEFAULT;
    int imgsz = argc > 2 ? atoi(argv[2]) : cfg.yolo_imgsz;
    float conf = argc > 3 ? (float)atof(argv[3]) : cfg.yolo_conf;

    YoloPose *yolo = yolo_create(&cfg);
    if (!yolo) return 1;

    CvsCapture *cap = cvs_open(argv[1], 0, 0);
    if (!cap) {
        fprintf(stderr, "입력을 열 수 없습니다: %s\n", argv[1]);
        return 1;
    }
    FrameView f;
    if (!cvs_read(cap, &f.data, &f.w, &f.h, &f.stride)) {
        fprintf(stderr, "프레임 읽기 실패\n");
        return 1;
    }

    Detection dets[MAX_DETECTIONS];
    int n = yolo_infer(yolo, &f, 0, 0, f.w, f.h, imgsz, conf,
                       dets, MAX_DETECTIONS);

    printf("frame %dx%d imgsz=%d conf>=%.2f → %d명\n", f.w, f.h, imgsz, conf, n);
    for (int i = 0; i < n; i++) {
        Detection *d = &dets[i];
        printf("#%d conf=%.3f bbox=[%.1f, %.1f, %.1f, %.1f]\n",
               i, d->conf, d->bbox[0], d->bbox[1], d->bbox[2], d->bbox[3]);
        printf("   nose(%.1f,%.1f,%.2f) L_sh(%.1f,%.1f,%.2f) R_sh(%.1f,%.1f,%.2f) "
               "L_hip(%.1f,%.1f,%.2f)\n",
               d->kpts[KPT_NOSE].x, d->kpts[KPT_NOSE].y, d->kpts[KPT_NOSE].conf,
               d->kpts[KPT_L_SHOULDER].x, d->kpts[KPT_L_SHOULDER].y, d->kpts[KPT_L_SHOULDER].conf,
               d->kpts[KPT_R_SHOULDER].x, d->kpts[KPT_R_SHOULDER].y, d->kpts[KPT_R_SHOULDER].conf,
               d->kpts[KPT_L_HIP].x, d->kpts[KPT_L_HIP].y, d->kpts[KPT_L_HIP].conf);
    }

    cvs_close(cap);
    yolo_destroy(yolo);
    return 0;
}
