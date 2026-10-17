// SPDX-License-Identifier: Apache-2.0
#include "st/track/sky_index.hpp"

#include <algorithm>
#include <cmath>

namespace st {
namespace {

// Face 0..5 = +X, -X, +Y, -Y, +Z, -Z; (u, v) in [-1, 1] on the face.
void face_uv(const Vec3& v, std::uint32_t& face, double& u, double& w) noexcept {
  const double ax = std::fabs(v.x), ay = std::fabs(v.y), az = std::fabs(v.z);
  if (ax >= ay && ax >= az) {
    face = v.x >= 0 ? 0u : 1u;
    u = v.y / ax;
    w = v.z / ax;
  } else if (ay >= az) {
    face = v.y >= 0 ? 2u : 3u;
    u = v.x / ay;
    w = v.z / ay;
  } else {
    face = v.z >= 0 ? 4u : 5u;
    u = v.x / az;
    w = v.y / az;
  }
}

Vec3 from_face(std::uint32_t face, double u, double w) noexcept {
  Vec3 v;
  switch (face) {
    case 0: v = {1, u, w}; break;
    case 1: v = {-1, u, w}; break;
    case 2: v = {u, 1, w}; break;
    case 3: v = {u, -1, w}; break;
    case 4: v = {u, w, 1}; break;
    default: v = {u, w, -1}; break;
  }
  return normalized(v);
}

}  // namespace

std::uint32_t SkyIndex::cell_of(const Vec3& v, std::uint32_t grid) noexcept {
  std::uint32_t face = 0;
  double u = 0, w = 0;
  face_uv(v, face, u, w);
  auto idx = [&](double t) {
    auto i = static_cast<long>(std::floor((t + 1.0) / 2.0 * grid));
    return static_cast<std::uint32_t>(std::clamp<long>(i, 0, static_cast<long>(grid) - 1));
  };
  return (face * grid + idx(w)) * grid + idx(u);
}

Result<SkyIndex> SkyIndex::create(const Database& db, std::uint32_t grid, Arena& arena) noexcept {
  if (grid == 0 || grid > 1024) return Status::kInvalidArgument;
  SkyIndex s;
  s.db_ = &db;
  s.grid_ = grid;
  s.ncells_ = 6 * grid * grid;
  ST_TRY_ASSIGN(s.cell_start_, arena.allocate<std::uint32_t>(std::size_t{s.ncells_} + 1));
  ST_TRY_ASSIGN(s.stars_, arena.allocate<std::uint32_t>(db.num_stars()));
  ST_TRY_ASSIGN(s.cell_center_, arena.allocate<Vec3>(s.ncells_));
  ST_TRY_ASSIGN(s.cell_radius_, arena.allocate<double>(s.ncells_));
  auto vec = [&](std::uint32_t i) {
    const auto f = db.star_vector(i);
    return Vec3{static_cast<double>(f[0]), static_cast<double>(f[1]), static_cast<double>(f[2])};
  };
  // Counting sort into cells (stable: index order within a cell).
  for (std::uint32_t i = 0; i < db.num_stars(); ++i) ++s.cell_start_[cell_of(vec(i), grid) + 1];
  for (std::uint32_t c = 0; c < s.ncells_; ++c) s.cell_start_[c + 1] += s.cell_start_[c];
  for (std::uint32_t i = 0; i < db.num_stars(); ++i) {
    const std::uint32_t c = cell_of(vec(i), grid);
    // cell_start_[c] currently points at the next free slot of cell c (shifted below).
    s.stars_[s.cell_start_[c]++] = i;
  }
  // Undo the shift: cell_start_[c] now holds the end of cell c.
  for (std::uint32_t c = s.ncells_; c > 0; --c) s.cell_start_[c] = s.cell_start_[c - 1];
  s.cell_start_[0] = 0;
  // Cell centres and bounding radii (max angle from centre to a corner).
  for (std::uint32_t face = 0; face < 6; ++face) {
    for (std::uint32_t j = 0; j < grid; ++j) {
      for (std::uint32_t i = 0; i < grid; ++i) {
        const double u0 = -1 + 2.0 * i / grid, u1 = -1 + 2.0 * (i + 1) / grid;
        const double w0 = -1 + 2.0 * j / grid, w1 = -1 + 2.0 * (j + 1) / grid;
        const std::uint32_t c = (face * grid + j) * grid + i;
        const Vec3 center = from_face(face, (u0 + u1) / 2, (w0 + w1) / 2);
        double r = 0;
        for (double u : {u0, u1}) {
          for (double w : {w0, w1})
            r = std::max(r, angle_between_unit(center, from_face(face, u, w)));
        }
        s.cell_center_[c] = center;
        s.cell_radius_[c] = r * 1.0001 + 1e-9;
      }
    }
  }
  return s;
}

}  // namespace st
