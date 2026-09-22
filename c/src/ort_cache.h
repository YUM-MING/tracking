/*
 * ONNX Runtime 최적화 그래프 캐시 (9/22 회의: 시작 시 CPU 50% 스파이크 감소)
 * - 세션 생성 때마다 수행되던 그래프 최적화(상수 폴딩·노드 융합)를 첫 실행에서
 *   "<모델>.opt.onnx"로 저장하고, 이후 실행은 저장본을 재최적화 없이 로드한다.
 * - 모델 파일이 교체되면(파인튜닝 배포) 캐시가 원본보다 오래되므로 자동 재생성.
 */
#ifndef ORT_CACHE_H
#define ORT_CACHE_H

#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>

/* 캐시 경로를 채우고, 캐시가 존재하며 원본보다 최신이면 true */
static inline bool ort_opt_cache_fresh(const char *model,
                                       char *cache, size_t len)
{
    snprintf(cache, len, "%s.opt.onnx", model);
    struct stat sm, sc;
    if (stat(model, &sm) != 0 || stat(cache, &sc) != 0) return false;
    return sc.st_mtime >= sm.st_mtime;
}

#endif /* ORT_CACHE_H */
