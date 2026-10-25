#!/usr/bin/env bash
# Downloads and builds everything build.sh needs into deps/.
# Any of ANDROID_NDK_HOME, BUILD_TOOLS, DYNARMIC_ROOT, BOOST_ROOT, APKTOOL_JAR can be
# set to reuse existing copies instead.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/deps"
mkdir -p "$DEPS"

NDK_VER=r26d
BUILD_TOOLS_ZIP=build-tools_r34-linux.zip
BOOST_VER=1.83.0
DYNARMIC_COMMIT=a41c380246d3d9f9874f0f792d234dc0cc17c180
APKTOOL_VER=2.11.1

need() { command -v "$1" >/dev/null || { echo "missing required tool: $1" >&2; exit 1; }; }
need curl; need unzip; need git; need cmake; need java; need keytool; need python3

fetch() {  # url dest
    [[ -f "$2" ]] || { echo "downloading $(basename "$2")"; curl -fL --retry 3 -o "$2.part" "$1" && mv "$2.part" "$2"; }
}

NDK="${ANDROID_NDK_HOME:-$DEPS/android-ndk-$NDK_VER}"
if [[ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]]; then
    fetch "https://dl.google.com/android/repository/android-ndk-$NDK_VER-linux.zip" "$DEPS/ndk.zip"
    unzip -q "$DEPS/ndk.zip" -d "$DEPS" && rm "$DEPS/ndk.zip"
fi

BT="${BUILD_TOOLS:-$DEPS/build-tools}"
if [[ ! -x "$BT/apksigner" ]]; then
    fetch "https://dl.google.com/android/repository/$BUILD_TOOLS_ZIP" "$DEPS/bt.zip"
    unzip -q "$DEPS/bt.zip" -d "$DEPS" && rm "$DEPS/bt.zip"
    mv "$DEPS/android-14" "$DEPS/build-tools"
fi

BOOST="${BOOST_ROOT:-$DEPS/boost}"
if [[ ! -f "$BOOST/boost/version.hpp" ]]; then
    fetch "https://archives.boost.io/release/$BOOST_VER/source/boost_${BOOST_VER//./_}.tar.gz" "$DEPS/boost.tar.gz"
    mkdir -p "$DEPS/boost"
    tar -xzf "$DEPS/boost.tar.gz" -C "$DEPS/boost" --strip-components=1 "boost_${BOOST_VER//./_}/boost"
    rm "$DEPS/boost.tar.gz"
fi

DYN="${DYNARMIC_ROOT:-$DEPS/dynarmic}"
if [[ ! -d "$DYN/src" ]]; then
    git clone https://github.com/lioncash/dynarmic.git "$DYN"
    git -C "$DYN" checkout -q "$DYNARMIC_COMMIT"
fi
if [[ ! -f "$DYN/build-arm64/src/dynarmic/libdynarmic.a" ]]; then
    cmake -S "$DYN" -B "$DYN/build-arm64" \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DCMAKE_BUILD_TYPE=Release \
        -DDYNARMIC_FRONTENDS=A32 -DDYNARMIC_TESTS=OFF -DDYNARMIC_USE_LLVM=OFF \
        -DBoost_INCLUDE_DIR="$BOOST" -DBOOST_ROOT="$BOOST"
    cmake --build "$DYN/build-arm64" --target dynarmic -j"$(nproc)"
fi

APKTOOL="${APKTOOL_JAR:-$DEPS/apktool.jar}"
fetch "https://github.com/iBotPeaches/Apktool/releases/download/v$APKTOOL_VER/apktool_$APKTOOL_VER.jar" "$APKTOOL"

KS="$DEPS/debug.keystore"
if [[ ! -f "$KS" ]]; then
    keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias androiddebugkey \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" >/dev/null
fi

cat > "$DEPS/env.sh" <<EOF
NDK="$NDK"
BT="$BT"
BOOST="$BOOST"
DYN="$DYN"
APKTOOL="$APKTOOL"
KS="$KS"
EOF
echo "setup complete; run ./build.sh <path to Stardunk 2.3.9 APK>"
