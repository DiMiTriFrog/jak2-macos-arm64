#include <gtest/gtest.h>

#include <initializer_list>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "goalc/aot/AotAssemblyWriter.h"

using U32x4 = std::uint32_t __attribute__((vector_size(16)));

extern "C" std::uint64_t aot_fixture_scalar(std::uint64_t value,
                                             std::uint64_t increment,
                                             std::uint64_t count);
extern "C" std::uint64_t aot_fixture_caller(std::uint64_t value, std::uint64_t increment);
extern "C" U32x4 aot_fixture_vector(U32x4 left, U32x4 right);

namespace {

goal_aot::AssemblyFunction function(const char* symbol,
                                    std::initializer_list<std::uint32_t> words,
                                    std::uint32_t alignment = 4) {
  return {symbol, std::vector<std::uint32_t>(words), alignment, true};
}

}  // namespace

TEST(AotAssembly, EmitsDeterministicAppleTextAndSortsFunctions) {
  const std::vector<goal_aot::AssemblyFunction> input = {
      function("second", {0xd65f03c0}),
      function("first", {0x91000400, 0xd65f03c0}, 16),
  };

  const auto output = goal_aot::AppleArm64AssemblyWriter::write(input);
  const std::string expected =
      ".section __TEXT,__text,regular,pure_instructions\n"
      ".subsections_via_symbols\n"
      ".p2align 4\n"
      ".globl _first\n"
      "_first:\n"
      "  .inst 0x91000400\n"
      "  .inst 0xd65f03c0\n"
      ".p2align 2\n"
      ".globl _second\n"
      "_second:\n"
      "  .inst 0xd65f03c0\n";
  EXPECT_EQ(output, expected);

  const std::vector<goal_aot::AssemblyFunction> reversed = {input[1], input[0]};
  EXPECT_EQ(output, goal_aot::AppleArm64AssemblyWriter::write(reversed));
}

TEST(AotAssembly, EmitsPrivateSymbolsWithoutChangingMachONameRules) {
  const std::vector<goal_aot::AssemblyFunction> input = {
      {"og_aot_local.0", {0xd65f03c0}, 4, false},
  };
  const auto output = goal_aot::AppleArm64AssemblyWriter::write(input);
  EXPECT_NE(output.find(".private_extern _og_aot_local.0\n"), std::string::npos);
  EXPECT_EQ(output.find(".globl _og_aot_local.0"), std::string::npos);
}

TEST(AotAssembly, RejectsUnsafeOrAmbiguousInput) {
  const auto expect_invalid = [](std::vector<goal_aot::AssemblyFunction> input) {
    EXPECT_THROW(goal_aot::AppleArm64AssemblyWriter::write(input), std::invalid_argument);
  };
  expect_invalid({function("bad name", {0})});
  expect_invalid({function("_already_mangled", {0})});
  expect_invalid({function("empty", {})});
  expect_invalid({function("duplicate", {0}), function("duplicate", {0})});
  expect_invalid({function("bad_alignment", {0}, 3)});
}

TEST(AotAssembly, LinkedArm64FixtureExecutesScalarBranchesCallsAndSimd) {
  EXPECT_EQ(aot_fixture_scalar(5, 3, 4), 24);
  EXPECT_EQ(aot_fixture_scalar(5, 3, 0), 12);
  EXPECT_EQ(aot_fixture_caller(5, 3), 15);

  const U32x4 left = {1, 2, 3, 4};
  const U32x4 right = {10, 20, 30, 40};
  const U32x4 result = aot_fixture_vector(left, right);
  EXPECT_EQ(result[0], 11u);
  EXPECT_EQ(result[1], 22u);
  EXPECT_EQ(result[2], 33u);
  EXPECT_EQ(result[3], 44u);
}
