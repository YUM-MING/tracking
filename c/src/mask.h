/*
 * 인식 범위 마스크 (점주 캘리브레이션 — 8/3 회의 ① 영점 맞추기)
 *
 * 목적: 거울·포스터·TV·창밖 행인처럼 '사람으로 오인되는 고정 영역'을
 * 분석에서 제외한다. 두 가지 방법으로 만들어진다:
 *   1) 자동: 점주 페이지 [세팅 시작하기] → 매장을 비운 상태로 N분 가동 →
 *      그동안 '사람'으로 잡힌 셀은 전부 오탐원이므로 자동 마스킹
 *   2) 수동: 점주 페이지 캔버스에서 셀을 직접 칠하기/지우기
 *
 * 구조: 프레임을 32x18 셀 그리드로 나눈 비트마스크 (576비트 = 72바이트).
 * - 픽셀 단위가 아니라 셀 단위인 이유: 저장/전송이 144자 hex 문자열로 끝나고
 *   (JSON 설정 파일에 그대로 들어감), 판정이 비트 연산 1회라 핫패스 비용이 없다.
 * - 판정 기준점은 검출 BBox의 '발밑'(가로 중앙, 세로 하단) — 구역 판정과 동일.
 *
 * 순수 인라인 헤더 (정적 할당, 라이브러리 없음 — 8/7 회의 자료구조 원칙).
 */
#ifndef MASK_H
#define MASK_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define MASK_W 32              /* 가로 셀 수 */
#define MASK_H 18              /* 세로 셀 수 (16:9 프레임에서 정사각형에 가깝게) */
#define MASK_CELLS (MASK_W * MASK_H)
#define MASK_BYTES (MASK_CELLS / 8)          /* 72바이트 */
#define MASK_HEX_LEN (MASK_BYTES * 2)        /* hex 직렬화 144자 */

/* 정규화 좌표(0~1) → 셀 인덱스. 경계 밖은 가장자리 셀로 클램프. */
static inline int mask_cell_index(float nx, float ny)
{
    int cx = (int)(nx * MASK_W);
    int cy = (int)(ny * MASK_H);
    if (cx < 0) cx = 0;
    if (cx >= MASK_W) cx = MASK_W - 1;
    if (cy < 0) cy = 0;
    if (cy >= MASK_H) cy = MASK_H - 1;
    return cy * MASK_W + cx;
}

static inline bool mask_test(const uint8_t *mask, float nx, float ny)
{
    int i = mask_cell_index(nx, ny);
    return (mask[i >> 3] >> (i & 7)) & 1;
}

static inline void mask_set_cell(uint8_t *mask, int cell)
{
    mask[cell >> 3] |= (uint8_t)(1u << (cell & 7));
}

static inline int mask_count(const uint8_t *mask)
{
    int n = 0;
    for (int i = 0; i < MASK_CELLS; i++)
        n += (mask[i >> 3] >> (i & 7)) & 1;
    return n;
}

/* hex 문자열(144자) ↔ 바이트 배열. JSON 설정 파일 저장/점주 페이지 전송용. */
static inline void mask_to_hex(const uint8_t *mask, char *hex /* 145바이트 이상 */)
{
    static const char tbl[] = "0123456789abcdef";
    for (int i = 0; i < MASK_BYTES; i++) {
        hex[i * 2] = tbl[mask[i] >> 4];
        hex[i * 2 + 1] = tbl[mask[i] & 0xF];
    }
    hex[MASK_HEX_LEN] = 0;
}

static inline int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 길이/문자 오류 시 false (마스크는 변경되지 않음) */
static inline bool mask_from_hex(const char *hex, uint8_t *mask)
{
    uint8_t tmp[MASK_BYTES];
    for (int i = 0; i < MASK_BYTES; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hi < 0 ? -1 : hex_nibble(hex[i * 2 + 1]);
        if (lo < 0) return false;
        tmp[i] = (uint8_t)((hi << 4) | lo);
    }
    if (hex[MASK_HEX_LEN] != 0 && hex[MASK_HEX_LEN] != '"') return false;
    memcpy(mask, tmp, MASK_BYTES);
    return true;
}

#endif /* MASK_H */
