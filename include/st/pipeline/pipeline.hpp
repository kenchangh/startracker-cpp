// SPDX-License-Identifier: Apache-2.0
#pragma once

// Streaming pipeline: acquire -> centroid -> solve (track / lost-in-space) -> output.
//
// A fixed pool of frame slots (pixels, centroids, solution, timestamps) circulates through
// three lock-free SPSC queues: acquire -> centroid, centroid -> solve, solve -> free. Outputs go
// to a fourth SPSC queue the application drains with poll(), and/or to a callback that runs on
// the solve stage and must be real-time safe.
//
// Modes:
//   kSingleThreaded: the caller drives step(); one frame goes through every stage. Allows
//                    ROI centroiding while tracking.
//   kThreaded:       each stage loop (run_stage) runs on its own thread. The core does not create
//                    threads (std::thread allocates); st_platform's PipelineRunner starts them
//                    with pthreads and optional real-time priority / CPU affinity. Full-frame
//                    centroiding every frame.
// Drop policies (what happens when the pipeline is behind):
//   kDropNewest: no free slot -> the new frame is read into a scratch slot and discarded.
//   kKeepLatest: the centroid stage skips stale queued frames and processes the newest.
//   kBlock:      the acquire stage waits for a free slot (deterministic replay).
//
// Worst-case latency per frame = source copy O(W*H) + centroiding O(W*H) + tracker update or
// one lost-in-space solve (bounded by its WorkBudget) + queue hand-offs (wait-free).

#include <atomic>
#include <cstdint>
#include <span>

#include "st/centroid/centroider.hpp"
#include "st/containers/spsc_queue.hpp"
#include "st/core/arena.hpp"
#include "st/core/clock.hpp"
#include "st/core/result.hpp"
#include "st/pipeline/frame_source.hpp"
#include "st/track/tracker.hpp"

namespace st {

enum class PipelineMode : std::uint8_t { kSingleThreaded, kThreaded };
enum class DropPolicy : std::uint8_t { kDropNewest, kKeepLatest, kBlock };

struct PipelineOutput {
  std::uint64_t seq = 0;  // pipeline frame counter
  std::uint64_t source_seq = 0;
  std::int64_t t_capture_ns = 0;     // source timestamp
  std::int64_t t_acquired_ns = 0;    // pipeline clock: frame copied
  std::int64_t t_centroided_ns = 0;  // pipeline clock: centroids ready
  std::int64_t t_solved_ns = 0;      // pipeline clock: attitude ready
  std::uint32_t num_centroids = 0;
  bool used_rois = false;
  AttitudeSolution solution{};
  [[nodiscard]] std::int64_t latency_ns() const noexcept { return t_solved_ns - t_acquired_ns; }
};

struct PipelineStats {
  std::uint64_t acquired = 0;
  std::uint64_t processed = 0;
  std::uint64_t dropped_no_slot = 0;  // kDropNewest
  std::uint64_t dropped_stale = 0;    // kKeepLatest
  std::uint64_t dropped_output = 0;   // output queue full (oldest kept)
  std::uint64_t source_errors = 0;
  std::uint64_t solved = 0;
};

using OutputCallback = void (*)(const PipelineOutput& out, void* user) noexcept;

enum class Stage : std::uint8_t { kAcquire = 0, kCentroid = 1, kSolve = 2 };

struct PipelineConfig {
  PipelineMode mode = PipelineMode::kSingleThreaded;
  DropPolicy drop = DropPolicy::kDropNewest;
  std::uint32_t slots = 4;           // frame slots (>= 2; one is reserved as scratch)
  std::uint32_t max_centroids = 64;  // centroids kept per frame (brightest)
  std::uint32_t output_queue = 64;   // power of two
  std::int64_t acquire_timeout_ns = 100'000'000;
  bool roi_tracking = true;  // single-threaded mode only
  std::uint32_t roi_half_size = 8;
  std::uint32_t max_rois = 32;
  CentroiderConfig centroider{};  // max_width/height default to the source format
  RoiConfig roi{};
  OutputCallback callback = nullptr;
  void* callback_user = nullptr;
};

// Queues are cache-line aligned on purpose (no false sharing between stages).
class Pipeline {  // NOLINT(clang-analyzer-optin.performance.Padding)
 public:
  Pipeline() noexcept = default;
  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;

  // Everything (slots, queues, centroider buffers) comes from `arena`. `source`, `tracker` and
  // `clock` must outlive the pipeline. Errors: kInvalidArgument, kArenaExhausted.
  Status init(FrameSource& source, Tracker& tracker, const Clock& clock,
              const PipelineConfig& config, Arena& arena) noexcept;
  static std::size_t workspace_bytes(const FrameFormat& format,
                                     const PipelineConfig& config) noexcept;

  // kSingleThreaded: acquire, centroid, solve and publish one frame. Returns kOk, kTimeout,
  // kCancelled (end of stream) or kIoError. Worst case: see the file comment.
  Status step() noexcept;

  // kThreaded: runs one stage loop. Call each stage exactly once, each from its own thread.
  // The acquire stage returns when `stop` is set or at end of stream; the centroid and solve
  // stages drain every queued frame and return once the stage before them has returned.
  // Never allocates; idle stages wait by yielding.
  void run_stage(Stage stage, const std::atomic<bool>& stop) noexcept;

  // True after the source reported end of stream and every frame has been published.
  [[nodiscard]] bool finished() const noexcept { return finished_.load(std::memory_order_acquire); }

  // Next output, if any (single consumer, any thread). Wait-free.
  bool poll(PipelineOutput& out) noexcept { return outputs_.try_pop(out); }

  // Snapshot of the counters (approximate while threads run). O(1).
  [[nodiscard]] PipelineStats stats() const noexcept;
  [[nodiscard]] const PipelineConfig& config() const noexcept { return cfg_; }

 private:
  struct Slot;
  void acquire_loop(const std::atomic<bool>& stop) noexcept;
  void centroid_loop() noexcept;
  void solve_loop() noexcept;
  Status acquire_into(std::uint32_t slot) noexcept;
  void centroid(std::uint32_t slot, bool allow_roi) noexcept;
  void solve(std::uint32_t slot) noexcept;
  void publish(std::uint32_t slot) noexcept;

  FrameSource* source_ = nullptr;
  Tracker* tracker_ = nullptr;
  const Clock* clock_ = nullptr;
  PipelineConfig cfg_{};
  FrameFormat fmt_{};
  Centroider centroider_{};
  std::span<Slot> slots_;
  std::span<Spot> spots_;  // spot scratch for full-frame extraction (shared by one stage)
  std::span<Roi> rois_;
  std::span<Spot> roi_spots_;
  std::span<bool> roi_found_;
  SpscQueue<std::uint32_t> q_free_, q_acquired_, q_centroided_;
  SpscQueue<PipelineOutput> outputs_;
  std::uint32_t scratch_slot_ = 0;
  std::uint64_t seq_ = 0;
  std::atomic<bool> source_done_{false};    // acquire stage saw end of stream
  std::atomic<bool> centroid_done_{false};  // centroid stage drained after end of stream
  std::atomic<bool> finished_{false};
  // Counters (each written by one stage only).
  std::atomic<std::uint64_t> acquired_{0}, processed_{0}, dropped_no_slot_{0}, dropped_stale_{0},
      dropped_output_{0}, source_errors_{0}, solved_{0};
};

}  // namespace st
