#include <gtest/gtest.h>

#include <stdexcept>

#include "common/aot/AotManifest.h"

namespace {

goal_aot::FunctionRecord function(const char* object, uint32_t ordinal, const char* name) {
  goal_aot::FunctionRecord result;
  result.key = {"jak2", object, "code", ordinal};
  result.goal_name = name;
  result.code_size = ordinal + 16;
  return result;
}

goal_aot::ManifestBuilder builder() {
  return goal_aot::ManifestBuilder({"2.0", "arm64-apple-macos", "source-sha"});
}

}  // namespace

TEST(AotManifest, InputOrderDoesNotChangeCanonicalBytes) {
  auto first = builder();
  first.add_function(function("z-object", 2, "same-name"));
  first.add_function(function("a-object", 1, "same-name"));
  auto second = builder();
  second.add_function(function("a-object", 1, "same-name"));
  second.add_function(function("z-object", 2, "same-name"));

  EXPECT_EQ(first.build().serialize_canonical(), second.build().serialize_canonical());
}

TEST(AotManifest, HomonymousFunctionsHaveDifferentIdsAndSymbols) {
  auto manifest_builder = builder();
  manifest_builder.add_function(function("object-a", 7, "tick"));
  manifest_builder.add_function(function("object-b", 7, "tick"));
  const auto manifest = manifest_builder.build();

  ASSERT_EQ(manifest.functions.size(), 2);
  EXPECT_NE(manifest.functions[0].id, manifest.functions[1].id);
  EXPECT_NE(manifest.functions[0].symbol, manifest.functions[1].symbol);
}

TEST(AotManifest, RejectsDuplicateAndReservedIds) {
  auto manifest_builder = builder();
  manifest_builder.add_function(function("object-a", 1, "tick"));
  manifest_builder.add_function(function("object-a", 1, "tick-again"));
  EXPECT_THROW(manifest_builder.build(), std::invalid_argument);
  EXPECT_THROW(goal_aot::validate_function_id(0), std::invalid_argument);
}

TEST(AotManifest, RejectsOverflowPathsAndFutureSchemas) {
  EXPECT_THROW(goal_aot::checked_u32(UINT64_C(0x100000000), "offset"), std::overflow_error);
  EXPECT_THROW(goal_aot::validate_relative_manifest_path("/absolute/file"), std::invalid_argument);
  EXPECT_THROW(goal_aot::validate_relative_manifest_path("objects/../file"), std::invalid_argument);
  EXPECT_THROW(goal_aot::validate_schema(2, goal_aot::kManifestAotAbiVersion),
               std::invalid_argument);
}

TEST(AotManifest, StableSymbolIsAsciiAndContainsFullIdentity) {
  goal_aot::FunctionKey key{"jak2", "obj-a", "seg-b", 42};
  const auto symbol = goal_aot::stable_function_symbol(key);
  EXPECT_EQ(symbol, goal_aot::stable_function_symbol(key));
  EXPECT_NE(symbol.find("obj_a"), std::string::npos);
  EXPECT_NE(symbol.find("_f42_"), std::string::npos);
}
