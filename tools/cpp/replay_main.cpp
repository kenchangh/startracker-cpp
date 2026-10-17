// SPDX-License-Identifier: Apache-2.0
// st_replay: make and play .strp replay files.
//   st_replay pack   --out x.strp [--period-ms P] a.pgm b.pgm ...
//   st_replay render --db db.stdb --out x.strp [--frames N] [--period-ms P] [--rate DEG/S]
//                    [--fov DEG] [--size WxH] [--seed S]       (also writes x.strp.truth)
//   st_replay run    --db db.stdb --in x.strp --fov DEG [--threaded] [--drop newest|latest|block]
//                    [--pace SPEED] [--quiet]
// `run` streams the file through the pipeline (camera: ideal pinhole with the given horizontal
// FOV), prints one line per output, compares to x.strp.truth when present, and reports the
// latency distribution.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sequence.hpp"
#include "st/math/attitude.hpp"
#include "st/platform/mapped_file.hpp"
#include "st/platform/pipeline_runner.hpp"
#include "st/platform/replay_source.hpp"

using namespace st;

namespace {

double pct(std::vector<double> v, double q) {
  if (v.empty()) return std::nan("");
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1,
                    static_cast<std::size_t>(q * static_cast<double>(v.size() - 1) + 0.5))];
}

