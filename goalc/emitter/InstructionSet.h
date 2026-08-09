#pragma once

#include <string>

#include "common/util/Assert.h"

namespace emitter {
enum class InstructionSet { X86, ARM64 };

inline const char* to_string(InstructionSet instr_set) {
  switch (instr_set) {
    case InstructionSet::X86:
      return "x86_64";
    case InstructionSet::ARM64:
      return "arm64";
    default:
      ASSERT(false);
      return "unknown";
  }
}

// Parse a user-facing target architecture name into an InstructionSet.
// Throws a descriptive error for unknown values.
inline InstructionSet parse_instruction_set(const std::string& name) {
  if (name == "x86_64") {
    return InstructionSet::X86;
  }
  if (name == "arm64") {
    return InstructionSet::ARM64;
  }
  throw std::runtime_error(std::string("unknown target architecture '") + name +
                           "'.  Valid values are 'x86_64' and 'arm64'");
}

// Suffix appended to compiler output prefixes so that objects produced for
// different architectures never share a directory.  The x86_64 suffix is the
// empty string to keep existing x86_64 output layouts byte-for-byte identical.
inline const char* output_suffix(InstructionSet instr_set) {
  switch (instr_set) {
    case InstructionSet::X86:
      return "";
    case InstructionSet::ARM64:
      return "-arm64";
    default:
      ASSERT(false);
      return "";
  }
}
};  // namespace emitter
