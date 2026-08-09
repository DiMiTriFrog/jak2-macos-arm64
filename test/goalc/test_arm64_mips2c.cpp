#include <cstdint>
#include <cstring>
#include <string>
#include <unistd.h>

#include "common/goal_constants.h"
#include "common/symbols.h"

#include "game/kernel/common/arm64_trampoline.h"
#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/memory_layout.h"
#include "game/kernel/jak2/kscheme.h"
#include "game/mips2c/mips2c_table.h"
#include "game/runtime.h"
#include "gtest/gtest.h"
#include <sys/mman.h>

#if defined(__aarch64__)

namespace Mips2C::jak2::ripple_matrix_scale {
extern uint64_t execute(void*);
}

namespace {

using u64 = uint64_t;

extern "C" void _mips2c_call_arm64() asm("_mips2c_call_arm64");

template <typename T>
void* function_address(T* function) {
  return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(function));
}

extern "C" u64 mips2c_context_sum(void* context) {
  u64 arg0 = 0;
  u64 arg1 = 0;
  u64 arg2 = 0;
  u64 arg3 = 0;
  std::memcpy(&arg0, static_cast<u8*>(context) + 64, sizeof(arg0));
  std::memcpy(&arg1, static_cast<u8*>(context) + 80, sizeof(arg1));
  std::memcpy(&arg2, static_cast<u8*>(context) + 96, sizeof(arg2));
  std::memcpy(&arg3, static_cast<u8*>(context) + 112, sizeof(arg3));

  void* mips_stack = nullptr;
  std::memcpy(&mips_stack, static_cast<u8*>(context) + 464, sizeof(mips_stack));
  const u64 stack_marker = mips_stack == context ? 0x100000000ull : 0;
  return arg0 + arg1 * 2 + arg2 * 3 + arg3 * 4 + stack_marker;
}

extern "C" __attribute__((naked)) u64 invoke_mips2c_stub(void*, u64, u64, u64, u64) {
  asm("stp x20, x21, [sp, #-16]!\n"
      "str x22, [sp, #-16]!\n"
      "str x30, [sp, #-16]!\n"
      "mov x9, x0\n"
      "mov x20, #0x1234\n"
      "mov x21, #0x5678\n"
      "mov x22, #0\n"
      "mov x0, x1\n"
      "mov x1, x2\n"
      "mov x2, x3\n"
      "mov x3, x4\n"
      "mov x4, #0\n"
      "mov x5, #0\n"
      "mov x6, #0\n"
      "mov x7, #0\n"
      "blr x9\n"
      "ldr x30, [sp], #16\n"
      "ldr x22, [sp], #16\n"
      "ldp x20, x21, [sp], #16\n"
      "ret\n");
}

class ExecutablePage {
 public:
  ExecutablePage() {
    page_size_ = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    memory_ = static_cast<u8*>(mmap(nullptr, page_size_, PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE | MAP_JIT, -1, 0));
    if (memory_ == MAP_FAILED) {
      memory_ = nullptr;
    }
  }

  ~ExecutablePage() {
    if (memory_ != nullptr) {
      EXPECT_EQ(munmap(memory_, page_size_), 0);
    }
  }

  bool valid() const { return memory_ != nullptr; }
  u8* data() { return memory_; }

  void make_executable(size_t code_size) {
    arm64_trampoline::flush(memory_, code_size);
    ASSERT_EQ(mprotect(memory_, page_size_, PROT_READ | PROT_EXEC), 0);
  }

 private:
  u8* memory_ = nullptr;
  size_t page_size_ = 0;
};

}  // namespace

TEST(ARM64Mips2C, GeneratedStubExecutesContextAndStackContract) {
  ExecutablePage page;
  ASSERT_TRUE(page.valid());
  const auto size = arm64_trampoline::emit_mips2c(
      page.data(), 7, function_address(&mips2c_context_sum), function_address(_mips2c_call_arm64));
  page.make_executable(size);

  EXPECT_EQ(invoke_mips2c_stub(page.data(), 2, 3, 5, 7),
            2 + 3 * 2 + 5 * 3 + 7 * 4 + 0x100000000ull);
}

