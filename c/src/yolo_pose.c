#include "yolo_pose.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgproc.h"
#include "onnxruntime_c_api.h"
#include "ort_cache.h"

/* 윈도우 ORT는 모델 경로가 wchar_t(ORTCHAR_T) — ASCII 파일명 전제 단순 변환 */
#ifdef _WIN32
#define ORT_PATH_DECL(var, utf8) \
    ORTCHAR_T var[512]; mbstowcs(var, (utf8), 512)
#else
#define ORT_PATH_DECL(var, utf8) const char *var = (utf8)
#endif


/* 출력 텐서 레이아웃: (1, 56, N)  — 56 = cx,cy,w,h + conf + 17*(x,y,conf) */
#define OUT_CHANNELS 56

struct YoloPose {
    const OrtApi *ort;
    OrtEnv *env;
    OrtSessionOptions *opts;
    OrtSession *session;
    OrtMemoryInfo *meminfo;
    char *input_name;
    char *output_name;
    OrtAllocator *allocator;

    float *input_buf;          /* 최대 imgsz 기준으로 한 번만 할당해 재사용 */
    int input_buf_size;        /* 현재 버퍼가 감당하는 imgsz */
    const PipelineConfig *cfg;
};

static bool ort_ok(const OrtApi *ort, OrtStatus *st, const char *what) {
    if (st == NULL) return true;
    fprintf(stderr, "[yolo] %s 실패: %s\n", what, ort->GetErrorMessage(st));
    ort->ReleaseStatus(st);
    return false;
}

YoloPose *yolo_create(const PipelineConfig *cfg)
{
    const OrtApi *ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!ort) {
        fprintf(stderr, "[yolo] ONNX Runtime API 획득 실패\n");
        return NULL;
    }

    YoloPose *y = calloc(1, sizeof(YoloPose));
    y->ort = ort;
    y->cfg = cfg;

    if (!ort_ok(ort, ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "kiosk", &y->env), "CreateEnv"))
        goto fail;
    if (!ort_ok(ort, ort->CreateSessionOptions(&y->opts), "CreateSessionOptions"))
        goto fail;

    /* 리소스 가드: torch.set_num_threads 에 해당 — 코어 점유 상한 */
    ort->SetIntraOpNumThreads(y->opts, cfg->max_threads);
    ort->SetInterOpNumThreads(y->opts, 1);

    /* 최적화 그래프 캐시: 첫 실행에 저장, 이후 재최적화 없이 로드
     * (시작 시 CPU 스파이크 감소 — 9/22 회의) */
    char cache_path[512];
    bool cached = ort_opt_cache_fresh(cfg->yolo_model, cache_path,
                                      sizeof(cache_path));
    if (cached) {
        ort->SetSessionGraphOptimizationLevel(y->opts, ORT_DISABLE_ALL);
        fprintf(stderr, "[yolo] 최적화 캐시 로드: %s\n", cache_path);
    } else {
        ort->SetSessionGraphOptimizationLevel(y->opts, ORT_ENABLE_ALL);
        ORT_PATH_DECL(cache_w, cache_path);
        ort->SetOptimizedModelFilePath(y->opts, cache_w);
    }

    ORT_PATH_DECL(model_path, cached ? cache_path : cfg->yolo_model);
    if (!ort_ok(ort, ort->CreateSession(y->env, model_path, y->opts, &y->session),
                "CreateSession(모델 로드)"))
        goto fail;

    if (!ort_ok(ort, ort->GetAllocatorWithDefaultOptions(&y->allocator), "GetAllocator"))
        goto fail;
    if (!ort_ok(ort, ort->SessionGetInputName(y->session, 0, y->allocator, &y->input_name),
                "SessionGetInputName"))
        goto fail;
    if (!ort_ok(ort, ort->SessionGetOutputName(y->session, 0, y->allocator, &y->output_name),
                "SessionGetOutputName"))
        goto fail;
    if (!ort_ok(ort, ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &y->meminfo),
                "CreateCpuMemoryInfo"))
        goto fail;

    /* 전역 추론 크기 기준으로 입력 버퍼 선할당 (크롭 256은 더 작으므로 함께 커버) */
    y->input_buf_size = cfg->yolo_imgsz > cfg->crop_imgsz ? cfg->yolo_imgsz : cfg->crop_imgsz;
    y->input_buf = malloc(sizeof(float) * 3u * y->input_buf_size * y->input_buf_size);

    fprintf(stderr, "[yolo] 모델 로드 완료 (intra-op 스레드 %d)\n", cfg->max_threads);
    return y;

fail:
    yolo_destroy(y);
    return NULL;
}

void yolo_destroy(YoloPose *y)
{
    if (!y) return;
    const OrtApi *ort = y->ort;
    if (y->input_name)  ort->AllocatorFree(y->allocator, y->input_name);
    if (y->output_name) ort->AllocatorFree(y->allocator, y->output_name);
    if (y->meminfo) ort->ReleaseMemoryInfo(y->meminfo);
    if (y->session) ort->ReleaseSession(y->session);
    if (y->opts)    ort->ReleaseSessionOptions(y->opts);
    if (y->env)     ort->ReleaseEnv(y->env);
    free(y->input_buf);
    free(y);
}

static float iou(const float *a, const float *b)
{
    float x1 = a[0] > b[0] ? a[0] : b[0];
    float y1 = a[1] > b[1] ? a[1] : b[1];
    float x2 = a[2] < b[2] ? a[2] : b[2];
    float y2 = a[3] < b[3] ? a[3] : b[3];
    float iw = x2 - x1, ih = y2 - y1;
    if (iw <= 0 || ih <= 0) return 0.f;
    float inter = iw * ih;
    float area_a = (a[2] - a[0]) * (a[3] - a[1]);
    float area_b = (b[2] - b[0]) * (b[3] - b[1]);
    return inter / (area_a + area_b - inter + 1e-9f);
}

