#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$ROOT_DIR/src"
DIST_DIR="$ROOT_DIR/dist"

mkdir -p "$DIST_DIR"

x86_64-w64-mingw32-windres \
  "$SRC_DIR/WindowsNetworkConfigTool.rc" \
  -I "$SRC_DIR" \
  -O coff \
  -o "$DIST_DIR/WindowsNetworkConfigTool-win-x64.res"

x86_64-w64-mingw32-g++ \
  -std=c++17 \
  -municode \
  -mwindows \
  -Os \
  -s \
  -static \
  -static-libgcc \
  -static-libstdc++ \
  "$SRC_DIR/WindowsNetworkConfigTool.cpp" \
  "$DIST_DIR/WindowsNetworkConfigTool-win-x64.res" \
  -o "$DIST_DIR/WindowsNetworkConfigTool-win-x64.exe" \
  -lshell32 \
  -lole32 \
  -ladvapi32 \
  -liphlpapi \
  -lws2_32

ditto -c -k --keepParent \
  "$DIST_DIR/WindowsNetworkConfigTool-win-x64.exe" \
  "$DIST_DIR/WindowsNetworkConfigTool-win-x64.zip"

if command -v zig >/dev/null 2>&1 && [ -x /opt/homebrew/opt/llvm@21/bin/llvm-windres ]; then
  /opt/homebrew/opt/llvm@21/bin/llvm-windres \
    --target=aarch64-w64-mingw32 \
    --input "$SRC_DIR/WindowsNetworkConfigTool.rc" \
    --include-dir "$SRC_DIR" \
    --output-format coff \
    --output "$DIST_DIR/WindowsNetworkConfigTool-win-arm64.res"

  zig c++ \
    -target aarch64-windows-gnu \
    -std=c++17 \
    -municode \
    -Os \
    -s \
    "$SRC_DIR/WindowsNetworkConfigTool.cpp" \
    "$DIST_DIR/WindowsNetworkConfigTool-win-arm64.res" \
    -Wl,/subsystem:windows \
    -o "$DIST_DIR/WindowsNetworkConfigTool-win-arm64.exe" \
    -lshell32 \
    -lole32 \
    -ladvapi32 \
    -liphlpapi \
    -lws2_32 \
    -lgdi32 \
    -luser32

  ditto -c -k --keepParent \
    "$DIST_DIR/WindowsNetworkConfigTool-win-arm64.exe" \
    "$DIST_DIR/WindowsNetworkConfigTool-win-arm64.zip"
else
  echo "Skipped ARM64 build: install zig and llvm@21 first."
fi

echo "Built x86_64: $DIST_DIR/WindowsNetworkConfigTool-win-x64.exe"
echo "Packaged x86_64: $DIST_DIR/WindowsNetworkConfigTool-win-x64.zip"
if [ -f "$DIST_DIR/WindowsNetworkConfigTool-win-arm64.zip" ]; then
  echo "Built ARM64: $DIST_DIR/WindowsNetworkConfigTool-win-arm64.exe"
  echo "Packaged ARM64: $DIST_DIR/WindowsNetworkConfigTool-win-arm64.zip"
fi
