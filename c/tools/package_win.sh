#!/bin/sh
# 윈도우 배포 패키지 생성 (8/31 회의: "윈도우 패키징해서 이메일로")
# 사전 조건:
#   brew install mingw-w64
#   third_party/win/ 에 onnxruntime-win-x64-*, ffmpeg-*-win64-*-shared 압축 해제
# 사용: tools/package_win.sh   →  dist/kiosk_tracking_win64_YYYYMMDD.zip

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
cp "$DIR"/models/*.onnx "$OUT/"
cp "$DIR/web/owner_page.html" "$OUT/"
cp "$DIR"/win_pkg/*.bat "$DIR/win_pkg/README_설치안내.md" "$OUT/"

# 3) zip (이메일/드라이브 전달용)
STAMP=$(date +%Y%m%d)
ZIP="$DIR/dist/kiosk_tracking_win64_${STAMP}.zip"
rm -f "$ZIP"
(cd "$DIR/dist" && zip -qr "$ZIP" kiosk_tracking)
echo "패키지 완성: $ZIP"
du -sh "$OUT" "$ZIP"