// SPDX-License-Identifier: Apache-2.0
#include "npy.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>

#include "st/db/format.hpp"

namespace st::tools {

std::size_t NpyArray::count() const {
  std::size_t n = 1;
  for (auto s : shape) n *= s;
  return n;
}
std::size_t NpyArray::item_size() const {
  return static_cast<std::size_t>(std::stoi(dtype.substr(2)));
}

double NpyArray::as_double(std::size_t i) const {
  const std::uint8_t* p = data.data() + i * item_size();
  const char kind = dtype[1];
  const std::size_t sz = item_size();
  if (kind == 'f') {
    if (sz == 4) {
      float v;
      std::memcpy(&v, p, 4);
      return v;
    }
    if (sz == 8) {
      double v;
      std::memcpy(&v, p, 8);
      return v;
    }
    if (sz == 2) {
      std::uint16_t v;
      std::memcpy(&v, p, 2);
      return st::db::half_to_float(v);
    }
  }
  return static_cast<double>(as_u64(i));
}

std::uint64_t NpyArray::as_u64(std::size_t i) const {
  const std::uint8_t* p = data.data() + i * item_size();
  const char kind = dtype[1];
  if (kind != 'u' && kind != 'i' && kind != 'b')
    throw std::runtime_error("npy: not an integer dtype");
  std::uint64_t v = 0;
  std::memcpy(&v, p, item_size());
  return v;
}

NpyArray read_npy(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("npy: cannot open " + path);
  char magic[6];
  f.read(magic, 6);
  if (std::memcmp(magic, "\x93NUMPY", 6) != 0) throw std::runtime_error("npy: bad magic " + path);
  unsigned char ver[2];
  f.read(reinterpret_cast<char*>(ver), 2);
  std::uint32_t hlen = 0;
  if (ver[0] == 1) {
    unsigned char b[2];
    f.read(reinterpret_cast<char*>(b), 2);
    hlen = b[0] | (b[1] << 8);
  } else {
    unsigned char b[4];
    f.read(reinterpret_cast<char*>(b), 4);
    hlen = b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
  }
  std::string header(hlen, '\0');
  f.read(header.data(), hlen);
  NpyArray a;
  // Header is a Python dict literal: {'descr': '<f4', 'fortran_order': False, 'shape': (3, 4), }
  auto value_after = [&](const std::string& key) -> std::string {
    const auto k = header.find("'" + key + "'");
    if (k == std::string::npos) throw std::runtime_error("npy: no " + key + " in " + path);
    auto v = header.find(':', k);
    if (v == std::string::npos) throw std::runtime_error("npy: malformed header " + path);
    ++v;
    while (v < header.size() && header[v] == ' ') ++v;
    return header.substr(v);
  };
  {
    const std::string d = value_after("descr");
    const auto q = d.find('\'', 1);
    if (d.empty() || d[0] != '\'' || q == std::string::npos)
      throw std::runtime_error("npy: bad descr");
    a.dtype = d.substr(1, q - 1);
  }
  if (a.dtype == "|u1" || a.dtype == "|i1" || a.dtype == "|b1")
    a.dtype = std::string("<") + a.dtype[1] + "1";
  if (a.dtype[0] != '<') throw std::runtime_error("npy: only little-endian supported: " + a.dtype);
  if (value_after("fortran_order").rfind("True", 0) == 0)
    throw std::runtime_error("npy: fortran order");
  {
    const std::string sh = value_after("shape");
    const auto end = sh.find(')');
    if (sh.empty() || sh[0] != '(' || end == std::string::npos)
      throw std::runtime_error("npy: bad shape");
    std::size_t i = 1;
    while (i < end) {
      if (std::isdigit(static_cast<unsigned char>(sh[i])) != 0) {
        std::size_t j = i;
        while (j < end && std::isdigit(static_cast<unsigned char>(sh[j])) != 0) ++j;
        a.shape.push_back(std::stoull(sh.substr(i, j - i)));
        i = j;
      } else {
        ++i;
      }
    }
  }
  a.data.resize(a.count() * a.item_size());
  f.read(reinterpret_cast<char*>(a.data.data()), static_cast<std::streamsize>(a.data.size()));
  if (!f) throw std::runtime_error("npy: truncated " + path);
  return a;
}

void write_npy(const std::string& path, const NpyArray& a) {
  std::string shape = "(";
  for (std::size_t i = 0; i < a.shape.size(); ++i)
    shape += std::to_string(a.shape[i]) +
             (a.shape.size() == 1 ? "," : (i + 1 < a.shape.size() ? ", " : ""));
  shape += ")";
  std::string header =
      "{'descr': '" + a.dtype + "', 'fortran_order': False, 'shape': " + shape + ", }";
  const std::size_t total = 10 + header.size() + 1;
  header.append((64 - total % 64) % 64, ' ');
  header += '\n';
  std::ofstream f(path, std::ios::binary);
  f.write("\x93NUMPY\x01\x00", 8);
  const auto hl = static_cast<std::uint16_t>(header.size());
  const char hb[2] = {static_cast<char>(hl & 0xFF), static_cast<char>(hl >> 8)};
  f.write(hb, 2);
  f.write(header.data(), static_cast<std::streamsize>(header.size()));
  f.write(reinterpret_cast<const char*>(a.data.data()),
          static_cast<std::streamsize>(a.data.size()));
  if (!f) throw std::runtime_error("npy: write failed " + path);
}

}  // namespace st::tools
