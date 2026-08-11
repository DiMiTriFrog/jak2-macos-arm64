#include <cstdint>
#include <cstring>

#include "common/jit_memory.h"

#include "game/kernel/common/arm64_trampoline.h"
#include "gtest/gtest.h"

#if defined(__aarch64__)

namespace {

using u64 = uint64_t;

extern "C" void _arg_call_arm64() asm("_arg_call_arm64");
extern "C" void _stack_call_arm64() asm("_stack_call_arm64");

template <typename T>
void* function_address(T* function) {
  return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(function));
}

extern "C" u64 trampoline_add4(u64 a0, u64 a1, u64 a2, u64 a3) {
  return a0 + a1 + a2 + a3;
}

extern "C" u64 return_fourth(u64, u64, u64, u64 a3) {
  return a3;
}

extern "C" u64 sum_packed(const u64* args) {
  u64 result = 0;
  for (int i = 0; i < 8; ++i) {
    result += args[i];
  }
  return result;
}

extern "C" u64 first_packed(const u64* args) {
  return args[0];
}

extern "C" __attribute__((naked)) u64 invoke_with_pp(void*, u64, u64, u64, u64) {
  asm("stp x20, x30, [sp, #-16]!\n"
      "mov x9, x0\n"
      "mov x20, x4\n"
      "mov x0, x1\n"
      "mov x1, x2\n"
      "mov x2, x3\n"
      "blr x9\n"
      "ldp x20, x30, [sp], #16\n"
      "ret\n");
}

extern "C" __attribute__((naked)) u64 invoke_nothing_with_sentinel(void*) {
  asm("stp x29, x30, [sp, #-16]!\n"
      "mov x9, x0\n"
      "mov x0, #0x1357\n"
      "blr x9\n"
      "ldp x29, x30, [sp], #16\n"
      "ret\n");
}

class ExecutablePage {
 public:
  ExecutablePage() : region_(jit_memory::JitRegion::allocate(4096)) {}

  u8* data(size_t offset = 0) {
    EXPECT_LE(offset, region_.size());
    return static_cast<u8*>(region_.data()) + offset;
  }

  bool valid() const { return region_.data() != nullptr; }

  jit_memory::JitWriteScope write_scope() { return region_.write_scope(); }

  void make_executable(size_t code_size) {
    region_.flush_instruction_cache(data(), code_size);
    region_.make_executable();
  }

  void make_writable() { region_.make_writable(); }

 private:
  jit_memory::JitRegion region_;
};

u32 read_word(const u8* code, size_t offset) {
  u32 result = 0;
  std::memcpy(&result, code + offset, sizeof(result));
  return result;
}

u64 read_literal(const u8* code, size_t offset) {
  u64 result = 0;
  std::memcpy(&result, code + offset, sizeof(result));
  return result;
}

}  // namespace

TEST(ARM64Trampoline, EmitsRelocatableLiteralCall) {
  ExecutablePage page;
  ASSERT_TRUE(page.valid());
  auto* code = page.data();
  auto scope = page.write_scope();
  const auto target = function_address(&trampoline_add4);
  const auto bridge = function_address(_arg_call_arm64);
  const auto size = arm64_trampoline::emit_c_function(code, target, bridge, false);

  ASSERT_EQ(size, 40u);
  EXPECT_EQ(read_word(code, 0), arm64_trampoline::encode_ldr_literal_x16(0, 24));
  EXPECT_EQ(read_word(code, 4), 0xd503201f);
  EXPECT_EQ(read_word(code, 8), arm64_trampoline::encode_ldr_literal_x17(8, 32));
  EXPECT_EQ(read_word(code, 12), 0xd61f0220);
  EXPECT_EQ(read_literal(code, 24), reinterpret_cast<uint64_t>(target));
  EXPECT_EQ(read_literal(code, 32), reinterpret_cast<uint64_t>(bridge));
  scope.finish();
}

TEST(ARM64Trampoline, CFunctionPreservesFullPointerAndArguments) {
  ExecutablePage page;
  ASSERT_TRUE(page.valid());
  auto* code = page.data();
  auto scope = page.write_scope();
  const auto size = arm64_trampoline::emit_c_function(code, function_address(&trampoline_add4),
                                                      function_address(_arg_call_arm64), false);
  scope.finish();
  page.make_executable(size);

  using Function = u64 (*)(u64, u64, u64, u64);
  auto function = reinterpret_cast<Function>(code);
  EXPECT_EQ(function(1, 2, 3, 4), 10u);
}

TEST(ARM64Trampoline, CFunctionCanMoveProcessPointerIntoFourthArgument) {
  ExecutablePage page;
  ASSERT_TRUE(page.valid());
  auto* code = page.data();
  auto scope = page.write_scope();
  const auto size = arm64_trampoline::emit_c_function(code, function_address(&return_fourth),
                                                      function_address(_arg_call_arm64), true);
  scope.finish();
  page.make_executable(size);

  using Function = u64 (*)(void*, u64, u64, u64, u64);
  auto function = reinterpret_cast<Function>(&invoke_with_pp);
  EXPECT_EQ(function(code, 1, 2, 3, 0xfeedfacecafebeefull), 0xfeedfacecafebeefull);
}

