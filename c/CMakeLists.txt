# 키오스크 트래킹 파이프라인 — C 포팅 빌드 (macOS)
#
# 의존성 (Homebrew, 저장소 미포함):
#   brew install onnxruntime opencv
#
# 빌드:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
cmake_minimum_required(VERSION 3.20)
project(kiosk_tracking C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Homebrew 패키지 검색 경로 (Apple Silicon 기본 prefix. Intel Mac이면
# /usr/local 도 CMAKE_PREFIX_PATH에 자동 포함되도록 brew --prefix 사용)
execute_process(COMMAND brew --prefix OUTPUT_VARIABLE HOMEBREW_PREFIX
                 OUTPUT_STRIP_TRAILING_WHITESPACE)
list(APPEND CMAKE_PREFIX_PATH ${HOMEBREW_PREFIX})

find_package(onnxruntime REQUIRED)
find_package(OpenCV REQUIRED)
find_package(Threads REQUIRED)

set(CORE_SOURCES
    src/imgproc.c
    src/yolo_pose.c
    src/tracker.c
    src/reid.c
    src/roi_router.c
    src/analyzer.c
    src/fsm.c
    src/ws_sender.c
    src/profiler.c
    src/resource.c
    src/logger.c        # 통합 로깅 (8/10 회의 ④)
    src/journey.c       # 설명 가능한 동선 기록 (8/18 회의 ①)
    src/datastore.c     # 파일럿 데이터 JSONL 축적 (이벤트/여정 — 사후 분석용)
    src/camhealth.c     # 카메라 자가 진단 (8/18 회의 ②)
    src/framebus.c      # lock-free 프레임 버스 (8/9 최적화 계획)
    src/store_settings.c # 점주 설정 영속화 + 오탐 피드백 캘리브레이션
    src/admin_server.c  # 점주 페이지 내장 HTTP 서버
    src/objdet.c        # 확장 객체 검출 — 반려동물/외부음식 (COCO YOLO11n)
    src/age_est.c       # 나이 추정 — 노키즈존 (InsightFace genderage)
    src/behavior.c      # 제로샷 이상행동 — 폭력/파손/낙상/배회/무단조작
)

add_executable(kiosk_tracking
    src/main.c
    ${CORE_SOURCES}
    src/cv_shim.cpp        # OpenCV 격리 계층 — 유일한 C++ 파일
)
target_include_directories(kiosk_tracking PRIVATE
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(kiosk_tracking PRIVATE
    onnxruntime::onnxruntime
    ${OpenCV_LIBS}
    Threads::Threads
)

# 검출 파리티 테스트 도구 (파이썬 ultralytics 결과와 비교 검증용)
add_executable(test_detect
    src/test_detect.c
    src/imgproc.c
    src/yolo_pose.c
    src/roi_router.c       # reason_str
    src/logger.c           # roi_router의 판단 로그 의존
    src/cv_shim.cpp
)
target_include_directories(test_detect PRIVATE
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(test_detect PRIVATE
    onnxruntime::onnxruntime
    ${OpenCV_LIBS}
)

# 순수 로직 단위 테스트 (모델/카메라/OpenCV 불필요)
add_executable(test_logic
    src/test_logic.c
    src/imgproc.c
    src/roi_router.c
    src/reid.c
    src/fsm.c
    src/logger.c
    src/journey.c
    src/datastore.c
    src/camhealth.c
    src/framebus.c
    src/store_settings.c
    src/behavior.c
)
target_link_libraries(test_logic PRIVATE Threads::Threads)

# 웹소켓 전송 테스트 도구 (로컬 서버 대상 실통신 검증)
add_executable(test_ws
    src/test_ws.c
    src/ws_sender.c
    src/fsm.c
    src/logger.c
    src/journey.c
    src/datastore.c
    src/resource.c
)
target_link_libraries(test_ws PRIVATE Threads::Threads)

# 실행에 필요한 모델을 출력 폴더로 복사 (동적 라이브러리는 @rpath로 참조되므로 복사 불필요)
# yolo11n.onnx(반려동물/외부음식)와 genderage.onnx(노키즈존)는 없어도 빌드/실행 가능
# — 해당 룰만 자동 비활성된다.
foreach(MODEL yolo11n-pose.onnx yolo11n.onnx genderage.onnx)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/models/${MODEL})
        add_custom_command(TARGET kiosk_tracking POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/models/${MODEL}
                $<TARGET_FILE_DIR:kiosk_tracking>
        )
    endif()
endforeach()

# 점주 페이지 HTML — 실행 파일 옆에서 매 요청 시 읽는다 (재빌드 없이 수정 가능)
add_custom_command(TARGET kiosk_tracking POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/web/owner_page.html
        $<TARGET_FILE_DIR:kiosk_tracking>
)
