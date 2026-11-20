# andropkg runtime: which AOSP modules we build and how they deviate from Android.bp.
# Consumed by tools/gen_cmake.py. AOSP (abs path) and PLATFORM ("macos"|"ios") are injected.
import os
ANDROPKG = os.path.normpath(os.path.join(AOSP, "..", ".."))

VARIANT = dict(arch="arm64", targets=("host", "darwin", "not_windows"), codegen=())

# Soong's global defaults that matter for this code base.
GLOBAL_CFLAGS = [
    "-D__STDC_FORMAT_MACROS", "-D__STDC_CONSTANT_MACROS", "-D_FILE_OFFSET_BITS=64",
    "-fno-exceptions", "-Wno-unused-parameter", "-Wno-missing-field-initializers",
    "-Wno-unknown-warning-option", "-Wno-deprecated-declarations", "-Wno-unused-command-line-argument",
    "-Wno-thread-safety-negative", "-Wno-c99-designator", "-Wno-gnu-designator",
    "-DANDROPKG=1",
    "-DANDROID",  # Soong defines it for every module, host builds included (≠ __ANDROID__)
    # Soong builds everything with zero-initialized stack/alloca memory, and some AOSP code
    # silently depends on it (e.g. ART's vtable linking BitVector over alloca()).
    "-ftrivial-auto-var-init=zero",
]
GLOBAL_CPPFLAGS = ["-std=gnu++17", "-Wno-invalid-specialization", "-Wno-c++11-narrowing", "-Wno-strict-primary-template-shadow", "-fno-rtti", "-Wno-c++20-designator", "-Wno-deprecated-copy"]
GLOBAL_CONLYFLAGS = ["-std=gnu17"]

# art/build/art.go globalFlags() + hostFlags() for our configuration (interpreter only).
# Stack-overflow gaps must be multiples of the 16 KB Darwin page (static_asserts check all ISAs).
ART_CFLAGS = [
    "-DART_DEFAULT_GC_TYPE_IS_CMC", "-DART_USE_READ_BARRIER=1", "-DART_READ_BARRIER_TYPE_IS_BAKER=1",
    "-DART_USE_GENERATIONAL_CC=1", "-DART_USE_TLAB=1", "-DART_DEFAULT_COMPACT_DEX_LEVEL=fast",
    "-DART_STACK_OVERFLOW_GAP_arm=16384", "-DART_STACK_OVERFLOW_GAP_arm64=16384",
    "-DART_STACK_OVERFLOW_GAP_riscv64=16384", "-DART_STACK_OVERFLOW_GAP_x86=16384",
    "-DART_STACK_OVERFLOW_GAP_x86_64=16384", "-DUSE_D8_DESUGAR=1",
    "-DART_FRAME_SIZE_LIMIT=1736", "-DART_BASE_ADDRESS=0x60000000",
    "-DART_BASE_ADDRESS_MIN_DELTA=(-0x1000000)", "-DART_BASE_ADDRESS_MAX_DELTA=0x1000000",
    "-Wno-shadow", "-Wno-used-but-marked-unused", "-Wno-deprecated", "-Wno-unreachable-code",
    "-Wno-unreachable-code-break", "-Wno-unreachable-code-return", "-Wno-float-equal",
]
ART_ASFLAGS = ["-DART_USE_READ_BARRIER=1", "-DART_READ_BARRIER_TYPE_IS_BAKER=1"]

# Headers for dependencies that aren't (yet) built from AOSP modules.
EXTERNAL_INCLUDES = {
    # NDK stub library; the real headers are ICU4C's.
    "libicu": [f"{AOSP}/external/icu/icu4c/source/common", f"{AOSP}/external/icu/icu4c/source/i18n",
               f"{AOSP}/external/icu/android_icu4c/include"],  # uconfig_local.h (with -DANDROID)
    "libnativehelper_compat_libc++": [f"{AOSP}/libnativehelper/header_only_include"],
    "libz": [],  # Darwin's libz
    "libarect": [f"{AOSP}/frameworks/native/libs/arect/include"],
    "libnativewindow_headers": [f"{AOSP}/frameworks/native/libs/nativewindow/include",
                                f"{AOSP}/frameworks/native/libs/nativebase/include"],
}

