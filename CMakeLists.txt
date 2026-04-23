# NOTE that this CMake file doesn't current build all of avian.
# It only builds what's required for example/kaleidoscope.

cmake_minimum_required (VERSION 3.5)
project (avian)

include_directories (include src)

if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set (AVIAN_CMAKE_TARGET_ARCH AVIAN_ARCH_ARM64)
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
  set (AVIAN_CMAKE_TARGET_ARCH AVIAN_ARCH_X86_64)
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(i[3-6]86|x86)$")
  set (AVIAN_CMAKE_TARGET_ARCH AVIAN_ARCH_X86)
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
  set (AVIAN_CMAKE_TARGET_ARCH AVIAN_ARCH_ARM)
else ()
  message (FATAL_ERROR "Unsupported target architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif ()

add_definitions (
  -DAVIAN_TARGET_FORMAT=AVIAN_FORMAT_MACHO

  -DAVIAN_TARGET_ARCH=${AVIAN_CMAKE_TARGET_ARCH}

  -DTARGET_BYTES_PER_WORD=8
  -D__STDC_LIMIT_MACROS
  -D__STDC_CONSTANT_MACROS
)

include ("cmake/Platform.cmake")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${PLATFORM_CXX_FLAGS}")

include (CTest)

# Sadly, we can't use the 'test' target, as that's coopted by ctest
add_custom_target(check ${CMAKE_CTEST_COMMAND} -V)

add_subdirectory (src)
add_subdirectory (unittest)
