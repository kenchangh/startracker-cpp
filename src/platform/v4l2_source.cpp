// SPDX-License-Identifier: Apache-2.0
#include "st/platform/v4l2_source.hpp"

#include <cstring>

#if defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace st {

V4L2Source::V4L2Source(V4L2Source&& o) noexcept { *this = static_cast<V4L2Source&&>(o); }

V4L2Source& V4L2Source::operator=(V4L2Source&& o) noexcept {
  if (this != &o) {
    close();
    fd_ = o.fd_;
    fmt_ = o.fmt_;
    bytesperline_ = o.bytesperline_;
    nbuf_ = o.nbuf_;
    for (std::uint32_t i = 0; i < kMaxBuffers; ++i) {
      buf_[i] = o.buf_[i];
      len_[i] = o.len_[i];
      o.buf_[i] = nullptr;
      o.len_[i] = 0;
    }
    o.fd_ = -1;
    o.nbuf_ = 0;
  }
  return *this;
}

V4L2Source::~V4L2Source() { close(); }

#if defined(__linux__)

namespace {
int xioctl(int fd, unsigned long req, void* arg) noexcept {
  int r = 0;
  do {
    r = ::ioctl(fd, req, arg);
  } while (r == -1 && errno == EINTR);
  return r;
}
}  // namespace

void V4L2Source::close() noexcept {
  if (fd_ >= 0) {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    static_cast<void>(xioctl(fd_, VIDIOC_STREAMOFF, &type));
  }
  for (std::uint32_t i = 0; i < nbuf_; ++i) {
    if (buf_[i] != nullptr) ::munmap(buf_[i], len_[i]);
    buf_[i] = nullptr;
  }
  nbuf_ = 0;
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

Result<V4L2Source> V4L2Source::open(const char* device, const V4L2Options& o) noexcept {
  if (device == nullptr || o.buffers < 2 || o.buffers > kMaxBuffers)
    return Status::kInvalidArgument;
  V4L2Source s;
  s.fd_ = ::open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (s.fd_ < 0) return Status::kIoError;
  v4l2_capability cap{};
  if (xioctl(s.fd_, VIDIOC_QUERYCAP, &cap) != 0) return Status::kIoError;
  const std::uint32_t caps =
      (cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0 ? cap.device_caps : cap.capabilities;
  if ((caps & V4L2_CAP_VIDEO_CAPTURE) == 0 || (caps & V4L2_CAP_STREAMING) == 0)
    return Status::kUnsupported;

  v4l2_format f{};
  f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(s.fd_, VIDIOC_G_FMT, &f) != 0) return Status::kIoError;
  if (o.width != 0) f.fmt.pix.width = o.width;
  if (o.height != 0) f.fmt.pix.height = o.height;
  f.fmt.pix.pixelformat = o.format == PixelFormat::kMono8 ? V4L2_PIX_FMT_GREY : V4L2_PIX_FMT_Y16;
  f.fmt.pix.field = V4L2_FIELD_NONE;
  if (xioctl(s.fd_, VIDIOC_S_FMT, &f) != 0) return Status::kIoError;
  const std::uint32_t pf = f.fmt.pix.pixelformat;
  if (pf == V4L2_PIX_FMT_GREY) {
    s.fmt_.format = PixelFormat::kMono8;
  } else if (pf == V4L2_PIX_FMT_Y16 || pf == V4L2_PIX_FMT_Y10 || pf == V4L2_PIX_FMT_Y12) {
    s.fmt_.format = PixelFormat::kMono16;
  } else {
    return Status::kUnsupported;
  }
  s.fmt_.width = f.fmt.pix.width;
  s.fmt_.height = f.fmt.pix.height;
  s.bytesperline_ = f.fmt.pix.bytesperline;
  const std::uint32_t bpp = s.fmt_.format == PixelFormat::kMono8 ? 1 : 2;
  if (s.bytesperline_ < s.fmt_.width * bpp) s.bytesperline_ = s.fmt_.width * bpp;

  v4l2_requestbuffers req{};
  req.count = o.buffers;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(s.fd_, VIDIOC_REQBUFS, &req) != 0) return Status::kIoError;
  if (req.count < 2 || req.count > kMaxBuffers) return Status::kUnsupported;
  for (std::uint32_t i = 0; i < req.count; ++i) {
    v4l2_buffer b{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = i;
    if (xioctl(s.fd_, VIDIOC_QUERYBUF, &b) != 0) return Status::kIoError;
    void* p = ::mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, s.fd_, b.m.offset);
    if (p == MAP_FAILED) return Status::kIoError;
    s.buf_[i] = p;
    s.len_[i] = b.length;
    s.nbuf_ = i + 1;
    if (xioctl(s.fd_, VIDIOC_QBUF, &b) != 0) return Status::kIoError;
  }
  v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(s.fd_, VIDIOC_STREAMON, &type) != 0) return Status::kIoError;
  return s;
}

Status V4L2Source::acquire(std::span<std::byte> dst, std::int64_t timeout_ns,
                           FrameInfo& info) noexcept {
  if (fd_ < 0) return Status::kNotInitialized;
  const std::size_t n = fmt_.bytes();
  if (dst.size() < n) return Status::kInvalidArgument;
  pollfd p{fd_, POLLIN, 0};
  const int ms = timeout_ns <= 0 ? 0 : static_cast<int>((timeout_ns + 999'999) / 1'000'000);
  const int r = ::poll(&p, 1, ms);
  if (r == 0) return Status::kTimeout;
  if (r < 0) return errno == EINTR ? Status::kTimeout : Status::kIoError;
  v4l2_buffer b{};
  b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  b.memory = V4L2_MEMORY_MMAP;
  if (xioctl(fd_, VIDIOC_DQBUF, &b) != 0)
    return errno == EAGAIN ? Status::kTimeout : Status::kIoError;
  Status st = Status::kOk;
  const std::size_t row = std::size_t{fmt_.width} * (fmt_.format == PixelFormat::kMono8 ? 1 : 2);
  if (b.index >= nbuf_ || std::size_t{bytesperline_} * (fmt_.height - 1) + row > len_[b.index] ||
      (b.flags & V4L2_BUF_FLAG_ERROR) != 0) {
    st = Status::kIoError;
  } else {
    const auto* src = static_cast<const std::byte*>(buf_[b.index]);
    for (std::uint32_t y = 0; y < fmt_.height; ++y) {
      std::memcpy(dst.data() + y * row, src + std::size_t{y} * bytesperline_, row);
    }
    info.t_capture_ns = static_cast<std::int64_t>(b.timestamp.tv_sec) * 1'000'000'000 +
                        static_cast<std::int64_t>(b.timestamp.tv_usec) * 1000;
    info.source_seq = b.sequence;
  }
  if (xioctl(fd_, VIDIOC_QBUF, &b) != 0) return Status::kIoError;
  return st;
}

#else  // !__linux__

void V4L2Source::close() noexcept {}
Result<V4L2Source> V4L2Source::open(const char* device, const V4L2Options& o) noexcept {
  if (device == nullptr || o.buffers < 2 || o.buffers > kMaxBuffers)
    return Status::kInvalidArgument;
  return Status::kUnsupported;
}
Status V4L2Source::acquire(std::span<std::byte>, std::int64_t, FrameInfo&) noexcept {
  return Status::kUnsupported;
}

#endif

}  // namespace st
