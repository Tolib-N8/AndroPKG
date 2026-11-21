# andropkg runtime: AOSP ART + libcore natives, built for Darwin (macOS dev loop, iOS device).
#   cmake -S runtime -B build/runtime-macos -G Ninja
#   cmake -S runtime -B build/runtime-ios -G Ninja -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64
cmake_minimum_required(VERSION 3.24)
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  # os_sync_wait_on_address (futex emulation) needs iOS 17.4 / macOS 14.4.
  set(CMAKE_OSX_DEPLOYMENT_TARGET 14.4 CACHE STRING "")
endif()
project(andropkg_runtime C CXX ASM OBJC OBJCXX)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(ANDROPKG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(ANDROPKG_AOSP ${ANDROPKG_ROOT}/third_party/aosp CACHE PATH "AOSP sources (scripts/fetch-aosp.sh)")
set(ANDROPKG_TOOLS ${ANDROPKG_ROOT}/tools)
set(ANDROPKG_GEN_DIR ${CMAKE_BINARY_DIR}/gen)
option(ANDROPKG_ART_DEBUG "Build ART with debug checks (kIsDebugBuild), -O1" OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(ANDROPKG_PLATFORM ios)
else()
  set(ANDROPKG_PLATFORM macos)
endif()

# Regenerate module lists from Android.bp whenever the config or generator changes.
set(_gen ${CMAKE_BINARY_DIR}/modules.cmake)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/modules.py ${ANDROPKG_TOOLS}/gen_cmake.py ${ANDROPKG_TOOLS}/bp.py)
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env ANDROPKG_PLATFORM=${ANDROPKG_PLATFORM}
          ${Python3_EXECUTABLE} ${ANDROPKG_TOOLS}/gen_cmake.py ${ANDROPKG_AOSP} ${_gen} ${CMAKE_CURRENT_SOURCE_DIR}/modules.py
  RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "gen_cmake.py failed")
endif()

# Everything is built like Soong release builds (NDEBUG: some AOSP asserts don't even compile);
# ART modules optionally get kIsDebugBuild checks (see ANDROPKG_ART_TARGETS below).
if(ANDROPKG_ART_DEBUG)
  set(ANDROPKG_OPT_FLAGS -UNDEBUG -DDYNAMIC_ANNOTATIONS_ENABLED=1 -O1 -g)
  # Line tables only outside ART: keeps symbolized stacks, saves gigabytes of debug info.
  add_compile_options(-DNDEBUG -O1 -gline-tables-only)
else()
  set(ANDROPKG_OPT_FLAGS -DNDEBUG -O2 -g)
  add_compile_options(${ANDROPKG_OPT_FLAGS})
endif()
if(NOT CMAKE_OSX_SYSROOT)
  execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE CMAKE_OSX_SYSROOT OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
# Our shims for Linux/bionic-only headers come first; the compat header is force-included.
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/compat/include)
set(ANDROPKG_FORCE_INCLUDE "-include ${CMAKE_CURRENT_SOURCE_DIR}/compat/include/andropkg_darwin_compat.h")
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:${ANDROPKG_FORCE_INCLUDE}>")
# Linux UAPI + bionic headers (for <elf.h> etc.), searched only after the real system headers.
foreach(_d bionic/libc/include/bits/.. bionic/libc/kernel/uapi bionic/libc/kernel/uapi/asm-arm64 bionic/libc/kernel/android/uapi)
  add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-idirafter${ANDROPKG_AOSP}/${_d}>")
endforeach()

include(${_gen})
if(ANDROPKG_ART_DEBUG)
  foreach(_t ${ANDROPKG_ART_TARGETS})
    target_compile_options(${_t} PRIVATE -UNDEBUG -DDYNAMIC_ANNOTATIONS_ENABLED=1)
  endforeach()
endif()

