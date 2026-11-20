# andropkg runtime: AOSP ART + libcore natives, built for Darwin (macOS dev loop, iOS device).
#   cmake -S runtime -B build/runtime-macos -G Ninja
#   cmake -S runtime -B build/runtime-ios -G Ninja -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64
cmake_minimum_required(VERSION 3.24)
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  # os_sync_wait_on_address (futex emulation) needs iOS 17.4 / macOS 14.4.
  set(CMAKE_OSX_DEPLOYMENT_TARGET 14.4 CACHE STRING "")
endif()
project(andropkg_runtime C CXX ASM)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(ANDROPKG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(ANDROPKG_AOSP ${ANDROPKG_ROOT}/third_party/aosp CACHE PATH "AOSP sources (scripts/fetch-aosp.sh)")
set(ANDROPKG_TOOLS ${ANDROPKG_ROOT}/tools)
set(ANDROPKG_GEN_DIR ${CMAKE_BINARY_DIR}/gen)
option(ANDROPKG_ART_DEBUG "Build ART with debug checks (kIsDebugBuild)" ON)
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
andropkg_jni_dylib(libandroid_runtime libandroid_runtime libandroidfw libincfs-utils libutils libcutils
    libhwui libskia libminikin libharfbuzz_ng libft2 libpng libjpeg libwebp-decode libwebp-encode libwuffs
    libsfntly libultrahdr libjpegencoder libjpegdecoder libimage_io libmodpb64 libexpat)
# Skia's Apple ports (CoreText fonts, CG/ImageIO codecs) are part of the build.
if(ANDROPKG_PLATFORM STREQUAL "ios")
  set(_uti_fw "-framework MobileCoreServices")
else()
  set(_uti_fw "-framework CoreServices")
endif()
target_link_libraries(libandroid_runtime_dylib PRIVATE "-framework CoreFoundation" "-framework CoreGraphics"
                      "-framework CoreText" "-framework ImageIO" ${_uti_fw})

# dalvikvm: AOSP's command-line launcher (macOS dev loop; iOS embeds the VM in the app).
if(ANDROPKG_PLATFORM STREQUAL "macos")
  add_executable(dalvikvm ${ANDROPKG_AOSP}/art/dalvikvm/dalvikvm.cc)
  target_include_directories(dalvikvm PRIVATE ${ANDROPKG_INCLUDES_libart} ${ANDROPKG_AOSP}/libnativehelper/include_platform ${ANDROPKG_AOSP}/libnativehelper/header_only_include ${ANDROPKG_AOSP}/libnativehelper/include)
  target_compile_options(dalvikvm PRIVATE -std=gnu++17 -fno-rtti ${ANDROPKG_CXXFLAGS_libart})
  target_link_libraries(dalvikvm PRIVATE andropkg_art)
  set_target_properties(dalvikvm PROPERTIES BUILD_RPATH "@loader_path")
  add_dependencies(dalvikvm libjavacore_dylib libopenjdk_dylib libicu_jni_dylib)
endif()
