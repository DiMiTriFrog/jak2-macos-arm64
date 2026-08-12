#include "goalc/aot/AotAssemblyWriter.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>

namespace goal_aot {

void AppleArm64AssemblyWriter::validate_symbol(const std::string& symbol) {
  if (symbol.empty()) {
    throw std::invalid_argument("AOT assembly symbol cannot be empty");
  }
  const auto first = static_cast<unsigned char>(symbol.front());
  if (!(std::isalpha(first) || first == '_')) {
    throw std::invalid_argument("AOT assembly symbol must start with a letter or underscore");
  }
  for (const auto byte : symbol) {
    const auto c = static_cast<unsigned char>(byte);
    if (!(std::isalnum(c) || c == '_' || c == '$' || c == '.')) {
      throw std::invalid_argument("AOT assembly symbol contains an unsafe character");
    }
  }
  if (symbol.front() == '_') {
    throw std::invalid_argument(
        "AOT assembly symbols use source names; Mach-O underscore is added by the writer");
  }
}

void AppleArm64AssemblyWriter::validate(std::span<const AssemblyFunction> functions) {
  std::vector<std::string> symbols;
  symbols.reserve(functions.size());
  for (const auto& function : functions) {
    validate_symbol(function.symbol);
    if (function.words.empty()) {
      throw std::invalid_argument("AOT assembly function cannot be empty: " + function.symbol);
    }
    if (function.alignment == 0 || (function.alignment & (function.alignment - 1)) != 0 ||
        function.alignment < 4) {
      throw std::invalid_argument("AOT assembly alignment must be a power of two >= 4");
    }
    if ((function.words.size() * sizeof(std::uint32_t)) % 4 != 0) {
      throw std::invalid_argument("AOT ARM64 instructions must be 4-byte aligned");
    }
    symbols.push_back(function.symbol);
  }
  std::sort(symbols.begin(), symbols.end());
  if (std::adjacent_find(symbols.begin(), symbols.end()) != symbols.end()) {
    throw std::invalid_argument("duplicate AOT assembly symbol");
  }
}

std::string AppleArm64AssemblyWriter::write(std::span<const AssemblyFunction> functions) {
  validate(functions);

  std::vector<const AssemblyFunction*> ordered;
  ordered.reserve(functions.size());
  for (const auto& function : functions) {
    ordered.push_back(&function);
  }
  std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
    return left->symbol < right->symbol;
  });

  std::ostringstream output;
  output << ".section __TEXT,__text,regular,pure_instructions\n";
  output << ".subsections_via_symbols\n";
  for (const auto* function : ordered) {
    output << ".p2align " << __builtin_ctz(function->alignment) << "\n";
    if (function->externally_visible) {
      output << ".globl _" << function->symbol << "\n";
    } else {
      output << ".private_extern _" << function->symbol << "\n";
    }
    output << "_" << function->symbol << ":\n";
    for (const auto word : function->words) {
      output << "  .inst 0x" << std::hex;
      output.width(8);
      output.fill('0');
      output << word << std::dec << "\n";
    }
  }
  return output.str();
}

}  // namespace goal_aot
