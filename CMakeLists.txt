cmake_minimum_required(VERSION 3.25)

find_package(cmake-napi REQUIRED PATHS node_modules/cmake-napi)
find_package(cmake-npm REQUIRED PATHS node_modules/cmake-npm)

project(yencode LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_NATIVE_TUNING "Enable -march=native for host-target native builds" ON)
option(DISABLE_AVX256 "Disable AVX256 runtime path" OFF)
option(DISABLE_CRCUTIL "Disable bundled crcutil backend" OFF)
option(BUILT_WITH_ELECTRON "Mimic node-gyp electron build behavior" OFF)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" YENC_TARGET_PROCESSOR)
string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" YENC_HOST_PROCESSOR)

set(YENC_ARCH_X86 FALSE)
set(YENC_ARCH_X86_32 FALSE)
set(YENC_ARCH_X86_64 FALSE)
set(YENC_ARCH_ARM FALSE)
set(YENC_ARCH_ARM32 FALSE)
set(YENC_ARCH_ARM64 FALSE)
set(YENC_ARCH_RISCV FALSE)
set(YENC_ARCH_RISCV32 FALSE)
set(YENC_ARCH_RISCV64 FALSE)

if(YENC_TARGET_PROCESSOR MATCHES "^(x86_64|amd64|x64)$")
  set(YENC_ARCH_X86 TRUE)
  set(YENC_ARCH_X86_64 TRUE)
elseif(YENC_TARGET_PROCESSOR MATCHES "^(x86|i[3-6]86)$")
  set(YENC_ARCH_X86 TRUE)
  set(YENC_ARCH_X86_32 TRUE)
