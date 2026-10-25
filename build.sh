#!/usr/bin/env bash
# Usage: ./build.sh <Stardunk 2.3.9 APK> [output.apk]
# Builds an arm64-v8a APK from your own copy of Stardunk 2.3.9 (com.gzl.stardunk).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
[[ $# -ge 1 && -f "$1" ]] || { echo "usage: $0 <Stardunk 2.3.9 APK> [output.apk]" >&2; exit 1; }
[[ -f "$ROOT/deps/env.sh" ]] || { echo "run scripts/setup.sh first" >&2; exit 1; }
source "$ROOT/deps/env.sh"

APK="$(realpath "$1")"
OUT="$(realpath -m "${2:-$ROOT/out/Stardunk-arm64.apk}")"
WORK="$ROOT/build/work"
GUEST_SHA=afcccb174ae88a119dc6ab56d39f421bbeeddfc32ae2959c7fb55bc3c11e7566
rm -rf "$WORK" && mkdir -p "$WORK" "$(dirname "$OUT")"

echo "== extracting guest library"
unzip -q -o "$APK" lib/armeabi-v7a/libGame.so -d "$WORK"
GUEST="$WORK/lib/armeabi-v7a/libGame.so"
if [[ "$(sha256sum "$GUEST" | cut -d' ' -f1)" != "$GUEST_SHA" ]]; then
    echo "libGame.so does not match Stardunk 2.3.9 (armeabi-v7a); refusing to build" >&2
    exit 1
fi

echo "== building bridge"
cmake -S "$ROOT" -B "$ROOT/build/cmake" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DCMAKE_BUILD_TYPE=Release \
    -DGUEST_SO="$GUEST" -DDYNARMIC_ROOT="$DYN" -DBOOST_ROOT="$BOOST" \
    ${BRIDGE_TRACE:+-DBRIDGE_TRACE=ON} >/dev/null
cmake --build "$ROOT/build/cmake" -j"$(nproc)"
SO="$WORK/libGame.so"
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded -o "$SO" "$ROOT/build/cmake/libGame.so"

echo "== patching Java"
java -jar "$APKTOOL" d -q -r -f "$APK" -o "$WORK/decoded"
python3 -I "$ROOT/scripts/patch_java.py" "$WORK/decoded"
java -jar "$APKTOOL" b -q "$WORK/decoded" -o "$WORK/decoded.apk"
unzip -q -o "$WORK/decoded.apk" classes.dex -d "$WORK"

echo "== packaging"
python3 -I - "$APK" "$WORK/unsigned.apk" "$SO" "$WORK/classes.dex" <<'PY'
import re, sys, zipfile
src, dst, so, dex = sys.argv[1:5]
drop = re.compile(r'^(lib/|META-INF/[^/]+\.(SF|RSA|DSA|EC|MF)$)')
with zipfile.ZipFile(src) as zi, zipfile.ZipFile(dst, 'w') as zo:
    for info in zi.infolist():
        if drop.match(info.filename):
            continue
        data = open(dex, 'rb').read() if info.filename == 'classes.dex' else zi.read(info.filename)
        zo.writestr(info, data, compress_type=info.compress_type)
    zo.write(so, 'lib/arm64-v8a/libGame.so', compress_type=zipfile.ZIP_STORED)
PY
"$BT/zipalign" -f -p 4 "$WORK/unsigned.apk" "$WORK/aligned.apk"
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --out "$OUT" "$WORK/aligned.apk"
rm -f "$OUT.idsig"
echo "built $OUT"
