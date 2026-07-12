#include "reid.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgproc.h"

#define MAX_ENTRIES 256        /* raw 매핑/시그니처/소실 갤러리 공통 상한 */

/* 비식별 외형 시그니처 (색상은 HSV 평균, 비율은 무차원) */
typedef struct {
    float upper_hue, upper_sat;    /* 상의 색상(0~180 순환)/채도(0~255) */
    float lower_hue, lower_sat;    /* 하의 */
    float shoulder_torso;          /* 어깨너비/상체길이, <0 = 없음 */
    float leg_torso;               /* 다리길이/상체길이, <0 = 없음 */
} Signature;

typedef struct { bool used; int raw_id; int stable_id; double last_seen; } RawMap;
typedef struct { bool used; bool has_sig; int stable_id; Signature sig; } LiveSig;
typedef struct { bool used; int stable_id; Signature sig; double lost_ts; } LostSig;

struct Reid {
    const PipelineConfig *cfg;
    int next_stable;
    RawMap raw[MAX_ENTRIES];       /* 트래커 ID → 안정 ID */
    LiveSig live[MAX_ENTRIES];     /* 안정 ID → 시그니처(EMA) */
    LostSig lost[MAX_ENTRIES];     /* 소실 갤러리 */
};

Reid *reid_create(const PipelineConfig *cfg)
{
    Reid *r = calloc(1, sizeof(Reid));
    r->cfg = cfg;
    r->next_stable = 1;
    return r;
}

void reid_destroy(Reid *r) { free(r); }

/* OpenCV Hue(0~180)는 순환값이므로 원형 거리로 계산 후 0~1 정규화 */
static float hue_dist(float a, float b)
{
    float d = fabsf(a - b);
    if (180.f - d < d) d = 180.f - d;
    return d / 90.f;
}

/* 가용한 성분만 골라 평균 거리 (0에 가까울수록 동일인) */
static float sig_distance(const Signature *a, const Signature *b)
{
    float sum = hue_dist(a->upper_hue, b->upper_hue)
              + fabsf(a->upper_sat - b->upper_sat) / 255.f
              + hue_dist(a->lower_hue, b->lower_hue)
              + fabsf(a->lower_sat - b->lower_sat) / 255.f;
    int n = 4;
    if (a->shoulder_torso >= 0 && b->shoulder_torso >= 0) {
        float d = fabsf(a->shoulder_torso - b->shoulder_torso);
        sum += d > 1.f ? 1.f : d;
        n++;
    }
    if (a->leg_torso >= 0 && b->leg_torso >= 0) {
        float d = fabsf(a->leg_torso - b->leg_torso) / 2.f;
        sum += d > 1.f ? 1.f : d;
        n++;
    }
    return sum / (float)n;
}

/* 살아있는 트랙의 시그니처를 지수이동평균으로 갱신 (조명 변화 완충) */
static float mix(float o, float n, float alpha)
{
    if (o < 0) return n;
    if (n < 0) return o;
    return o * (1 - alpha) + n * alpha;
}

static void sig_ema(Signature *old, const Signature *new_, float alpha)
{
    old->upper_hue = mix(old->upper_hue, new_->upper_hue, alpha);
    old->upper_sat = mix(old->upper_sat, new_->upper_sat, alpha);
    old->lower_hue = mix(old->lower_hue, new_->lower_hue, alpha);
    old->lower_sat = mix(old->lower_sat, new_->lower_sat, alpha);
    old->shoulder_torso = mix(old->shoulder_torso, new_->shoulder_torso, alpha);
    old->leg_torso = mix(old->leg_torso, new_->leg_torso, alpha);
}

static float dist2d(float ax, float ay, float bx, float by)
{
    float dx = ax - bx, dy = ay - by;
    return sqrtf(dx * dx + dy * dy);
}

