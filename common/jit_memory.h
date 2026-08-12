#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "common/common_types.h"
#include "common/platform/BuildConfig.h"

#ifdef OS_POSIX
#include <unistd.h>

#include <sys/mman.h>
#elif defined(_WIN32)
#include "third-party/mman/mman.h"
#endif

namespace jit_memory {

enum class Protection { Writable, Executable, NoAccess };

#if defined(__APPLE__) && defined(__aarch64__) && !OG_EXECUTION_MODE_AOT
#if !defined(MAP_JIT)
#error "Apple ARM64 builds require MAP_JIT"
#endif
inline constexpr bool kRequiresWxorX = true;
#else
inline constexpr bool kRequiresWxorX = false;
#endif

inline size_t page_size() {
#ifdef OS_POSIX
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
    throw std::invalid_argument("JIT region size is invalid");
  }
  return (size + page - 1) & ~(page - 1);
}

struct AlignedRange {
  void* address;
  size_t size;
};

inline AlignedRange aligned_range(void* address, size_t size) {
  if (!address || size == 0) {
    throw std::invalid_argument("JIT protection range is invalid");
  }

  const auto page = page_size();
  const auto begin = reinterpret_cast<uintptr_t>(address);
  const auto end = begin + size;
  if (end < begin || begin < page) {
    throw std::invalid_argument("JIT protection range overflows");
  }

  const auto aligned_begin = begin & ~(static_cast<uintptr_t>(page) - 1);
  const auto aligned_end = (end + page - 1) & ~(static_cast<uintptr_t>(page) - 1);
  if (aligned_end < aligned_begin) {
    throw std::invalid_argument("JIT protection range overflows");
  }
  return {reinterpret_cast<void*>(aligned_begin), aligned_end - aligned_begin};
}

inline int protection_flags(Protection protection) {
#ifdef OS_POSIX
  switch (protection) {
    case Protection::Writable:
      return PROT_READ | PROT_WRITE;
    case Protection::Executable:
      return PROT_READ | PROT_EXEC;
    case Protection::NoAccess:
      return PROT_NONE;
  }
#elif defined(_WIN32)
  switch (protection) {
    case Protection::Writable:
      return PROT_READ | PROT_WRITE;
    case Protection::Executable:
      return PROT_READ | PROT_EXEC;
    case Protection::NoAccess:
      return PROT_NONE;
  }
#else
  (void)protection;
#endif
  throw std::invalid_argument("unknown JIT protection");
}

inline void set_protection(void* address, size_t size, Protection protection) {
#if OG_EXECUTION_MODE_AOT
  if (protection == Protection::Executable) {
    throw std::logic_error("executable anonymous memory is disabled in AOT mode");
  }
#endif
#if defined(OS_POSIX) || defined(_WIN32)
  if (protection != Protection::NoAccess && !kRequiresWxorX) {
    return;
  }
  const auto range = aligned_range(address, size);
  if (mprotect(range.address, range.size, protection_flags(protection)) != 0) {
    throw std::system_error(errno, std::generic_category(), "mprotect JIT region");
  }
#else
  (void)address;
  (void)size;
  (void)protection;
#endif
}

inline void flush_instruction_cache(void* address, size_t size) {
#if defined(__aarch64__)
  if (!address || size == 0) {
    throw std::invalid_argument("JIT flush range is invalid");
  }
  __builtin___clear_cache(static_cast<char*>(address), static_cast<char*>(address) + size);
#else
  (void)address;
  (void)size;
#endif
}

inline void make_writable(void* address, size_t size) {
  set_protection(address, size, Protection::Writable);
}

inline void make_executable(void* address, size_t size) {
  flush_instruction_cache(address, size);
  set_protection(address, size, Protection::Executable);
}

inline void make_no_access(void* address, size_t size) {
  set_protection(address, size, Protection::NoAccess);
}

inline const char* protection_name(Protection protection) {
  switch (protection) {
    case Protection::Writable:
      return "rw";
    case Protection::Executable:
      return "rx";
    case Protection::NoAccess:
      return "none";
  }
  return "unknown";
}

class JitRegion;

class JitWriteScope {
 public:
  JitWriteScope(void* address, size_t size) : address_(address), size_(size), active_(true) {
    make_writable(address_, size_);
  }

  explicit JitWriteScope(JitRegion& region);

  JitWriteScope(const JitWriteScope&) = delete;
  JitWriteScope& operator=(const JitWriteScope&) = delete;
  JitWriteScope(JitWriteScope&& other) noexcept
      : address_(other.address_),
        size_(other.size_),
        region_(other.region_),
        active_(other.active_) {
    other.active_ = false;
    other.region_ = nullptr;
  }
  JitWriteScope& operator=(JitWriteScope&& other) noexcept {
    if (this != &other) {
      finish_or_terminate();
      address_ = other.address_;
      size_ = other.size_;
      region_ = other.region_;
      active_ = other.active_;
      other.active_ = false;
      other.region_ = nullptr;
    }
    return *this;
  }

