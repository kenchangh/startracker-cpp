# SPDX-License-Identifier: Apache-2.0
# Host tools: may use the heap, exceptions and RTTI. Never linked into st_core.
add_library(st_tools STATIC
  cpp/npy.cpp
  cpp/db_writer.cpp
  cpp/generator.cpp
  cpp/pgm.cpp
  cpp/render.cpp
)
target_include_directories(st_tools PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cpp)
target_link_libraries(st_tools PUBLIC st_core)
st_apply_test_options(st_tools)

add_executable(st_dbtool cpp/dbtool_main.cpp)
target_link_libraries(st_dbtool PRIVATE st_tools)
st_apply_test_options(st_dbtool)

add_executable(st_solve_cases cpp/solve_cases_main.cpp)
target_link_libraries(st_solve_cases PRIVATE st_tools st_platform)
st_apply_test_options(st_solve_cases)

add_executable(st_centroid cpp/centroid_main.cpp)
target_link_libraries(st_centroid PRIVATE st_tools)
st_apply_test_options(st_centroid)

add_executable(st_render cpp/render_main.cpp)
target_link_libraries(st_render PRIVATE st_tools st_platform)
st_apply_test_options(st_render)

add_executable(st_sweep cpp/sweep_main.cpp)
target_link_libraries(st_sweep PRIVATE st_tools st_platform)
st_apply_test_options(st_sweep)
