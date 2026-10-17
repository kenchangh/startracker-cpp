# SPDX-License-Identifier: Apache-2.0
# libFuzzer targets (Clang, configure with the `fuzz` preset).
add_executable(st_fuzz_db_open fuzz_db_open.cpp)
target_link_libraries(st_fuzz_db_open PRIVATE st_core)
target_link_options(st_fuzz_db_open PRIVATE -fsanitize=fuzzer)

add_executable(st_make_db_seed make_db_seed.cpp)
target_link_libraries(st_make_db_seed PRIVATE st_tools)
