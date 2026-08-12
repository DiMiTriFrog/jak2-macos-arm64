#include <array>

#include <gtest/gtest.h>

#include "common/aot/AotExecution.h"
#include "common/aot/AotFunction.h"

namespace {

std::uint64_t add_arguments(goal_aot::GoalCallContext* context) {
  EXPECT_NE(context, nullptr);
  EXPECT_EQ(context->arg_count, 2u);
  return context->args[0] + context->args[1];
}

std::uint64_t return_process_pointer(goal_aot::GoalCallContext* context) {
  EXPECT_NE(context, nullptr);
  return context->process_pointer;
}

constexpr goal_aot::NativeEntry kEntries[] = {
    {1, 7, goal_aot::kNone, goal_aot::kFunctionAbiVersion, add_arguments, "add"},
    {2, 7, goal_aot::kAcceptsProcessPointer, goal_aot::kFunctionAbiVersion,
     return_process_pointer, "process-pointer"},
};

}  // namespace

TEST(AotFunction, ResolvesStaticEntryAndCallsIt) {
  const goal_aot::Registry registry(kEntries);
  ASSERT_TRUE(registry.valid());

  auto descriptor = goal_aot::make_descriptor(1, 7, goal_aot::kNone);
  const auto result = registry.resolve(descriptor);
  ASSERT_TRUE(result);
  ASSERT_NE(result.entry, nullptr);

  goal_aot::GoalCallContext context;
  context.arg_count = 2;
  context.args[0] = 40;
  context.args[1] = 2;
  ASSERT_NE(result.entry->entry, nullptr);
  EXPECT_EQ(result.entry->entry(&context), 42u);
}

TEST(AotFunction, ResolvesProcessPointerWithoutStoringNativePointerInDescriptor) {
  const goal_aot::Registry registry(kEntries);
  const auto descriptor = goal_aot::make_descriptor(2, 7, goal_aot::kAcceptsProcessPointer);
  const auto result = registry.resolve(descriptor);
  ASSERT_TRUE(result);

  goal_aot::GoalCallContext context;
  context.process_pointer = 0x123456789abcdef0ull;
  EXPECT_EQ(result.entry->entry(&context), 0x123456789abcdef0ull);
  EXPECT_EQ(sizeof(goal_aot::FunctionDescriptor), 16u);
}

TEST(AotFunction, RejectsInvalidDescriptors) {
  const goal_aot::Registry registry(kEntries);

  auto invalid_magic = goal_aot::make_descriptor(1, 7, goal_aot::kNone);
  invalid_magic.magic = 0;
  EXPECT_EQ(registry.resolve(invalid_magic).error, goal_aot::ResolveError::InvalidMagic);

  EXPECT_EQ(registry.resolve(goal_aot::make_descriptor(0, 7, goal_aot::kNone)).error,
            goal_aot::ResolveError::NullId);
  EXPECT_EQ(registry.resolve(goal_aot::make_descriptor(1, 0, goal_aot::kNone)).error,
            goal_aot::ResolveError::InvalidGeneration);
  EXPECT_EQ(registry.resolve(goal_aot::make_descriptor(99, 7, goal_aot::kNone)).error,
            goal_aot::ResolveError::UnknownId);
  EXPECT_EQ(registry.resolve(goal_aot::make_descriptor(1, 8, goal_aot::kNone)).error,
            goal_aot::ResolveError::GenerationMismatch);
}

TEST(AotFunction, RejectsUnsortedOrIncompleteRegistries) {
  constexpr goal_aot::NativeEntry unsorted[] = {
      {2, 7, goal_aot::kNone, goal_aot::kFunctionAbiVersion, add_arguments, "second"},
      {1, 7, goal_aot::kNone, goal_aot::kFunctionAbiVersion, add_arguments, "first"},
  };
  constexpr goal_aot::NativeEntry missing_name[] = {
      {1, 7, goal_aot::kNone, goal_aot::kFunctionAbiVersion, add_arguments, nullptr},
  };

  EXPECT_FALSE(goal_aot::Registry(unsorted).valid());
  EXPECT_FALSE(goal_aot::Registry(missing_name).valid());
  EXPECT_EQ(goal_aot::Registry(unsorted).resolve(goal_aot::make_descriptor(1, 7, 0)).error,
            goal_aot::ResolveError::InvalidRegistry);
}

TEST(AotFunction, ExecutionBackendCarriesZeroThreeAndEightArguments) {
  static std::array<std::uint64_t, 3> observed{};
  static const goal_aot::NativeEntry entries[] = {
      {7, 1, goal_aot::kNone, goal_aot::kFunctionAbiVersion,
       [](goal_aot::GoalCallContext* context) -> std::uint64_t {
         observed[0] = context->arg_count;
         observed[1] = context->process_pointer;
         observed[2] = context->ee_base;
         std::uint64_t result = 0;
         for (std::uint32_t i = 0; i < context->arg_count; ++i) {
           result += context->args[i];
         }
         return result;
       },
       "sum"},
  };
  const goal_aot::Registry registry(entries);
  auto& backend = goal_aot::execution_backend();
  backend.install(&registry);

  const auto descriptor = goal_aot::make_descriptor(7, 1, goal_aot::kNone);
  const auto zero = backend.invoke(&descriptor, {}, 0x1234, 0x5000, 0x7000000000);
  EXPECT_TRUE(zero);
  EXPECT_EQ(zero.value, 0u);
  EXPECT_EQ(observed[0], 0u);

  const std::array three_args{1ull, 2ull, 3ull};
  const auto three = backend.invoke(&descriptor, three_args, 0x1234, 0x5000, 0x7000000000);
  EXPECT_TRUE(three);
  EXPECT_EQ(three.value, 6u);
  EXPECT_EQ(observed[0], 3u);

  const std::array eight_args{1ull, 2ull, 3ull, 4ull, 5ull, 6ull, 7ull, 8ull};
  const auto eight = backend.invoke(&descriptor, eight_args, 0x1234, 0x5000, 0x7000000000);
  EXPECT_TRUE(eight);
  EXPECT_EQ(eight.value, 36u);
  EXPECT_EQ(observed[0], 8u);
  EXPECT_EQ(observed[1], 0x1234u);
  EXPECT_EQ(observed[2], 0x7000000000u);
  backend.clear();
}
