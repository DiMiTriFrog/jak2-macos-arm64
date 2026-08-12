#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "common/aot/AotFunction.h"

namespace goal_aot {

enum class InvocationError : std::uint8_t {
  None,
  RegistryNotInstalled,
  InvalidDescriptorAddress,
  ResolutionFailed,
};

struct InvocationResult {
  std::uint64_t value = 0;
  InvocationError error = InvocationError::None;
  ResolveError resolve_error = ResolveError::None;

  explicit operator bool() const { return error == InvocationError::None; }
};

class ExecutionBackend {
 public:
  void install(const Registry* registry);
  void clear();
  bool installed() const { return registry_ != nullptr; }

  InvocationResult invoke(const FunctionDescriptor* descriptor,
                          std::span<const std::uint64_t> args,
                          std::uint64_t process_pointer,
                          std::uint64_t goal_stack_pointer,
                          std::uintptr_t ee_base) const;

 private:
  const Registry* registry_ = nullptr;
};

ExecutionBackend& execution_backend();

// Registers a process-local C++ bridge and installs the resulting immutable
// registry. The returned ID is the only value written into EE memory.
FunctionId register_native_bridge(void* function,
                                  std::uint32_t flags,
                                  std::string name,
                                  void* target = nullptr);

const char* invocation_error_name(InvocationError error);
const char* resolve_error_name(ResolveError error);

}  // namespace goal_aot
