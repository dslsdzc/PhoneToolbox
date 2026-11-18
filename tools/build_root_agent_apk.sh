#!/usr/bin/env bash
# 构建 Android Root Agent APK（arm64-v8a）。
#
# 前置：
#   - Android SDK/NDK（默认 /opt/android-sdk，NDK 30.0.16248370）
#   - libsodium 的 Android 构建（默认 agent/build-android/deps/arm64-v8a）
#   - Gradle + AGP 9.5.1（首次会下载依赖）
#
# 用法：
#   tools/build_root_agent_apk.sh [--app-id <package>] [--abi arm64-v8a]
#
# 产物：
#   android/root-agent/app/build/outputs/apk/debug/app-debug.apk
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ABI="arm64-v8a"
APP_ID="com.phonetoolbox.agent"
APP_LABEL="${APP_LABEL:-PhoneToolbox Agent}"

while [ $# -gt 0 ]; do
    case "$1" in
        --app-id) APP_ID="$2"; shift 2 ;;
        --abi) ABI="$2"; shift 2 ;;
        --label) APP_LABEL="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 1 ;;
    esac
done

# 开源项目默认固定包名；需要随机/自定义时传 --app-id。
# 包名每段必须符合 Java 包名规则（字母开头）。
NDK="${ANDROID_NDK_HOME:-/opt/android-sdk/ndk/30.0.16248370}"
SODIUM_ROOT="${SODIUM_ROOT:-$ROOT/agent/build-android/deps/$ABI}"
PLATFORM="${ANDROID_PLATFORM:-android-21}"
BUILD_DIR="$ROOT/agent/build-android-$ABI"
JNI_DIR="$ROOT/android/root-agent/app/src/main/jniLibs/$ABI"

if [ ! -x "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang" ]; then
    echo "找不到 NDK 工具链：$NDK" >&2
    exit 1
fi
if [ ! -f "$SODIUM_ROOT/lib/libsodium.a" ]; then
    echo "缺少 libsodium Android 静态库：$SODIUM_ROOT/lib/libsodium.a" >&2
    echo "先按 dist-build/android-armv8-a.sh 构建，或设置 SODIUM_ROOT。" >&2
    exit 1
fi

echo "[1/3] 编译 agent ($ABI)"
cmake -S "$ROOT/agent" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="$PLATFORM" \
    -DSODIUM_ROOT="$SODIUM_ROOT" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --target phone_agent -j"$(nproc)"

mkdir -p "$JNI_DIR"
cp "$BUILD_DIR/phone_agent" "$JNI_DIR/libservice.so"
chmod 0755 "$JNI_DIR/libservice.so"

echo "[2/3] 构建 release APK（applicationId=$APP_ID, label=$APP_LABEL）"
KEYSTORE="$ROOT/android/root-agent/agent-release.jks"
PASSFILE="$ROOT/android/root-agent/.jks-pass"
if [ ! -f "$KEYSTORE" ]; then
    if [ ! -f "$PASSFILE" ]; then
        openssl rand -hex 16 > "$PASSFILE"
        chmod 600 "$PASSFILE"
    fi
    KEYPASS="$(cat "$PASSFILE")"
    keytool -genkeypair -keystore "$KEYSTORE" -alias agent \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -storepass "$KEYPASS" -keypass "$KEYPASS" \
        -dname "CN=Service,O=Local,C=US" >/dev/null 2>&1
    chmod 600 "$KEYSTORE"
fi
if [ ! -f "$PASSFILE" ]; then
    echo "缺少签名密码文件：$PASSFILE" >&2
    exit 1
fi
KEYPASS="$(cat "$PASSFILE")"

cd "$ROOT/android/root-agent"
gradle --no-daemon assembleRelease \
    -PappId="$APP_ID" -PappLabel="$APP_LABEL" \
    -PkeystorePath="$KEYSTORE" -PkeystorePass="$KEYPASS"

APK="$ROOT/android/root-agent/app/build/outputs/apk/release/app-release.apk"
if [ ! -f "$APK" ]; then
    echo "APK 未生成：$APK" >&2
    exit 1
fi

echo "[3/3] 完成"
echo "APK : $APK"
echo "包名: $APP_ID"
echo "安装: adb install -r \"$APK\""