# andropkg's own Darwin implementations of Linux/bionic facilities (futex, ...).
file(GLOB ANDROPKG_COMPAT_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/compat/src/*.cc ${CMAKE_CURRENT_SOURCE_DIR}/compat/src/*.c)
add_library(andropkg_compat OBJECT ${ANDROPKG_COMPAT_SRCS})
target_compile_options(andropkg_compat PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)

# asm_defines.h: compile asm_defines.cc to assembly with libart's flags, scrape the constants
# (art/tools/cpp-define-generator). Must use the *target* triple/sysroot.
if(DEFINED ANDROPKG_ASM_DEFINES_OWNER)
  set(_o ${ANDROPKG_ASM_DEFINES_OWNER})
  list(TRANSFORM ANDROPKG_INCLUDES_${_o} PREPEND "-I" OUTPUT_VARIABLE _incs)
  set(_target_flags -arch arm64 -isysroot ${CMAKE_OSX_SYSROOT})
  if(CMAKE_OSX_DEPLOYMENT_TARGET)
    if(ANDROPKG_PLATFORM STREQUAL "ios")
      list(APPEND _target_flags -mios-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET})
    else()
      list(APPEND _target_flags -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET})
    endif()
  endif()
  set(_s ${ANDROPKG_ASM_DEFINES_DIR}/asm_defines.s)
  add_custom_command(OUTPUT ${_s}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ANDROPKG_ASM_DEFINES_DIR}
    COMMAND ${CMAKE_CXX_COMPILER} ${_target_flags} ${ANDROPKG_OPT_FLAGS}
            -include ${CMAKE_CURRENT_SOURCE_DIR}/compat/include/andropkg_darwin_compat.h ${ANDROPKG_CXXFLAGS_${_o}}
            -I${CMAKE_CURRENT_SOURCE_DIR}/compat/include ${_incs} -S ${ANDROPKG_ASM_DEFINES_SRC} -o ${_s}
    DEPENDS ${ANDROPKG_ASM_DEFINES_SRC} VERBATIM COMMAND_EXPAND_LISTS)
  add_custom_command(OUTPUT ${ANDROPKG_ASM_DEFINES_DIR}/asm_defines.h
    COMMAND ${Python3_EXECUTABLE} ${ANDROPKG_TOOLS}/redirect.py ${ANDROPKG_ASM_DEFINES_DIR}/asm_defines.h
            ${ANDROPKG_ASM_DEFINES_TOOL} ${_s}
    DEPENDS ${_s} ${ANDROPKG_ASM_DEFINES_TOOL} VERBATIM)
  add_custom_target(andropkg_asm_defines DEPENDS ${ANDROPKG_ASM_DEFINES_DIR}/asm_defines.h)
endif()

# ELF-flavoured assembly -> Mach-O, in two phases (see tools/elf2macho_asm.py, asm_symbols.py):
#   1. assemble the ported sources as-is and collect every global symbol they use,
#   2. re-assemble with `#define sym _sym` force-included (Mach-O C symbol prefix).
# Produces OBJECT library <target>_asm.
function(andropkg_port_asm target)
  set(_root ${ANDROPKG_GEN_DIR}/asm)
  set(_ported)
  set(_deps)
  foreach(_s ${ANDROPKG_ASM_SRCS_${target}})
    list(APPEND _ported ${_root}/${_s})
  endforeach()
  foreach(_d ${ANDROPKG_ASM_PORT_DIRS_${target}})
    file(GLOB_RECURSE _g ${ANDROPKG_AOSP}/${_d} ${ANDROPKG_AOSP}/${_d}/*.S ${ANDROPKG_AOSP}/${_d}/*.h)
    list(APPEND _deps ${_g})
  endforeach()
  add_custom_command(OUTPUT ${_ported} ${_root}/.stamp
    COMMAND ${Python3_EXECUTABLE} ${ANDROPKG_TOOLS}/elf2macho_asm.py ${ANDROPKG_AOSP} ${_root} ${ANDROPKG_ASM_PORT_DIRS_${target}}
    DEPENDS ${_deps} ${ANDROPKG_TOOLS}/elf2macho_asm.py VERBATIM)
  set(_incs ${_root}/art/runtime ${ANDROPKG_INCLUDES_${target}})
  set_source_files_properties(${_ported} PROPERTIES LANGUAGE ASM)

  add_library(${target}_asm1 OBJECT ${_ported})
  target_include_directories(${target}_asm1 PRIVATE ${_incs})
  target_compile_options(${target}_asm1 PRIVATE ${ANDROPKG_ASMFLAGS_${target}})
  add_dependencies(${target}_asm1 andropkg_asm_defines)

  set(_hdr ${_root}/asm_c_symbols.h)
  add_custom_command(OUTPUT ${_hdr}
    COMMAND ${Python3_EXECUTABLE} ${ANDROPKG_TOOLS}/asm_symbols.py ${_hdr} $<TARGET_OBJECTS:${target}_asm1>
    DEPENDS ${target}_asm1 $<TARGET_OBJECTS:${target}_asm1> ${ANDROPKG_TOOLS}/asm_symbols.py
    COMMAND_EXPAND_LISTS VERBATIM)
  add_custom_target(${target}_asm_symbols DEPENDS ${_hdr})

  # Phase 2 compiles tiny wrappers so the header dependency doesn't leak into phase 1.
  set(_wrappers)
  foreach(_p ${_ported})
    string(REPLACE "${_root}/" "${ANDROPKG_GEN_DIR}/asm2/" _w ${_p})
    file(WRITE ${_w}.in "#include \"${_hdr}\"\n#include \"${_p}\"\n")
    configure_file(${_w}.in ${_w} COPYONLY)
    list(APPEND _wrappers ${_w})
  endforeach()
  set_source_files_properties(${_wrappers} PROPERTIES LANGUAGE ASM OBJECT_DEPENDS "${_hdr};${_ported}")
  add_library(${target}_asm OBJECT ${_wrappers})
  target_include_directories(${target}_asm PRIVATE ${_incs})
  target_compile_options(${target}_asm PRIVATE ${ANDROPKG_ASMFLAGS_${target}})
  add_dependencies(${target}_asm ${target}_asm_symbols)
endfunction()

foreach(_t libart)
  if(DEFINED ANDROPKG_ASM_SRCS_${_t})
    andropkg_port_asm(${_t})
  endif()
endforeach()

# ---- Final images ---------------------------------------------------------------------
# libart.dylib: the VM and everything it statically depends on.
set(ANDROPKG_ART_PARTS fmtlib liblog libbase liblz4 libziparchive libartpalette tinyxml2
    libartbase libdexfile libprofile libelffile libsigchain libnativebridge libnativeloader
    libnativehelper libodrstatslog libart libart_asm libopenjdkjvm andropkg_compat
    libicuuc libicui18n libicuuc_stubdata libandroidicuinit libandroidio libcrypto libssl)
set(_objs)
foreach(_p ${ANDROPKG_ART_PARTS})
  list(APPEND _objs $<TARGET_OBJECTS:${_p}>)
endforeach()
add_library(andropkg_art SHARED ${_objs})
set_target_properties(andropkg_art PROPERTIES OUTPUT_NAME art LINKER_LANGUAGE CXX
                      INSTALL_NAME_DIR "@rpath")
target_link_libraries(andropkg_art PRIVATE z)
set_target_properties(andropkg_art PROPERTIES BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")

# libcore's JNI libraries: loaded by name ("libjavacore.so" -> @rpath/libjavacore.dylib).
function(andropkg_jni_dylib name)
  set(_o)
  foreach(_p ${ARGN})
    list(APPEND _o $<TARGET_OBJECTS:${_p}>)
  endforeach()
  add_library(${name}_dylib SHARED ${_o})
  set_target_properties(${name}_dylib PROPERTIES OUTPUT_NAME ${name} PREFIX "" LINKER_LANGUAGE CXX
                        INSTALL_NAME_DIR "@rpath" BUILD_RPATH "@loader_path")
  target_link_libraries(${name}_dylib PRIVATE andropkg_art z)
endfunction()
andropkg_jni_dylib(libjavacore libjavacore libexpat)
andropkg_jni_dylib(libopenjdk libopenjdk libfdlibm)
andropkg_jni_dylib(libicu_jni libicu_jni)
andropkg_jni_dylib(libjavacrypto libjavacrypto)
# andropkg-owned stand-ins for platform JNI libraries that talk to absent system daemons.
function(andropkg_stub_jni name src)
  add_library(andropkg_${name} OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/android_runtime/${src})
  target_compile_options(andropkg_${name} PRIVATE -std=gnu++17)
  target_include_directories(andropkg_${name} PRIVATE ${ANDROPKG_AOSP}/libnativehelper/include_jni)
  andropkg_jni_dylib(${name} andropkg_${name})
endfunction()
andropkg_stub_jni(libstats_jni stats_jni_stub.cc)
andropkg_stub_jni(libframework-connectivity-jni connectivity_jni_stub.cc)
andropkg_stub_jni(libframework-connectivity-tiramisu-jni connectivity_t_jni_stub.cc)
# toolbox_input_labels genrule: key/axis label table for libinput, from the kernel uapi headers.
set(_labels ${CMAKE_CURRENT_BINARY_DIR}/gen/input-labels/input.h-labels.h)
add_custom_command(OUTPUT ${_labels}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen/input-labels
  COMMAND python3 ${ANDROPKG_AOSP}/system/core/toolbox/generate-input.h-labels.py
          ${ANDROPKG_AOSP}/bionic/libc/kernel/uapi/linux/input.h
          ${ANDROPKG_AOSP}/bionic/libc/kernel/uapi/linux/input-event-codes.h > ${_labels}
  DEPENDS ${ANDROPKG_AOSP}/system/core/toolbox/generate-input.h-labels.py VERBATIM)
add_custom_target(input_labels DEPENDS ${_labels})
add_dependencies(libinput input_labels)
target_include_directories(libinput PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen/input-labels)

# Audio and media on Core Audio / AVFoundation (runtime/media, Objective-C++ with ARC; no AOSP
# headers, so the Linux compat include setup does not apply to them).
add_library(andropkg_audio OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/media/audio_track.mm)
add_library(andropkg_media_player OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/media/media_player.mm
            ${CMAKE_CURRENT_SOURCE_DIR}/media/decoder.mm ${CMAKE_CURRENT_SOURCE_DIR}/media/media_codec.mm)
add_library(andropkg_soundpool OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/media/soundpool.mm
            ${CMAKE_CURRENT_SOURCE_DIR}/media/decoder.mm)
foreach(_t andropkg_audio andropkg_media_player andropkg_soundpool)
  target_compile_options(${_t} PRIVATE -fobjc-arc -std=gnu++17)
  target_include_directories(${_t} PRIVATE ${ANDROPKG_AOSP}/libnativehelper/include_jni
                             ${ANDROPKG_AOSP}/external/tremolo/Tremolo ${ANDROPKG_AOSP}/external/libopus/include)
endforeach()
# Each dylib carries its own copy of the Ogg decoders (Tremolo, libopus).
andropkg_jni_dylib(libmedia_jni andropkg_media_player libvorbisidec libopus)
target_link_libraries(libmedia_jni_dylib PRIVATE "-framework Foundation" "-framework AVFoundation" "-framework CoreMedia"
                      "-framework AudioToolbox")
andropkg_jni_dylib(libsoundpool andropkg_soundpool libvorbisidec libopus)
target_link_libraries(libsoundpool_dylib PRIVATE "-framework Foundation" "-framework AVFoundation" "-framework AudioToolbox")

andropkg_jni_dylib(libandroid_runtime andropkg_audio libandroid_runtime libandroidfw libincfs-utils libutils libcutils
    libhwui libskia libminikin libharfbuzz_ng libft2 libpng libjpeg libwebp-decode libwebp-encode libwuffs
    libsfntly libultrahdr libjpegencoder libjpegdecoder libimage_io libmodpb64 libexpat libinput
    libsqlite libsqlite3_android)
# Skia's Apple ports (CoreText fonts, CG/ImageIO codecs) are part of the build.
if(ANDROPKG_PLATFORM STREQUAL "ios")
  set(_uti_fw "-framework MobileCoreServices")
else()
  set(_uti_fw "-framework CoreServices")
endif()
target_link_libraries(libandroid_runtime_dylib PRIVATE "-framework CoreFoundation" "-framework CoreGraphics"
                      "-framework CoreText" "-framework ImageIO" ${_uti_fw}
                      "-framework Foundation" "-framework AudioToolbox" "-framework AVFoundation"
                      "-framework MediaPlayer")
if(ANDROPKG_PLATFORM STREQUAL "ios")
  target_link_libraries(libandroid_runtime_dylib PRIVATE "-framework UIKit")
else()
  target_link_libraries(libandroid_runtime_dylib PRIVATE "-framework AppKit")
endif()

# ---- NDK layer: Android arm64 ELF libraries of apps (loader/, shim/, ndk-libs/) -------------
# ART hands System.loadLibrary of an ELF to libandropkg_ndk (runtime/patches/art.patch).
set(_ndkgen ${CMAKE_BINARY_DIR}/gen/ndk)
file(GLOB _ndk_symbol_lists CONFIGURE_DEPENDS ${ANDROPKG_ROOT}/ndk-libs/symbols/*.txt)
add_custom_command(OUTPUT ${_ndkgen}/ndk_symbols.inc ${_ndkgen}/ndk_traps.inc
  COMMAND ${Python3_EXECUTABLE} ${ANDROPKG_TOOLS}/gen_ndk_symbols.py ${ANDROPKG_ROOT}/ndk-libs ${_ndkgen}
  DEPENDS ${ANDROPKG_TOOLS}/gen_ndk_symbols.py ${ANDROPKG_ROOT}/ndk-libs/direct.txt ${_ndk_symbol_lists}
  VERBATIM)
file(GLOB _ndk_srcs CONFIGURE_DEPENDS ${ANDROPKG_ROOT}/ndk-libs/*.c)
add_library(andropkg_ndk SHARED ${_ndk_srcs}
  ${ANDROPKG_ROOT}/loader/src/elf_loader.c
  ${ANDROPKG_ROOT}/shim/src/syscall.c ${ANDROPKG_ROOT}/shim/src/tls.c ${ANDROPKG_ROOT}/shim/src/svc_entry.S
  ${ANDROPKG_ROOT}/ndk-libs/exec_entry.S ${ANDROPKG_ROOT}/ndk-libs/setjmp.S
  ${_ndkgen}/ndk_symbols.inc)
set_target_properties(andropkg_ndk PROPERTIES OUTPUT_NAME andropkg_ndk INSTALL_NAME_DIR "@rpath"
                      BUILD_RPATH "@loader_path" C_VISIBILITY_PRESET hidden)
target_include_directories(andropkg_ndk PRIVATE ${ANDROPKG_ROOT}/loader/include ${ANDROPKG_ROOT}/shim/include
                           ${ANDROPKG_ROOT}/ndk-libs ${_ndkgen} ${ANDROPKG_AOSP}/libnativehelper/include_jni)
target_compile_options(andropkg_ndk PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -Wno-unused-parameter>)
target_link_libraries(andropkg_ndk PRIVATE andropkg_art libandroid_runtime_dylib andropkg_gl z
                      "-framework AudioToolbox")  # AAudio / OpenSL ES (aaudio.c)

# ---- EGL + GLES (runtime/opengl): libandropkg_gl -----------------------------------------------
# EGL 1.4 (egl.m: OpenGL ES/EAGL on iOS, OpenGL 2.1 as a GLES 2.0 stand-in on macOS) for NDK code
# (libandropkg_ndk resolves bionic_egl*/bionic_gl* here) and the framework's GL JNI (EGL14, EGL10,
# GLES20/30, GL10), which libandroid_runtime registers at startup through andropkg_register_opengl.
set(_gljni ${ANDROPKG_AOSP}/frameworks/base/core/jni)
add_library(andropkg_gljni OBJECT ${_gljni}/android_opengl_EGL14.cpp ${_gljni}/android_opengl_EGLExt.cpp
  ${_gljni}/android_opengl_GLES20.cpp ${_gljni}/android_opengl_GLES30.cpp
  ${_gljni}/com_google_android_gles_jni_EGLImpl.cpp ${_gljni}/com_google_android_gles_jni_GLImpl.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/opengl/register_opengl.cc)
