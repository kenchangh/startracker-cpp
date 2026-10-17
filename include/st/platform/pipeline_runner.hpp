// SPDX-License-Identifier: Apache-2.0
#pragma once

// Runs a kThreaded Pipeline: one POSIX thread per stage, with optional SCHED_FIFO priority,
// CPU affinity (Linux) and stack size. Thread creation happens in start(), outside the
// runtime path (pthread_create may allocate); the stage loops themselves never allocate.

#include <pthread.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "st/core/status.hpp"
#include "st/pipeline/pipeline.hpp"

namespace st {

struct StageThreadConfig {
  int priority = 0;             // > 0: SCHED_FIFO at this priority (needs CAP_SYS_NICE)
  int cpu = -1;                 // >= 0: pin to this CPU (Linux only)
  std::size_t stack_bytes = 0;  // 0: system default
};

class PipelineRunner {
 public:
  PipelineRunner() noexcept = default;
  PipelineRunner(const PipelineRunner&) = delete;
  PipelineRunner& operator=(const PipelineRunner&) = delete;
  ~PipelineRunner();  // stop() + join()

  // Starts the three stage threads. Errors: kInvalidArgument (pipeline not kThreaded, already
  // running), kIoError (thread creation failed; any started thread is stopped and joined),
  // kUnsupported (priority/affinity request refused; nothing is left running).
  Status start(Pipeline& pipeline, const StageThreadConfig (&threads)[3]) noexcept;
  Status start(Pipeline& pipeline) noexcept;

  // Asks the acquire stage to stop; the other stages drain and exit.
  void stop() noexcept { stop_.store(true, std::memory_order_relaxed); }
  // Waits for all stage threads (after stop() or end of stream).
  void join() noexcept;
  [[nodiscard]] bool running() const noexcept { return started_ != 0; }

 private:
  struct Ctx {
    PipelineRunner* runner = nullptr;
    Stage stage = Stage::kAcquire;
  };
  static void* thread_main(void* arg) noexcept;
  Pipeline* pipeline_ = nullptr;
  std::atomic<bool> stop_{false};
  Ctx ctx_[3] = {};
  pthread_t thread_[3] = {};
  std::uint32_t started_ = 0;
};

}  // namespace st
