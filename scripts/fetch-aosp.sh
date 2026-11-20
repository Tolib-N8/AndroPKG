#!/bin/sh
# Shallow-clones only the AOSP projects the andropkg runtime builds from (no repo, no full
# checkout). Pinned to one tag so ART (C++) and the prebuilt Android 14 jars stay in sync.
#   scripts/fetch-aosp.sh [project ...]     (default: all)
set -eu
TAG=${AOSP_TAG:-android-14.0.0_r18}
root=$(cd "$(dirname "$0")/.." && pwd)
dst="$root/third_party/aosp"
base=https://android.googlesource.com/platform

# path-in-tree  gitiles-project
PROJECTS="
art                    art
libcore                libcore
libnativehelper        libnativehelper
system/libbase         system/libbase
system/logging         system/logging
system/core            system/core
system/libziparchive   system/libziparchive
external/fmtlib        external/fmtlib
external/lz4           external/lz4
external/expat         external/expat
external/fdlibm        external/fdlibm
external/icu           external/icu
external/tinyxml2      external/tinyxml2
external/dlmalloc      external/dlmalloc
bionic                 bionic
external/boringssl     external/boringssl
external/conscrypt     external/conscrypt
frameworks/base        frameworks/base
frameworks/native      frameworks/native
system/incremental_delivery system/incremental_delivery
"
# Big repos: partial clone + sparse checkout of just what the runtime builds.
sparse_paths() {
  case "$1" in
    bionic) echo "/libc/include/elf.h /libc/include/bits/elf_common.h /libc/include/bits/auxvec.h
                 /libc/kernel/uapi/ /libc/kernel/android/uapi/
                 !/libc/kernel/uapi/linux/netfilter*/" ;;
    system/incremental_delivery) echo "/incfs/util/ /incfs/Android.bp /incfs/include/" ;;
    frameworks/base) echo "/core/jni/ /libs/androidfw/ /libs/hwui/ /include/ /native/android/ /core/proto/" ;;
    frameworks/native) echo "/include/ /libs/binder/include/ /libs/nativewindow/include/ /libs/nativebase/include/
                            /libs/arect/include/ /libs/math/include/ /libs/ui/include/ /libs/gui/include/
                            /libs/input/ /include/input/" ;;
    external/boringssl) echo "/Android.bp /sources.bp /err_data.c /src/include/ /src/crypto/ /src/ssl/ /src/third_party/fiat/
                             /src/decrepit/ /apple-aarch64/" ;;
    external/conscrypt) echo "/Android.bp /common/src/jni/" ;;
    external/icu) echo "/Android.bp /build/ /icu4c/source/Android.bp /icu4c/source/common/ /icu4c/source/i18n/
                       /icu4c/source/stubdata/ /libicu/ /libandroidicuinit/ /android_icu4c/ /android_icu4j/Android.bp
                       /android_icu4j/libcore_bridge/" ;;
  esac
}
mkdir -p "$dst"
echo "$PROJECTS" | while read -r path proj; do
  [ -n "$path" ] || continue
  if [ $# -gt 0 ]; then case " $* " in *" $path "*) ;; *) continue ;; esac; fi
  if [ -d "$dst/$path/.git" ]; then echo "= $path (present)"; continue; fi
  echo "+ $path @ $TAG"
  sp=$(sparse_paths "$path")
  if [ -n "$sp" ]; then
    git -c advice.detachedHead=false clone -q --depth 1 --filter=blob:none --sparse --branch "$TAG" "$base/$proj" "$dst/$path"
    # shellcheck disable=SC2086
    git -C "$dst/$path" sparse-checkout set --no-cone $sp
  else
    git -c advice.detachedHead=false clone -q --depth 1 --branch "$TAG" "$base/$proj" "$dst/$path"
  fi
done
# Re-apply andropkg's patches (scripts/save-patches.sh) to freshly cloned projects.
for patch in "$root"/runtime/patches/*.patch; do
  [ -f "$patch" ] || continue
  path=$(basename "$patch" .patch | tr _ /)
  [ -d "$dst/$path" ] || path=$(basename "$patch" .patch | sed 's|_|/|')   # e.g. system_libbase
  if git -C "$dst/$path" apply --check "$patch" 2>/dev/null; then
    git -C "$dst/$path" apply "$patch" && echo "patched $path"
  fi
done
echo "$TAG" > "$dst/TAG"
du -sh "$dst"
