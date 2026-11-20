#!/bin/sh
# Saves local modifications of the AOSP clones as runtime/patches/<project>.patch
# (fetch-aosp.sh re-applies them after cloning).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
dst="$root/runtime/patches"
mkdir -p "$dst"
cd "$root/third_party/aosp"
find . -name .git -maxdepth 3 -type d | sed 's|^\./||; s|/\.git$||' | sort | while read -r p; do
  name=$(echo "$p" | tr / _)
  case "$p" in bionic) continue ;; esac   # read-only headers (case-colliding netfilter names on APFS)| tr / _)
  if git -C "$p" diff --quiet; then
    rm -f "$dst/$name.patch"
  else
    git -C "$p" diff --no-color > "$dst/$name.patch"
    echo "saved $name.patch ($(git -C "$p" diff --stat | tail -1))"
  fi
done
