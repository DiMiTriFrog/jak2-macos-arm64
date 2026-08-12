#include <cstdint>

using U32x4 = std::uint32_t __attribute__((vector_size(16)));

extern "C" std::uint64_t aot_fixture_scalar(std::uint64_t value,
                                             std::uint64_t increment,
                                             std::uint64_t count) {
  for (std::uint64_t i = 0; i < count; ++i) {
    value += increment;
  }
  return value + 7;
}

extern "C" std::uint64_t aot_fixture_callee(std::uint64_t value, std::uint64_t increment) {
  return value + increment;
}

extern "C" std::uint64_t aot_fixture_caller(std::uint64_t value, std::uint64_t increment) {
  return aot_fixture_callee(value, increment) + 7;
}

extern "C" U32x4 aot_fixture_vector(U32x4 left, U32x4 right) { return left + right; }