  void flush_instruction_cache() {
    if (!active_) {
      throw std::logic_error("JIT write scope is inactive");
    }
    jit_memory::flush_instruction_cache(address_, size_);
  }

  void finish() { finish_or_terminate(); }

  ~JitWriteScope();

 private:
  void finish_or_terminate() noexcept;

  void* address_;
  size_t size_;
  JitRegion* region_ = nullptr;
  bool active_;
};

class JitRegion {
 public:
  static JitRegion allocate(size_t requested_size, void* hint = nullptr) {
    const auto mapped_size = round_up_to_page(requested_size);
#if defined(OS_POSIX) || defined(_WIN32)
    int flags = MAP_ANONYMOUS | MAP_PRIVATE;
#if defined(__APPLE__) && defined(__aarch64__) && !OG_EXECUTION_MODE_AOT
    flags |= MAP_JIT;
#endif
    const int prot =
        kRequiresWxorX ? (PROT_READ | PROT_WRITE) : (PROT_READ | PROT_WRITE | PROT_EXEC);
    void* mapped = mmap(hint, mapped_size, prot, flags,
#ifdef OS_POSIX
                        -1,
#else
                        0,
#endif
                        0);
    if (mapped == MAP_FAILED) {
      throw std::system_error(errno, std::generic_category(), "mmap JIT region");
    }
    return JitRegion(mapped, mapped_size);
#else
    (void)hint;
    (void)mapped_size;
    throw std::runtime_error("JIT regions are unsupported on this platform");
#endif
  }

  JitRegion(const JitRegion&) = delete;
  JitRegion& operator=(const JitRegion&) = delete;

  JitRegion(JitRegion&& other) noexcept : address_(other.address_), size_(other.size_) {
    other.address_ = nullptr;
    other.size_ = 0;
  }
  JitRegion& operator=(JitRegion&& other) noexcept {
    if (this != &other) {
      release();
      address_ = other.address_;
      size_ = other.size_;
      other.address_ = nullptr;
      other.size_ = 0;
    }
    return *this;
  }

  ~JitRegion() { release(); }

  void* data() const { return address_; }
  size_t size() const { return size_; }
  bool contains(const void* address, size_t size) const {
    const auto begin = reinterpret_cast<uintptr_t>(address_);
    const auto end = begin + size_;
    const auto requested_begin = reinterpret_cast<uintptr_t>(address);
    const auto requested_end = requested_begin + size;
    return address_ && requested_end >= requested_begin && requested_begin >= begin &&
           requested_end <= end;
  }

  void make_writable() {
    jit_memory::make_writable(address_, size_);
    protection_ = Protection::Writable;
  }
  void make_executable() {
    jit_memory::make_executable(address_, size_);
    protection_ = Protection::Executable;
  }
  void make_no_access() {
    jit_memory::make_no_access(address_, size_);
    protection_ = Protection::NoAccess;
  }
  void flush_instruction_cache(void* begin, size_t size) const {
    if (!contains(begin, size)) {
      throw std::invalid_argument("JIT flush range is outside its region");
    }
    jit_memory::flush_instruction_cache(begin, size);
  }
  JitWriteScope write_scope() {
    protection_ = Protection::Writable;
    return JitWriteScope(*this);
  }
  Protection protection() const { return protection_; }

 private:
  friend class JitWriteScope;

  JitRegion(void* address, size_t size) : address_(address), size_(size) {}

  void release() noexcept {
#if defined(OS_POSIX) || defined(_WIN32)
    if (address_) {
      munmap(address_, size_);
    }
#endif
    address_ = nullptr;
    size_ = 0;
  }

  void* address_ = nullptr;
  size_t size_ = 0;
  Protection protection_ = Protection::Writable;
};

inline JitWriteScope::JitWriteScope(JitRegion& region)
    : address_(region.data()), size_(region.size()), region_(&region), active_(true) {
  if (!address_ || size_ == 0) {
    throw std::invalid_argument("cannot write an empty JIT region");
  }
  make_writable(address_, size_);
}

inline JitWriteScope::~JitWriteScope() {
  finish_or_terminate();
}

inline void JitWriteScope::finish_or_terminate() noexcept {
  if (!active_) {
    return;
  }
  try {
    make_executable(address_, size_);
    if (region_) {
      region_->protection_ = Protection::Executable;
    }
  } catch (...) {
    std::terminate();
  }
  active_ = false;
}

}  // namespace jit_memory
