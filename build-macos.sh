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
  -o "$DIST_DIR/WindowsNetworkConfigTool.res"

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
  "$DIST_DIR/WindowsNetworkConfigTool.res" \
  -o "$DIST_DIR/WindowsNetworkConfigTool.exe" \
  -lshell32 \
  -lole32 \
  -ladvapi32 \
  -liphlpapi \
  -lws2_32

ditto -c -k --keepParent \
  "$DIST_DIR/WindowsNetworkConfigTool.exe" \
  "$DIST_DIR/WindowsNetworkConfigTool-win-x64.zip"

echo "Built: $DIST_DIR/WindowsNetworkConfigTool.exe"
echo "Packaged: $DIST_DIR/WindowsNetworkConfigTool-win-x64.zip"
