#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace goal_aot {

// A function emitted by the host AOT pipeline.  The words are already encoded
// by the ARM64 emitter; this class only writes a deterministic Apple assembly
// container around them.  It deliberately has no process addresses or source
// paths in its output.
struct AssemblyFunction {
  std::string symbol;
  std::vector<std::uint32_t> words;
  std::uint32_t alignment = 4;
  bool externally_visible = true;
};

class AppleArm64AssemblyWriter {
 public:
  // Sorts functions by symbol, validates the complete input, and returns
  // AppleClang-compatible ARM64 assembly.  The returned text always ends with
  // one newline and is byte-for-byte stable for equivalent input.
  static std::string write(std::span<const AssemblyFunction> functions);

  // Validate without emitting.  This is useful to make the generator fail
  // before it writes a partial directory.
  static void validate(std::span<const AssemblyFunction> functions);

 private:
  static void validate_symbol(const std::string& symbol);
};

}  // namespace goal_aot
