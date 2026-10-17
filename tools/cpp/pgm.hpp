// SPDX-License-Identifier: Apache-2.0
#pragma once
// Binary PGM (P5) read/write, 8 or 16 bit (host tool only). 16-bit samples are big-endian on
// disk (PGM spec) and host order in memory.
#include <cstdint>
#include <string>
#include <vector>

#include "st/centroid/centroider.hpp"

namespace st::tools {

struct Image16 {
  std::uint32_t width = 0, height = 0;
  std::uint32_t maxval = 65535;       // 255 for 8-bit files
  std::vector<std::uint16_t> pixels;  // row-major
  ImageView view() const {
    return ImageView{pixels.data(), width, height, 0, PixelFormat::kMono16};
  }
};

Image16 read_pgm(const std::string& path);
void write_pgm(const std::string& path, const Image16& img);

}  // namespace st::tools
