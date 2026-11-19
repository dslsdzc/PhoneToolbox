#!/usr/bin/env bash
# Build the Android 1.0-4.4 legacy agent APK.
#
# Unlike android/root-agent (AGP, minSdk 21), this APK is built with the
# platform tools directly so it can keep minSdkVersion 1 and package the
# static ARM phone_agent executable as an asset.
#
# Usage:
#   ANDROID_SDK_ROOT=/opt/android-sdk \
#   ANDROID_BUILD_TOOLS=/opt/android-sdk/build-tools/34.0.0 \
#   ./android/legacy-agent/build.sh
#
# Optional:
#   AGENT_BINARY=agent/build-legacy/phone_agent
#   LEGACY_ANDROID_JAR=/path/to/1.0/android.jar   (javac API check)
#   AGENT_KEYSTORE=android/root-agent/agent-release.jks
#   AGENT_KEYSTORE_PASS_FILE=android/root-agent/.jks-pass
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

SDK="${ANDROID_SDK_ROOT:-/opt/android-sdk}"
BUILD_TOOLS="${ANDROID_BUILD_TOOLS:-$SDK/build-tools/34.0.0}"
AAPT2="$BUILD_TOOLS/aapt2"
D8="$BUILD_TOOLS/d8"
ZIPALIGN="$BUILD_TOOLS/zipalign"
APKSIGNER="$BUILD_TOOLS/apksigner"

AGENT="${AGENT_BINARY:-$ROOT/agent/build-legacy/phone_agent}"
OUT="$HERE/build"
ANDROID_JAR="${ANDROID_JAR:-$SDK/platforms/android-34/android.jar}"
JAVAC_JAR="${LEGACY_ANDROID_JAR:-$ANDROID_JAR}"
KEYSTORE="${AGENT_KEYSTORE:-$ROOT/android/root-agent/agent-release.jks}"
PASS_FILE="${AGENT_KEYSTORE_PASS_FILE:-$ROOT/android/root-agent/.jks-pass}"

for tool in "$AAPT2" "$D8" "$ZIPALIGN" "$APKSIGNER"; do
    if [ ! -x "$tool" ]; then
        echo "missing build tool: $tool" >&2
        exit 1
    fi
done
if [ ! -f "$AGENT" ]; then
    echo "missing agent binary: $AGENT" >&2
    echo "build it first (see agent/README or CMakeLists AGENT_LEGACY_STATIC)" >&2
    exit 1
fi
if [ ! -f "$ANDROID_JAR" ]; then
    echo "missing android.jar: $ANDROID_JAR" >&2
    exit 1
fi
if [ ! -f "$KEYSTORE" ] || [ ! -f "$PASS_FILE" ]; then
    echo "missing keystore or password file; set AGENT_KEYSTORE/AGENT_KEYSTORE_PASS_FILE" >&2
    exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/dex"

echo "==> javac (API check against $JAVAC_JAR)"
find "$HERE/src" -name '*.java' > "$OUT/sources.txt"
javac -source 8 -target 8 -Xlint:-options \
    -bootclasspath "$JAVAC_JAR" \
    -d "$OUT/classes" @"$OUT/sources.txt"

echo "==> d8 (min-api 1)"
"$D8" --min-api 1 --lib "$ANDROID_JAR" --output "$OUT/dex" \
    $(find "$OUT/classes" -name '*.class')

echo "==> aapt2 link"
"$AAPT2" link \
    -o "$OUT/base.apk" \
    -I "$ANDROID_JAR" \
    --manifest "$HERE/AndroidManifest.xml" \
    --min-sdk-version 1 \
    --target-sdk-version 3 \
    --version-code 1 \
    --version-name "0.1.0-beta04-legacy"

# Android 1.x AssetManager refuses compressed assets above 1 MiB. Keep the
# executable stored; classes.dex can stay compressed.
mkdir -p "$OUT/package/assets"
cp "$AGENT" "$OUT/package/assets/phone_agent"
cp "$OUT/dex/classes.dex" "$OUT/package/classes.dex"

echo "==> package assets/dex"
( cd "$OUT/package" && zip -q -X -0 "$OUT/base.apk" assets/phone_agent )
( cd "$OUT/package" && zip -q -X "$OUT/base.apk" classes.dex )

echo "==> zipalign"
"$ZIPALIGN" -f 4 "$OUT/base.apk" "$OUT/aligned.apk"

echo "==> sign (v1 only, SHA1 for API 1)"
"$APKSIGNER" sign \
    --ks "$KEYSTORE" \
    --ks-key-alias agent \
    --ks-pass "file:$PASS_FILE" \
    --min-sdk-version 1 \
    --max-sdk-version 17 \
    --v1-signing-enabled true \
    --v2-signing-enabled false \
    --v3-signing-enabled false \
    --out "$OUT/PhoneToolbox-Agent-legacy.apk" \
    "$OUT/aligned.apk"

echo "==> verify"
"$APKSIGNER" verify --min-sdk-version 1 --max-sdk-version 17 -v \
    "$OUT/PhoneToolbox-Agent-legacy.apk" 2>&1 | sed -n '1,12p'
echo
ls -lh "$OUT/PhoneToolbox-Agent-legacy.apk"
sha256sum "$OUT/PhoneToolbox-Agent-legacy.apk"
