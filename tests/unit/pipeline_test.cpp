// SPDX-License-Identifier: Apache-2.0
#include "st/pipeline/pipeline.hpp"

#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include "alloc_guard.hpp"
#include "sequence.hpp"
#include "st/math/attitude.hpp"
#include "st/pipeline/memory_source.hpp"
#include "st/platform/pipeline_runner.hpp"
#include "st/platform/replay_source.hpp"
#include "st/platform/v4l2_source.hpp"
#include "support/synthetic_sky.hpp"

namespace {
using namespace st;
using st::test::synthetic_db;

constexpr std::int64_t kPeriod = 100'000'000;  // 10 Hz

CameraIntrinsics camera() { return CameraModel::ideal(640, 480, 17 * kDegToRad); }

const tools::Sequence& sequence() {
  static const tools::Sequence* s = [] {
    tools::RenderConfig rc;
    rc.camera = camera();
    rc.exposure_s = 0.05;
    rc.q_cam_icrs = attitude_from_radec_roll(0.5, 0.2, 0.7);
    rc.seed = 7;
    auto* seq = new tools::Sequence(tools::render_sequence(
        tools::stars_from_database(synthetic_db().db), rc, Vec3{0.0, 0.003, 0.001}, 16, kPeriod));
    return seq;
  }();
  return *s;
}

void sleep_ms(int ms) {
  timespec ts{0, ms * 1'000'000L};
  nanosleep(&ts, nullptr);
}

// Wraps a source; returns kTimeout until open() so the test can start the guard first.
class GatedSource final : public FrameSource {
 public:
  explicit GatedSource(FrameSource& s) : s_(s) {}
  FrameFormat format() const noexcept override { return s_.format(); }
  Status acquire(std::span<std::byte> d, std::int64_t t, FrameInfo& i) noexcept override {
    polls.fetch_add(1);
    if (!open.load()) {
      std::this_thread::yield();
      return Status::kTimeout;
    }
    return s_.acquire(d, t, i);
  }
  std::atomic<bool> open{false};
  std::atomic<int> polls{0};

 private:
  FrameSource& s_;
};

class PipelineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    mem_.resize(128 << 20);
    arena_ = std::make_unique<Arena>(mem_);
    auto idx = SkyIndex::create(synthetic_db().db, 16, *arena_);
    ASSERT_TRUE(idx.ok());
    index_ = idx.value();
    auto lis = LostInSpaceSolver::create(synthetic_db().db, SolverLimits{}, *arena_);
    ASSERT_TRUE(lis.ok());
    lis_ = lis.value();
    TrackerConfig tc;
    tc.camera = camera();
    auto tr = Tracker::create(index_, &lis_, tc, *arena_);
    ASSERT_TRUE(tr.ok());
    tracker_ = tr.value();
  }
  MemoryFrameSource memory_source() const {
    const auto& s = sequence();
    return MemoryFrameSource(s.format, s.pixels, s.t_ns, kPeriod);
  }
  // Checks outputs against the truth; returns how many there were.
  int check_outputs(Pipeline& p, bool expect_all) {
    const auto& s = sequence();
    PipelineOutput o;
    int n = 0;
    std::uint64_t last_seq = 0;
    while (p.poll(o)) {
      if (n > 0) {
        EXPECT_GT(o.seq, last_seq);
      }
      last_seq = o.seq;
      const std::size_t k = o.source_seq;
      EXPECT_LT(k, s.truth.size());
      EXPECT_EQ(o.t_capture_ns, s.t_ns[k]);
      EXPECT_EQ(o.solution.status, Status::kOk) << "frame " << k;
      if (o.solution.status == Status::kOk) {
        EXPECT_LT(angle_between(o.solution.q_cam_icrs, s.truth[k]) / kArcsecToRad, 40.0)
            << "frame " << k;
      }
      EXPECT_LE(o.t_acquired_ns, o.t_centroided_ns);
      EXPECT_LE(o.t_centroided_ns, o.t_solved_ns);
      EXPECT_GE(o.latency_ns(), 0);
      ++n;
    }
    if (expect_all) {
      EXPECT_EQ(n, static_cast<int>(s.truth.size()));
    }
    return n;
  }
  std::vector<std::byte> mem_;
  std::unique_ptr<Arena> arena_;
  SkyIndex index_;
  LostInSpaceSolver lis_;
  Tracker tracker_;
  SteadyClock clock_;
};

