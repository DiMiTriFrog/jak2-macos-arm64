#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace goal_aot {

using FunctionId = std::uint32_t;

inline constexpr std::uint32_t kFunctionDescriptorMagic = 0x464f5441;  // "AOTF"
inline constexpr std::uint32_t kFunctionAbiVersion = 1;

enum FunctionFlags : std::uint32_t {
  kNone = 0,
  kAcceptsProcessPointer = 1u << 0,
  kUsesGoalStack = 1u << 1,
  kMips2c = 1u << 2,
};

// This is the only call context initially exposed to statically linked AOT entries.  The layout
// is deliberately fixed so generated assembly and C++ wrappers can share it without depending on
// std::vector, exceptions, or host pointers stored in EE memory.
struct GoalCallContext {
  std::array<std::uint64_t, 8> args{};
  std::uint32_t arg_count = 0;
  std::uint32_t flags = 0;
  std::uint64_t process_pointer = 0;
  std::uint64_t goal_stack_pointer = 0;
  std::uint64_t ee_base = 0;
  // Process-local target consumed by an adapter; never serialized into EE memory.
  void* native_target = nullptr;
};

using EntryPoint = std::uint64_t (*)(GoalCallContext* context);

// This payload is stored in the RW EE data region. It is a handle, not a native function pointer.
struct FunctionDescriptor {
  std::uint32_t magic = kFunctionDescriptorMagic;
  FunctionId id = 0;
  std::uint32_t generation = 0;
  std::uint32_t flags = kNone;
};
static_assert(sizeof(FunctionDescriptor) == 16);

struct NativeEntry {
  FunctionId id = 0;
  std::uint32_t generation = 0;
  std::uint32_t flags = kNone;
  std::uint32_t abi_version = kFunctionAbiVersion;
  EntryPoint entry = nullptr;
  const char* name = nullptr;
  // Optional process-local bridge target. It is never stored in the EE
  // descriptor; it exists only for the static C++ bridge registry.
  void* native_function = nullptr;
  // Optional target consumed by native_function. It allows one static ABI shim
  // to dispatch to multiple linked functions.
  void* native_target = nullptr;
};

enum class ResolveError : std::uint8_t {
  None,
  InvalidMagic,
  NullId,
  InvalidGeneration,
  UnknownId,
  GenerationMismatch,
  AbiMismatch,
  MissingEntryPoint,
  InvalidRegistry,
};

struct ResolveResult {
  const NativeEntry* entry = nullptr;
  ResolveError error = ResolveError::None;

  explicit operator bool() const { return entry != nullptr && error == ResolveError::None; }
};

class Registry {
 public:
  explicit Registry(std::span<const NativeEntry> entries);

  bool valid() const { return valid_; }
  std::span<const NativeEntry> entries() const { return entries_; }
  ResolveResult resolve(const FunctionDescriptor& descriptor) const;

 private:
  std::span<const NativeEntry> entries_;
  bool valid_ = false;
};

FunctionDescriptor make_descriptor(FunctionId id, std::uint32_t generation, std::uint32_t flags);

}  // namespace goal_aot
