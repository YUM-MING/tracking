/*
 * Lock-Free 프레임 버스 (8/7 회의 ②·8/9 보고 계획의 구현)
 * - 생산자(캡처 스레드)와 소비자(추론 스레드)를 완전히 분리해
 *   "DB 저장과 읽기를 한 스레드에 묶으면 느려진다"는 병목을 제거한다.
 * - 뮤텍스 없이 C11 atomic_exchange 하나로 동작하는 고전 트리플 버퍼:
 *     슬롯 3개 = [생산자 기록 중] [최신 공개본] [소비자 사용 중]
 *   생산자는 기록을 마친 슬롯을 공개본과 원자적으로 교환하고,
 *   소비자는 자기 슬롯을 공개본과 교환해 항상 '가장 최신' 프레임을 얻는다.
 * - 실시간 우선: 소비자가 느리면 낡은 프레임은 자동 폐기(드롭 카운트 집계).
 * - 메모리는 최초 프레임 크기로 3슬롯을 1회 할당 — 이후 루프에서 malloc 0회.
 */
#ifndef FRAMEBUS_H
#define FRAMEBUS_H

#include <stdbool.h>
#include <stdint.h>

#include "types.h"

typedef struct FrameBus FrameBus;

FrameBus *framebus_create(void);
void framebus_destroy(FrameBus *b);

/* 생산자 전용: data를 내부 슬롯에 복사하고 최신본으로 공개.
 * 슬롯 버퍼는 첫 호출 시 실제 프레임 크기로 확보된다. */
bool framebus_publish(FrameBus *b, const uint8_t *data, int w, int h, int stride);

/* 소비자 전용: 새 프레임이 있으면 out을 채우고 true.
 * out->data는 다음 framebus_acquire 호출 전까지 유효 (소비자 소유 슬롯). */
bool framebus_acquire(FrameBus *b, FrameView *out, uint64_t *seq);

/* 생산자가 마지막으로 공개한 프레임 번호 (프리즈 감시용) */
uint64_t framebus_published_seq(const FrameBus *b);

/* 소비되지 못하고 교체된 프레임 수 (실시간 유지의 비용 — 상태 페이지 표시) */
uint64_t framebus_dropped(const FrameBus *b);

#endif /* FRAMEBUS_H */
