// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: the centroider must handle any image content, size and configuration.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "st/centroid/centroider.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 8) return 0;
  const std::uint32_t w = 1 + data[0] % 96, h = 1 + data[1] % 96;
  const bool mono8 = (data[2] & 1) != 0;
  st::CentroiderConfig cfg;
  cfg.max_width = 96;
  cfg.max_height = 96;
  cfg.max_blobs = 1 + data[3] % 64;
  cfg.binary_open = (data[2] & 2) != 0;
  cfg.background = static_cast<st::BackgroundMode>(data[4] % 4);
  cfg.sigma_mode = static_cast<st::SigmaMode>(data[5] % 3);
  cfg.filter_size = 1 + 2 * (data[6] % 13);
  cfg.min_area = data[7] % 4;
  cfg.max_area = data[7] % 2 != 0 ? 0 : 50;
  cfg.max_axis_ratio = data[7] % 3 == 0 ? 2.0 : 0.0;
  cfg.centroid_window = data[6] % 5;
  cfg.use_hot_pixel_map = (data[2] & 4) != 0;
  static std::vector<std::byte> mem;
  mem.assign(st::Centroider::workspace_bytes(cfg), std::byte{0});
  st::Arena arena(mem);
  auto c = st::Centroider::create(cfg, arena);
  if (!c.ok()) return 0;
  if (cfg.use_hot_pixel_map) c.value().hot_pixels().set(data[3] % (96 * 96));
  const std::size_t bpp = mono8 ? 1 : 2;
  std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * bpp, 0);
  std::memcpy(px.data(), data + 8, std::min(px.size(), size - 8));
  std::vector<st::Spot> out(32);
  st::CentroidStats stats;
  static_cast<void>(c.value().extract(
      {px.data(), w, h, 0, mono8 ? st::PixelFormat::kMono8 : st::PixelFormat::kMono16}, out,
      &stats));
  const st::Roi rois[2] = {{0, 0, w, h}, {w / 2, h / 2, 16, 16}};
  st::Spot rs[2];
  bool found[2];
  static_cast<void>(c.value().extract_rois(
      {px.data(), w, h, 0, mono8 ? st::PixelFormat::kMono8 : st::PixelFormat::kMono16}, rois,
      st::RoiConfig{}, rs, found));
  return 0;
}
