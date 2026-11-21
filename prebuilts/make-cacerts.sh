#!/bin/sh
# Builds Android's CA store layout (<subject_hash_old>.<n> PEM files, read by Conscrypt's
# TrustedCertificateStore from /apex/com.android.conscrypt/cacerts) from a PEM root bundle.
# Default: Apple's system root store on macOS (/etc/ssl/cert.pem there is years out of date),
# else the distro bundle. Output: prebuilts/out/android-14/cacerts
#   prebuilts/make-cacerts.sh [bundle.pem]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out="$here/out/android-14/cacerts"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if [ $# -ge 1 ]; then
  bundle=$1
elif [ -f /System/Library/Keychains/SystemRootCertificates.keychain ]; then
  bundle="$tmp/apple-roots.pem"
  security find-certificate -a -p /System/Library/Keychains/SystemRootCertificates.keychain > "$bundle"
else
  bundle=/etc/ssl/certs/ca-certificates.crt
fi
rm -rf "$out" && mkdir -p "$out"
awk -v d="$tmp" '/-----BEGIN CERTIFICATE-----/{n++; f=sprintf("%s/c%04d.pem", d, n)} f{print > f} /-----END CERTIFICATE-----/{close(f); f=""}' "$bundle"
for c in "$tmp"/c*.pem; do
  h=$(openssl x509 -noout -subject_hash_old -in "$c" 2>/dev/null) || continue
  i=0; while [ -e "$out/$h.$i" ]; do i=$((i + 1)); done
  openssl x509 -in "$c" -outform PEM > "$out/$h.$i"
done
echo "cacerts: $(ls "$out" | wc -l | tr -d ' ') roots -> $out"
