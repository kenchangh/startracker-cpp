// SPDX-License-Identifier: Apache-2.0
// st_bench: latency and memory benchmarks on rendered frames.
//   st_bench [--db db.stdb] [--fov DEG] [--size WxH] [--frames N] [--out report.md] [--json
//   out.json]
// Without --db a synthetic 9000-star catalog and database are generated (numbers are then not
// comparable with the ESA database). Reports median / p99 / max per component and per frame
// through the pipeline, arena bytes per component, database size, peak RSS and the host.
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "db_writer.hpp"
#include "generator.hpp"
#include "sequence.hpp"
#include "st/math/attitude.hpp"
#include "st/pipeline/memory_source.hpp"
#include "st/platform/mapped_file.hpp"
#include "st/platform/pipeline_runner.hpp"

using namespace st;

namespace {

struct Dist {
  explicit Dist(std::string n) : name(std::move(n)) {}
  std::string name;
  std::vector<double> ms;
};

double pct(std::vector<double> v, double q) {
  if (v.empty()) return std::nan("");
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1,
                    static_cast<std::size_t>(q * static_cast<double>(v.size() - 1) + 0.5))];
}

std::string cpu_model() {
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  while (std::getline(f, line)) {
    for (const char* key : {"model name", "Model", "CPU part"}) {
      if (line.rfind(key, 0) == 0) {
        const auto p = line.find(':');
        if (p != std::string::npos) return line.substr(p + 2);
      }
    }
  }
#if defined(__APPLE__)
  return "Apple (see sysctl machdep.cpu.brand_string)";
#else
  return "unknown";
#endif
}

std::vector<std::byte> synthetic_blob() {
  std::mt19937_64 g(42);
  std::uniform_real_distribution<double> u(0, 1);
  tools::Catalog cat;
  cat.kind = db::CatalogKind::kHipMain;
  cat.pm_origin = 1991.25;
  for (int i = 0; i < 9000; ++i) {
    tools::CatalogStar s;
    s.id = static_cast<std::uint32_t>(i + 1);
    s.ra = 360 * u(g);
    s.dec = std::asin(2 * u(g) - 1) * kRadToDeg;
    s.mag = 1 + 6 * std::pow(u(g), 0.3);
    cat.stars.push_back(s);
  }
  tools::GeneratorConfig cfg;
  cfg.max_fov_deg = 20;
  cfg.min_fov_deg = 10;
  cfg.star_max_magnitude = 7;
  return tools::build_blob(tools::generate_database(cat, cfg));
}

// Releases frames at a fixed period (sleeping; outside the measured interval).
class Paced final : public FrameSource {
 public:
  Paced(FrameSource& s, const Clock& c, std::int64_t p) : s_(s), c_(c), p_(p) {}
  FrameFormat format() const noexcept override { return s_.format(); }
  Status acquire(std::span<std::byte> dst, std::int64_t to, FrameInfo& i) noexcept override {
    if (next_ == 0) next_ = c_.now_ns();
    while (c_.now_ns() < next_) std::this_thread::sleep_for(std::chrono::microseconds(200));
    next_ += p_;
    return s_.acquire(dst, to, i);
  }

