#!/bin/bash
# Build mtk_bridge standalone binary with PyInstaller
# Usage: ./tools/build_bridge.sh [linux|windows|macos]

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VENV_DIR="/tmp/mtk_venv"

PLATFORM="${1:-linux}"
case "$PLATFORM" in
    linux)   BINARY_NAME="mtk_bridge"; OUT_DIR="linux/x64" ;;
    windows) BINARY_NAME="mtk_bridge.exe"; OUT_DIR="windows/x64" ;;
    macos)   BINARY_NAME="mtk_bridge"; OUT_DIR="macos/x64" ;;
    *) echo "Usage: $0 [linux|windows|macos]"; exit 1 ;;
esac

echo "=== Building mtk_bridge for $PLATFORM ==="

# Ensure venv exists with dependencies
if [ ! -f "$VENV_DIR/bin/python3" ]; then
    python3 -m venv "$VENV_DIR"
fi
"$VENV_DIR/bin/pip" install -q pyinstaller pyusb pycryptodome pycryptodomex pyserial colorama 2>&1 | tail -1
"$VENV_DIR/bin/pip" install -q -e "$REPO_DIR/mtkclient" 2>&1 | tail -1

BUILD_DIR="$REPO_DIR/build/mtk_bridge"
mkdir -p "$BUILD_DIR"

"$VENV_DIR/bin/pyinstaller" --onefile \
    --distpath "$BUILD_DIR/dist" \
    --workpath "$BUILD_DIR/work" \
    --specpath "$BUILD_DIR" \
    --name "$BINARY_NAME" \
    --exclude-module PySide6 \
    --exclude-module PySide6_Essentials \
    --exclude-module PySide6_Addons \
    --exclude-module shiboken6 \
    --exclude-module fusepy \
    --exclude-module matplotlib \
    --exclude-module PIL \
    --exclude-module cv2 \
    --exclude-module tkinter \
    --exclude-module unittest \
    --exclude-module keystone \
    --exclude-module capstone \
    --exclude-module unicorn \
    "$REPO_DIR/tools/mtk_bridge.py"

# Copy to third_party
OUT_PATH="$REPO_DIR/third_party/mtk_bridge/$OUT_DIR"
mkdir -p "$OUT_PATH"
cp "$BUILD_DIR/dist/$BINARY_NAME" "$OUT_PATH/$BINARY_NAME"
chmod +x "$OUT_PATH/$BINARY_NAME"

echo "=== Done: $(ls -lh "$OUT_PATH/$BINARY_NAME" | awk '{print $5}') ==="