TEST(ARM64Trampoline, StackFunctionReceivesEightArguments) {
  ExecutablePage page;
  ASSERT_TRUE(page.valid());
  auto* code = page.data();
  auto scope = page.write_scope();
  const auto size = arm64_trampoline::emit_stack_function(code, function_address(&sum_packed),
                                                          function_address(_stack_call_arm64));
  scope.finish();
  page.make_executable(size);

  using Function = u64 (*)(u64, u64, u64, u64, u64, u64, u64, u64);
  auto function = reinterpret_cast<Function>(code);
  EXPECT_EQ(function(1, 2, 4, 8, 16, 32, 64, 128), 255u);
}

TEST(ARM64Trampoline, StackFunctionPreservesArgumentOrder) {
  ExecutablePage page;
  ASSERT_TRUE(page.valid());
  auto* code = page.data();
  auto scope = page.write_scope();
  const auto size = arm64_trampoline::emit_stack_function(code, function_address(&first_packed),
                                                          function_address(_stack_call_arm64));
  scope.finish();
  page.make_executable(size);

  using Function = u64 (*)(u64, u64, u64, u64, u64, u64, u64, u64);
  auto function = reinterpret_cast<Function>(code);
  EXPECT_EQ(function(0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88), 0x11u);
}

TEST(ARM64Trampoline, NothingAndZeroHaveNativeReturns) {
  ExecutablePage page;
  ASSERT_TRUE(page.valid());
  auto* nothing = page.data(0);
  auto* zero = page.data(64);
  auto scope = page.write_scope();
  const auto nothing_size = arm64_trampoline::emit_nothing(nothing);
  const auto zero_size = arm64_trampoline::emit_zero(zero);
  scope.finish();
  page.make_executable(64 + zero_size);

  EXPECT_EQ(invoke_nothing_with_sentinel(nothing), 0x1357u);
  EXPECT_EQ(reinterpret_cast<u64 (*)()>(zero)(), 0u);
  EXPECT_EQ(nothing_size, 4u);
  EXPECT_EQ(zero_size, 8u);
}

TEST(ARM64Trampoline, Mips2cLayoutKeepsFullAddressAndStackSize) {
  ExecutablePage page;
  ASSERT_TRUE(page.valid());
  auto* code = page.data();
  auto scope = page.write_scope();
  const auto target = function_address(&sum_packed);
  const auto bridge = function_address(_stack_call_arm64);
  const auto size = arm64_trampoline::emit_mips2c(code, 0x12345, target, bridge);

  ASSERT_EQ(size, 56u);
  EXPECT_EQ(read_word(code, 0), arm64_trampoline::encode_ldr_literal_x17(0, 32));
  EXPECT_EQ(read_word(code, 4), arm64_trampoline::encode_ldr_literal_x16(4, 40));
  EXPECT_EQ(read_word(code, 8), arm64_trampoline::encode_ldr_literal(0x58000000 | 15, 8, 48));
  EXPECT_EQ(read_word(code, 12), arm64_trampoline::kBrX15);
  EXPECT_EQ(read_literal(code, 32), 0x12345u);
  EXPECT_EQ(read_literal(code, 40), reinterpret_cast<uint64_t>(target));
  EXPECT_EQ(read_literal(code, 48), reinterpret_cast<uint64_t>(bridge));
  scope.finish();
}

TEST(ARM64Trampoline, RepatchesWxxAndKeepsMultipleInstancesActive) {
  ExecutablePage page;
  ASSERT_TRUE(page.valid());
  auto* first = page.data(0);
  auto* second = page.data(64);
  const auto bridge = function_address(_arg_call_arm64);
  auto scope = page.write_scope();

  arm64_trampoline::emit_c_function(first, function_address(&trampoline_add4), bridge, false);
  arm64_trampoline::emit_c_function(second, function_address(&trampoline_add4), bridge, false);
  scope.finish();
  page.make_executable(104);
  using Function = u64 (*)(u64, u64, u64, u64);
  EXPECT_EQ(reinterpret_cast<Function>(first)(1, 2, 3, 4), 10u);
  EXPECT_EQ(reinterpret_cast<Function>(second)(4, 3, 2, 1), 10u);

  page.make_writable();
  auto repatch_scope = page.write_scope();
  arm64_trampoline::emit_c_function(first, function_address(&return_fourth), bridge, false);
  arm64_trampoline::flush(first, 40);
  repatch_scope.finish();
  page.make_executable(104);
  // The old target remains in the second active instance while the first is repatched.
  EXPECT_EQ(reinterpret_cast<Function>(second)(4, 3, 2, 1), 10u);
  EXPECT_EQ(reinterpret_cast<Function>(first)(4, 3, 2, 99), 99u);
}

#endif
