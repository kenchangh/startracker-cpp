// SPDX-License-Identifier: Apache-2.0
#pragma once

// Read-only memory-mapped file (POSIX). Part of st_platform, not st_core: the core depends only
// on the C++ standard library. No heap use.

#include <cstddef>
#include <span>

#include "st/core/result.hpp"

namespace st {

class MappedFile {
 public:
  MappedFile() noexcept = default;
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  MappedFile(MappedFile&& o) noexcept;
  MappedFile& operator=(MappedFile&& o) noexcept;
  ~MappedFile();

  // Maps the whole file read-only. Errors: kIoError. O(1) (pages load on demand), plus an
  // optional MAP_POPULATE-style prefault when `prefault` is true: O(file size).
  static Result<MappedFile> open(const char* path, bool prefault = false) noexcept;

  [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return {data_, size_}; }

  // Locks the pages in RAM (mlock) so lookups never page-fault. Errors: kIoError.
  Status lock() noexcept;

 private:
  const std::byte* data_ = nullptr;
  std::size_t size_ = 0;
};

}  // namespace st
