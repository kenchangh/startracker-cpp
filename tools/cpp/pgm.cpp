// SPDX-License-Identifier: Apache-2.0
#include "pgm.hpp"

#include <fstream>
#include <stdexcept>

namespace st::tools {
namespace {
std::string token(std::istream& f) {
  std::string t;
  char c = 0;
  while (f.get(c)) {
    if (c == '#') {
      std::string skip;
      std::getline(f, skip);
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!t.empty()) break;
      continue;
    }
    t += c;
  }
  return t;
}
}  // namespace

Image16 read_pgm(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  if (token(f) != "P5") throw std::runtime_error("not a binary PGM: " + path);
  Image16 im;
  im.width = static_cast<std::uint32_t>(std::stoul(token(f)));
  im.height = static_cast<std::uint32_t>(std::stoul(token(f)));
  im.maxval = static_cast<std::uint32_t>(std::stoul(token(f)));
  const std::size_t n = static_cast<std::size_t>(im.width) * im.height;
  im.pixels.resize(n);
  if (im.maxval < 256) {
    std::vector<unsigned char> b(n);
    f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n));
    for (std::size_t i = 0; i < n; ++i) im.pixels[i] = b[i];
  } else {
    std::vector<unsigned char> b(2 * n);
    f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(2 * n));
    for (std::size_t i = 0; i < n; ++i)
      im.pixels[i] = static_cast<std::uint16_t>(b[2 * i] << 8 | b[2 * i + 1]);
  }
  if (!f) throw std::runtime_error("truncated PGM: " + path);
  return im;
}

void write_pgm(const std::string& path, const Image16& im) {
  std::ofstream f(path, std::ios::binary);
  f << "P5\n" << im.width << " " << im.height << "\n" << im.maxval << "\n";
  const std::size_t n = static_cast<std::size_t>(im.width) * im.height;
  if (im.maxval < 256) {
    std::vector<unsigned char> b(n);
    for (std::size_t i = 0; i < n; ++i)
      b[i] = static_cast<unsigned char>(std::min<std::uint16_t>(im.pixels[i], 255));
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(n));
  } else {
    std::vector<unsigned char> b(2 * n);
    for (std::size_t i = 0; i < n; ++i) {
      b[2 * i] = static_cast<unsigned char>(im.pixels[i] >> 8);
      b[2 * i + 1] = static_cast<unsigned char>(im.pixels[i] & 0xFF);
    }
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(2 * n));
  }
  if (!f) throw std::runtime_error("cannot write " + path);
}

}  // namespace st::tools