 private:
  FrameSource& s_;
  const Clock& c_;
  std::int64_t p_, next_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
  std::string dbpath, out_md, out_json;
  double fov = 14;
  std::uint32_t w = 1280, h = 960;
  int frames = 60;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&] { return std::string(i + 1 < argc ? argv[++i] : ""); };
    if (a == "--db")
      dbpath = next();
    else if (a == "--fov")
      fov = std::stod(next());
    else if (a == "--frames")
      frames = std::stoi(next());
    else if (a == "--out")
      out_md = next();
    else if (a == "--json")
      out_json = next();
    else if (a == "--size") {
      const auto s = next();
      w = static_cast<std::uint32_t>(std::stoul(s));
      h = static_cast<std::uint32_t>(std::stoul(s.substr(s.find('x') + 1)));
    } else {
      std::cerr << "unknown option " << a << "\n";
      return 2;
    }
  }
  std::vector<std::byte> blob;
  MappedFile file;
  std::span<const std::byte> bytes;
  std::string db_name = "synthetic (9000 stars, 10-20 deg)";
  if (!dbpath.empty()) {
    auto f = MappedFile::open(dbpath.c_str(), true);
    if (!f.ok()) {
      std::cerr << "cannot map " << dbpath << "; skipping\n";
      return 77;
    }
    file = std::move(f.value());
    bytes = file.bytes();
    db_name = dbpath.substr(dbpath.find_last_of('/') + 1);
  } else {
    blob = synthetic_blob();
    bytes = blob;
  }
  auto db = Database::open(bytes);
  if (!db.ok()) return 1;

  // Rendered sequence: 0.5 deg/s, 10 Hz, 50 ms exposures, Tycho-like clutter.
  auto stars = tools::stars_from_database(db.value());
  tools::add_random_stars(stars, 100000, 7.0, 9.0, 99);
  tools::RenderConfig rc;
  rc.camera = CameraModel::ideal(w, h, fov * kDegToRad);
  rc.exposure_s = 0.05;
  rc.q_cam_icrs = attitude_from_radec_roll(1.1, 0.35, 0.4);
  const std::int64_t period = 100'000'000;
  const Vec3 omega = Vec3{0.3, 1.0, 0.2} * (0.5 * kDegToRad / std::sqrt(1.13));
  std::cerr << "rendering " << frames << " frames...\n";
  const auto seq = tools::render_sequence(stars, rc, omega, frames, period);
  const std::size_t fbytes = seq.format.bytes();
  auto frame = [&](int k) {
    return ImageView{seq.pixels.data() + static_cast<std::size_t>(k) * fbytes, w, h, 0,
                     PixelFormat::kMono16};
  };

  std::vector<std::byte> mem(LostInSpaceSolver::workspace_bytes(db.value(), SolverLimits{}) +
                             (256u << 20));
  Arena arena(mem);
  std::vector<std::pair<std::string, std::size_t>> memory;
  std::size_t m0 = arena.used();
  CentroiderConfig cc;
  cc.max_width = w;
  cc.max_height = h;
  auto cent = Centroider::create(cc, arena);
  memory.emplace_back("centroider", arena.used() - m0);
  m0 = arena.used();
  auto lis = LostInSpaceSolver::create(db.value(), SolverLimits{}, arena);
  memory.emplace_back("lost-in-space solver", arena.used() - m0);
  m0 = arena.used();
  auto idx = SkyIndex::create(db.value(), 16, arena);
  memory.emplace_back("sky index", arena.used() - m0);
  m0 = arena.used();
  TrackerConfig tc;
  tc.camera = rc.camera;
  auto tr = Tracker::create(idx.value(), &lis.value(), tc, arena);
  memory.emplace_back("tracker", arena.used() - m0);
  if (!cent.ok() || !lis.ok() || !idx.ok() || !tr.ok()) return 1;

  SteadyClock clock;
  std::vector<Dist> d;
  std::vector<Spot> spots(64);
  std::vector<Centroid> cents(64);
  // Components.
  Dist dc{"centroid, full frame"}, dl{"lost-in-space solve"}, dt{"tracker update (tracking)"},
      dr{"centroid, 32 ROIs"};
  std::vector<Roi> rois(32);
  std::vector<Spot> rspots(32);
  bool found[32];
  int lis_ok = 0;
  for (int k = 0; k < frames; ++k) {
    std::int64_t t0 = clock.now_ns();
    auto n = cent.value().extract(frame(k), spots);
    dc.ms.push_back(static_cast<double>(clock.now_ns() - t0) / 1e6);
    if (!n.ok()) continue;
    for (std::uint32_t i = 0; i < n.value(); ++i) cents[i] = {spots[i].x, spots[i].y};
    const std::span<const Centroid> cs(cents.data(), n.value());
    SolveResult res;
    t0 = clock.now_ns();
    lis.value().solve(cs, w, h, SolveOptions{}, res);
    dl.ms.push_back(static_cast<double>(clock.now_ns() - t0) / 1e6);
    lis_ok += res.status == Status::kOk ? 1 : 0;
    AttitudeSolution s;
    if (tr.value().mode() == TrackMode::kTracking) {
      const std::uint32_t nr =
          tr.value().predict_rois(seq.t_ns[static_cast<std::size_t>(k)], 8, rois);
      t0 = clock.now_ns();
      static_cast<void>(cent.value().extract_rois(frame(k), std::span<const Roi>(rois.data(), nr),
                                                  RoiConfig{}, std::span<Spot>(rspots.data(), nr),
                                                  std::span<bool>(found, nr)));
      dr.ms.push_back(static_cast<double>(clock.now_ns() - t0) / 1e6);
      t0 = clock.now_ns();
      tr.value().update(seq.t_ns[static_cast<std::size_t>(k)], cs, s);
      dt.ms.push_back(static_cast<double>(clock.now_ns() - t0) / 1e6);
    } else {
      tr.value().update(seq.t_ns[static_cast<std::size_t>(k)], cs, s);
    }
  }
  d.push_back(dc);
  d.push_back(dr);
  d.push_back(dl);
  d.push_back(dt);

  // Pipelines (per-frame latency: frame copied -> attitude ready).
  int pipeline_ok[2] = {0, 0};
  for (int threaded = 0; threaded < 2; ++threaded) {
    tr.value().reset();
    const auto mark = arena.mark();
    MemoryFrameSource src(seq.format, seq.pixels, seq.t_ns, period);
    PipelineConfig pc;
    pc.mode = threaded != 0 ? PipelineMode::kThreaded : PipelineMode::kSingleThreaded;
    pc.drop = DropPolicy::kBlock;
    pc.output_queue = 1024;
    // Threaded: feed frames at 20 Hz so latency excludes queueing behind a flooding source.
    Paced paced(src, clock, period / 2);
    FrameSource& use = threaded != 0 ? static_cast<FrameSource&>(paced) : src;
    auto pipe = std::make_unique<Pipeline>();
    m0 = arena.used();
    if (pipe->init(use, tr.value(), clock, pc, arena) != Status::kOk) return 1;
    if (threaded == 0) memory.emplace_back("pipeline (4 slots)", arena.used() - m0);
    Dist dp(threaded != 0 ? "pipeline frame, threaded (full-frame centroids)"
                          : "pipeline frame, single-threaded (ROI tracking)");
    if (threaded != 0) {
      PipelineRunner runner;
      if (runner.start(*pipe) != Status::kOk) return 1;
      runner.join();
    } else {
      while (pipe->step() == Status::kOk) {
      }
    }
    PipelineOutput o;
    while (pipe->poll(o)) {
      dp.ms.push_back(static_cast<double>(o.latency_ns()) / 1e6);
      pipeline_ok[threaded] += o.solution.status == Status::kOk ? 1 : 0;
    }
    d.push_back(dp);
    arena.release(mark);
  }

  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  utsname un{};
  uname(&un);
  std::ostringstream md;
  md << "## st_bench\n\n";
  md << "- host: " << cpu_model() << ", " << std::thread::hardware_concurrency() << " CPUs, "
     << un.sysname << " " << un.release << " " << un.machine << "\n";
