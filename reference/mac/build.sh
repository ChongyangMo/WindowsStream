#!/bin/bash
# 在 Mac 上编译 MacStream.app 并重新打开它。源码由 Windows 端同步到 ~/MacStream/src。
# 登录后自动启动由配置 launchAtLogin 控制（App 自己通过 SMAppService 注册，可在“系统设置 → 通用 → 登录项”中关闭）。
set -euo pipefail

ROOT=~/MacStream
SRC="$ROOT/src"
APP="$ROOT/MacStream.app"

echo "== 编译"
mkdir -p "$ROOT/build"
swiftc -O -swift-version 5 -module-name MacStream -import-objc-header "$SRC/core/VirtualDisplay.h" \
    -o "$ROOT/build/MacStream" "$SRC"/core/*.swift \
    -framework ScreenCaptureKit -framework VideoToolbox -framework CoreMedia -framework Metal -framework Accelerate \
    -framework AppKit -framework ApplicationServices -framework IOKit -framework CryptoKit -framework ServiceManagement

pkill -x MacStream 2>/dev/null && sleep 1 || true
mkdir -p "$APP/Contents/MacOS"
cp "$ROOT/build/MacStream" "$APP/Contents/MacOS/MacStream"
cp "$SRC/app/Info.plist" "$APP/Contents/Info.plist"

# 有本机签名证书（signing-setup.sh 生成）就用它，签名身份固定，重新编译不影响已授予的权限；否则 ad-hoc。
KEYCHAIN="$ROOT/signing/macstream-signing.keychain-db"
if [ -f "$KEYCHAIN" ]; then
    echo "== 打包并签名（MacStream Local Signing）"
    security unlock-keychain -p "$(cat "$ROOT/signing/keychain-password")" "$KEYCHAIN"
    codesign -s "MacStream Local Signing" --keychain "$KEYCHAIN" -f "$APP"
else
    echo "== 打包并签名（ad-hoc）"
    codesign -s - -f "$APP"
fi
codesign -d -r- "$APP" 2>&1 | grep designated || true

echo "== 启动"
open "$APP"
sleep 2
pgrep -lx MacStream || echo "MacStream 未在运行，请查看 ~/Library/Logs/MacStream.log"
