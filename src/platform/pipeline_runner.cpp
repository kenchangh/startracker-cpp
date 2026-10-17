// SPDX-License-Identifier: Apache-2.0
#include "st/platform/pipeline_runner.hpp"

#include <errno.h>
#include <sched.h>

namespace st {

PipelineRunner::~PipelineRunner() {
  stop();
  join();
}

void* PipelineRunner::thread_main(void* arg) noexcept {
  auto* c = static_cast<Ctx*>(arg);
  c->runner->pipeline_->run_stage(c->stage, c->runner->stop_);
  return nullptr;
}

Status PipelineRunner::start(Pipeline& pipeline) noexcept {
  const StageThreadConfig none[3] = {};
  return start(pipeline, none);
}

Status PipelineRunner::start(Pipeline& pipeline, const StageThreadConfig (&threads)[3]) noexcept {
  if (started_ != 0 || pipeline.config().mode != PipelineMode::kThreaded)
    return Status::kInvalidArgument;
  pipeline_ = &pipeline;
  stop_.store(false, std::memory_order_relaxed);
  Status result = Status::kOk;
  // Start downstream stages first so the acquire stage never runs ahead of a missing consumer.
  for (int k = 2; k >= 0; --k) {
    const auto i = static_cast<std::size_t>(k);
    ctx_[i] = {this, static_cast<Stage>(k)};
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
      result = Status::kIoError;
      break;
    }
    const StageThreadConfig& t = threads[i];
    if (t.stack_bytes != 0 && pthread_attr_setstacksize(&attr, t.stack_bytes) != 0)
      result = Status::kInvalidArgument;
    if (result == Status::kOk && t.priority > 0) {
      sched_param sp{};
      sp.sched_priority = t.priority;
      if (pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) != 0 ||
          pthread_attr_setschedpolicy(&attr, SCHED_FIFO) != 0 ||
          pthread_attr_setschedparam(&attr, &sp) != 0) {
        result = Status::kUnsupported;
      }
    }
#if defined(__linux__)
    if (result == Status::kOk && t.cpu >= 0) {
      cpu_set_t set;
      CPU_ZERO(&set);
      CPU_SET(static_cast<std::size_t>(t.cpu), &set);
      if (pthread_attr_setaffinity_np(&attr, sizeof(set), &set) != 0) result = Status::kUnsupported;
    }
#else
    if (t.cpu >= 0) result = Status::kUnsupported;
#endif
    if (result == Status::kOk) {
      const int rc = pthread_create(&thread_[i], &attr, &PipelineRunner::thread_main, &ctx_[i]);
      if (rc != 0) result = rc == EPERM ? Status::kUnsupported : Status::kIoError;
    }
    pthread_attr_destroy(&attr);
    if (result != Status::kOk) break;
    started_ |= 1u << i;
  }
  if (result != Status::kOk) {
    // Downstream stages exit only after the acquire stage; run a no-op acquire stage here
    // so they drain and finish.
    if ((started_ & 1u) == 0 && started_ != 0) {
      stop_.store(true, std::memory_order_relaxed);
      pipeline.run_stage(Stage::kAcquire, stop_);
    }
    join();
  }
  return result;
}

void PipelineRunner::join() noexcept {
  for (std::size_t i = 0; i < 3; ++i) {
    if ((started_ & (1u << i)) != 0) pthread_join(thread_[i], nullptr);
  }
  started_ = 0;
}

}  // namespace st
