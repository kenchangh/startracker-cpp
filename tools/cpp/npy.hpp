// SPDX-License-Identifier: Apache-2.0
#pragma once
// Minimal .npy reader (host tool only): little-endian numeric arrays, C order.
#include <cstdint>
#include <string>
#include <vector>

namespace st::tools {

struct NpyArray {
  std::string dtype;               // e.g. "<f4", "<u2", "|u1", "<f2", "<u4"
  std::vector<std::size_t> shape;  // C order
  std::vector<std::uint8_t> data;  // raw bytes
  std::size_t count() const;
  std::size_t item_size() const;
  // Element i converted to double / uint64 (numeric dtypes only).
  double as_double(std::size_t i) const;
  std::uint64_t as_u64(std::size_t i) const;
};

// Throws std::runtime_error on unsupported or malformed files.
NpyArray read_npy(const std::string& path);
void write_npy(const std::string& path, const NpyArray& a);

}  // namespace st::tools
