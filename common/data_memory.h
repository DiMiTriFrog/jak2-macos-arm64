#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(OS_POSIX) || defined(__APPLE__) || defined(__unix__)
#define OG_DATA_MEMORY_POSIX 1
#include <unistd.h>

#include <sys/mman.h>
#elif defined(_WIN32)
#define OG_DATA_MEMORY_POSIX 0
#include "third-party/mman/mman.h"
#else
#define OG_DATA_MEMORY_POSIX 0
#endif

namespace data_memory {

inline size_t page_size() {
#if OG_DATA_MEMORY_POSIX
  const auto result = sysconf(_SC_PAGESIZE);
  if (result <= 0) {
    throw std::system_error(errno, std::generic_category(), "sysconf(_SC_PAGESIZE)");
  }
  return static_cast<size_t>(result);
#else
  return 4096;
#endif
}

inline size_t round_up_to_page(size_t size) {
  const auto page = page_size();
  if (size == 0 || size > std::numeric_limits<size_t>::max() - (page - 1)) {
    throw std::invalid_argument("data region size is invalid");
  }
  return (size + page - 1) & ~(page - 1);
}

inline void make_no_access(void* address, size_t size) {
#if OG_DATA_MEMORY_POSIX || defined(_WIN32)
  if (!address || size == 0) {
    throw std::invalid_argument("data region protection range is invalid");
  }
  const auto page = page_size();
  const auto begin = reinterpret_cast<uintptr_t>(address);
  const auto end = begin + size;
  if (end < begin || begin < page) {
    throw std::invalid_argument("data region protection range overflows");
  }
  const auto aligned_begin = begin & ~(static_cast<uintptr_t>(page) - 1);
  const auto aligned_end = (end + page - 1) & ~(static_cast<uintptr_t>(page) - 1);
  if (aligned_end < aligned_begin) {
    throw std::invalid_argument("data region protection range overflows");
  }
  if (mprotect(reinterpret_cast<void*>(aligned_begin), aligned_end - aligned_begin, PROT_NONE) !=
      0) {
    throw std::system_error(errno, std::generic_category(), "mprotect data region");
  }
#else
  (void)address;
  (void)size;
#endif
}

class DataRegion {
 public:
  static DataRegion allocate(size_t requested_size, void* hint = nullptr) {
    const auto mapped_size = round_up_to_page(requested_size);
#if OG_DATA_MEMORY_POSIX || defined(_WIN32)
    // This mapping is deliberately never executable and never uses MAP_JIT.
    void* mapped = mmap(hint, mapped_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE,
#if OG_DATA_MEMORY_POSIX
                        -1,
#else
                        0,
#endif
                        0);
    if (mapped == MAP_FAILED) {
      throw std::system_error(errno, std::generic_category(), "mmap data region");
    }
    return DataRegion(mapped, mapped_size);
#else
    (void)hint;
    (void)mapped_size;
    throw std::runtime_error("data regions are unsupported on this platform");
#endif
  }

  DataRegion(const DataRegion&) = delete;
  DataRegion& operator=(const DataRegion&) = delete;

  DataRegion(DataRegion&& other) noexcept : address_(other.address_), size_(other.size_) {
    other.address_ = nullptr;
    other.size_ = 0;
  }

  DataRegion& operator=(DataRegion&& other) noexcept {
    if (this != &other) {
      release();
      address_ = other.address_;
      size_ = other.size_;
      other.address_ = nullptr;
      other.size_ = 0;
    }
    return *this;
  }

  ~DataRegion() { release(); }

  void* data() const { return address_; }
  size_t size() const { return size_; }

 private:
  DataRegion(void* address, size_t size) : address_(address), size_(size) {}

  void release() noexcept {
#if OG_DATA_MEMORY_POSIX || defined(_WIN32)
    if (address_) {
      munmap(address_, size_);
    }
#endif
    address_ = nullptr;
    size_ = 0;
  }

  void* address_ = nullptr;
  size_t size_ = 0;
};

}  // namespace data_memory