target_include_directories(andropkg_gljni BEFORE PRIVATE ${ANDROPKG_AOSP}/frameworks/native/opengl/include)
target_include_directories(andropkg_gljni PRIVATE $<TARGET_PROPERTY:libandroid_runtime,INCLUDE_DIRECTORIES>)
target_compile_options(andropkg_gljni PRIVATE $<TARGET_PROPERTY:libandroid_runtime,COMPILE_OPTIONS>
  "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/opengl/gl_redirect.h" -DGL_GLEXT_PROTOTYPES -DEGL_EGLEXT_PROTOTYPES
  -Wno-unused-variable -Wno-unused-but-set-variable)
set_target_properties(andropkg_gljni PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(andropkg_gl SHARED $<TARGET_OBJECTS:andropkg_gljni> ${CMAKE_CURRENT_SOURCE_DIR}/opengl/egl.m)
set_target_properties(andropkg_gl PROPERTIES OUTPUT_NAME andropkg_gl INSTALL_NAME_DIR "@rpath"
                      BUILD_RPATH "@loader_path" LINKER_LANGUAGE CXX)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/opengl/egl.m PROPERTIES
                            COMPILE_OPTIONS "-fno-objc-arc;-Wno-unused-parameter;-fvisibility=hidden")
target_include_directories(andropkg_gl PRIVATE ${ANDROPKG_AOSP}/frameworks/native/opengl/include
                           ${ANDROPKG_AOSP}/system/logging/liblog/include)
