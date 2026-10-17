#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Configure, build and test one CMake preset with one compiler.
# Usage: tools/ci.sh <preset> [compiler]   compiler: gcc-11, gcc-14, clang-14, clang-18, ...
set -euo pipefail
preset=${1:?preset}
compiler=${2:-}
if [[ -n "$compiler" ]]; then
  case "$compiler" in
    gcc-*)   export CC="gcc-${compiler#gcc-}" CXX="g++-${compiler#gcc-}" ;;
    clang-*) export CC="clang-${compiler#clang-}" CXX="clang++-${compiler#clang-}" ;;
    *) echo "unknown compiler '$compiler'" >&2; exit 2 ;;
  esac
fi
echo "== preset=$preset CXX=${CXX:-default}"
"${CXX:-c++}" --version | head -1
rm -rf "build/$preset"  # fresh configure (cmake --fresh needs 3.24; jammy has 3.22)
cmake --preset "$preset"
cmake --build --preset "$preset"
ctest --preset "$preset" -j "$(getconf _NPROCESSORS_ONLN)"
