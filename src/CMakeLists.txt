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
  core/track/sky_index.cpp
  core/track/tracker.cpp
  core/pipeline/pipeline.cpp
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
  platform/replay_source.cpp
  platform/v4l2_source.cpp
  platform/pipeline_runner.cpp
)
add_library(st::platform ALIAS st_platform)
target_link_libraries(st_platform PUBLIC st_core)
st_apply_core_options(st_platform)
find_package(Threads REQUIRED)
target_link_libraries(st_platform PUBLIC Threads::Threads)

# C API (libstartracker): a shared library over the core; only st_* symbols are exported.
set_target_properties(st_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(startracker SHARED capi/st_c.cpp)
target_link_libraries(startracker PRIVATE st_core)
target_include_directories(startracker PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_compile_definitions(startracker PRIVATE ST_VERSION_STRING="${PROJECT_VERSION}")
set_target_properties(startracker PROPERTIES
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
  VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
st_apply_core_options(startracker)
if(APPLE)
  target_link_options(startracker PRIVATE "LINKER:-exported_symbol,_st_*")
else()
  target_link_options(startracker PRIVATE "LINKER:--exclude-libs,ALL")
  if(NOT ST_SANITIZE)  # sanitizer runtimes are resolved from the executable
    target_link_options(startracker PRIVATE "LINKER:--no-undefined")
  endif()
endif()
