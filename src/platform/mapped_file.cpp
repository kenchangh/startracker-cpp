// SPDX-License-Identifier: Apache-2.0
#include "st/platform/mapped_file.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace st {

MappedFile::MappedFile(MappedFile&& o) noexcept : data_(o.data_), size_(o.size_) {
  o.data_ = nullptr;
  o.size_ = 0;
}

MappedFile& MappedFile::operator=(MappedFile&& o) noexcept {
  if (this != &o) {
    if (data_ != nullptr) ::munmap(const_cast<std::byte*>(data_), size_);
    data_ = o.data_;
    size_ = o.size_;
    o.data_ = nullptr;
    o.size_ = 0;
  }
  return *this;
}

MappedFile::~MappedFile() {
  if (data_ != nullptr) ::munmap(const_cast<std::byte*>(data_), size_);
}

Result<MappedFile> MappedFile::open(const char* path, bool prefault) noexcept {
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return Status::kIoError;
  struct stat st {};
  if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
    ::close(fd);
    return Status::kIoError;
  }
  int flags = MAP_PRIVATE;
#ifdef MAP_POPULATE
  if (prefault) flags |= MAP_POPULATE;
#else
  static_cast<void>(prefault);
#endif
  void* p = ::mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, flags, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) return Status::kIoError;
  MappedFile f;
  f.data_ = static_cast<const std::byte*>(p);
  f.size_ = static_cast<std::size_t>(st.st_size);
  return f;
}

Status MappedFile::lock() noexcept {
  if (data_ == nullptr) return Status::kNotInitialized;
  return ::mlock(data_, size_) == 0 ? Status::kOk : Status::kIoError;
}

}  // namespace st