int usage() {
  std::cerr << "usage: st_replay pack|render|run ... (see source header)\n";
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  const std::string cmd = argv[1];
  std::string dbpath, out, in;
  std::vector<std::string> inputs;
  int frames = 50;
  double period_ms = 100, rate = 0.2, fov = 14, pace = 0;
  std::uint32_t w = 1280, h = 960;
  std::uint64_t seed = 1;
  bool threaded = false, quiet = false;
  DropPolicy drop = DropPolicy::kBlock;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&] { return std::string(i + 1 < argc ? argv[++i] : ""); };
    if (a == "--db")
      dbpath = next();
    else if (a == "--out")
      out = next();
    else if (a == "--in")
      in = next();
    else if (a == "--frames")
      frames = std::stoi(next());
    else if (a == "--period-ms")
      period_ms = std::stod(next());
    else if (a == "--rate")
      rate = std::stod(next());
    else if (a == "--fov")
      fov = std::stod(next());
    else if (a == "--seed")
      seed = std::stoull(next());
    else if (a == "--pace")
      pace = std::stod(next());
    else if (a == "--threaded")
      threaded = true;
    else if (a == "--quiet")
      quiet = true;
    else if (a == "--size") {
      const auto s = next();
      w = static_cast<std::uint32_t>(std::stoul(s));
      h = static_cast<std::uint32_t>(std::stoul(s.substr(s.find('x') + 1)));
    } else if (a == "--drop") {
      const auto d = next();
      drop = d == "newest"   ? DropPolicy::kDropNewest
             : d == "latest" ? DropPolicy::kKeepLatest
                             : DropPolicy::kBlock;
    } else if (!a.empty() && a[0] != '-')
      inputs.push_back(a);
    else
      return usage();
  }
  const auto period_ns = static_cast<std::int64_t>(period_ms * 1e6);
  try {
    if (cmd == "pack") {
      if (out.empty() || inputs.empty()) return usage();
      tools::write_strp(out, tools::sequence_from_pgms(inputs, period_ns));
      return 0;
    }
    auto file = MappedFile::open(dbpath.c_str());
    if (!file.ok()) {
      std::cerr << "cannot map " << dbpath << "\n";
      return 1;
    }
    auto db = Database::open(file.value().bytes());
    if (!db.ok()) {
      std::cerr << "db: " << to_string(db.status()) << "\n";
      return 1;
    }
    if (cmd == "render") {
      if (out.empty()) return usage();
      auto stars = tools::stars_from_database(db.value());
      tools::add_random_stars(stars, 100000, 7.0, 9.0, seed ^ 0x5EED);
      tools::RenderConfig rc;
      rc.camera = CameraModel::ideal(w, h, fov * kDegToRad);
      rc.seed = seed;
      rc.exposure_s = std::min(0.1, period_ms * 1e-3);
      rc.q_cam_icrs = attitude_from_radec_roll(1.1, 0.35, 0.4);
      const Vec3 omega = Vec3{0.3, 1.0, 0.2} * (rate * kDegToRad / std::sqrt(1.13));
      const auto seq = tools::render_sequence(stars, rc, omega, frames, period_ns);
      tools::write_strp(out, seq);
      tools::write_truth(out + ".truth", seq);
      return 0;
    }
    if (cmd != "run" || in.empty()) return usage();
    SteadyClock clock;
    ReplayOptions ro;
    if (pace > 0) {
      ro.pace_clock = &clock;
      ro.speed = pace;
    }
    auto src = ReplaySource::open(in.c_str(), ro);
    if (!src.ok()) {
      std::cerr << "replay: " << to_string(src.status()) << "\n";
      return 1;
    }
    const FrameFormat fmt = src.value().format();
    const auto truth = tools::read_truth(in + ".truth");
    std::vector<std::byte> mem(LostInSpaceSolver::workspace_bytes(db.value(), SolverLimits{}) +
                               (64u << 20));
    Arena arena(mem);
    auto idx = SkyIndex::create(db.value(), 16, arena);
    auto lis = LostInSpaceSolver::create(db.value(), SolverLimits{}, arena);
    if (!idx.ok() || !lis.ok()) return 1;
    TrackerConfig tc;
    tc.camera = CameraModel::ideal(fmt.width, fmt.height, fov * kDegToRad);
    auto tr = Tracker::create(idx.value(), &lis.value(), tc, arena);
    if (!tr.ok()) return 1;
    PipelineConfig pc;
    pc.mode = threaded ? PipelineMode::kThreaded : PipelineMode::kSingleThreaded;
    pc.drop = drop;
    pc.output_queue = 1024;
    auto pipe = std::make_unique<Pipeline>();
    if (pipe->init(src.value(), tr.value(), clock, pc, arena) != Status::kOk) return 1;
    std::vector<double> lat, err;
    int ok = 0, n = 0;
    auto drain = [&] {
      PipelineOutput o;
      while (pipe->poll(o)) {
        ++n;
        lat.push_back(static_cast<double>(o.latency_ns()) / 1e6);
        double e = std::nan("");
        if (o.solution.status == Status::kOk) {
          ++ok;
          if (o.source_seq < truth.size()) {
            e = angle_between(o.solution.q_cam_icrs, truth[o.source_seq].second) / kArcsecToRad;
            err.push_back(e);
          }
        }
        if (!quiet) {
          std::printf(
              "%llu %s %s ra %.6f dec %.6f roll %.6f matches %u rois %d lat_ms %.3f err_arcsec "
              "%.2f\n",
              static_cast<unsigned long long>(o.source_seq), to_string(o.solution.status),
              o.solution.mode == TrackMode::kTracking      ? "track"
              : o.solution.mode == TrackMode::kLostInSpace ? "lis"
                                                           : "lost",
              o.solution.ra_deg, o.solution.dec_deg, o.solution.roll_deg, o.solution.matches,
              o.used_rois ? 1 : 0, static_cast<double>(o.latency_ns()) / 1e6, e);
        }
      }
    };
    if (threaded) {
      PipelineRunner runner;
      if (runner.start(*pipe) != Status::kOk) return 1;
      while (!pipe->finished()) {
        drain();
        std::this_thread::yield();
      }
      runner.join();
    } else {
      while (true) {
        const Status s = pipe->step();
        if (s == Status::kCancelled) break;
        drain();
      }
    }
    drain();
    const auto st = pipe->stats();
    std::printf("outputs %d solved %d acquired %llu dropped_no_slot %llu dropped_stale %llu\n", n,
                ok, static_cast<unsigned long long>(st.acquired),
                static_cast<unsigned long long>(st.dropped_no_slot),
                static_cast<unsigned long long>(st.dropped_stale));
    std::printf("latency ms: median %.3f p99 %.3f max %.3f\n", pct(lat, .5), pct(lat, .99),
                pct(lat, 1));
    if (!err.empty())
      std::printf("attitude error arcsec: median %.2f p99 %.2f max %.2f\n", pct(err, .5),
                  pct(err, .99), pct(err, 1));
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