/* conf 내림차순 정렬용 */
static int det_cmp(const void *pa, const void *pb)
{
    const Detection *a = pa, *b = pb;
    if (a->conf < b->conf) return 1;
    if (a->conf > b->conf) return -1;
    return 0;
}

int yolo_infer(YoloPose *y, const FrameView *frame,
               int rx1, int ry1, int rx2, int ry2,
               int imgsz, float conf_thres,
               Detection *out, int max_out)
{
    const OrtApi *ort = y->ort;

    if (imgsz > y->input_buf_size) {
        fprintf(stderr, "[yolo] imgsz %d > 버퍼 %d\n", imgsz, y->input_buf_size);
        return 0;
    }

    /* ── 전처리: letterbox + BGR→RGB CHW float ── */
    LetterboxInfo lb;
    letterbox_chw(frame, rx1, ry1, rx2, ry2, y->input_buf, imgsz, &lb);

    /* ── 추론 ── */
    int64_t in_shape[4] = { 1, 3, imgsz, imgsz };
    size_t in_len = sizeof(float) * 3u * imgsz * imgsz;
    OrtValue *in_tensor = NULL, *out_tensor = NULL;
    if (!ort_ok(ort, ort->CreateTensorWithDataAsOrtValue(
                    y->meminfo, y->input_buf, in_len, in_shape, 4,
                    ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in_tensor),
                "CreateTensor"))
        return 0;

    const char *in_names[]  = { y->input_name };
    const char *out_names[] = { y->output_name };
    bool ok = ort_ok(ort, ort->Run(y->session, NULL, in_names,
                                   (const OrtValue *const *)&in_tensor, 1,
                                   out_names, 1, &out_tensor),
                     "Run");
    ort->ReleaseValue(in_tensor);
    if (!ok) return 0;

    /* 출력 shape 확인: (1, 56, N) */
    OrtTensorTypeAndShapeInfo *shape_info = NULL;
    int64_t dims[3] = { 0 };
    size_t ndims = 0;
    ort->GetTensorTypeAndShape(out_tensor, &shape_info);
    ort->GetDimensionsCount(shape_info, &ndims);
    if (ndims == 3) ort->GetDimensions(shape_info, dims, 3);
    ort->ReleaseTensorTypeAndShapeInfo(shape_info);
    if (ndims != 3 || dims[1] != OUT_CHANNELS) {
        fprintf(stderr, "[yolo] 예상 밖 출력 shape (%zu차원, ch=%lld)\n",
                ndims, (long long)(ndims == 3 ? dims[1] : -1));
        ort->ReleaseValue(out_tensor);
        return 0;
    }
    const int n_anchors = (int)dims[2];

    float *raw = NULL;
    ort->GetTensorMutableData(out_tensor, (void **)&raw);

    /* ── 디코드: conf 필터 + letterbox 역변환 ── */
    Detection *cand = malloc(sizeof(Detection) * MAX_DETECTIONS);
    int n_cand = 0;
    const float inv_scale = 1.0f / lb.scale;
    const int rw = rx2 - rx1, rh = ry2 - ry1;

    for (int i = 0; i < n_anchors && n_cand < MAX_DETECTIONS; i++) {
        float conf = raw[4 * n_anchors + i];
        if (conf < conf_thres) continue;

        Detection *d = &cand[n_cand++];
        float cx = raw[0 * n_anchors + i];
        float cy = raw[1 * n_anchors + i];
        float w  = raw[2 * n_anchors + i];
        float h  = raw[3 * n_anchors + i];

        /* letterbox 좌표 → ROI 좌표 */
        float x1 = (cx - w / 2 - lb.pad_x) * inv_scale;
        float y1 = (cy - h / 2 - lb.pad_y) * inv_scale;
        float x2 = (cx + w / 2 - lb.pad_x) * inv_scale;
        float y2 = (cy + h / 2 - lb.pad_y) * inv_scale;
        d->bbox[0] = x1 < 0 ? 0 : (x1 > rw ? (float)rw : x1);
        d->bbox[1] = y1 < 0 ? 0 : (y1 > rh ? (float)rh : y1);
        d->bbox[2] = x2 < 0 ? 0 : (x2 > rw ? (float)rw : x2);
        d->bbox[3] = y2 < 0 ? 0 : (y2 > rh ? (float)rh : y2);
        d->conf = conf;

        for (int k = 0; k < KPT_COUNT; k++) {
            float kx = raw[(5 + 3 * k + 0) * n_anchors + i];
            float ky = raw[(5 + 3 * k + 1) * n_anchors + i];
            float kc = raw[(5 + 3 * k + 2) * n_anchors + i];
            d->kpts[k].x = (kx - lb.pad_x) * inv_scale;
            d->kpts[k].y = (ky - lb.pad_y) * inv_scale;
            d->kpts[k].conf = kc;
        }
    }
    ort->ReleaseValue(out_tensor);

    /* ── NMS (conf 내림차순 그리디) ── */
    qsort(cand, n_cand, sizeof(Detection), det_cmp);
    int n_out = 0;
    for (int i = 0; i < n_cand && n_out < max_out; i++) {
        bool keep = true;
        for (int j = 0; j < n_out; j++) {
            if (iou(cand[i].bbox, out[j].bbox) > y->cfg->yolo_nms_iou) {
                keep = false;
                break;
            }
        }
        if (keep) out[n_out++] = cand[i];
    }
    free(cand);
    return n_out;
}
