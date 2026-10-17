// SPDX-License-Identifier: Apache-2.0
#pragma once
// Rendered frame sequences and the .strp replay file writer (host tools and tests).
#include <cstdint>
#include <string>
#include <vector>

#include "render.hpp"
#include "st/pipeline/frame_source.hpp"

namespace st::tools {

struct Sequence {
  FrameFormat format{};
  std::vector<std::byte> pixels;   // frames back to back, tightly packed
  std::vector<std::int64_t> t_ns;  // mid-exposure time of each frame
  std::vector<Quat> truth;         // mid-exposure attitude (q_cam_icrs)
};

// Renders `frames` frames at `period_ns` spacing while the camera turns at `rate` (rad/s,
// camera frame) from base.q_cam_icrs at t = 0 (start of the first exposure). Mono16.
Sequence render_sequence(const std::vector<RenderStar>& stars, const RenderConfig& base,
                         const Vec3& rate, int frames, std::int64_t period_ns);

// Packs frames from PGM files (all the same size) with timestamps k * period_ns.
Sequence sequence_from_pgms(const std::vector<std::string>& paths, std::int64_t period_ns);

// Writes the .strp file (see st/platform/replay_source.hpp). Throws on I/O failure.
void write_strp(const std::string& path, const Sequence& seq);

// Writes "t_ns qw qx qy qz" per frame.
void write_truth(const std::string& path, const Sequence& seq);
// Reads it back (empty if the file is missing).
std::vector<std::pair<std::int64_t, Quat>> read_truth(const std::string& path);

}  // namespace st::tools
