#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "common/aot/AotDataTemplate.h"

namespace {

goal_aot::AotDataTemplate fixture() {
  goal_aot::AotDataTemplate object;
  object.abi_version = 1;
  object.game = "jak2";
  object.object = "fixture";
  object.data = {0, 0, 0, 0, 0x11, 0x22, 0x33, 0x44};
  object.source_hash.resize(32, 0xab);
  object.relocations.push_back(
      {goal_aot::RelocationKind::DataOffset32, 0, 7, 0x1234});
  return object;
}

}  // namespace

TEST(AotDataTemplate, RoundTripsDeterministically) {
  const auto input = fixture();
  const auto bytes = input.serialize();
  const auto parsed = goal_aot::AotDataTemplate::parse(bytes);
  EXPECT_EQ(parsed.serialize(), bytes);
  EXPECT_EQ(parsed.game, "jak2");
  EXPECT_EQ(parsed.object, "fixture");
  EXPECT_EQ(parsed.relocations.size(), 1u);
}

TEST(AotDataTemplate, InstantiationPublishesOnlyAfterAllRelocationsSucceed) {
  const auto object = fixture();
  std::vector<std::uint8_t> destination(object.data.size(), 0xee);
  goal_aot::instantiate_data_template(
      object, destination,
      [](goal_aot::RelocationKind kind, std::uint32_t target, std::uint32_t offset) {
        EXPECT_EQ(kind, goal_aot::RelocationKind::DataOffset32);
        EXPECT_EQ(target, 7u);
        EXPECT_EQ(offset, 0x1234u);
        return 0x76543210u;
      });
  EXPECT_EQ(destination[0], 0x10);
  EXPECT_EQ(destination[1], 0x32);
  EXPECT_EQ(destination[2], 0x54);
  EXPECT_EQ(destination[3], 0x76);
  EXPECT_EQ(destination[4], 0x11);

  const auto before_failure = destination;
  EXPECT_THROW(goal_aot::instantiate_data_template(
                   object, destination,
                   [](goal_aot::RelocationKind, std::uint32_t, std::uint32_t) -> std::uint32_t {
                     throw std::runtime_error("missing object");
                   }),
               std::runtime_error);
  EXPECT_EQ(destination, before_failure);
}

TEST(AotDataTemplate, RejectsCodeAndNativeRelocations) {
  auto object = fixture();
  object.relocations[0].kind = goal_aot::RelocationKind::CodePointer;
  EXPECT_THROW(object.serialize(), std::invalid_argument);

  object.relocations[0].kind = goal_aot::RelocationKind::NativePointer;
  EXPECT_THROW(object.serialize(), std::invalid_argument);
}

TEST(AotDataTemplate, RejectsDuplicateRelocationSourcesAndAbiChanges) {
  auto object = fixture();
  object.relocations.push_back(
      {goal_aot::RelocationKind::SymbolValue32, 0, 2, 3});
  EXPECT_THROW(object.serialize(), std::invalid_argument);

  object.relocations.pop_back();
  object.abi_version = 2;
  EXPECT_THROW(object.serialize(), std::invalid_argument);
}

TEST(AotDataTemplate, RejectsTruncationOverflowAndMisalignedSources) {
  const auto bytes = fixture().serialize();
  EXPECT_THROW(goal_aot::AotDataTemplate::parse(
                   std::vector<std::uint8_t>(bytes.begin(), bytes.end() - 1)),
               std::invalid_argument);

  auto object = fixture();
  object.relocations[0].source_offset = 1;
  EXPECT_THROW(object.serialize(), std::out_of_range);
  object.relocations[0].source_offset = 8;
  EXPECT_THROW(object.serialize(), std::out_of_range);
}