TARGETS = [
    dict(name="fmtlib", bp="fmtlib"),
    dict(name="liblog", bp="liblog", extra_includes=["system/core/libutils/include", "system/core/libsystem/include"]),
    dict(name="libbase", bp="libbase"),
    dict(name="liblz4", bp="liblz4"),
    dict(name="libziparchive", bp="libziparchive"),
    dict(name="libartpalette", bp="libartpalette", art=True),
    dict(name="tinyxml2", bp="libtinyxml2"),
    # libartbase is linked into libart.dylib, not a separate libartbase.dylib.
    dict(name="libartbase", bp="libartbase", art=True, cflags=["-DART_STATIC_LIBARTBASE"]),
    dict(name="libdexfile", bp="libdexfile", art=True),
    dict(name="libprofile", bp="libprofile", art=True),
    dict(name="libelffile", bp="libelffile", art=True,
         exclude_srcs=["art/libelffile/elf/xz_utils.cc"],
         andropkg_srcs=[f"{ANDROPKG}/runtime/art/xz_utils_disabled.cc"]),
    # Android.bp picks sigchain_fake.cc for darwin; ART's fault handler needs the real chain.
    dict(name="libsigchain", bp="libsigchain", art=True,
         exclude_srcs=["art/sigchainlib/sigchain_fake.cc"], extra_srcs=["art/sigchainlib/sigchain.cc"]),
    dict(name="libnativebridge", bp="libnativebridge", art=True),
    dict(name="libnativeloader", bp="libnativeloader", art=True),
    # JniInvocation.c is the dlopen("libart.so") proxy; we link ART into the same image.
    dict(name="libnativehelper", bp="libnativehelper",
         exclude_srcs=["libnativehelper/JniInvocation.c", "libnativehelper/DlHelp.c"]),
    dict(name="libodrstatslog", bp="libodrstatslog", art=True),
    dict(name="libart", bp="libart", art=True,
         exclude_srcs=["art/runtime/interpreter/mterp/nterp_impl.cc",
                       "art/runtime/backtrace_helper.cc",
                       "art/runtime/native_stack_dump.cc"],
         skip_generated=["libart_mterp.arm64ng"],
         # ELF/GNU-as sources ported to Mach-O at build time (tools/elf2macho_asm.py).
         asm_port=["art/runtime/arch/arm64", "art/runtime/arch/quick_alloc_entrypoints.S",
                   "art/runtime/interpreter/cfi_asm_support.h"],
         andropkg_srcs=[f"{ANDROPKG}/runtime/art/nterp_disabled.cc",
                        f"{ANDROPKG}/runtime/art/native_stack_darwin.cc",
                        f"{ANDROPKG}/runtime/art/jni_invocation_inproc.cc"]),
    dict(name="libopenjdkjvm", bp="libopenjdkjvm", art=True),

    # ---- ICU4C (one shared copy, inside libart.dylib) --------------------------------
    dict(name="libicuuc", bp="libicuuc"),
    dict(name="libicui18n", bp="libicui18n"),
    dict(name="libicuuc_stubdata", bp="libicuuc_stubdata"),  # real data: icudt72l.dat via libandroidicuinit
    dict(name="libandroidicuinit", bp="libandroidicuinit"),

    # ---- libcore JNI libraries (separate dylibs loaded by Runtime::InitNativeMethods) -
    dict(name="libandroidio", bp="libandroidio"),
    dict(name="libexpat", bp="libexpat"),
    dict(name="libfdlibm", bp="libfdlibm"),
    dict(name="libicu_jni", bp="libicu_jni"),
    # ---- BoringSSL (pure C: OPENSSL_NO_ASM, as Android.bp does for darwin) + Conscrypt --
    dict(name="libcrypto", bp="libcrypto"),
    dict(name="libssl", bp="libssl"),
    dict(name="libjavacrypto", bp="libjavacrypto"),
    dict(name="libjavacore", bp="libjavacore"),
    dict(name="libopenjdk", bp="libopenjdk"),

    # ---- Android framework natives (libandroid_runtime.dylib) -------------------------
    dict(name="libcutils", bp="libcutils", extra_includes=["system/core/libutils/include"]),
    # Looper (epoll + eventfd -> andropkg compat) isn't part of libutils' host variant.
    dict(name="libutils", bp="libutils", extra_srcs=["system/core/libutils/Looper.cpp"]),
    # ---- graphics: Skia (CPU), codecs, fonts/text, hwui (in libandroid_runtime.dylib) ---
    dict(name="libpng", bp="libpng"),
    dict(name="libjpeg", bp="libjpeg"),
    dict(name="libwebp-decode", bp="libwebp-decode"),
    dict(name="libwebp-encode", bp="libwebp-encode",  # cpu.c also in decode (archives dedupe on Android)
         exclude_srcs=["external/webp/src/dsp/cpu.c"]),
    dict(name="libwuffs", bp="libwuffs_mirror_release_c"),
    dict(name="libsfntly", bp="libsfntly"),
    dict(name="libft2", bp="libft2"),
    dict(name="libharfbuzz_ng", bp="libharfbuzz_ng"),
    # RAW (dng_sdk/piex) decoding is left out: not portable to Darwin's int64_t, rarely used.
    dict(name="libskia", bp="libskia", remove_cflags=["-DSK_CODEC_DECODES_RAW"],
         exclude_srcs=["external/skia/src/codec/SkRawCodec.cpp"],
         andropkg_srcs=[f"{ANDROPKG}/runtime/graphics/skia_raw_disabled.cc"]),
    dict(name="libminikin", bp="libminikin"),
    dict(name="libmodpb64", bp="libmodpb64"),
    dict(name="libimage_io", bp="libimage_io"),
    dict(name="libjpegencoder", bp="libjpegencoder"),
    dict(name="libjpegdecoder", bp="libjpegdecoder"),
    dict(name="libultrahdr", bp="libultrahdr"),
    # Host hwui is the layoutlib build; register JNI like the device (jni_runtime.cpp).
    dict(name="libhwui", bp="libhwui", cpp_std="gnu++20",
         exclude_srcs=["frameworks/base/libs/hwui/apex/LayoutlibLoader.cpp"],
         extra_srcs=["frameworks/base/libs/hwui/apex/jni_runtime.cpp",
                     "frameworks/base/libs/hwui/apex/android_canvas.cpp"],
         andropkg_srcs=[f"{ANDROPKG}/runtime/graphics/gpu_unavailable.cc",
                        f"{ANDROPKG}/runtime/graphics/hwui_jni_pending.cc",
                        f"{ANDROPKG}/runtime/graphics/window_jni.cc"],
         extra_includes=["frameworks/native/libs/ui/include_types", "frameworks/native/libs/ui/include",
                         "frameworks/native/include", "frameworks/native/libs/math/include",
                         "frameworks/native/opengl/include", "external/vulkan-headers/include",
                         "hardware/libhardware/include",
                         "frameworks/base/core/jni", "frameworks/base/core/jni/include"]),
    # ---- input: MotionEvent/KeyEvent/VelocityTracker (libinput core, no transport) --------
    dict(name="libinput", bp="libinput", cpp_std="gnu++20", skip_generated=["inputconstants-cpp", "cxx-bridge-header", "libinput_cxx_bridge_header", "libinput_cxx_bridge_code",
                                                 "toolbox_input_labels"],
         srcs=["frameworks/native/libs/input/Input.cpp",
               "frameworks/native/libs/input/InputEventLabels.cpp",
               "frameworks/native/libs/input/PrintTools.cpp",
               "frameworks/native/libs/input/VelocityTracker.cpp",
               "frameworks/native/libs/input/KeyCharacterMap.cpp",
               "frameworks/native/libs/input/Keyboard.cpp",
               "frameworks/native/libs/input/KeyLayoutMap.cpp",
               "frameworks/native/libs/input/VirtualKeyMap.cpp",
               "frameworks/native/libs/input/InputDevice.cpp",
               "frameworks/native/libs/input/PropertyMap.cpp",
               "frameworks/native/libs/ui/Transform.cpp",
               "frameworks/native/libs/ui/Region.cpp",
               "frameworks/native/libs/ui/Rect.cpp"],
         extra_includes=["frameworks/native/libs/ui/include", "frameworks/native/libs/ui/include_types",
                         "frameworks/native/libs/math/include", "frameworks/native/include",
                         "system/core/libutils/include", "system/core/libsystem/include",
                         "../../runtime/compat/aidl"]),
    dict(name="libincfs-utils", bp="libincfs-utils"),
    # androidfw (and code including its headers) is C++20 (std::span).
    dict(name="libandroidfw", bp="libandroidfw", cpp_std="gnu++20",
         extra_includes=["frameworks/native/include"]),
    dict(name="libandroid_runtime", bp="libandroid_runtime", cpp_std="gnu++20",
         srcs=["frameworks/base/core/jni/core_jni_helpers.cpp",
               "frameworks/base/core/jni/android_content_res_ApkAssets.cpp",
               "frameworks/base/core/jni/android_util_AssetManager.cpp",
               "frameworks/base/core/jni/android_util_StringBlock.cpp",
               "frameworks/base/core/jni/android_util_XmlBlock.cpp",
               "frameworks/base/core/jni/android_content_res_ResourceTimer.cpp",
               "frameworks/base/core/jni/android_os_SystemClock.cpp",
               "frameworks/base/core/jni/android_util_Log.cpp",
               "frameworks/base/core/jni/android_util_EventLog.cpp",
               "frameworks/base/core/jni/android_os_MessageQueue.cpp",
               "frameworks/base/core/jni/android_os_Trace.cpp",
               "frameworks/base/core/jni/android_os_SystemProperties.cpp",
               "frameworks/base/core/jni/com_android_internal_os_ClassLoaderFactory.cpp",
               "frameworks/base/core/jni/android_view_MotionEvent.cpp",
               "frameworks/base/core/jni/android_view_KeyEvent.cpp",
               "frameworks/base/core/jni/android_view_VelocityTracker.cpp",
               "frameworks/base/core/jni/android_view_KeyCharacterMap.cpp",
               "frameworks/base/core/jni/com_android_internal_util_VirtualRefBasePtr.cpp",
               "frameworks/base/core/jni/android_animation_PropertyValuesHolder.cpp",
               "frameworks/base/core/jni/android_text_AndroidCharacter.cpp",
               "frameworks/base/core/jni/android_text_Hyphenator.cpp",
               "frameworks/base/core/jni/android_util_CharsetUtils.cpp",
               "frameworks/base/core/jni/android_content_res_Configuration.cpp"],
         andropkg_srcs=[f"{ANDROPKG}/runtime/android_runtime/android_runtime_onload.cc",
                        f"{ANDROPKG}/runtime/android_runtime/binder_stubs.cc",
                        f"{ANDROPKG}/runtime/android_runtime/system_properties.cc",
                        f"{ANDROPKG}/runtime/android_runtime/process_stubs.cc",
                        f"{ANDROPKG}/runtime/android_runtime/framework_stubs.cc",
                        f"{ANDROPKG}/runtime/android_runtime/parcel_jni.cc"],
         skip_generated=["deviceproductinfoconstants_aidl"],
         extra_includes=["frameworks/base/core/jni", "frameworks/base/core/jni/include",
                         "art/libnativeloader/include",
                         "frameworks/native/libs/input/include", "frameworks/native/libs/ui/include",
                         "frameworks/native/libs/ui/include_types", "frameworks/native/libs/math/include",
                         "frameworks/base/libs/hwui/apex/include", "../../runtime/compat/aidl",
                         "frameworks/native/include"]),
]