TEST_F(PipelineTest, SingleThreadedTracksWithRoisAndNoAllocations) {
  auto src = memory_source();
  PipelineConfig cfg;
  cfg.output_queue = 32;
  Pipeline p;
  ASSERT_EQ(p.init(src, tracker_, clock_, cfg, *arena_), Status::kOk);
  int steps = 0, rois = 0;
  std::uint64_t allocs = 0;
  Status last = Status::kOk;
  {
    st::test::AllocGuard guard(st::test::AllocMode::kCount);
    while ((last = p.step()) == Status::kOk) ++steps;
    allocs = guard.allocations();
  }
  EXPECT_EQ(allocs, 0u);
  EXPECT_EQ(last, Status::kCancelled);
  EXPECT_EQ(steps, 16);
  EXPECT_TRUE(p.finished());
  const auto& s = sequence();
  PipelineOutput o;
  int k = 0;
  while (p.poll(o)) {
    EXPECT_EQ(o.solution.status, Status::kOk) << k;
    EXPECT_EQ(o.solution.mode, k == 0 ? TrackMode::kLostInSpace : TrackMode::kTracking) << k;
    rois += o.used_rois ? 1 : 0;
    EXPECT_LT(angle_between(o.solution.q_cam_icrs, s.truth[o.source_seq]) / kArcsecToRad, 40.0)
        << k;
    ++k;
  }
  EXPECT_EQ(k, 16);
  EXPECT_EQ(rois, 15);  // every frame after acquisition uses ROIs
  const auto st = p.stats();
  EXPECT_EQ(st.acquired, 16u);
  EXPECT_EQ(st.processed, 16u);
  EXPECT_EQ(st.solved, 16u);
}

TEST_F(PipelineTest, ThreadedBlockingReplayFromFile) {
  char path[] = "/tmp/st_pipeline_XXXXXX";
  const int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  close(fd);
  tools::write_strp(path, sequence());
  auto rs = ReplaySource::open(path, ReplayOptions{});
  ASSERT_TRUE(rs.ok()) << to_string(rs.status());
  EXPECT_EQ(rs.value().frame_count(), 16u);
  GatedSource gate(rs.value());
  PipelineConfig cfg;
  cfg.mode = PipelineMode::kThreaded;
  cfg.drop = DropPolicy::kBlock;
  Pipeline p;
  ASSERT_EQ(p.init(gate, tracker_, clock_, cfg, *arena_), Status::kOk);
  EXPECT_EQ(p.step(), Status::kInvalidArgument);
  PipelineRunner runner;
  ASSERT_EQ(runner.start(p), Status::kOk);
  while (gate.polls.load() < 3) std::this_thread::yield();
  sleep_ms(50);  // let every stage thread finish starting (sanitizer runtimes allocate there)
  std::uint64_t allocs = 0;
  {
    st::test::AllocGuard guard(st::test::AllocMode::kCount);
    gate.open.store(true);
    while (!p.finished()) std::this_thread::yield();
    allocs = guard.allocations();
  }
  runner.join();
  if (st::test::alloc_guard_hooks_aligned_alloc()) {
    EXPECT_EQ(allocs, 0u);
  }
  check_outputs(p, true);
  const auto st = p.stats();
  EXPECT_EQ(st.acquired, 16u);
  EXPECT_EQ(st.processed, 16u);
  EXPECT_EQ(st.dropped_no_slot + st.dropped_stale + st.dropped_output, 0u);
  std::remove(path);
}

// A slow solve stage (callback sleeps) forces the pipeline behind a fast source.
void slow_callback(const PipelineOutput&, void*) noexcept { sleep_ms(30); }