target_link_libraries(andropkg_gl PRIVATE andropkg_art libandroid_runtime_dylib "-framework Foundation")
# Entry points the framework JNI names but the platform lacks (GLES 1.x extensions; on macOS,
# GLES 1.x/3.0 beyond OpenGL 2.1): logging stubs, never reached by the contexts offered.
if(ANDROPKG_PLATFORM STREQUAL "ios")
  set(_gl_fw OpenGLES)
  set(_gl_tbds ${CMAKE_OSX_SYSROOT}/System/Library/Frameworks/OpenGLES.framework/OpenGLES.tbd)
else()
  set(_gl_fw OpenGL)
  set(_gl_tbd ${CMAKE_OSX_SYSROOT}/System/Library/Frameworks/OpenGL.framework/Versions/A)
  set(_gl_tbds ${_gl_tbd}/Libraries/libGL.tbd ${_gl_tbd}/OpenGL.tbd)
endif()
get_target_property(_gljni_srcs andropkg_gljni SOURCES)
set(_gl_stubs ${CMAKE_BINARY_DIR}/gen/opengl/gl_stubs.c)
add_custom_command(OUTPUT ${_gl_stubs}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/gen/opengl
  COMMAND ${Python3_EXECUTABLE} ${ANDROPKG_TOOLS}/gen_gl_stubs.py ${_gl_stubs}
          ${CMAKE_CURRENT_SOURCE_DIR}/opengl/gl_redirect.h ${ANDROPKG_PLATFORM} ${_gl_tbds} -- ${_gljni_srcs}
  DEPENDS ${ANDROPKG_TOOLS}/gen_gl_stubs.py ${CMAKE_CURRENT_SOURCE_DIR}/opengl/gl_redirect.h ${_gljni_srcs}
  VERBATIM)
