#include "objdet.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgproc.h"
#include "logger.h"
#include "onnxruntime_c_api.h"

/* COCO 검출 모델 출력: (1, 84, N) — 4 box + 80 클래스 점수 */
#define OUT_CHANNELS 84
#define NMS_IOU 0.55f

/* 관심 클래스 테이블 — 여기 없는 클래스는 디코드 단계에서 버린다 */
typedef struct {
    int coco_class;
    ObjCategory category;
    const char *ko;
} ClassEntry;

static const ClassEntry INTEREST[] = {
    { 15, OBJ_PET,  "고양이" },
    { 16, OBJ_PET,  "강아지" },
    { 46, OBJ_FOOD, "바나나" },
    { 47, OBJ_FOOD, "사과" },
    { 48, OBJ_FOOD, "샌드위치" },
    { 49, OBJ_FOOD, "오렌지" },
    { 50, OBJ_FOOD, "브로콜리" },
    { 51, OBJ_FOOD, "당근" },
    { 52, OBJ_FOOD, "핫도그" },
    { 53, OBJ_FOOD, "피자" },
    { 54, OBJ_FOOD, "도넛" },
    { 55, OBJ_FOOD, "케이크" },
    { 39, OBJ_TABLEWARE, "병" },
    { 41, OBJ_TABLEWARE, "컵" },
    { 45, OBJ_TABLEWARE, "그릇" },
};
#define N_INTEREST (int)(sizeof(INTEREST) / sizeof(INTEREST[0]))

static const ClassEntry *interest_lookup(int coco_class)
{
    for (int i = 0; i < N_INTEREST; i++)
        if (INTEREST[i].coco_class == coco_class) return &INTEREST[i];
    return NULL;
}

const char *objdet_class_str(int coco_class)
{
    const ClassEntry *e = interest_lookup(coco_class);
    return e ? e->ko : "객체";
}

struct ObjDet {
    const OrtApi *ort;
    OrtEnv *env;
    OrtSessionOptions *opts;
    OrtSession *session;
    OrtMemoryInfo *meminfo;
    OrtAllocator *allocator;
    char *input_name;
    char *output_name;
    float *input_buf;          /* imgsz 기준 1회 할당 재사용 (루프 내 malloc 0회) */
    const PipelineConfig *cfg;
};

static bool ort_ok(const OrtApi *ort, OrtStatus *st, const char *what)
{
    if (st == NULL) return true;
    LOGE("객체", "%s 실패: %s", what, ort->GetErrorMessage(st));
    ort->ReleaseStatus(st);
    return false;
}

ObjDet *objdet_create(const PipelineConfig *cfg)
{
    /* 모델 파일이 없으면 조용히 비활성 — 클론 직후에도 파이프라인은 돌아야 한다 */
    FILE *probe = fopen(cfg->obj_model, "rb");
    if (!probe) {
        LOGW("객체", "확장 모델 없음(%s) — 반려동물/외부음식 감지 비활성",
             cfg->obj_model);
        return NULL;
    }
    fclose(probe);

    const OrtApi *ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!ort) return NULL;

    ObjDet *d = calloc(1, sizeof(ObjDet));
    d->ort = ort;
    d->cfg = cfg;

    if (!ort_ok(ort, ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "objdet", &d->env),
                "CreateEnv")) goto fail;
    if (!ort_ok(ort, ort->CreateSessionOptions(&d->opts), "CreateSessionOptions"))
        goto fail;
    /* 주 모델(사람 pose)과 코어를 다투지 않도록 1스레드로 제한 */
    ort->SetIntraOpNumThreads(d->opts, 1);
    ort->SetInterOpNumThreads(d->opts, 1);
    ort->SetSessionGraphOptimizationLevel(d->opts, ORT_ENABLE_ALL);

    if (!ort_ok(ort, ort->CreateSession(d->env, cfg->obj_model, d->opts, &d->session),
                "CreateSession")) goto fail;
    if (!ort_ok(ort, ort->GetAllocatorWithDefaultOptions(&d->allocator),
                "GetAllocator")) goto fail;
    if (!ort_ok(ort, ort->SessionGetInputName(d->session, 0, d->allocator, &d->input_name),
                "GetInputName")) goto fail;
    if (!ort_ok(ort, ort->SessionGetOutputName(d->session, 0, d->allocator, &d->output_name),
                "GetOutputName")) goto fail;
    if (!ort_ok(ort, ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &d->meminfo),
                "CreateCpuMemoryInfo")) goto fail;

    d->input_buf = malloc(sizeof(float) * 3u * cfg->obj_imgsz * cfg->obj_imgsz);
    LOGI("객체", "확장 모델 로드 완료 (%dpx, 관심 클래스 %d종, intra-op 1)",
         cfg->obj_imgsz, N_INTEREST);
    return d;

fail:
    objdet_destroy(d);
    return NULL;
}

void objdet_destroy(ObjDet *d)
{
    if (!d) return;
    const OrtApi *ort = d->ort;
    if (d->input_name)  ort->AllocatorFree(d->allocator, d->input_name);
    if (d->output_name) ort->AllocatorFree(d->allocator, d->output_name);
    if (d->meminfo) ort->ReleaseMemoryInfo(d->meminfo);
    if (d->session) ort->ReleaseSession(d->session);
    if (d->opts)    ort->ReleaseSessionOptions(d->opts);
    if (d->env)     ort->ReleaseEnv(d->env);
    free(d->input_buf);
    free(d);
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
    float aa = (a[2] - a[0]) * (a[3] - a[1]);
    float ab = (b[2] - b[0]) * (b[3] - b[1]);
    return inter / (aa + ab - inter + 1e-9f);
}