TEST_F(PipelineTest, DropNewestWhenBehind) {
  auto src = memory_source();
  PipelineConfig cfg;
  cfg.mode = PipelineMode::kThreaded;
  cfg.drop = DropPolicy::kDropNewest;
  cfg.slots = 3;
  cfg.callback = &slow_callback;
  Pipeline p;
  ASSERT_EQ(p.init(src, tracker_, clock_, cfg, *arena_), Status::kOk);
  PipelineRunner runner;
  ASSERT_EQ(runner.start(p), Status::kOk);
  runner.join();
  ASSERT_TRUE(p.finished());
  const auto st = p.stats();
  EXPECT_EQ(st.acquired, 16u);
  EXPECT_GT(st.dropped_no_slot, 0u);
  EXPECT_EQ(st.processed + st.dropped_no_slot + st.dropped_stale, st.acquired);
  EXPECT_EQ(check_outputs(p, false), static_cast<int>(st.processed));
}

TEST_F(PipelineTest, KeepLatestSkipsStaleFrames) {
  auto src = memory_source();
  PipelineConfig cfg;
  cfg.mode = PipelineMode::kThreaded;
  cfg.drop = DropPolicy::kKeepLatest;
  cfg.slots = 6;
  cfg.callback = &slow_callback;
  Pipeline p;
  ASSERT_EQ(p.init(src, tracker_, clock_, cfg, *arena_), Status::kOk);
  PipelineRunner runner;
  ASSERT_EQ(runner.start(p), Status::kOk);
  runner.join();
  const auto st = p.stats();
  EXPECT_EQ(st.acquired, 16u);
  EXPECT_GT(st.dropped_stale, 0u);
  EXPECT_EQ(st.processed + st.dropped_no_slot + st.dropped_stale, st.acquired);
  EXPECT_EQ(check_outputs(p, false), static_cast<int>(st.processed));
}

TEST_F(PipelineTest, StopEndsAnEndlessSource) {
  const auto& s = sequence();
  MemoryFrameSource src(s.format, s.pixels, s.t_ns, kPeriod, /*loop=*/true);
  PipelineConfig cfg;
  cfg.mode = PipelineMode::kThreaded;
  cfg.drop = DropPolicy::kKeepLatest;
  Pipeline p;
  ASSERT_EQ(p.init(src, tracker_, clock_, cfg, *arena_), Status::kOk);
  PipelineRunner runner;
  ASSERT_EQ(runner.start(p), Status::kOk);
  PipelineOutput o;
  int got = 0, ok = 0;
  while (got < 20) {
    if (p.poll(o)) {
      // The loop wrap jumps the attitude back; the tracker may miss a frame there.
      ok += o.solution.status == Status::kOk ? 1 : 0;
      ++got;
    } else {
      std::this_thread::yield();
    }
  }
  runner.stop();
  runner.join();
  EXPECT_TRUE(p.finished());
  EXPECT_GE(ok, 15);
  EXPECT_GE(p.stats().acquired, 20u);
}

TEST_F(PipelineTest, InvalidConfigurations) {
  auto src = memory_source();
  Pipeline p;
  PipelineConfig cfg;
  cfg.slots = 1;
  EXPECT_EQ(p.init(src, tracker_, clock_, cfg, *arena_), Status::kInvalidArgument);
  cfg = PipelineConfig{};
  cfg.output_queue = 48;
  EXPECT_EQ(p.init(src, tracker_, clock_, cfg, *arena_), Status::kInvalidArgument);
  cfg = PipelineConfig{};
  Pipeline p2;
  ASSERT_EQ(p2.init(src, tracker_, clock_, cfg, *arena_), Status::kOk);
  PipelineRunner runner;
  EXPECT_EQ(runner.start(p2), Status::kInvalidArgument);  // single-threaded pipeline
  std::vector<std::byte> tiny(1024);
  Arena small(tiny);
  Pipeline p3;
  EXPECT_EQ(p3.init(src, tracker_, clock_, cfg, small), Status::kArenaExhausted);
  EXPECT_GT(Pipeline::workspace_bytes(src.format(), cfg), src.format().bytes() * cfg.slots);
}

