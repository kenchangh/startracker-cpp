# SPDX-License-Identifier: Apache-2.0
# Oldest supported toolchain: Ubuntu 22.04 (the JetPack 6 base) with GCC 11 and Clang 14
# against libstdc++ 11. Builds natively on x86-64 and ARM64.
#   docker build -t startracker-dev docker/
#   docker run --rm -v "$PWD":/src -w /src startracker-dev tools/ci.sh dev gcc-11
FROM ubuntu:22.04
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      g++-11 clang-14 cmake ninja-build git ca-certificates python3 \
 && rm -rf /var/lib/apt/lists/*
