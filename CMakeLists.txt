# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.21)
project(startracker_cpp VERSION 0.1.0 LANGUAGES CXX)

option(ST_STRICT "Build the core with -fno-exceptions -fno-rtti" ON)
option(ST_WERROR "Treat warnings as errors" OFF)
set(ST_ASSERTS "AUTO" CACHE STRING "ST_ASSERT checks: ON, OFF, or AUTO (on unless NDEBUG)")
set_property(CACHE ST_ASSERTS PROPERTY STRINGS AUTO ON OFF)
set(ST_SANITIZE "" CACHE STRING "Sanitizers: empty, 'address,undefined', or 'thread'")
option(ST_BUILD_TESTS "Build tests" ON)
option(ST_BUILD_TOOLS "Build host tools" OFF)
option(ST_FUZZ "Build libFuzzer targets (Clang only)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11)
  message(FATAL_ERROR "GCC >= 11 required")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
  message(FATAL_ERROR "Clang >= 14 required")
endif()

include(cmake/StOptions.cmake)

add_subdirectory(src)

if(ST_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(ST_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "ST_FUZZ requires Clang")
  endif()
  # Fuzz targets arrive in phase 2 (database loader) and phase 4 (centroider).
endif()
