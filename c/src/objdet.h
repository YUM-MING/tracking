/*
 * 확장 객체 검출기 — 반려동물·외부 음식 (8/18 회의 ③④ 출입 제한 필터)
 *
 * 사람 전용 pose 모델(yolo11n-pose)로는 불가능한 클래스를 담당하는
 * 두 번째 검출기. COCO 80클래스 사전학습 YOLO11n을 사용한다.
 *
 * 사양(4200U급) 최적화 전략:
 *   - 입력 320px (사람 검출 416보다 작게 — 개/음식은 원거리 정밀도 불필요)
 *   - 매 추론마다 돌리지 않고 사람 추론 K회당 1회만 (기본 5회 = 약 1Hz)
 *   - 반려동물/외부음식 룰이 모두 꺼져 있으면 아예 로드조차 안 함
 *   - intra-op 1스레드 (주 모델과 코어 경쟁 방지)
 *   - 모델 파일이 없으면 조용히 비활성 (파이프라인은 정상 동작)
 *
 * 관심 클래스만 남기고 나머지는 디코드 단계에서 버린다:
 *   반려동물: cat(15), dog(16)
 *   외부음식: banana(46) apple(47) sandwich(48) orange(49) broccoli(50)
 *             carrot(51) hot dog(52) pizza(53) donut(54) cake(55)
 *   ※ cup/bottle은 매장 판매품과 구분 불가라 의도적으로 제외 (오탐 방지)
 */
#ifndef OBJDET_H
#define OBJDET_H

#include "config.h"
#include "types.h"

#define MAX_OBJ_DETECTIONS 16

typedef enum {
    OBJ_PET,                   /* 반려동물 (cat/dog) */
    OBJ_FOOD,                  /* 외부 음식 의심 품목 */
    OBJ_TABLEWARE,             /* 컵/병/그릇 — 퇴석 후 잔여물(청소 알림) 전용.
                                * 매장 판매품과 구분이 안 되므로 외부음식
                                * 판정에는 절대 쓰지 않는다. */
} ObjCategory;

typedef struct {
    float bbox[4];             /* x1,y1,x2,y2 (프레임 좌표) */
    float conf;
    int coco_class;
    ObjCategory category;
} ObjDetection;

typedef struct ObjDet ObjDet;

/* 모델 로드. 파일이 없거나 실패하면 NULL (호출부는 룰 자동 비활성). */
ObjDet *objdet_create(const PipelineConfig *cfg);
void objdet_destroy(ObjDet *d);

/* 프레임 전체 추론 → 관심 클래스만 반환. conf_thres 미만 제외. */
int objdet_infer(ObjDet *d, const FrameView *frame, float conf_thres,
                 ObjDetection *out, int max_out);

/* COCO 클래스 → 한국어 표기 (이벤트 메시지용) */
const char *objdet_class_str(int coco_class);

#endif /* OBJDET_H */