static int obj_cmp(const void *pa, const void *pb)
{
    const ObjDetection *a = pa, *b = pb;
    if (a->conf < b->conf) return 1;
    if (a->conf > b->conf) return -1;
    return 0;
}

int objdet_infer(ObjDet *d, const FrameView *frame, float conf_thres,
                 ObjDetection *out, int max_out)
{
    if (!d) return 0;
    const OrtApi *ort = d->ort;
    const int imgsz = d->cfg->obj_imgsz;

    /* 전처리: 사람 검출과 동일한 letterbox 재사용 */
    LetterboxInfo lb;
    letterbox_chw(frame, 0, 0, frame->w, frame->h, d->input_buf, imgsz, &lb);

    int64_t in_shape[4] = { 1, 3, imgsz, imgsz };
    OrtValue *in_tensor = NULL, *out_tensor = NULL;
    if (!ort_ok(ort, ort->CreateTensorWithDataAsOrtValue(
                    d->meminfo, d->input_buf, sizeof(float) * 3u * imgsz * imgsz,
                    in_shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in_tensor),
                "CreateTensor"))
        return 0;

    const char *in_names[] = { d->input_name };
    const char *out_names[] = { d->output_name };
    bool ok = ort_ok(ort, ort->Run(d->session, NULL, in_names,
                                   (const OrtValue *const *)&in_tensor, 1,
                                   out_names, 1, &out_tensor), "Run");
    ort->ReleaseValue(in_tensor);
    if (!ok) return 0;

    /* 출력 shape 확인: (1, 84, N) */
    OrtTensorTypeAndShapeInfo *si = NULL;
    int64_t dims[3] = { 0 };
    size_t nd = 0;
    ort->GetTensorTypeAndShape(out_tensor, &si);
    ort->GetDimensionsCount(si, &nd);
    if (nd == 3) ort->GetDimensions(si, dims, 3);
    ort->ReleaseTensorTypeAndShapeInfo(si);
    if (nd != 3 || dims[1] != OUT_CHANNELS) {
        LOGE("객체", "예상 밖 출력 shape (ch=%lld)", (long long)(nd == 3 ? dims[1] : -1));
        ort->ReleaseValue(out_tensor);
        return 0;
    }
    const int n_anchors = (int)dims[2];
    float *raw = NULL;
    ort->GetTensorMutableData(out_tensor, (void **)&raw);

    /* 디코드: 관심 클래스만 + letterbox 역변환 (정적 후보 버퍼) */
    ObjDetection cand[MAX_OBJ_DETECTIONS * 4];
    int n_cand = 0;
    const float inv_scale = 1.0f / lb.scale;

    for (int i = 0; i < n_anchors && n_cand < (int)(sizeof(cand) / sizeof(cand[0])); i++) {
        /* 80클래스 중 최고 점수 클래스 — 관심 클래스가 아니면 즉시 스킵 */
        float best = 0;
        int best_c = -1;
        for (int c = 0; c < 80; c++) {
            float s = raw[(4 + c) * n_anchors + i];
            if (s > best) { best = s; best_c = c; }
        }
        if (best < conf_thres) continue;
        const ClassEntry *e = interest_lookup(best_c);
        if (!e) continue;

        ObjDetection *o = &cand[n_cand++];
        float cx = raw[0 * n_anchors + i], cy = raw[1 * n_anchors + i];
        float w = raw[2 * n_anchors + i], h = raw[3 * n_anchors + i];
        float x1 = (cx - w / 2 - lb.pad_x) * inv_scale;
        float y1 = (cy - h / 2 - lb.pad_y) * inv_scale;
        float x2 = (cx + w / 2 - lb.pad_x) * inv_scale;
        float y2 = (cy + h / 2 - lb.pad_y) * inv_scale;
        o->bbox[0] = x1 < 0 ? 0 : (x1 > frame->w ? (float)frame->w : x1);
        o->bbox[1] = y1 < 0 ? 0 : (y1 > frame->h ? (float)frame->h : y1);
        o->bbox[2] = x2 < 0 ? 0 : (x2 > frame->w ? (float)frame->w : x2);
        o->bbox[3] = y2 < 0 ? 0 : (y2 > frame->h ? (float)frame->h : y2);
        o->conf = best;
        o->coco_class = best_c;
        o->category = e->category;
    }
    ort->ReleaseValue(out_tensor);

    /* NMS (클래스 무관 그리디 — 관심 클래스끼리 겹치면 최고 conf만) */
    qsort(cand, (size_t)n_cand, sizeof(ObjDetection), obj_cmp);
    int n_out = 0;
    for (int i = 0; i < n_cand && n_out < max_out; i++) {
        bool keep = true;
        for (int j = 0; j < n_out; j++) {
            if (iou(cand[i].bbox, out[j].bbox) > NMS_IOU) { keep = false; break; }
        }
        if (keep) out[n_out++] = cand[i];
    }
    return n_out;
}