TEST(ARM64Mips2C, GeneratedStubSupportsRepeatedCallsAndStackBoundaries) {
  ExecutablePage page;
  ASSERT_TRUE(page.valid());
  for (u64 stack_size : {u64(0), u64(1), u64(7), u64(8), u64(9), u64(16)}) {
    const auto size = arm64_trampoline::emit_mips2c(page.data(), stack_size,
                                                    function_address(&mips2c_context_sum),
                                                    function_address(_mips2c_call_arm64));
    u64 observed_target = 0;
    std::memcpy(&observed_target, page.data() + 40, sizeof(observed_target));
    EXPECT_EQ(observed_target, reinterpret_cast<uintptr_t>(&mips2c_context_sum))
        << "stack_size=" << stack_size;
    page.make_executable(size);
    EXPECT_EQ(invoke_mips2c_stub(page.data(), 1, 2, 3, 4),
              1 + 2 * 2 + 3 * 3 + 4 * 4 + 0x100000000ull)
        << "stack_size=" << stack_size;
    std::memcpy(&observed_target, page.data() + 40, sizeof(observed_target));
    // The next iteration needs to rewrite the same page.
    if (stack_size != 16) {
      ASSERT_EQ(
          mprotect(page.data(), static_cast<size_t>(sysconf(_SC_PAGESIZE)), PROT_READ | PROT_WRITE),
          0);
    }
  }
}

TEST(ARM64Mips2CTable, RegistersSyntheticAndRealJak2Functions) {
  constexpr size_t kSyntheticFunctionCount = 128;
  constexpr u32 kFakeFunctionType = 0x300000;
  constexpr u32 kFakeS7 = 0x200000;
  const auto page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  auto* memory = static_cast<u8*>(mmap(nullptr, EE_MAIN_MEM_SIZE, PROT_READ | PROT_WRITE,
                                       MAP_ANONYMOUS | MAP_PRIVATE | MAP_JIT, -1, 0));
  ASSERT_NE(memory, MAP_FAILED);

  auto* old_ee_memory = g_ee_main_mem;
  const auto old_game_version = g_game_version;
  g_ee_main_mem = memory;
  g_game_version = GameVersion::Jak2;

  ::Mips2C::gLinkedFunctionTable = {};
  kmalloc_init_globals_common();
  kinitheap(kglobalheap, Ptr<u8>(HEAP_START), GLOBAL_HEAP_END - HEAP_START);
  s7 = Ptr<u32>(kFakeS7);
  Ptr<Symbol4<Ptr<kheapinfo>>>(s7.offset + jak2_symbols::FIX_SYM_GLOBAL_HEAP)->value() =
      kglobalheap;
  Ptr<Symbol4<u32>>(s7.offset + jak2_symbols::FIX_SYM_FUNCTION_TYPE)->value() = kFakeFunctionType;
  Ptr<jak2::Type>(kFakeFunctionType)->symbol.offset = 0;

  for (size_t i = 0; i < kSyntheticFunctionCount; i++) {
    ::Mips2C::gLinkedFunctionTable.reg("arm64-synthetic-" + std::to_string(i), &mips2c_context_sum,
                                       16 + (i & 15));
  }
  ::Mips2C::gLinkedFunctionTable.reg("arm64-real-ripple-matrix-scale",
                                     &::Mips2C::jak2::ripple_matrix_scale::execute, 128);

  const auto first_offset = ::Mips2C::gLinkedFunctionTable.get("arm64-synthetic-0");
  const auto last_offset = ::Mips2C::gLinkedFunctionTable.get(
      "arm64-synthetic-" + std::to_string(kSyntheticFunctionCount - 1));
  const auto real_offset = ::Mips2C::gLinkedFunctionTable.get("arm64-real-ripple-matrix-scale");
  EXPECT_NE(first_offset, 0u);
  EXPECT_NE(last_offset, 0u);
  EXPECT_NE(real_offset, 0u);
  EXPECT_NE(first_offset, last_offset);
  EXPECT_NE(first_offset, real_offset);

  const auto heap_begin = HEAP_START & static_cast<u32>(-static_cast<s32>(page_size));
  const auto heap_end = (kglobalheap->current.offset + static_cast<u32>(page_size) - 1) &
                        ~static_cast<u32>(page_size - 1);
  ASSERT_EQ(mprotect(memory + heap_begin, heap_end - heap_begin, PROT_READ | PROT_EXEC), 0);

  auto first_function = memory + first_offset;
  EXPECT_EQ(invoke_mips2c_stub(first_function, 2, 3, 5, 7),
            2 + 3 * 2 + 5 * 3 + 7 * 4 + 0x100000000ull);
  EXPECT_DEATH(::Mips2C::gLinkedFunctionTable.get("arm64-missing"), "");

  ::Mips2C::gLinkedFunctionTable = {};
  EXPECT_EQ(munmap(memory, EE_MAIN_MEM_SIZE), 0);
  g_ee_main_mem = old_ee_memory;
  g_game_version = old_game_version;
}

#endif
