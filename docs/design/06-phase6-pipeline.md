# Phase 6: pipeline, frame sources, benchmarks

Status: **implemented** (2026-10-27). Written as built.

## 1. Pipeline (`include/st/pipeline/pipeline.hpp`, core)

* A fixed pool of frame slots (pixels, centroids, solution, timestamps) is allocated from the
  caller's arena in `init()`. Slot indices circulate through three wait-free SPSC queues:
  free → acquired → centroided → free. Outputs go to a fourth SPSC queue (`poll()`) and,
  optionally, to a callback on the solve stage, which must be real-time safe.
* `PipelineOutput`: quaternion, covariance, rate, match count, mode, RA/Dec/roll, centroid
  count, ROI flag, and four timestamps: capture (source clock), then acquired, centroided and
  solved (pipeline clock). `latency_ns()` = solved − acquired.
* **Single-threaded** (`step()`): one frame through every stage on the caller's thread. While
  tracking it centroids only the tracker's predicted ROIs.
* **Threaded** (`run_stage()`): each stage loop runs on its own thread. The core cannot
  create threads, because `std::thread` allocates and the symbol audit forbids that. So
  `st_platform`'s `PipelineRunner` starts them with pthreads, with optional SCHED_FIFO
  priority, CPU affinity and stack size. Idle stages yield. In this mode every frame is
  centroided in full: an ROI prediction would read the tracker from the centroid stage while
  the solve stage updates it, which is a data race.
* **Shutdown**: the acquire stage stops on `stop()` or at end of stream. The centroid and
  solve stages drain every queued frame, then exit once their upstream stage has.
  `finished()` turns true after the last output is published.
* **Drop policies** (when the pipeline falls behind):
  * `kDropNewest`: with no free slot, the frame is read into a reserved scratch slot and
    discarded. The source is never stalled, so a camera driver does not overrun.
  * `kKeepLatest`: the centroid stage takes the newest queued frame. Stale frames are
    flagged and returned through the solve stage without being solved.
  * `kBlock`: the acquire stage waits for a slot. Used for deterministic replay.
  * Invariant (tested): processed + dropped_no_slot + dropped_stale = acquired.
* **Worst case per frame**: source copy O(W·H) + centroiding O(W·H) (or O(ROIs)) + one
  tracker update or one budgeted lost-in-space solve + O(1) queue hand-offs.

## 2. Frame sources

* `FrameSource` is an abstract interface. `acquire(dst, timeout, info)` returns kOk,
  kTimeout, kCancelled (end of stream) or kIoError, and never allocates.
* `MemoryFrameSource` (core, header-only): frames already in memory, with optional looping.
  Used by tests and benchmarks.
* `ReplaySource` (platform): a memory-mapped `.strp` file. Layout:
  * a 64-byte header (magic `STRPLAY1`, version, size, pixel format, frame count, stride);
  * then per frame: a timestamp, a sequence number and the pixels.

  It can pace frames at their recorded timestamps (× speed) and loop. Header and size are
  validated (kCorruptData).
* `V4L2Source` (platform, Linux): MMAP streaming I/O with up to 8 driver buffers.
  * Formats: GREY, Y16, Y10 and Y12. Rows are copied honouring `bytesperline`, and the
    driver's timestamp and sequence number are passed through.
  * Waiting uses poll() with the given timeout.
  * On other systems `open()` returns kUnsupported.
  * Only compile and error-path tests run here (no camera in CI). It has not been run
    against a real device.

## 3. Tools

* `st_replay pack | render | run`:
  * `pack` turns PGMs into a `.strp`.
  * `render` writes a rotating synthetic sequence plus a `.truth` file.
  * `run` streams a file through the pipeline (either mode, any drop policy, optional
    pacing) and reports per-frame output, the latency distribution and the error vs truth.
* `st_bench` reports:
  * median, p99 and max for full-frame centroiding, ROI centroiding, lost-in-space solve,
    tracker update, and the per-frame pipeline latency in both modes (the threaded run is
    paced at 20 Hz, so queueing behind a flooding source is excluded);
  * arena bytes per component, database size and peak RSS;
  * host CPU, kernel, architecture and compiler.

  Without `--db` it generates a synthetic database. Output is Markdown and JSON.

## 4. Measured (Intel Xeon @ 2.80 GHz, 4 vCPU container, Linux x86_64, GCC 11.5 -O3)

ESA default database (99.2 MB, 8818 stars), 40 rendered 1280×960 mono16 frames, FOV 14°,
0.5°/s, 10 Hz:

| measurement | median ms | p99 ms | max ms |
|---|---|---|---|
| centroid, full frame | 15.49 | 22.48 | 22.48 |
| centroid, 32 ROIs | 0.077 | 0.161 | 0.161 |
| lost-in-space solve | 0.277 | 0.366 | 0.366 |
| tracker update | 0.023 | 0.080 | 0.080 |
| pipeline frame, single-threaded (ROI tracking) | 0.075 | 15.21 | 15.21 |
| pipeline frame, threaded (full-frame centroids) | 15.46 | 24.37 | 24.37 |

* The single-threaded p99 and max are the first frame, which is a full-frame lost-in-space
  acquisition.
* Arena use: centroider 17.6 MB (float work images at 1280×960), solver 67 kB, sky index
  91 kB, tracker 10 kB, pipeline with 4 slots 27.6 MB (centroider plus 4 frame buffers).
* Attitude error vs truth (`st_replay run`, 50 frames): median 2.8″ and p99 7.9″ with ROI
  tracking; median 3.0″ and p99 13″ with full-frame centroids.
* ARM64, from the CI `bench` job: GitHub-hosted `ubuntu-24.04-arm` runner, CPU part 0xd49
  (Arm Neoverse N2), 2 vCPU, GCC 14.2 -O3, synthetic database, same 1280×960 frames.
  Shared runners are noisy, so treat these as indicative. Nothing is claimed for a flight
  processor or a Raspberry Pi.

  | measurement | median ms | p99 ms |
  |---|---|---|
  | centroid, full frame | 10.94 | 11.11 |
  | centroid, 32 ROIs | 0.033 | 0.035 |
  | lost-in-space solve | 0.038 | 0.162 |
  | tracker update | 0.018 | 0.029 |
  | pipeline frame, single-threaded (ROI tracking) | 0.053 | 11.15 |
  | pipeline frame, threaded | 10.92 | 11.15 |

## 5. Verification

* Unit tests (`tests/unit/pipeline_test.cpp`):
  * single-threaded tracking with ROIs, zero allocations across all steps;
  * threaded kBlock replay from a `.strp` file, zero allocations process-wide while all three
    stage threads run (allocation guard; skipped under TSan, whose `aligned_alloc` is not
    hooked);
  * kDropNewest and kKeepLatest under a deliberately slow solve stage (accounting
    invariant, ordered outputs);
  * `stop()` on an endless looping source;
  * invalid configurations; `workspace_bytes()` is sufficient;
  * replay pacing, looping, truncation and missing files; the V4L2 missing-device path.
* ASan+UBSan and TSan (5 repeated runs) are clean.
* UBSan's `vptr` check is now disabled: it needs RTTI typeinfo for polymorphic classes,
  which the core and platform are built without (`ReplaySource`, `V4L2Source`).
* CI: a `bench` job on x64 and arm64 runners; the parity job runs `st_replay` and `st_bench`
  on the ESA database and writes the reports to the job summary.
