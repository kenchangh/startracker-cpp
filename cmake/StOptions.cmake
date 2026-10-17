# SPDX-License-Identifier: Apache-2.0
# Project-wide compile settings and helpers.

# Same bits on x86-64 and ARM64: no FMA contraction (GCC on aarch64 contracts by default,
# Clang contracts within statements by default), and never fast-math.
add_compile_options(-ffp-contract=off)

if(ST_SANITIZE)
  add_compile_options(-fsanitize=${ST_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${ST_SANITIZE})
  if(ST_SANITIZE MATCHES "undefined")
    add_compile_options(-fno-sanitize-recover=undefined)
  endif()
endif()

set(ST_CORE_WARNINGS
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wdouble-promotion
    -Wold-style-cast -Wcast-align -Wnull-dereference -Wnon-virtual-dtor)

set(ST_STRICT_FLAGS -fno-exceptions -fno-rtti)

# Warnings and strict flags for core code (the library and the strict-header check).
function(st_apply_core_options target)
  target_compile_options(${target} PRIVATE ${ST_CORE_WARNINGS})
  if(ST_STRICT)
    target_compile_options(${target} PRIVATE ${ST_STRICT_FLAGS})
  endif()
  if(ST_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()

# Lighter warnings for tests and tools (third-party headers are not ours to fix).
function(st_apply_test_options target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  if(ST_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()
