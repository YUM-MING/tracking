# 키오스크 트래킹 파이프라인 — C 포팅 빌드
#
# 의존성 (third_party/ 아래, 저장소 미포함 — README 참고):
#   - onnxruntime-win-x64-1.20.1 : YOLO ONNX 추론 (C API)
#   - opencv 4.10 prebuilt       : 카메라 캡처 + 디버그 창 (cv_shim.cpp만 사용)
#
# 빌드:
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release
cmake_minimum_required(VERSION 3.20)
project(kiosk_tracking C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(ORT_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime-win-x64-1.20.1)
set(OPENCV_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/opencv/build)

add_executable(kiosk_tracking
    src/main.c
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
    src/cv_shim.cpp        # OpenCV 격리 계층 — 유일한 C++ 파일
)

target_include_directories(kiosk_tracking PRIVATE
    ${ORT_DIR}/include
    ${OPENCV_DIR}/include
)

target_link_libraries(kiosk_tracking PRIVATE
    ${ORT_DIR}/lib/onnxruntime.lib
    ${OPENCV_DIR}/x64/vc16/lib/opencv_world4100.lib
    ws2_32                 # 웹소켓(Winsock)
    psapi                  # 리소스 모니터(GetProcessMemoryInfo)
)

if(MSVC)
    # 소스가 UTF-8(한글 주석/로그)이므로 필수
    target_compile_options(kiosk_tracking PRIVATE /utf-8 /W3)
endif()

# 검출 파리티 테스트 도구 (파이썬 ultralytics 결과와 비교 검증용)
add_executable(test_detect
    src/test_detect.c
    src/imgproc.c
    src/yolo_pose.c
    src/roi_router.c       # reason_str
    src/cv_shim.cpp
)
target_include_directories(test_detect PRIVATE
    ${ORT_DIR}/include
    ${OPENCV_DIR}/include
)
target_link_libraries(test_detect PRIVATE
    ${ORT_DIR}/lib/onnxruntime.lib
    ${OPENCV_DIR}/x64/vc16/lib/opencv_world4100.lib
)
if(MSVC)
    target_compile_options(test_detect PRIVATE /utf-8 /W3)
endif()

# 순수 로직 단위 테스트 (모델/카메라/OpenCV 불필요)
add_executable(test_logic
    src/test_logic.c
    src/imgproc.c
    src/roi_router.c
    src/reid.c
    src/fsm.c
)
if(MSVC)
    target_compile_options(test_logic PRIVATE /utf-8 /W3)
endif()

# 웹소켓 전송 테스트 도구 (로컬 서버 대상 실통신 검증)
add_executable(test_ws
    src/test_ws.c
    src/ws_sender.c
    src/fsm.c
)
target_link_libraries(test_ws PRIVATE ws2_32)
if(MSVC)
    target_compile_options(test_ws PRIVATE /utf-8 /W3)
endif()

# 실행에 필요한 DLL/모델을 출력 폴더로 복사
add_custom_command(TARGET kiosk_tracking POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${ORT_DIR}/lib/onnxruntime.dll
        ${OPENCV_DIR}/x64/vc16/bin/opencv_world4100.dll
        ${OPENCV_DIR}/x64/vc16/bin/opencv_videoio_ffmpeg4100_64.dll  # mp4/RTSP 디코딩
        ${CMAKE_CURRENT_SOURCE_DIR}/models/yolo11n-pose.onnx
        $<TARGET_FILE_DIR:kiosk_tracking>
)
