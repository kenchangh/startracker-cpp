# SPDX-License-Identifier: Apache-2.0
add_library(st_core STATIC
  core/assert.cpp
  core/status.cpp
  core/math/linalg.cpp
  core/camera/camera.cpp
  core/db/format.cpp
  core/db/database.cpp
  core/solver/lost_in_space.cpp
  core/centroid/centroider.cpp
)
add_library(st::core ALIAS st_core)
target_include_directories(st_core PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_compile_features(st_core PUBLIC cxx_std_20)
if(ST_ASSERTS STREQUAL "ON")
  target_compile_definitions(st_core PUBLIC ST_ENABLE_ASSERTS=1)
elseif(ST_ASSERTS STREQUAL "OFF")
  target_compile_definitions(st_core PUBLIC ST_ENABLE_ASSERTS=0)
endif()
st_apply_core_options(st_core)

# Platform layer (POSIX/Linux): mmap, V4L2, threads affinity. Same strict rules as the core.
add_library(st_platform STATIC
  platform/mapped_file.cpp
)
add_library(st::platform ALIAS st_platform)
target_link_libraries(st_platform PUBLIC st_core)
st_apply_core_options(st_platform)
