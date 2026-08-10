#include <atomic>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

#include "common/jit_memory.h"

#include "gtest/gtest.h"

#if defined(__aarch64__)

namespace {

using Function = uint64_t (*)();

constexpr uint32_t kRet = 0xd65f03c0;

void emit_return(uint8_t* code, uint64_t value) {
  ASSERT_LT(value, 4096u);
  const uint32_t mov = 0xd2800000 | (static_cast<uint32_t>(value) << 5);
  std::memcpy(code, &mov, sizeof(mov));
  std::memcpy(code + sizeof(mov), &kRet, sizeof(kRet));
}

uint64_t execute(const jit_memory::JitRegion& region, size_t offset = 0) {
  return reinterpret_cast<Function>(static_cast<uint8_t*>(region.data()) + offset)();
}

}  // namespace

TEST(JitRegion, WriteExecutePatchCyclesAndPartialFlush) {
  auto region = jit_memory::JitRegion::allocate(4096);

  for (uint64_t cycle = 0; cycle < 100000; ++cycle) {
    auto scope = region.write_scope();
    emit_return(static_cast<uint8_t*>(region.data()), cycle & 0xfff);
    scope.flush_instruction_cache();
    scope.finish();

    EXPECT_EQ(execute(region), cycle & 0xfff) << "cycle=" << cycle;
    EXPECT_EQ(region.protection(), jit_memory::Protection::Executable);
  }
}

TEST(JitRegion, MultipleFunctionsShareOneRegion) {
  auto region = jit_memory::JitRegion::allocate(4096);
  {
    auto scope = region.write_scope();
    emit_return(static_cast<uint8_t*>(region.data()), 17);
    emit_return(static_cast<uint8_t*>(region.data()) + 64, 31);
    region.flush_instruction_cache(static_cast<uint8_t*>(region.data()), 8);
    region.flush_instruction_cache(static_cast<uint8_t*>(region.data()) + 64, 8);
    scope.finish();
  }

  EXPECT_EQ(execute(region), 17u);
  EXPECT_EQ(execute(region, 64), 31u);
}

TEST(JitRegion, IndependentThreadsCanGenerateAndExecute) {
  constexpr int kThreadCount = 4;
  constexpr int kCyclesPerThread = 1000;
  std::atomic<int> failures = 0;
  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);

  for (int thread_id = 0; thread_id < kThreadCount; ++thread_id) {
    threads.emplace_back([thread_id, &failures]() {
      auto region = jit_memory::JitRegion::allocate(4096);
      for (int cycle = 0; cycle < kCyclesPerThread; ++cycle) {
        auto scope = region.write_scope();
        emit_return(static_cast<uint8_t*>(region.data()), (thread_id * 97 + cycle) & 0xfff);
        scope.finish();
        if (execute(region) != static_cast<uint64_t>((thread_id * 97 + cycle) & 0xfff)) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(failures.load(std::memory_order_relaxed), 0);
}

TEST(JitWriteScope, WritingOutsideScopeIsRejected) {
  auto region = jit_memory::JitRegion::allocate(4096);
  {
    auto scope = region.write_scope();
    emit_return(static_cast<uint8_t*>(region.data()), 7);
    scope.finish();
  }

  EXPECT_DEATH(
      {
        auto* code = static_cast<volatile uint32_t*>(region.data());
        *code = 0;
      },
      "");
}

TEST(JitWriteScope, ExecutingDuringWritablePhaseIsRejected) {
  auto region = jit_memory::JitRegion::allocate(4096);
  auto scope = region.write_scope();
  emit_return(static_cast<uint8_t*>(region.data()), 11);

  EXPECT_DEATH(
      {
        auto function = reinterpret_cast<Function>(region.data());
        (void)function();
      },
      "");
  scope.finish();
}

TEST(JitRegion, InvalidMappingsAndRangesFailExplicitly) {
  EXPECT_THROW(jit_memory::JitRegion::allocate(0), std::invalid_argument);
  EXPECT_THROW(jit_memory::make_executable(nullptr, 8), std::invalid_argument);
  auto region = jit_memory::JitRegion::allocate(4096);
  EXPECT_THROW(
      region.flush_instruction_cache(static_cast<uint8_t*>(region.data()) + region.size(), 4),
      std::invalid_argument);
}

#endif
