#include "tracker.h"

#include <stdlib.h>
#include <string.h>

#define MAX_TRACKS 128

typedef struct {
    bool used;
    int id;
    float bbox[4];
    float vel[4];              /* 등속 예측용 프레임당 변화량 */
    int hits;                  /* 연속 매칭 수 (확정 게이트) */
    int misses;                /* 연속 소실 수 */
    bool confirmed;
} Track;

struct Tracker {
    const PipelineConfig *cfg;
    Track tracks[MAX_TRACKS];
    int next_id;
};

Tracker *tracker_create(const PipelineConfig *cfg)
{
    Tracker *t = calloc(1, sizeof(Tracker));
    t->cfg = cfg;
    t->next_id = 1;
    return t;
}

void tracker_destroy(Tracker *t) { free(t); }

static float iou_box(const float *a, const float *b)
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

/*
 * 그리디 IoU 매칭: (트랙, 검출) 쌍 중 IoU가 가장 큰 것부터 확정.
 * 헝가리안 대비 근사지만 사람 수가 적은 매장 시나리오에선 차이가 미미하다.
 */
static void greedy_match(Track **tracks, int nt, const Detection *dets,
                         const int *det_idx, int nd, float min_iou,
                         int *track_to_det /* nt, -1 = 미매칭 */,
                         bool *det_taken /* 전체 검출 수 기준 */)
{
    for (int i = 0; i < nt; i++) track_to_det[i] = -1;
    for (;;) {
        float best = min_iou;
        int bi = -1, bj = -1;
        for (int i = 0; i < nt; i++) {
            if (track_to_det[i] >= 0) continue;
            for (int j = 0; j < nd; j++) {
                int dj = det_idx[j];
                if (det_taken[dj]) continue;
                float v = iou_box(tracks[i]->bbox, dets[dj].bbox);
                if (v > best) { best = v; bi = i; bj = dj; }
            }
        }
        if (bi < 0) return;
        track_to_det[bi] = bj;
        det_taken[bj] = true;
    }
}

static void track_absorb(Track *tr, const Detection *d)
{
    for (int k = 0; k < 4; k++) {
        /* 속도는 관측 차이의 EMA (노이즈 완충) */
        float delta = d->bbox[k] - tr->bbox[k];
        tr->vel[k] = tr->vel[k] * 0.5f + delta * 0.5f;
        tr->bbox[k] = d->bbox[k];
    }
    tr->hits++;
    tr->misses = 0;
}

int tracker_update(Tracker *t, const Detection *dets, int n_dets,
                   TrackedPerson *out, int max_out)
{
    const PipelineConfig *cfg = t->cfg;

    /* 0) 등속 예측 */
    Track *live[MAX_TRACKS];
    int n_live = 0;
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (!t->tracks[i].used) continue;
        Track *tr = &t->tracks[i];
        for (int k = 0; k < 4; k++) tr->bbox[k] += tr->vel[k];
        live[n_live++] = tr;
    }

    /* 1) 검출을 고신뢰/저신뢰로 분리 (ByteTrack 핵심) */
    int high_idx[MAX_DETECTIONS], low_idx[MAX_DETECTIONS];
    int n_high = 0, n_low = 0;
    for (int j = 0; j < n_dets; j++) {
        if (dets[j].conf >= cfg->yolo_conf) high_idx[n_high++] = j;
        else if (dets[j].conf >= cfg->track_low_conf) low_idx[n_low++] = j;
    }

    bool det_taken[MAX_DETECTIONS] = { false };
    int match[MAX_TRACKS];

    /* 2) 1차: 전체 트랙 × 고신뢰 검출 */
    greedy_match(live, n_live, dets, high_idx, n_high,
                 cfg->track_match_iou, match, det_taken);

    /* 3) 2차: 미매칭 트랙 × 저신뢰 검출 (가림 상황 유지) */
    Track *left[MAX_TRACKS];
    int left_map[MAX_TRACKS], n_left = 0;
    for (int i = 0; i < n_live; i++) {
        if (match[i] < 0) { left_map[n_left] = i; left[n_left++] = live[i]; }
    }
    int match2[MAX_TRACKS];
    greedy_match(left, n_left, dets, low_idx, n_low,
                 cfg->track_match_iou, match2, det_taken);
    for (int i = 0; i < n_left; i++) {
        if (match2[i] >= 0) match[left_map[i]] = match2[i];
    }

    /* 4) 트랙 상태 갱신 + 결과 생성 */
    int n_out = 0;
    for (int i = 0; i < n_live; i++) {
        Track *tr = live[i];
        int dj = match[i];
        if (dj < 0) {
            tr->misses++;
            tr->hits = 0;
            /* 소실 중에는 예측 이동 중단 (드리프트 방지) */
            for (int k = 0; k < 4; k++) { tr->bbox[k] -= tr->vel[k]; tr->vel[k] *= 0.5f; }
            if (tr->misses > cfg->track_max_misses) tr->used = false;
            continue;
        }
        track_absorb(tr, &dets[dj]);
        if (!tr->confirmed && tr->hits >= cfg->track_min_hits) tr->confirmed = true;

        /* 저신뢰 매칭만으로 유지 중인 트랙은 상태만 갱신하고 출력하지 않는다 */
        if (tr->confirmed && dets[dj].conf >= cfg->yolo_conf && n_out < max_out) {
            TrackedPerson *p = &out[n_out++];
            p->track_id = tr->id;
            memcpy(p->bbox, dets[dj].bbox, sizeof(p->bbox));
            p->conf = dets[dj].conf;
            p->has_kpts = true;
            memcpy(p->kpts, dets[dj].kpts, sizeof(p->kpts));
            /* 발밑 기준점(구역 판정용): 가로 중앙, 세로 하단 */
            p->center_x = (p->bbox[0] + p->bbox[2]) / 2;
            p->center_y = p->bbox[3];
        }
    }

    /* 5) 미매칭 고신뢰 검출 → 신규 트랙 */
    for (int j = 0; j < n_high; j++) {
        int dj = high_idx[j];
        if (det_taken[dj]) continue;
        for (int i = 0; i < MAX_TRACKS; i++) {
            if (t->tracks[i].used) continue;
            Track *tr = &t->tracks[i];
            memset(tr, 0, sizeof(*tr));
            tr->used = true;
            tr->id = t->next_id++;
            memcpy(tr->bbox, dets[dj].bbox, sizeof(tr->bbox));
            tr->hits = 1;
            /* min_hits=1 설정이면 첫 프레임부터 확정 */
            tr->confirmed = cfg->track_min_hits <= 1;
            if (tr->confirmed && n_out < max_out) {
                TrackedPerson *p = &out[n_out++];
                p->track_id = tr->id;
                memcpy(p->bbox, dets[dj].bbox, sizeof(p->bbox));
                p->conf = dets[dj].conf;
                p->has_kpts = true;
                memcpy(p->kpts, dets[dj].kpts, sizeof(p->kpts));
                p->center_x = (p->bbox[0] + p->bbox[2]) / 2;
                p->center_y = p->bbox[3];
            }
            break;
        }
    }
    return n_out;
}
