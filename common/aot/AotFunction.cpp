#include "common/aot/AotFunction.h"

#include <algorithm>

namespace goal_aot {

Registry::Registry(std::span<const NativeEntry> entries) : entries_(entries) {
  valid_ = true;
  FunctionId previous_id = 0;
  for (const auto& entry : entries_) {
    if (entry.id == 0 || entry.id <= previous_id || entry.generation == 0 ||
        entry.abi_version != kFunctionAbiVersion ||
        (entry.entry == nullptr && entry.native_function == nullptr) || entry.name == nullptr) {
      valid_ = false;
      break;
    }
    previous_id = entry.id;
  }
}

ResolveResult Registry::resolve(const FunctionDescriptor& descriptor) const {
  if (!valid_) {
    return {nullptr, ResolveError::InvalidRegistry};
  }
  if (descriptor.magic != kFunctionDescriptorMagic) {
    return {nullptr, ResolveError::InvalidMagic};
  }
  if (descriptor.id == 0) {
    return {nullptr, ResolveError::NullId};
  }
  if (descriptor.generation == 0) {
    return {nullptr, ResolveError::InvalidGeneration};
  }

  const auto it = std::lower_bound(
      entries_.begin(), entries_.end(), descriptor.id,
      [](const NativeEntry& entry, FunctionId id) { return entry.id < id; });
  if (it == entries_.end() || it->id != descriptor.id) {
    return {nullptr, ResolveError::UnknownId};
  }
  if (it->generation != descriptor.generation) {
    return {nullptr, ResolveError::GenerationMismatch};
  }
  if (it->abi_version != kFunctionAbiVersion) {
    return {nullptr, ResolveError::AbiMismatch};
  }
  if (!it->entry && !it->native_function) {
    return {nullptr, ResolveError::MissingEntryPoint};
  }
  return {&*it, ResolveError::None};
}

FunctionDescriptor make_descriptor(FunctionId id, std::uint32_t generation, std::uint32_t flags) {
  return FunctionDescriptor{kFunctionDescriptorMagic, id, generation, flags};
}

}  // namespace goal_aot