elseif(YENC_TARGET_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(YENC_ARCH_ARM TRUE)
  set(YENC_ARCH_ARM64 TRUE)
elseif(YENC_TARGET_PROCESSOR MATCHES "^(arm|armv7|armv7l)$")
  set(YENC_ARCH_ARM TRUE)
  set(YENC_ARCH_ARM32 TRUE)
elseif(YENC_TARGET_PROCESSOR MATCHES "^(riscv64)$")
  set(YENC_ARCH_RISCV TRUE)
  set(YENC_ARCH_RISCV64 TRUE)
elseif(YENC_TARGET_PROCESSOR MATCHES "^(riscv32)$")
  set(YENC_ARCH_RISCV TRUE)
  set(YENC_ARCH_RISCV32 TRUE)
endif()

include(CheckCXXCompilerFlag)

function(yenc_all_flags_supported out_var)
  set(_all_supported TRUE)
  foreach(_flag IN ITEMS ${ARGN})
    string(REGEX REPLACE "[^A-Za-z0-9]" "_" _safe "${_flag}")
    set(_var "YENC_FLAG_${_safe}")
    check_cxx_compiler_flag("${_flag}" ${_var})
    if(NOT ${_var})
      set(_all_supported FALSE)
    endif()
  endforeach()
  set(${out_var} ${_all_supported} PARENT_SCOPE)
endfunction()

add_library(yencode_build_flags INTERFACE)

if(ANDROID)
  file(GLOB YENC_NDK_PREBUILT_DIRS "${CMAKE_ANDROID_NDK}/toolchains/llvm/prebuilt/*")
  list(LENGTH YENC_NDK_PREBUILT_DIRS YENC_NDK_PREBUILT_COUNT)
  if(YENC_NDK_PREBUILT_COUNT GREATER 0)
    list(GET YENC_NDK_PREBUILT_DIRS 0 YENC_NDK_PREBUILT_DIR)
    set(YENC_ANDROID_LIBCXX_INCLUDE "${YENC_NDK_PREBUILT_DIR}/sysroot/usr/include/c++/v1")
    if(EXISTS "${YENC_ANDROID_LIBCXX_INCLUDE}/memory")
      # Some Android toolchains/build wrappers pass -nostdinc++, so provide libc++ headers explicitly.
      target_include_directories(yencode_build_flags SYSTEM INTERFACE "${YENC_ANDROID_LIBCXX_INCLUDE}")
    endif()
  endif()
endif()

if(MSVC)
  target_compile_options(yencode_build_flags INTERFACE /O2)
else()
  target_compile_options(yencode_build_flags INTERFACE -Wno-unused-function)
  target_compile_options(yencode_build_flags INTERFACE $<$<CONFIG:Release>:-fomit-frame-pointer>)
endif()

if(DISABLE_AVX256)
  target_compile_definitions(yencode_build_flags INTERFACE YENC_DISABLE_AVX256=1)
endif()
if(DISABLE_CRCUTIL)
  target_compile_definitions(yencode_build_flags INTERFACE YENC_DISABLE_CRCUTIL=1)
endif()

if(MSVC AND YENC_ARCH_X86_32)
  target_compile_options(yencode_build_flags INTERFACE /arch:SSE2)
endif()

if(NOT WIN32)
  # Avoid false negatives in cross-targeted CI probe builds.
  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
  try_compile(
    YENC_HAS_ALIGN_ALLOC
    "${CMAKE_BINARY_DIR}/cmake-check-alignalloc"
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/test_alignalloc.cc"
  )
  if(NOT YENC_HAS_ALIGN_ALLOC AND NOT APPLE)
    target_compile_definitions(yencode_build_flags INTERFACE _POSIX_C_SOURCE=200112L)
  endif()
endif()

if(
  NOT WIN32
  AND ENABLE_NATIVE_TUNING
  AND NOT CMAKE_CROSSCOMPILING
  AND YENC_TARGET_PROCESSOR STREQUAL YENC_HOST_PROCESSOR
  AND (NOT BUILT_WITH_ELECTRON OR NOT YENC_HOST_PROCESSOR STREQUAL "x64")
)
  yenc_all_flags_supported(YENC_HAS_NATIVE -march=native)
  if(YENC_HAS_NATIVE)
    target_compile_options(yencode_build_flags INTERFACE -march=native)
    target_compile_definitions(yencode_build_flags INTERFACE YENC_BUILD_NATIVE=1)
  endif()
elseif(NOT WIN32 AND YENC_ARCH_X86 AND NOT ENABLE_NATIVE_TUNING)
  yenc_all_flags_supported(
    YENC_HAS_AVX2_NOSPLIT
    -mavx2
    -mno-avx256-split-unaligned-load
    -mno-avx256-split-unaligned-store
  )
  if(YENC_HAS_AVX2_NOSPLIT)
    target_compile_options(
      yencode_build_flags
      INTERFACE
      -mno-avx256-split-unaligned-load
      -mno-avx256-split-unaligned-store
    )
  endif()
endif()

function(yenc_add_object_target name)
  add_library(${name} OBJECT ${ARGN})
  target_link_libraries(${name} PRIVATE yencode_build_flags)
  if(MSVC)
    target_compile_options(${name} PRIVATE /GS-)
  endif()
endfunction()

add_napi_module(yencode_module)

target_sources(
  ${yencode_module}
  PRIVATE
    src/yencode.cc
    src/platform.cc
    src/encoder.cc
    src/decoder.cc
    src/crc.cc
)

target_link_libraries(${yencode_module} PRIVATE yencode_build_flags)
if(MSVC)
  target_compile_options(${yencode_module} PRIVATE /GS- /O2)
endif()

set_target_properties(
  ${yencode_module}
  PROPERTIES
    PREFIX ""
    OUTPUT_NAME "yencode"
)

napi_target(YENC_NAPI_HOST)
if(YENC_NAPI_HOST MATCHES "ios")
  napi_module_target("." YENC_IOS_MODULE_TARGET NAME YENC_IOS_MODULE_NAME VERSION YENC_IOS_MODULE_VERSION)
  string(REGEX MATCH "^[0-9]+" YENC_IOS_MODULE_MAJOR "${YENC_IOS_MODULE_VERSION}")

  add_library(${yencode_module}_module SHARED)
  set_target_properties(
    ${yencode_module}_module
    PROPERTIES
      OUTPUT_NAME ${YENC_IOS_MODULE_NAME}
      PREFIX ""
      SUFFIX ".node"
      IMPORT_PREFIX ""
      IMPORT_SUFFIX ".node.lib"
      NO_SONAME ON
      MACHO_CURRENT_VERSION ${YENC_IOS_MODULE_VERSION}
      MACHO_COMPATIBILITY_VERSION ${YENC_IOS_MODULE_MAJOR}
      WINDOWS_EXPORT_ALL_SYMBOLS ON
  )

  target_link_libraries(${yencode_module}_module PRIVATE ${yencode_module})
  target_link_options(${yencode_module}_module PRIVATE -Wl,-undefined,dynamic_lookup)

  install(
    FILES $<TARGET_FILE:${yencode_module}_module>
    DESTINATION ${YENC_NAPI_HOST}
    RENAME ${YENC_IOS_MODULE_NAME}.node
  )
endif()

function(yenc_link_dependency dep)
  target_link_libraries(${yencode_module} PRIVATE ${dep})
  if(TARGET ${yencode_module}_module)
    target_link_libraries(${yencode_module}_module PRIVATE ${dep})
  endif()
endfunction()

if(YENC_ARCH_X86)
  yenc_add_object_target(yencode_sse2_obj src/encoder_sse2.cc src/decoder_sse2.cc)
  if(NOT MSVC)
    target_compile_options(yencode_sse2_obj PRIVATE -msse2)
  endif()
  yenc_link_dependency(yencode_sse2_obj)

  yenc_add_object_target(yencode_ssse3_obj src/encoder_ssse3.cc src/decoder_ssse3.cc)
  if(NOT MSVC)
    target_compile_options(yencode_ssse3_obj PRIVATE -mssse3)
  endif()
  yenc_link_dependency(yencode_ssse3_obj)

  yenc_add_object_target(yencode_clmul_obj src/crc_folding.cc)
  if(NOT MSVC)
    target_compile_options(yencode_clmul_obj PRIVATE -mssse3 -msse4.1 -mpclmul)
  endif()
  yenc_link_dependency(yencode_clmul_obj)

  yenc_add_object_target(yencode_avx_obj src/encoder_avx.cc src/decoder_avx.cc)
  if(MSVC)
    target_compile_options(yencode_avx_obj PRIVATE /arch:AVX)
  else()
    target_compile_options(yencode_avx_obj PRIVATE -mavx -mpopcnt)
  endif()
  yenc_link_dependency(yencode_avx_obj)

  yenc_add_object_target(yencode_avx2_obj src/decoder_avx2.cc src/encoder_avx2.cc)
  if(MSVC)
    target_compile_options(yencode_avx2_obj PRIVATE /arch:AVX2)
    yenc_link_dependency(yencode_avx2_obj)
  else()
    yenc_all_flags_supported(YENC_HAS_AVX2 -mavx2 -mpopcnt -mbmi -mbmi2 -mlzcnt)
    if(YENC_HAS_AVX2)
      target_compile_options(yencode_avx2_obj PRIVATE -mavx2 -mpopcnt -mbmi -mbmi2 -mlzcnt)
      yenc_link_dependency(yencode_avx2_obj)
    endif()
  endif()

  yenc_add_object_target(yencode_clmul256_obj src/crc_folding_256.cc)
  if(MSVC)
    target_compile_options(yencode_clmul256_obj PRIVATE /arch:AVX2)
    yenc_link_dependency(yencode_clmul256_obj)
  else()
    yenc_all_flags_supported(YENC_HAS_VPCLMUL -mavx2 -mvpclmulqdq -mpclmul)
    if(YENC_HAS_VPCLMUL)
      target_compile_options(yencode_clmul256_obj PRIVATE -mavx2 -mvpclmulqdq -mpclmul)
      yenc_link_dependency(yencode_clmul256_obj)
    endif()
  endif()

  yenc_add_object_target(yencode_vbmi2_obj src/decoder_vbmi2.cc src/encoder_vbmi2.cc)
  if(MSVC)
    target_compile_options(yencode_vbmi2_obj PRIVATE /arch:AVX512)
    yenc_link_dependency(yencode_vbmi2_obj)
  else()
    yenc_all_flags_supported(
      YENC_HAS_VBMI2
      -mavx512vbmi2
      -mavx512vl
      -mavx512bw
      -mpopcnt
      -mbmi
      -mbmi2
      -mlzcnt
    )
    if(YENC_HAS_VBMI2)
      target_compile_options(
        yencode_vbmi2_obj
        PRIVATE
        -mavx512vbmi2
        -mavx512vl
        -mavx512bw
        -mpopcnt
        -mbmi
        -mbmi2
        -mlzcnt
      )
      yenc_all_flags_supported(YENC_HAS_AVX10 -mavx512vl -mno-evex512)
      if(YENC_HAS_AVX10)
        target_compile_options(yencode_vbmi2_obj PRIVATE -mno-evex512)
      endif()
      yenc_link_dependency(yencode_vbmi2_obj)
    endif()
  endif()

  if(NOT DISABLE_CRCUTIL)
    add_library(
      yencode_crcutil
      STATIC
        crcutil-1.0/code/crc32c_sse4.cc
        crcutil-1.0/code/multiword_64_64_cl_i386_mmx.cc
        crcutil-1.0/code/multiword_64_64_gcc_amd64_asm.cc
        crcutil-1.0/code/multiword_64_64_gcc_i386_mmx.cc
        crcutil-1.0/code/multiword_64_64_intrinsic_i386_mmx.cc
        crcutil-1.0/code/multiword_128_64_gcc_amd64_sse2.cc
        crcutil-1.0/examples/interface.cc
    )

    target_include_directories(yencode_crcutil PUBLIC crcutil-1.0/code crcutil-1.0/tests)
    target_compile_definitions(yencode_crcutil PUBLIC CRCUTIL_USE_MM_CRC32=0)
    target_link_libraries(yencode_crcutil PRIVATE yencode_build_flags)

    if(MSVC)
      target_compile_options(yencode_crcutil PRIVATE /GS- /O2)
    else()
      yenc_all_flags_supported(YENC_HAS_WNO_EXP_TO_DEFINED -Wno-expansion-to-defined)
      if(YENC_HAS_WNO_EXP_TO_DEFINED)
        target_compile_options(yencode_crcutil PRIVATE -Wno-expansion-to-defined)
      endif()
    endif()

    target_include_directories(${yencode_module} PRIVATE crcutil-1.0/code crcutil-1.0/examples)
    yenc_link_dependency(yencode_crcutil)
  endif()
endif()

if(YENC_ARCH_ARM)
  yenc_add_object_target(yencode_neon_obj src/encoder_neon.cc)
  if(YENC_ARCH_ARM64)
    target_sources(yencode_neon_obj PRIVATE src/decoder_neon64.cc)
  else()
    target_sources(yencode_neon_obj PRIVATE src/decoder_neon.cc)
  endif()

  if(NOT MSVC AND YENC_ARCH_ARM32)
    target_compile_options(yencode_neon_obj PRIVATE -mfpu=neon -fno-lto)
  endif()

  yenc_link_dependency(yencode_neon_obj)

  yenc_add_object_target(yencode_armcrc_obj src/crc_arm.cc)
  if(NOT MSVC)
    target_compile_options(yencode_armcrc_obj PRIVATE -march=armv8-a+crc)
    if(YENC_ARCH_ARM32)
      target_compile_options(yencode_armcrc_obj PRIVATE -mfpu=fp-armv8 -fno-lto)
    endif()
  endif()
  yenc_link_dependency(yencode_armcrc_obj)

  yenc_add_object_target(yencode_pmull_obj src/crc_arm_pmull.cc)
  if(NOT MSVC)
    target_compile_options(yencode_pmull_obj PRIVATE -march=armv8-a+crc+crypto)
    if(YENC_ARCH_ARM32)
      target_compile_options(yencode_pmull_obj PRIVATE -mfpu=neon -fno-lto)
    endif()
  endif()
  yenc_link_dependency(yencode_pmull_obj)
endif()

if(YENC_ARCH_RISCV)
  yenc_add_object_target(yencode_rvv_obj src/encoder_rvv.cc src/decoder_rvv.cc)
  if(NOT WIN32)
    if(YENC_ARCH_RISCV64)
      yenc_all_flags_supported(YENC_HAS_RVV -march=rv64gcv)
      if(YENC_HAS_RVV)
        target_compile_options(yencode_rvv_obj PRIVATE -march=rv64gcv)
        yenc_link_dependency(yencode_rvv_obj)
      endif()
    elseif(YENC_ARCH_RISCV32)
      yenc_all_flags_supported(YENC_HAS_RVV -march=rv32gcv)
      if(YENC_HAS_RVV)
        target_compile_options(yencode_rvv_obj PRIVATE -march=rv32gcv)
        yenc_link_dependency(yencode_rvv_obj)
      endif()
    endif()
  endif()

  yenc_add_object_target(yencode_zbkc_obj src/crc_riscv.cc)
  if(NOT WIN32)
    if(YENC_ARCH_RISCV64)
      yenc_all_flags_supported(YENC_HAS_ZBKC -march=rv64gc_zbkc)
      if(YENC_HAS_ZBKC)
        target_compile_options(yencode_zbkc_obj PRIVATE -march=rv64gc_zbkc)
        yenc_link_dependency(yencode_zbkc_obj)
      endif()
    elseif(YENC_ARCH_RISCV32)
      yenc_all_flags_supported(YENC_HAS_ZBKC -march=rv32gc_zbkc)
      if(YENC_HAS_ZBKC)
        target_compile_options(yencode_zbkc_obj PRIVATE -march=rv32gc_zbkc)
        yenc_link_dependency(yencode_zbkc_obj)
      endif()
    endif()
  endif()
endif()