TEST_F(PipelineTest, WorkspaceBytesIsEnough) {
  auto src = memory_source();
  for (auto mode : {PipelineMode::kSingleThreaded, PipelineMode::kThreaded}) {
    PipelineConfig cfg;
    cfg.mode = mode;
    std::vector<std::byte> m(Pipeline::workspace_bytes(src.format(), cfg));
    Arena a(m);
    Pipeline p;
    EXPECT_EQ(p.init(src, tracker_, clock_, cfg, a), Status::kOk);
  }
}

TEST(ReplaySourceTest, LoopsPacesAndRejectsCorruptFiles) {
  tools::Sequence s;
  s.format = {4, 3, PixelFormat::kMono8};
  for (int k = 0; k < 3; ++k) {
    for (int i = 0; i < 12; ++i) s.pixels.push_back(static_cast<std::byte>(k * 16 + i));
    s.t_ns.push_back(1'000'000'000 + k * 20'000'000LL);
  }
  char path[] = "/tmp/st_replay_XXXXXX";
  const int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  close(fd);
  tools::write_strp(path, s);
  SteadyClock clock;
  ReplayOptions opt;
  opt.loop = true;
  opt.pace_clock = &clock;
  opt.speed = 2.0;  // 10 ms between frames
  auto r = ReplaySource::open(path, opt);
  ASSERT_TRUE(r.ok());
  std::byte buf[12];
  FrameInfo info;
  const std::int64_t t0 = clock.now_ns();
  for (int k = 0; k < 7; ++k) {
    ASSERT_EQ(r.value().acquire(buf, 1'000'000'000, info), Status::kOk);
    EXPECT_EQ(static_cast<int>(buf[0]), (k % 3) * 16);
    EXPECT_EQ(info.t_capture_ns, 1'000'000'000 + k * 20'000'000LL);
    EXPECT_EQ(info.source_seq, static_cast<std::uint64_t>(k));
  }
  EXPECT_GE(clock.now_ns() - t0, 6 * 10'000'000LL - 1'000'000);
  // Not due within a 1 ms timeout: kTimeout.
  EXPECT_EQ(r.value().acquire(buf, 1'000'000, info), Status::kTimeout);
  // Without loop: kCancelled at the end.
  auto once = ReplaySource::open(path, ReplayOptions{});
  ASSERT_TRUE(once.ok());
  for (int k = 0; k < 3; ++k) EXPECT_EQ(once.value().acquire(buf, 0, info), Status::kOk);
  EXPECT_EQ(once.value().acquire(buf, 0, info), Status::kCancelled);
  std::byte small[4];
  auto again = ReplaySource::open(path, ReplayOptions{});
  EXPECT_EQ(again.value().acquire(small, 0, info), Status::kInvalidArgument);
  // Truncated file.
  if (FILE* f = std::fopen(path, "r+b")) {
    std::fclose(f);
    ASSERT_EQ(truncate(path, 64 + 28 * 2 + 5), 0);
  }
  EXPECT_EQ(ReplaySource::open(path, ReplayOptions{}).status(), Status::kCorruptData);
  std::remove(path);
  EXPECT_EQ(ReplaySource::open("/nonexistent/x.strp", ReplayOptions{}).status(), Status::kIoError);
}

TEST(V4L2SourceTest, MissingDeviceIsAnError) {
  auto r = V4L2Source::open("/dev/st_no_such_video_device", V4L2Options{});
#if defined(__linux__)
  EXPECT_EQ(r.status(), Status::kIoError);
#else
  EXPECT_EQ(r.status(), Status::kUnsupported);
#endif
  V4L2Options bad;
  bad.buffers = 1;
  EXPECT_EQ(V4L2Source::open("/dev/video0", bad).status(), Status::kInvalidArgument);
}

}  // namespace