#if defined(__clang__)
  md << "- compiler: clang " << __clang_version__ << "\n";
#elif defined(__GNUC__)
  md << "- compiler: gcc " << __VERSION__ << "\n";
#endif
  md << "- database: " << db_name << ", " << bytes.size() << " bytes (" << db.value().num_stars()
     << " stars)\n";
  md << "- frames: " << frames << " rendered " << w << "x" << h << " mono16, FOV " << fov
     << " deg, 0.5 deg/s, 10 Hz; lost-in-space solved " << lis_ok << "/" << frames
     << "; pipeline solved " << pipeline_ok[0] << " (single) / " << pipeline_ok[1]
     << " (threaded)\n\n";
  md << "| measurement | n | median ms | p99 ms | max ms |\n|---|---|---|---|---|\n";
  char line[256];
  for (const auto& x : d) {
    std::snprintf(line, sizeof line, "| %s | %zu | %.3f | %.3f | %.3f |\n", x.name.c_str(),
                  x.ms.size(), pct(x.ms, .5), pct(x.ms, .99), pct(x.ms, 1));
    md << line;
  }
  md << "\n| memory | bytes |\n|---|---|\n";
  for (const auto& [name, b] : memory) md << "| arena: " << name << " | " << b << " |\n";
  md << "| database (mapped) | " << bytes.size() << " |\n";
  md << "| peak RSS of st_bench (includes rendering) | "
     << static_cast<long long>(ru.ru_maxrss) * 1024 << " |\n";
  std::cout << md.str();
  if (!out_md.empty()) std::ofstream(out_md) << md.str();
  if (!out_json.empty()) {
    std::ofstream j(out_json);
    j << "{\"host\":\"" << cpu_model() << "\",\"machine\":\"" << un.machine << "\",\"results\":[";
    for (std::size_t i = 0; i < d.size(); ++i) {
      j << (i ? "," : "") << "{\"name\":\"" << d[i].name << "\",\"median_ms\":" << pct(d[i].ms, .5)
        << ",\"p99_ms\":" << pct(d[i].ms, .99) << ",\"max_ms\":" << pct(d[i].ms, 1) << "}";
    }
    j << "]}\n";
  }
  return 0;
}
