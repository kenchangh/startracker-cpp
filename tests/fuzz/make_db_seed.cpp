// SPDX-License-Identifier: Apache-2.0
// Writes a small valid database to seed the loader fuzzer.
#include <cmath>
#include <iostream>

#include "db_writer.hpp"

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  st::tools::DbContents c;
  for (int i = 0; i < 8; ++i) {
    const float a = 0.05f * static_cast<float>(i);
    c.star_table.insert(c.star_table.end(), {a, 0.0f, std::cos(a), std::sin(a), 0.0f, 2.0f + a});
    c.catalog_ids.push_back(static_cast<std::uint32_t>(i + 1));
  }
  c.slots.assign(12, {0, 0, 0, 0});
  c.slots[1] = {0, 1, 2, 3};
  c.slots[2] = {4, 5, 6, 7};
  c.slots[5] = {1, 3, 5, 7};
  c.key_hash.assign(12, 3);
  c.largest_edge_half.assign(12, 0x5800);
  c.header.pattern_bins = 50;
  c.header.pattern_max_error = 0.005;
  c.header.max_fov = c.header.min_fov = 0.4;
  st::tools::write_file(argv[1], st::tools::build_blob(c));
  return 0;
}