/* 시그니처 추출: BBox 세로 구간(비율)의 HSV 평균 + 신체 비율 */
static void make_signature(const Reid *r, const FrameView *frame,
                           const TrackedPerson *p, Signature *sig)
{
    int x1 = (int)p->bbox[0], y1 = (int)p->bbox[1];
    int x2 = (int)p->bbox[2], y2 = (int)p->bbox[3];
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > frame->w) x2 = frame->w;
    if (y2 > frame->h) y2 = frame->h;
    int bh = y2 - y1;
    if (bh < 1) bh = 1;

    /* 상의(어깨~허리 부근) / 하의(허리~무릎 아래) */
    bgr_roi_hsv_mean(frame, x1, y1 + (int)(bh * 0.20f), x2, y1 + (int)(bh * 0.50f),
                     &sig->upper_hue, &sig->upper_sat);
    bgr_roi_hsv_mean(frame, x1, y1 + (int)(bh * 0.55f), x2, y1 + (int)(bh * 0.85f),
                     &sig->lower_hue, &sig->lower_sat);

    /* 신체 비율 (키포인트가 잡힌 경우에만) */
    sig->shoulder_torso = -1.f;
    sig->leg_torso = -1.f;
    const float mc = r->cfg->kpt_valid_conf;
    if (kpt_valid(p, KPT_L_SHOULDER, mc) && kpt_valid(p, KPT_R_SHOULDER, mc) &&
        kpt_valid(p, KPT_L_HIP, mc) && kpt_valid(p, KPT_R_HIP, mc)) {
        const Keypoint *k = p->kpts;
        float sh_mx = (k[KPT_L_SHOULDER].x + k[KPT_R_SHOULDER].x) / 2;
        float sh_my = (k[KPT_L_SHOULDER].y + k[KPT_R_SHOULDER].y) / 2;
        float hp_mx = (k[KPT_L_HIP].x + k[KPT_R_HIP].x) / 2;
        float hp_my = (k[KPT_L_HIP].y + k[KPT_R_HIP].y) / 2;
        float torso = dist2d(sh_mx, sh_my, hp_mx, hp_my);
        if (torso > 1e-3f) {
            sig->shoulder_torso = dist2d(k[KPT_L_SHOULDER].x, k[KPT_L_SHOULDER].y,
                                         k[KPT_R_SHOULDER].x, k[KPT_R_SHOULDER].y) / torso;
            if (kpt_valid(p, KPT_L_ANKLE, mc) && kpt_valid(p, KPT_R_ANKLE, mc)) {
                float an_mx = (k[KPT_L_ANKLE].x + k[KPT_R_ANKLE].x) / 2;
                float an_my = (k[KPT_L_ANKLE].y + k[KPT_R_ANKLE].y) / 2;
                sig->leg_torso = dist2d(hp_mx, hp_my, an_mx, an_my) / torso;
            }
        }
    }
}

/* 최근 소실된 시그니처 중 가장 가까운 것을 복원 (임계값 이내일 때만) */
static int match_lost(Reid *r, const Signature *sig)
{
    float best_d = r->cfg->reid_match_threshold;
    int best = -1;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (!r->lost[i].used) continue;
        float d = sig_distance(sig, &r->lost[i].sig);
        if (d < best_d) { best_d = d; best = i; }
    }
    if (best < 0) return -1;
    int sid = r->lost[best].stable_id;
    r->lost[best].used = false;
    return sid;
}

static RawMap *find_raw(Reid *r, int raw_id)
{
    for (int i = 0; i < MAX_ENTRIES; i++)
        if (r->raw[i].used && r->raw[i].raw_id == raw_id) return &r->raw[i];
    return NULL;
}

static LiveSig *find_live(Reid *r, int stable_id, bool create)
{
    int free_slot = -1;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (r->live[i].used && r->live[i].stable_id == stable_id) return &r->live[i];
        if (!r->live[i].used && free_slot < 0) free_slot = i;
    }
    if (!create || free_slot < 0) return NULL;
    r->live[free_slot].used = true;
    r->live[free_slot].has_sig = false;
    r->live[free_slot].stable_id = stable_id;
    return &r->live[free_slot];
}

static void reid_gc(Reid *r, double now)
{
    /* 사라진 raw 트랙 → 소실 갤러리로 이동 */
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (!r->raw[i].used) continue;
        if (now - r->raw[i].last_seen <= r->cfg->stale_track_ttl_sec) continue;
        int sid = r->raw[i].stable_id;
        r->raw[i].used = false;
        LiveSig *ls = find_live(r, sid, false);
        if (ls) {
            for (int j = 0; j < MAX_ENTRIES; j++) {
                if (r->lost[j].used) continue;
                r->lost[j].used = true;
                r->lost[j].stable_id = sid;
                r->lost[j].sig = ls->sig;
                r->lost[j].lost_ts = now;
                break;
            }
            ls->used = false;
        }
    }
    /* 보관 시간이 지난 소실 시그니처 폐기 */
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (r->lost[i].used && now - r->lost[i].lost_ts > r->cfg->reid_gallery_ttl_sec)
            r->lost[i].used = false;
    }
}

void reid_assign(Reid *r, const FrameView *frame,
                 TrackedPerson *people, int n_people, double now)
{
    for (int i = 0; i < n_people; i++) {
        TrackedPerson *p = &people[i];
        int raw_id = p->track_id;

        Signature sig;
        make_signature(r, frame, p, &sig);

        RawMap *rm = find_raw(r, raw_id);
        int sid;
        if (rm == NULL) {
            sid = match_lost(r, &sig);
            if (sid < 0) sid = r->next_stable++;
            else fprintf(stderr, "[reid] 재식별: raw %d → 안정 ID %d 복원\n", raw_id, sid);
            for (int j = 0; j < MAX_ENTRIES; j++) {
                if (r->raw[j].used) continue;
                r->raw[j].used = true;
                r->raw[j].raw_id = raw_id;
                r->raw[j].stable_id = sid;
                rm = &r->raw[j];
                break;
            }
        } else {
            sid = rm->stable_id;
        }
        if (rm) rm->last_seen = now;

        LiveSig *ls = find_live(r, sid, true);
        if (ls) {
            if (!ls->has_sig) { ls->sig = sig; ls->has_sig = true; }
            else sig_ema(&ls->sig, &sig, 0.3f);   /* 조명 변화 완충 EMA */
        }
        p->track_id = sid;   /* 이후 파이프라인은 안정 ID 사용 */
    }
    reid_gc(r, now);
}
