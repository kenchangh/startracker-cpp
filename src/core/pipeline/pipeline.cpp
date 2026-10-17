// SPDX-License-Identifier: Apache-2.0
#include "st/pipeline/pipeline.hpp"

#include <algorithm>
#include <bit>
#include <thread>

namespace st {

struct Pipeline::Slot {
  std::span<std::byte> pixels;
  std::span<Centroid> centroids;
  std::uint32_t num_centroids = 0;
  FrameInfo info{};
  std::uint64_t seq = 0;
  std::int64_t t_acquired = 0, t_centroided = 0, t_solved = 0;
  bool used_rois = false;
  bool skip = false;  // dropped as stale; recycled by the solve stage without solving
  AttitudeSolution solution{};
};

namespace {
std::uint32_t pow2_at_least(std::uint32_t n) noexcept {
  return std::bit_ceil(std::max<std::uint32_t>(n, 2));
}
}  // namespace

std::size_t Pipeline::workspace_bytes(const FrameFormat& f, const PipelineConfig& c) noexcept {
  CentroiderConfig cc = c.centroider;
  if (cc.max_width == 0) cc.max_width = f.width;
  if (cc.max_height == 0) cc.max_height = f.height;
  const std::size_t q = pow2_at_least(c.slots);
  return Centroider::workspace_bytes(cc) +
         c.slots * (f.bytes() + 64 + c.max_centroids * sizeof(Centroid) + sizeof(Slot)) +
         3 * q * 4 + std::size_t{pow2_at_least(c.output_queue)} * sizeof(PipelineOutput) +
         c.max_centroids * sizeof(Spot) + c.max_rois * (sizeof(Roi) + sizeof(Spot) + 1) + 4096;
}

Status Pipeline::init(FrameSource& source, Tracker& tracker, const Clock& clock,
                      const PipelineConfig& config, Arena& arena) noexcept {
  cfg_ = config;
  fmt_ = source.format();
  if (fmt_.width == 0 || fmt_.height == 0 || cfg_.slots < 2 || cfg_.max_centroids < 4 ||
      (cfg_.output_queue & (cfg_.output_queue - 1)) != 0 || cfg_.output_queue < 2) {
    return Status::kInvalidArgument;
  }
  source_ = &source;
  tracker_ = &tracker;
  clock_ = &clock;
  CentroiderConfig cc = cfg_.centroider;
  if (cc.max_width == 0) cc.max_width = fmt_.width;
  if (cc.max_height == 0) cc.max_height = fmt_.height;
  if (cc.max_width < fmt_.width || cc.max_height < fmt_.height) return Status::kInvalidArgument;
  ST_TRY_ASSIGN(centroider_, Centroider::create(cc, arena));
  ST_TRY_ASSIGN(slots_, arena.allocate<Slot>(cfg_.slots));
  for (Slot& s : slots_) {
    ST_TRY_ASSIGN(s.pixels, arena.allocate_uninit<std::byte>(fmt_.bytes(), 64));
    ST_TRY_ASSIGN(s.centroids, arena.allocate<Centroid>(cfg_.max_centroids));
  }
  ST_TRY_ASSIGN(spots_, arena.allocate<Spot>(cfg_.max_centroids));
  ST_TRY_ASSIGN(rois_, arena.allocate<Roi>(cfg_.max_rois));
  ST_TRY_ASSIGN(roi_spots_, arena.allocate<Spot>(cfg_.max_rois));
  ST_TRY_ASSIGN(roi_found_, arena.allocate<bool>(cfg_.max_rois));
  const std::uint32_t q = pow2_at_least(cfg_.slots);
  ST_TRY_ASSIGN(auto qf, arena.allocate<std::uint32_t>(q, kCacheLineSize));
  ST_TRY_ASSIGN(auto qa, arena.allocate<std::uint32_t>(q, kCacheLineSize));
  ST_TRY_ASSIGN(auto qc, arena.allocate<std::uint32_t>(q, kCacheLineSize));
  ST_TRY_ASSIGN(auto qo, arena.allocate<PipelineOutput>(cfg_.output_queue, kCacheLineSize));
  ST_TRY(q_free_.init(qf));
  ST_TRY(q_acquired_.init(qa));
  ST_TRY(q_centroided_.init(qc));
  ST_TRY(outputs_.init(qo));
  // The last slot is the scratch slot for kDropNewest; the others start free. This happens
  // before any stage thread exists, so it does not break the single-producer rule.
  scratch_slot_ = cfg_.slots - 1;
  for (std::uint32_t i = 0; i < scratch_slot_; ++i) {
    if (!q_free_.try_push(i)) return Status::kCapacityExceeded;
  }
  return Status::kOk;
}

Status Pipeline::acquire_into(std::uint32_t idx) noexcept {
  Slot& s = slots_[idx];
  const Status st = source_->acquire(s.pixels, cfg_.acquire_timeout_ns, s.info);
  if (st != Status::kOk) return st;
  s.t_acquired = clock_->now_ns();
  s.seq = seq_++;
  s.skip = false;
  s.used_rois = false;
  acquired_.fetch_add(1, std::memory_order_relaxed);
  return Status::kOk;
}

void Pipeline::centroid(std::uint32_t idx, bool allow_roi) noexcept {
  Slot& s = slots_[idx];
  const ImageView view{s.pixels.data(), fmt_.width, fmt_.height, 0, fmt_.format};
  std::uint32_t n = 0;
  if (allow_roi && cfg_.roi_tracking && tracker_->mode() == TrackMode::kTracking) {
    const std::uint32_t nr = tracker_->predict_rois(s.info.t_capture_ns, cfg_.roi_half_size, rois_);
    if (nr >= 4 && centroider_.extract_rois(view, rois_.first(nr), cfg_.roi, roi_spots_.first(nr),
                                            roi_found_.first(nr)) == Status::kOk) {
      for (std::uint32_t i = 0; i < nr && n < s.centroids.size(); ++i) {
        if (roi_found_[i]) s.centroids[n++] = {roi_spots_[i].x, roi_spots_[i].y};
      }
      s.used_rois = true;
    }
  }
  if (!s.used_rois) {
    auto r = centroider_.extract(view, spots_);
    if (r.ok()) {
      for (std::uint32_t i = 0; i < r.value(); ++i) s.centroids[n++] = {spots_[i].x, spots_[i].y};
    }
  }
  s.num_centroids = n;
  s.t_centroided = clock_->now_ns();
}

void Pipeline::solve(std::uint32_t idx) noexcept {
  Slot& s = slots_[idx];
  tracker_->update(s.info.t_capture_ns, s.centroids.first(s.num_centroids), s.solution);
  s.t_solved = clock_->now_ns();
  if (s.solution.status == Status::kOk) solved_.fetch_add(1, std::memory_order_relaxed);
}

void Pipeline::publish(std::uint32_t idx) noexcept {
  const Slot& s = slots_[idx];
  PipelineOutput o;
  o.seq = s.seq;
  o.source_seq = s.info.source_seq;
  o.t_capture_ns = s.info.t_capture_ns;
  o.t_acquired_ns = s.t_acquired;
  o.t_centroided_ns = s.t_centroided;
  o.t_solved_ns = s.t_solved;
  o.num_centroids = s.num_centroids;
  o.used_rois = s.used_rois;
  o.solution = s.solution;
  if (cfg_.callback != nullptr) cfg_.callback(o, cfg_.callback_user);
  if (!outputs_.try_push(o)) dropped_output_.fetch_add(1, std::memory_order_relaxed);
  processed_.fetch_add(1, std::memory_order_relaxed);
}

Status Pipeline::step() noexcept {
  if (cfg_.mode != PipelineMode::kSingleThreaded || source_ == nullptr)
    return Status::kInvalidArgument;
  const Status st = acquire_into(0);
  if (st != Status::kOk) {
    if (st == Status::kIoError) source_errors_.fetch_add(1, std::memory_order_relaxed);
    if (st == Status::kCancelled) finished_.store(true, std::memory_order_release);
    return st;
  }
  centroid(0, true);
  solve(0);
  publish(0);
  return Status::kOk;
}

void Pipeline::run_stage(Stage stage, const std::atomic<bool>& stop) noexcept {
  switch (stage) {
    case Stage::kAcquire: acquire_loop(stop); break;
    case Stage::kCentroid: centroid_loop(); break;
    case Stage::kSolve: solve_loop(); break;
  }
}

void Pipeline::acquire_loop(const std::atomic<bool>& stop) noexcept {
  std::uint32_t held = 0;
  bool have = false;
  while (!stop.load(std::memory_order_relaxed)) {
    if (!have) have = q_free_.try_pop(held);
    std::uint32_t target = held;
    bool discard = false;
    if (!have) {
      if (cfg_.drop == DropPolicy::kBlock) {
        std::this_thread::yield();
        continue;
      }
      target = scratch_slot_;  // behind: read the frame and drop it
      discard = true;
    }
    const Status st = acquire_into(target);
    if (st == Status::kCancelled) break;
    if (st == Status::kIoError) source_errors_.fetch_add(1, std::memory_order_relaxed);
    if (st != Status::kOk) continue;
    if (discard) {
      dropped_no_slot_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    static_cast<void>(q_acquired_.try_push(held));  // capacity >= slots: cannot fail
    have = false;
  }
  source_done_.store(true, std::memory_order_release);
}

void Pipeline::centroid_loop() noexcept {
  for (;;) {
    std::uint32_t idx = 0;
    if (!q_acquired_.try_pop(idx)) {
      // Drain until upstream is done; the acquire stage is the one that honours `stop`.
      if (source_done_.load(std::memory_order_acquire)) {
        if (!q_acquired_.try_pop(idx)) break;
      } else {
        std::this_thread::yield();
        continue;
      }
    }
    if (cfg_.drop == DropPolicy::kKeepLatest) {
      std::uint32_t newer = 0;
      while (q_acquired_.try_pop(newer)) {
        slots_[idx].skip = true;
        static_cast<void>(q_centroided_.try_push(idx));
        dropped_stale_.fetch_add(1, std::memory_order_relaxed);
        idx = newer;
      }
    }
    centroid(idx, false);
    static_cast<void>(q_centroided_.try_push(idx));
  }
  centroid_done_.store(true, std::memory_order_release);
}

void Pipeline::solve_loop() noexcept {
  for (;;) {
    std::uint32_t idx = 0;
    if (!q_centroided_.try_pop(idx)) {
      // Drain until upstream is done; the acquire stage is the one that honours `stop`.
      if (centroid_done_.load(std::memory_order_acquire)) {
        if (!q_centroided_.try_pop(idx)) break;
      } else {
        std::this_thread::yield();
        continue;
      }
    }
    if (!slots_[idx].skip) {
      solve(idx);
      publish(idx);
    }
    static_cast<void>(q_free_.try_push(idx));
  }
  finished_.store(true, std::memory_order_release);
}

PipelineStats Pipeline::stats() const noexcept {
  PipelineStats s;
  s.acquired = acquired_.load(std::memory_order_relaxed);
  s.processed = processed_.load(std::memory_order_relaxed);
  s.dropped_no_slot = dropped_no_slot_.load(std::memory_order_relaxed);
  s.dropped_stale = dropped_stale_.load(std::memory_order_relaxed);
  s.dropped_output = dropped_output_.load(std::memory_order_relaxed);
  s.source_errors = source_errors_.load(std::memory_order_relaxed);
  s.solved = solved_.load(std::memory_order_relaxed);
  return s;
}

}  // namespace st
