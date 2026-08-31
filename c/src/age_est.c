#include "age_est.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgproc.h"
#include "logger.h"
#include "onnxruntime_c_api.h"

#define AGE_INPUT 96           /* genderage.onnx 고정 입력 크기 */

struct AgeEst {
    const OrtApi *ort;
    OrtEnv *env;
    OrtSessionOptions *opts;
    OrtSession *session;
    OrtMemoryInfo *meminfo;
    OrtAllocator *allocator;
    char *input_name;
    char *output_name;
    float input_buf[3 * AGE_INPUT * AGE_INPUT];   /* 고정 크기 — 정적 멤버 */
};

static bool ort_ok(const OrtApi *ort, OrtStatus *st, const char *what)
{
    if (st == NULL) return true;
    LOGE("나이", "%s 실패: %s", what, ort->GetErrorMessage(st));
    ort->ReleaseStatus(st);
    return false;
}

AgeEst *age_create(const PipelineConfig *cfg)
{
    FILE *probe = fopen(cfg->age_model, "rb");
    if (!probe) {
        LOGW("나이", "나이 추정 모델 없음(%s) — 노키즈존 감지 비활성", cfg->age_model);
        return NULL;
    }
    fclose(probe);

    const OrtApi *ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!ort) return NULL;

    AgeEst *a = calloc(1, sizeof(AgeEst));
    a->ort = ort;

    if (!ort_ok(ort, ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "age", &a->env),
                "CreateEnv")) goto fail;
    if (!ort_ok(ort, ort->CreateSessionOptions(&a->opts), "CreateSessionOptions"))
        goto fail;
    ort->SetIntraOpNumThreads(a->opts, 1);        /* 초경량 모델 — 1스레드로 충분 */
    ort->SetInterOpNumThreads(a->opts, 1);
    ort->SetSessionGraphOptimizationLevel(a->opts, ORT_ENABLE_ALL);

    if (!ort_ok(ort, ort->CreateSession(a->env, cfg->age_model, a->opts, &a->session),
                "CreateSession")) goto fail;
    if (!ort_ok(ort, ort->GetAllocatorWithDefaultOptions(&a->allocator),
                "GetAllocator")) goto fail;
    if (!ort_ok(ort, ort->SessionGetInputName(a->session, 0, a->allocator, &a->input_name),
                "GetInputName")) goto fail;
    if (!ort_ok(ort, ort->SessionGetOutputName(a->session, 0, a->allocator, &a->output_name),
                "GetOutputName")) goto fail;
    if (!ort_ok(ort, ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &a->meminfo),
                "CreateCpuMemoryInfo")) goto fail;

    LOGI("나이", "나이 추정 모델 로드 완료 (96px, 1.3MB)");
    return a;

fail:
    age_destroy(a);
    return NULL;
}

void age_destroy(AgeEst *a)
{
    if (!a) return;
    const OrtApi *ort = a->ort;
    if (a->input_name)  ort->AllocatorFree(a->allocator, a->input_name);
    if (a->output_name) ort->AllocatorFree(a->allocator, a->output_name);
    if (a->meminfo) ort->ReleaseMemoryInfo(a->meminfo);
    if (a->session) ort->ReleaseSession(a->session);
    if (a->opts)    ort->ReleaseSessionOptions(a->opts);
    if (a->env)     ort->ReleaseEnv(a->env);
    free(a);
}

/*
 * 눈 간격으로 얼굴 사각형 근사.
 * InsightFace는 랜드마크 정렬(warp)을 쓰지만, 우리는 이미 pose 키포인트가
 * 있으므로 눈 중심 기준 정사각형 크롭으로 근사한다 (정확도 소폭 손해,
 * 얼굴 검출 모델 1개를 아끼는 트레이드오프 — 리서치 문서 참고).
 */
bool age_face_box(const TrackedPerson *p, float min_conf, int frame_w, int frame_h,
                  int *fx1, int *fy1, int *fx2, int *fy2)
{
    if (!kpt_valid(p, KPT_L_EYE, min_conf) || !kpt_valid(p, KPT_R_EYE, min_conf))
        return false;
    float ex = (p->kpts[KPT_L_EYE].x + p->kpts[KPT_R_EYE].x) / 2;
    float ey = (p->kpts[KPT_L_EYE].y + p->kpts[KPT_R_EYE].y) / 2;
    float eye_dist = fabsf(p->kpts[KPT_L_EYE].x - p->kpts[KPT_R_EYE].x);
    if (eye_dist < 8.f) return false;             /* 너무 원거리 — 신뢰 불가 */

    /* 통계적 비율: 얼굴 폭 ≈ 눈 간격 × 2.4, 눈은 얼굴 세로의 40% 지점 */
    float half = eye_dist * 1.2f;
    *fx1 = (int)(ex - half);
    *fx2 = (int)(ex + half);
    *fy1 = (int)(ey - half * 0.8f);
    *fy2 = (int)(ey + half * 1.2f);
    if (*fx1 < 0) *fx1 = 0;
    if (*fy1 < 0) *fy1 = 0;
    if (*fx2 > frame_w) *fx2 = frame_w;
    if (*fy2 > frame_h) *fy2 = frame_h;
    return *fx2 - *fx1 >= 24 && *fy2 - *fy1 >= 24;
}

int age_estimate(AgeEst *a, const FrameView *frame,
                 int fx1, int fy1, int fx2, int fy2)
{
    if (!a) return -1;
    const OrtApi *ort = a->ort;

    /* 전처리: letterbox(0~1 RGB) 재사용 후 ×255 — genderage는 0~255 입력 */
    LetterboxInfo lb;
    letterbox_chw(frame, fx1, fy1, fx2, fy2, a->input_buf, AGE_INPUT, &lb);
    for (int i = 0; i < 3 * AGE_INPUT * AGE_INPUT; i++)
        a->input_buf[i] *= 255.0f;

    int64_t in_shape[4] = { 1, 3, AGE_INPUT, AGE_INPUT };
    OrtValue *in_tensor = NULL, *out_tensor = NULL;
    if (!ort_ok(ort, ort->CreateTensorWithDataAsOrtValue(
                    a->meminfo, a->input_buf, sizeof(a->input_buf),
                    in_shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in_tensor),
                "CreateTensor"))
        return -1;

    const char *in_names[] = { a->input_name };
    const char *out_names[] = { a->output_name };
    bool ok = ort_ok(ort, ort->Run(a->session, NULL, in_names,
                                   (const OrtValue *const *)&in_tensor, 1,
                                   out_names, 1, &out_tensor), "Run");
    ort->ReleaseValue(in_tensor);
    if (!ok) return -1;

    float *raw = NULL;
    ort->GetTensorMutableData(out_tensor, (void **)&raw);
    /* 출력 [여성 logit, 남성 logit, 나이/100] — 나이만 사용 (비식별) */
    int age = (int)lroundf(raw[2] * 100.0f);
    ort->ReleaseValue(out_tensor);
    if (age < 0) age = 0;
    if (age > 100) age = 100;
    return age;
}
