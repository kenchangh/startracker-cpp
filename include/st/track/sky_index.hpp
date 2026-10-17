// SPDX-License-Identifier: Apache-2.0
#pragma once

// Cube-map spatial index over a database's star table for cone queries. Built once from an
// Arena (CSR arrays); queries never allocate.

#include <algorithm>
#include <cstdint>
#include <span>

#include "st/core/arena.hpp"
#include "st/core/result.hpp"
#include "st/db/database.hpp"
#include "st/math/vec.hpp"

namespace st {

class SkyIndex {
 public:
  // `grid` cells per cube-face edge (6 * grid^2 cells). Memory: 4 * (6 grid^2 + 1 + stars)
  // bytes plus 32 bytes per cell. Build: O(stars + cells).
  static Result<SkyIndex> create(const Database& db, std::uint32_t grid, Arena& arena) noexcept;
  // Upper bound of the arena bytes create() takes (alignment padding included). O(1).
  static std::size_t workspace_bytes(const Database& db, std::uint32_t grid) noexcept;

  SkyIndex() noexcept = default;

  // Calls f(star_index) for every star within `radius` (radians) of unit vector `center`.
  // Order: by cell, then by star index within a cell. Worst case: O(cells + stars visited).
  template <class F>
  void query(const Vec3& center, double radius, F&& f) const noexcept {
    const double cos_r = std::cos(radius);
    for (std::uint32_t c = 0; c < ncells_; ++c) {
      // Skip cells whose bounding cap cannot reach the cone.
      const double d = dot(center, cell_center_[c]);
      if (d < std::cos(std::min(kPi, radius + cell_radius_[c]))) continue;
      for (std::uint32_t k = cell_start_[c]; k < cell_start_[c + 1]; ++k) {
        const std::uint32_t s = stars_[k];
        const auto v = db_->star_vector(s);
        const Vec3 sv{static_cast<double>(v[0]), static_cast<double>(v[1]),
                      static_cast<double>(v[2])};
        if (dot(center, sv) >= cos_r) f(s);
      }
    }
  }

  [[nodiscard]] std::uint32_t num_cells() const noexcept { return ncells_; }
  [[nodiscard]] const Database& database() const noexcept { return *db_; }

  // Cell of a unit vector. O(1).
  static std::uint32_t cell_of(const Vec3& v, std::uint32_t grid) noexcept;

 private:
  const Database* db_ = nullptr;
  std::uint32_t grid_ = 0, ncells_ = 0;
  std::span<std::uint32_t> cell_start_;
  std::span<std::uint32_t> stars_;
  std::span<Vec3> cell_center_;
  std::span<double> cell_radius_;
};

}  // namespace st
