// SPDX-License-Identifier: Apache-2.0
#include "sequence.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>

#include "st/platform/replay_source.hpp"

namespace st::tools {

namespace {
void append_frame(Sequence& s, const Image16& img) {
  const std::size_t off = s.pixels.size();
  s.pixels.resize(off + img.pixels.size() * 2);
  std::memcpy(s.pixels.data() + off, img.pixels.data(), img.pixels.size() * 2);
}
}  // namespace

Sequence render_sequence(const std::vector<RenderStar>& stars, const RenderConfig& base,
                         const Vec3& rate, int frames, std::int64_t period_ns) {
  Sequence s;
  s.format = {base.camera.width, base.camera.height, PixelFormat::kMono16};
  for (int k = 0; k < frames; ++k) {
    RenderConfig rc = base;
    const double t0 = static_cast<double>(k) * static_cast<double>(period_ns) * 1e-9;
    rc.q_cam_icrs = normalized(from_rotation_vector(-t0 * rate) * base.q_cam_icrs);
    rc.body_rate = rate;
    rc.seed = base.seed + static_cast<std::uint64_t>(k);
    append_frame(s, render(stars, rc).image);
    const double tm = t0 + 0.5 * base.exposure_s;
    s.t_ns.push_back(static_cast<std::int64_t>(tm * 1e9 + 0.5));
    s.truth.push_back(normalized(from_rotation_vector(-tm * rate) * base.q_cam_icrs));
  }
  return s;
}

Sequence sequence_from_pgms(const std::vector<std::string>& paths, std::int64_t period_ns) {
  Sequence s;
  for (std::size_t k = 0; k < paths.size(); ++k) {
    const Image16 img = read_pgm(paths[k]);
    if (k == 0) {
      s.format = {img.width, img.height, PixelFormat::kMono16};
    } else if (img.width != s.format.width || img.height != s.format.height) {
      throw std::runtime_error("image size differs: " + paths[k]);
    }
    append_frame(s, img);
    s.t_ns.push_back(static_cast<std::int64_t>(k) * period_ns);
  }
  return s;
}

void write_strp(const std::string& path, const Sequence& seq) {
  const std::size_t n = seq.format.bytes();
  const std::uint64_t count = n == 0 ? 0 : seq.pixels.size() / n;
  unsigned char h[kReplayHeaderBytes] = {};
  std::memcpy(h, "STRPLAY1", 8);
  const std::uint32_t u32[4] = {1, seq.format.width, seq.format.height,
                                seq.format.format == PixelFormat::kMono8 ? 0u : 1u};
  std::memcpy(h + 8, u32, sizeof(u32));
  const std::uint64_t u64[2] = {count, kReplayRecordHeaderBytes + n};
  std::memcpy(h + 24, u64, sizeof(u64));
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(h), sizeof(h));
  for (std::uint64_t k = 0; k < count; ++k) {
    const std::int64_t t = k < seq.t_ns.size() ? seq.t_ns[k] : 0;
    f.write(reinterpret_cast<const char*>(&t), 8);
    f.write(reinterpret_cast<const char*>(&k), 8);
    f.write(reinterpret_cast<const char*>(seq.pixels.data() + k * n),
            static_cast<std::streamsize>(n));
  }
  if (!f) throw std::runtime_error("cannot write " + path);
}

void write_truth(const std::string& path, const Sequence& seq) {
  std::ofstream f(path);
  f.precision(17);
  for (std::size_t k = 0; k < seq.truth.size(); ++k) {
    const Quat& q = seq.truth[k];
    f << seq.t_ns[k] << ' ' << q.w << ' ' << q.x << ' ' << q.y << ' ' << q.z << '\n';
  }
  if (!f) throw std::runtime_error("cannot write " + path);
}

std::vector<std::pair<std::int64_t, Quat>> read_truth(const std::string& path) {
  std::vector<std::pair<std::int64_t, Quat>> out;
  std::ifstream f(path);
  std::int64_t t = 0;
  Quat q;
  while (f >> t >> q.w >> q.x >> q.y >> q.z) out.emplace_back(t, q);
  return out;
}

}  // namespace st::tools
