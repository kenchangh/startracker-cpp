# SPDX-License-Identifier: Apache-2.0
include(FetchContent)

# GoogleTest v1.15.2, pinned by commit. Tests only; never linked into st_core.
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG b514bdc898e2951020cbdca1304b75f5950d1f59)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

# ---- Allocation guard (object library so its malloc/new definitions are always linked) ----
add_library(st_alloc_guard OBJECT noalloc/alloc_guard.cpp)
target_include_directories(st_alloc_guard PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/noalloc)
target_compile_features(st_alloc_guard PUBLIC cxx_std_20)
st_apply_test_options(st_alloc_guard)

add_executable(st_noalloc_tests noalloc/alloc_guard_test.cpp)
target_link_libraries(st_noalloc_tests PRIVATE st_alloc_guard GTest::gtest_main)
st_apply_test_options(st_noalloc_tests)
gtest_discover_tests(st_noalloc_tests DISCOVERY_MODE PRE_TEST)

# ---- Unit tests ----
add_executable(st_unit_tests
  unit/arena_test.cpp
  unit/assert_test.cpp
  unit/bitset_test.cpp
  unit/clock_test.cpp
  unit/fixed_vector_test.cpp
  unit/result_test.cpp
  unit/ring_buffer_test.cpp
  unit/span_vector_test.cpp
  unit/spsc_queue_test.cpp
  unit/status_test.cpp
  unit/math_test.cpp
  unit/camera_test.cpp
  unit/db_test.cpp
  unit/generator_test.cpp
  unit/esa_db_test.cpp
  unit/solver_test.cpp
)
target_include_directories(st_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(st_unit_tests PRIVATE ST_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/fixtures")
target_link_libraries(st_unit_tests PRIVATE st_core st_platform st_tools st_alloc_guard GTest::gtest_main)
st_apply_test_options(st_unit_tests)
gtest_discover_tests(st_unit_tests DISCOVERY_MODE PRE_TEST)

# ---- Symbol audit of st_core (+ a negative control that must be caught) ----
set(_audit_args -DNM=${CMAKE_NM} -DALLOWLIST=${PROJECT_SOURCE_DIR}/cmake/symbol_audit_allowlist.txt)
if(APPLE)
  list(APPEND _audit_args -DSTRIP_UNDERSCORE=ON)
endif()
foreach(lib st_core st_platform)
  add_test(NAME symbol_audit.${lib}
    COMMAND ${CMAKE_COMMAND} ${_audit_args} -DLIB=$<TARGET_FILE:${lib}>
            -P ${PROJECT_SOURCE_DIR}/cmake/symbol_audit.cmake)
endforeach()

add_library(st_audit_negative_control STATIC symbol_audit/negative_control.cpp)
target_compile_options(st_audit_negative_control PRIVATE ${ST_STRICT_FLAGS})
add_test(NAME symbol_audit.negative_control
  COMMAND ${CMAKE_COMMAND} ${_audit_args} -DLIB=$<TARGET_FILE:st_audit_negative_control>
          -P ${PROJECT_SOURCE_DIR}/cmake/symbol_audit.cmake)
set_tests_properties(symbol_audit.negative_control PROPERTIES
  PASS_REGULAR_EXPRESSION "forbidden symbol: _Znwm|forbidden symbol: _Znwj")

# ---- Strict-header check: each public header alone in a TU, strict flags, -Werror ----
file(GLOB_RECURSE _st_headers CONFIGURE_DEPENDS
     RELATIVE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/include/st/*.hpp)
set(_strict_srcs "")
foreach(h IN LISTS _st_headers)
  string(MAKE_C_IDENTIFIER "${h}" id)
  set(src ${CMAKE_CURRENT_BINARY_DIR}/strict_headers/${id}.cpp)
  file(CONFIGURE OUTPUT ${src} CONTENT "#include <${h}>\n#include <${h}>  // include-guard check\n")
  list(APPEND _strict_srcs ${src})
endforeach()
add_library(st_strict_headers OBJECT ${_strict_srcs})
target_link_libraries(st_strict_headers PRIVATE st_core)
target_compile_options(st_strict_headers PRIVATE ${ST_CORE_WARNINGS} ${ST_STRICT_FLAGS} -Werror)

# ---- Compile-fail tests: each file must fail to compile with the error in its header ----
file(GLOB _cf_srcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/*.cpp)
foreach(src IN LISTS _cf_srcs)
  get_filename_component(name ${src} NAME_WE)
  file(STRINGS ${src} expect REGEX "^// EXPECT-ERROR: ")
  string(REGEX REPLACE "^// EXPECT-ERROR: " "" expect "${expect}")
  if(NOT expect)
    message(FATAL_ERROR "${src}: missing '// EXPECT-ERROR: <regex>' line")
  endif()
  add_library(cf_${name} OBJECT EXCLUDE_FROM_ALL ${src})
  target_link_libraries(cf_${name} PRIVATE st_core)
  target_compile_options(cf_${name} PRIVATE ${ST_STRICT_FLAGS} -Werror)
  add_test(NAME compile_fail.${name}
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target cf_${name} --config $<CONFIG>)
  set_tests_properties(compile_fail.${name} PROPERTIES
    PASS_REGULAR_EXPRESSION "${expect}"
    RESOURCE_LOCK st_build_dir)
endforeach()
