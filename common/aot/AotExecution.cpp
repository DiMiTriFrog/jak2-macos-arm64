#include "common/aot/AotExecution.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace goal_aot {
namespace {

struct BridgeKey {
  std::uintptr_t function = 0;
  std::uintptr_t target = 0;
  std::uint32_t flags = 0;

  bool operator==(const BridgeKey& other) const {
    return function == other.function && target == other.target && flags == other.flags;
  }
};

struct BridgeKeyHash {
  size_t operator()(const BridgeKey& key) const {
    return std::hash<std::uintptr_t>{}(key.function) ^
           (std::hash<std::uintptr_t>{}(key.target) << 2) ^
           (std::hash<std::uint32_t>{}(key.flags) << 1);
  }
};

struct NativeBridgeRegistry {
  std::vector<NativeEntry> entries;
  std::vector<std::string> names;
  std::unordered_map<BridgeKey, FunctionId, BridgeKeyHash> ids;
  std::unique_ptr<Registry> registry;
  FunctionId next_id = 0x80000000u;

  NativeBridgeRegistry() {
    entries.reserve(4096);
    names.reserve(4096);
  }
};

NativeBridgeRegistry& native_bridges() {
  static NativeBridgeRegistry state;
  return state;
}

std::uint64_t invoke_native(const NativeEntry& entry, GoalCallContext* context) {
  context->native_target = entry.native_target;
  if (entry.flags & kMips2c) {
    using ContextFunction = std::uint64_t (*)(GoalCallContext*);
    return reinterpret_cast<ContextFunction>(entry.native_function)(context);
  }
  if (entry.flags & kUsesGoalStack) {
    using StackFunction = std::uint64_t (*)(std::uint64_t*);
    return reinterpret_cast<StackFunction>(entry.native_function)(context->args.data());
  }

  std::array<std::uint64_t, 8> args = context->args;
  if (entry.flags & kAcceptsProcessPointer) {
    args[3] = context->process_pointer;
  }
  using NativeFunction = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t,
                                           std::uint64_t, std::uint64_t, std::uint64_t,
                                           std::uint64_t, std::uint64_t);
  return reinterpret_cast<NativeFunction>(entry.native_function)(args[0], args[1], args[2], args[3],
                                                                  args[4], args[5], args[6], args[7]);
}

}  // namespace

void ExecutionBackend::install(const Registry* registry) {
  if (!registry || !registry->valid()) {
    throw std::invalid_argument("cannot install an invalid AOT registry");
  }
  registry_ = registry;
}

void ExecutionBackend::clear() {
  registry_ = nullptr;
}

InvocationResult ExecutionBackend::invoke(const FunctionDescriptor* descriptor,
                                          std::span<const std::uint64_t> args,
                                          std::uint64_t process_pointer,
                                          std::uint64_t goal_stack_pointer,
                                          std::uintptr_t ee_base) const {
  if (!registry_) {
    return {0, InvocationError::RegistryNotInstalled, ResolveError::None};
  }
  if (!descriptor ||
      (reinterpret_cast<std::uintptr_t>(descriptor) & (alignof(std::uint32_t) - 1))) {
    return {0, InvocationError::InvalidDescriptorAddress, ResolveError::None};
  }
  if (args.size() > GoalCallContext{}.args.size()) {
    throw std::invalid_argument("AOT invocation has more than eight arguments");
  }

  const auto resolved = registry_->resolve(*descriptor);
  if (!resolved) {
    return {0, InvocationError::ResolutionFailed, resolved.error};
  }

  GoalCallContext context;
  std::copy(args.begin(), args.end(), context.args.begin());
  context.arg_count = static_cast<std::uint32_t>(args.size());
  context.flags = resolved.entry->flags;
  context.process_pointer = process_pointer;
  context.goal_stack_pointer = goal_stack_pointer;
  context.ee_base = ee_base;
  const auto value = resolved.entry->native_function
                         ? invoke_native(*resolved.entry, &context)
                         : resolved.entry->entry(&context);
  return {value, InvocationError::None, ResolveError::None};
}

ExecutionBackend& execution_backend() {
  static ExecutionBackend backend;
  return backend;
}

FunctionId register_native_bridge(void* function,
                                  std::uint32_t flags,
                                  std::string name,
                                  void* target) {
  if (!function || name.empty()) {
    throw std::invalid_argument("AOT native bridge requires a function and name");
  }
  auto& state = native_bridges();
  const BridgeKey key{reinterpret_cast<std::uintptr_t>(function),
                      reinterpret_cast<std::uintptr_t>(target), flags};
  if (const auto existing = state.ids.find(key); existing != state.ids.end()) {
    return existing->second;
  }
  if (state.next_id == std::numeric_limits<FunctionId>::max()) {
    throw std::overflow_error("AOT native bridge registry exhausted");
  }
  const FunctionId id = state.next_id++;
  state.names.push_back(std::move(name));
  state.entries.push_back(
      {id, 1, flags, kFunctionAbiVersion, nullptr, state.names.back().c_str(), function, target});
  state.ids.emplace(key, id);
  state.registry = std::make_unique<Registry>(std::span<const NativeEntry>(state.entries));
  if (!state.registry->valid()) {
    throw std::logic_error("AOT native bridge registry became invalid");
  }
  execution_backend().install(state.registry.get());
  return id;
}

const char* invocation_error_name(InvocationError error) {
  switch (error) {
    case InvocationError::None:
      return "none";
    case InvocationError::RegistryNotInstalled:
      return "registry-not-installed";
    case InvocationError::InvalidDescriptorAddress:
      return "invalid-descriptor-address";
    case InvocationError::ResolutionFailed:
      return "resolution-failed";
  }
  return "unknown";
}

const char* resolve_error_name(ResolveError error) {
  switch (error) {
    case ResolveError::None:
      return "none";
    case ResolveError::InvalidMagic:
      return "invalid-magic";
    case ResolveError::NullId:
      return "null-id";
    case ResolveError::InvalidGeneration:
      return "invalid-generation";
    case ResolveError::UnknownId:
      return "unknown-id";
    case ResolveError::GenerationMismatch:
      return "generation-mismatch";
    case ResolveError::AbiMismatch:
      return "abi-mismatch";
    case ResolveError::MissingEntryPoint:
      return "missing-entrypoint";
    case ResolveError::InvalidRegistry:
      return "invalid-registry";
  }
  return "unknown";
}

}  // namespace goal_aot
