#!/bin/sh
# 윈도우 배포 패키지 생성 (8/31 회의: "윈도우 패키징해서 이메일로")
# 사전 조건:
#   brew install mingw-w64 makensis
#   third_party/win/ 에 onnxruntime-win-x64-*, ffmpeg-*-win64-*-shared 압축 해제
#   third_party/win/msvc_runtime_dlls/ 에 MSVC 런타임 x64 DLL
#     (취득: pip download msvc-runtime --platform win_amd64 --only-binary=:all:
#      → whl 압축 해제 후 *.dll 복사. vcruntime140*·msvcp140* 필수)
# 사용: tools/package_win.sh
#   →  dist/kiosk_tracking_win64_YYYYMMDD.zip (포터블)
#   →  dist/kiosk_tracking_setup_YYYYMMDD.exe (설치형, NSIS)

set -e
DIR="$(cd "$(dirname "$0")/.." && pwd)"
ORT="$DIR/third_party/win/onnxruntime-win-x64-1.27.1"
FFM="$DIR/third_party/win/ffmpeg-master-latest-win64-lgpl-shared"
OUT="$DIR/dist/kiosk_tracking"

# 1) 크로스 빌드
cmake -S "$DIR" -B "$DIR/build-win" \
      -DCMAKE_TOOLCHAIN_FILE="$DIR/cmake/toolchain-mingw64.cmake" \
      -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$DIR/build-win" -j >/dev/null

# 2) 패키지 폴더 구성
rm -rf "$OUT"
mkdir -p "$OUT"
cp "$DIR/build-win/kiosk_tracking.exe" "$OUT/"
cp "$ORT/lib/onnxruntime.dll" "$ORT/lib/onnxruntime_providers_shared.dll" "$OUT/"
for dll in avcodec avdevice avformat avutil swscale swresample avfilter; do
    cp "$FFM"/bin/${dll}-*.dll "$OUT/" 2>/dev/null || true
done
# MSVC 런타임 동봉 (풀릴리즈 — 9/13 회의): onnxruntime.dll이 vcruntime140/
# msvcp140 계열을 요구한다. VC++ 재배포판 미설치 PC에서도 실행되도록 app-local 배치.
cp "$DIR"/third_party/win/msvc_runtime_dlls/*.dll "$OUT/"
cp "$DIR"/models/*.onnx "$OUT/"
cp "$DIR/web/owner_page.html" "$OUT/"
cp "$DIR"/win_pkg/*.bat "$DIR/win_pkg/README_설치안내.md" "$OUT/"

# 3) zip (포터블판)
STAMP=$(date +%Y%m%d)
ZIP="$DIR/dist/kiosk_tracking_win64_${STAMP}.zip"
rm -f "$ZIP"
(cd "$DIR/dist" && zip -qr "$ZIP" kiosk_tracking)
echo "zip 완성: $ZIP"

# 4) 설치형 인스톨러 (NSIS — brew install makensis)
if command -v makensis >/dev/null 2>&1; then
    SETUP="$DIR/dist/kiosk_tracking_setup_${STAMP}.exe"
    makensis -V2 -DDISTDIR="$OUT" -DOUTFILE="$SETUP" \
             "$DIR/win_pkg/installer.nsi"
    echo "인스톨러 완성: $SETUP"
else
    echo "makensis 없음 — 인스톨러 생략 (brew install makensis)"
fi
du -sh "$OUT" "$DIR"/dist/kiosk_tracking_* 2>/dev/null