target_sources(andropkg_gl PRIVATE ${_gl_stubs})
target_include_directories(andropkg_gl PRIVATE ${ANDROPKG_AOSP}/system/logging/liblog/include)
target_link_libraries(andropkg_gl PRIVATE "-framework ${_gl_fw}")

# dalvikvm: AOSP's command-line launcher (macOS dev loop; iOS embeds the VM in the app).
if(ANDROPKG_PLATFORM STREQUAL "macos")
  add_executable(dalvikvm ${ANDROPKG_AOSP}/art/dalvikvm/dalvikvm.cc ${CMAKE_CURRENT_SOURCE_DIR}/macos/dalvikvm_runloop.cc)
  # dalvikvm.cc's main() becomes the VM thread's entry; ours keeps the main run loop going.
  set_source_files_properties(${ANDROPKG_AOSP}/art/dalvikvm/dalvikvm.cc PROPERTIES
                              COMPILE_DEFINITIONS "main=andropkg_dalvikvm_main_cxx")
  target_include_directories(dalvikvm PRIVATE ${ANDROPKG_INCLUDES_libart} ${ANDROPKG_AOSP}/libnativehelper/include_platform ${ANDROPKG_AOSP}/libnativehelper/header_only_include ${ANDROPKG_AOSP}/libnativehelper/include)
  target_compile_options(dalvikvm PRIVATE -std=gnu++17 -fno-rtti ${ANDROPKG_CXXFLAGS_libart})
  target_link_libraries(dalvikvm PRIVATE andropkg_art "-framework CoreFoundation")
  set_target_properties(dalvikvm PROPERTIES BUILD_RPATH "@loader_path")
  add_dependencies(dalvikvm libjavacore_dylib libopenjdk_dylib libicu_jni_dylib libmedia_jni_dylib libsoundpool_dylib andropkg_ndk andropkg_gl)
endif()
