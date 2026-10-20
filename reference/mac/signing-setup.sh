#!/bin/bash
# 生成本机专用的自签名代码签名证书，放进独立钥匙串 ~/MacStream/signing/macstream-signing.keychain-db。
# 签名身份固定后，重新编译不会让“屏幕录制 / 辅助功能”授权失效。
# 不修改登录钥匙串，也不改钥匙串搜索列表；build.sh 通过 --keychain 显式使用它。
set -euo pipefail

DIR=~/MacStream/signing
KEYCHAIN="$DIR/macstream-signing.keychain-db"
NAME="MacStream Local Signing"

if [ -f "$KEYCHAIN" ]; then
    echo "签名钥匙串已存在：$KEYCHAIN"
    exit 0
fi

mkdir -p "$DIR"
chmod 700 "$DIR"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/cert.cnf" <<EOF
[req]
distinguished_name = dn
x509_extensions = ext
prompt = no
[dn]
CN = $NAME
[ext]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature
extendedKeyUsage = critical,codeSigning
EOF

openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -config "$WORK/cert.cnf" \
    -keyout "$WORK/key.pem" -out "$WORK/cert.pem" 2>/dev/null
P12_PASS=$(openssl rand -hex 16)
openssl pkcs12 -export -inkey "$WORK/key.pem" -in "$WORK/cert.pem" -name "$NAME" \
    -out "$WORK/identity.p12" -passout "pass:$P12_PASS" 2>/dev/null

KEYCHAIN_PASS=$(openssl rand -hex 24)
umask 077
printf '%s' "$KEYCHAIN_PASS" > "$DIR/keychain-password"
security create-keychain -p "$KEYCHAIN_PASS" "$KEYCHAIN"
security set-keychain-settings "$KEYCHAIN"
security unlock-keychain -p "$KEYCHAIN_PASS" "$KEYCHAIN"
security import "$WORK/identity.p12" -k "$KEYCHAIN" -P "$P12_PASS" -T /usr/bin/codesign >/dev/null
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$KEYCHAIN_PASS" "$KEYCHAIN" >/dev/null
cp "$WORK/cert.pem" "$DIR/certificate.pem"

echo "已生成证书：$NAME"
openssl x509 -in "$DIR/certificate.pem" -noout -fingerprint -sha256
