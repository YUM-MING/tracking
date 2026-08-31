#include "framebus.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "logger.h"

#define SLOT_COUNT 3
#define FRESH_BIT 0x100        /* mailbox 하위 8비트 = 슬롯 인덱스, 상위 = 신규 플래그 */
#define IDX_MASK 0xFF

typedef struct {
    uint8_t *data;             /* 초기화 시 1회 할당되는 고정 버퍼 */
    int w, h, stride;
    uint64_t seq;              /* 이 슬롯에 담긴 프레임 번호 */
} FrameSlot;

struct FrameBus {
    FrameSlot slots[SLOT_COUNT];
    size_t capacity;           /* 슬롯당 버퍼 크기 (첫 프레임 기준) */
    _Atomic int mailbox;       /* 최신 공개 슬롯 인덱스 | FRESH_BIT */
    int back;                  /* 생산자 소유 슬롯 */
    int front;                 /* 소비자 소유 슬롯 */
    uint64_t next_seq;         /* 생산자 단독 갱신 */
    _Atomic uint64_t published_seq;
    _Atomic uint64_t dropped;
};

FrameBus *framebus_create(void)
{
    FrameBus *b = calloc(1, sizeof(FrameBus));
    b->back = 0;
    atomic_init(&b->mailbox, 1);               /* 슬롯1 공개칸(빈), FRESH 없음 */
    b->front = 2;
    b->next_seq = 1;
    atomic_init(&b->published_seq, 0);
    atomic_init(&b->dropped, 0);
    return b;
}

void framebus_destroy(FrameBus *b)
{
    if (!b) return;
    for (int i = 0; i < SLOT_COUNT; i++) free(b->slots[i].data);
    free(b);
}

/* 첫 프레임 크기로 3슬롯 일괄 확보 — 이후 크기가 다른 프레임은 거부 */
static bool ensure_capacity(FrameBus *b, int h, int stride)
{
    size_t need = (size_t)h * (size_t)stride;
    if (b->capacity == 0) {
        for (int i = 0; i < SLOT_COUNT; i++) {
            b->slots[i].data = malloc(need);
            if (!b->slots[i].data) return false;
        }
        b->capacity = need;
        LOGI("버스", "프레임 슬롯 3개 확보 (%zu KB x3)", need / 1024);
    }
    return need <= b->capacity;
}

bool framebus_publish(FrameBus *b, const uint8_t *data, int w, int h, int stride)
{
    if (!ensure_capacity(b, h, stride)) {
        LOGW("버스", "프레임 크기 변경 감지 (%dx%d) — 폐기", w, h);
        return false;
    }
    FrameSlot *s = &b->slots[b->back];
    memcpy(s->data, data, (size_t)h * (size_t)stride);
    s->w = w;
    s->h = h;
    s->stride = stride;
    s->seq = b->next_seq++;

    int old = atomic_exchange(&b->mailbox, b->back | FRESH_BIT);
    if (old & FRESH_BIT)                        /* 소비 안 된 프레임을 밀어냄 */
        atomic_fetch_add(&b->dropped, 1);
    b->back = old & IDX_MASK;
    atomic_store(&b->published_seq, s->seq);
    return true;
}

bool framebus_acquire(FrameBus *b, FrameView *out, uint64_t *seq)
{
    int old = atomic_exchange(&b->mailbox, b->front);
    b->front = old & IDX_MASK;                  /* 소유권 교환은 항상 성립 */
    if (!(old & FRESH_BIT)) return false;       /* 새 프레임 없음 */

    FrameSlot *s = &b->slots[b->front];
    out->data = s->data;
    out->w = s->w;
    out->h = s->h;
    out->stride = s->stride;
    if (seq) *seq = s->seq;
    return true;
}

uint64_t framebus_published_seq(const FrameBus *b)
{
    return atomic_load(&((FrameBus *)b)->published_seq);
}

uint64_t framebus_dropped(const FrameBus *b)
{
    return atomic_load(&((FrameBus *)b)->dropped);
}
