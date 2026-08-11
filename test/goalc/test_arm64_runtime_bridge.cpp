#include <cstdint>
#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>

#include "gtest/gtest.h"

#if defined(__aarch64__)

using u64 = uint64_t;

extern "C" __attribute__((naked)) u64 test_invoke_arg_call_arm64(void*, u64, u64, u64, u64) {
  asm("stp x29, x30, [sp, #-16]!\n"
      "mov x16, x0\n"
      "mov x0, x1\n"
      "mov x1, x2\n"
      "mov x2, x3\n"
      "mov x3, x4\n"
      "bl _arg_call_arm64\n"
      "ldp x29, x30, [sp], #16\n"
      "ret\n");
}

extern "C" __attribute__((naked)) u64 test_invoke_stack_call_arm64(void*, u64, u64, u64, u64) {
  asm("stp x29, x30, [sp, #-16]!\n"
      "mov x16, x0\n"
      "mov x0, x1\n"
      "mov x1, x2\n"
      "mov x2, x3\n"
      "mov x3, x4\n"
      "bl _stack_call_arm64\n"
      "ldp x29, x30, [sp], #16\n"
      "ret\n");
}

extern "C" __attribute__((naked)) u64 test_invoke_mips2c_call_arm64(void*, u64, u64, u64, u64,
                                                                      u64) {
  asm("stp x29, x30, [sp, #-16]!\n"
      "mov x9, x0\n"
      "mov x8, x1\n"
      "mov x0, x2\n"
      "mov x1, x3\n"
      "mov x2, x4\n"
      "mov x3, x5\n"
      "mov x4, #0\n"
      "mov x5, #0\n"
      "mov x6, #0\n"
      "mov x7, #0\n"
      "mov x17, x8\n"
      "mov x16, x9\n"
      "bl _mips2c_call_arm64\n"
      "ldp x29, x30, [sp], #16\n"
      "ret\n");
}

extern "C" __attribute__((naked)) u64 clobber_callers_and_simd() {
  asm("mov x0, #0x5a\n"
      "mov x1, #0x101\n"
      "mov x2, #0x202\n"
      "mov x3, #0x303\n"
      "mov x4, #0x404\n"
      "mov x5, #0x505\n"
      "mov x6, #0x606\n"
      "mov x7, #0x707\n"
      "mov x8, #0x808\n"
      "mov x9, #0x909\n"
      "mov x10, #0xa0a\n"
      "mov x11, #0xb0b\n"
      "mov x12, #0xc0c\n"
      "mov x13, #0xd0d\n"
      "mov x14, #0xe0e\n"
      "mov x15, #0xf0f\n"
      "mov x16, #0x1616\n"
      "mov x17, #0x1717\n"
      "movi v8.16b, #0xa8\n"
      "movi v9.16b, #0xa9\n"
      "movi v10.16b, #0xaa\n"
      "movi v11.16b, #0xab\n"
      "movi v12.16b, #0xac\n"
      "movi v13.16b, #0xad\n"
      "movi v14.16b, #0xae\n"
      "movi v15.16b, #0xaf\n"
      "ret\n");
}

extern "C" __attribute__((naked)) u64 clobber_goal_saved_registers() {
  asm("mov x20, #0x2020\n"
      "mov x21, #0x2121\n"
      "mov x22, #0x2222\n"
      "mov x0, #0x5a\n"
      "ret\n");
}


extern "C" u64 call_goal_asm_arm64(u64 a0,
                                    u64 a1,
                                    u64 a2,
                                    void* target,
                                    void* st,
                                    void* offset) asm("_call_goal_asm_arm64");
extern "C" u64 call_goal8_asm_arm64(void* target,
                                     u64* args,
                                     u64 zero,
                                     u64 pp,
                                     u64 st,
                                     void* offset) asm("_call_goal8_asm_arm64");
extern "C" u64 call_goal_on_stack_asm_arm64(u64 stack,
                                             u64 unused0,
                                             u64 unused1,
                                             void* target,
                                             void* st,
                                             void* offset) asm("_call_goal_on_stack_asm_arm64");

extern "C" u64 add4(u64 a0, u64 a1, u64 a2, u64 a3) {
  return a0 + a1 + a2 + a3;
}

extern "C" u64 sum_packed4(const u64* args) {
  return args[0] + args[1] + args[2] + args[3];
}

extern "C" u64 sum_mips_context(const u64* context) {
  return context[8] + context[10] + context[12] + context[14];
}

extern "C" u64 add3(u64 a0, u64 a1, u64 a2) {
  return a0 + a1 + a2;
}

extern "C" u64 zero_goal() {
  return 0;
}

extern "C" u64 add8(u64 a0,
                     u64 a1,
                     u64 a2,
                     u64 a3,
                     u64 a4,
                     u64 a5,
                     u64 a6,
                     u64 a7) {
  return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7;
}

extern "C" u64 zero_args() {
  return 0x100;
}

extern "C" u64 one_arg(u64 a0) {
  return a0 + 0x100;
}

extern "C" u64 seven_args(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6) {
  return a0 + a1 + a2 + a3 + a4 + a5 + a6;
}

extern "C" u64 eight_args(u64 a0,
                           u64 a1,
                           u64 a2,
                           u64 a3,
                           u64 a4,
                           u64 a5,
                           u64 a6,
                           u64 a7) {
  return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7;
}

extern "C" u64 nested_goal(u64 a0, u64 a1, u64 a2) {
  return call_goal_asm_arm64(a0, a1, a2, reinterpret_cast<void*>(&add3),
                             reinterpret_cast<void*>(0x1111),
                             reinterpret_cast<void*>(0x2222));
}

extern "C" __attribute__((naked)) u64 test_arg_call_preserves_sentinels_arm64(void*) {
  asm("stp x29, x30, [sp, #-16]!\n"
      "stp x19, x20, [sp, #-16]!\n"
      "stp x21, x22, [sp, #-16]!\n"
      "stp q8, q9, [sp, #-32]!\n"
      "stp q10, q11, [sp, #-32]!\n"
      "stp q12, q13, [sp, #-32]!\n"
      "stp q14, q15, [sp, #-32]!\n"
      "mov x9, x0\n"
      "mov x19, #0x1111\n"
      "mov x20, #0x2222\n"
      "mov x21, #0x3333\n"
      "mov x22, #0x4444\n"
      "movi v8.16b, #0x18\n"
      "movi v9.16b, #0x19\n"
      "movi v10.16b, #0x1a\n"
      "movi v11.16b, #0x1b\n"
      "movi v12.16b, #0x1c\n"
      "movi v13.16b, #0x1d\n"
      "movi v14.16b, #0x1e\n"
      "movi v15.16b, #0x1f\n"
      "mov x0, #1\n"
      "mov x1, #2\n"
      "mov x2, #3\n"
      "mov x3, #4\n"
      "mov x16, x9\n"
      "bl _arg_call_arm64\n"
      "mov x10, #1\n"
      "mov x11, #0x1111\n"
      "cmp x19, x11\n"
      "b.ne 1f\n"
      "mov x11, #0x2222\n"
      "cmp x20, x11\n"
      "b.ne 1f\n"
      "mov x11, #0x3333\n"
      "cmp x21, x11\n"
      "b.ne 1f\n"
      "mov x11, #0x4444\n"
      "cmp x22, x11\n"
      "b.ne 1f\n"
      "umov w11, v8.b[0]\n"
      "mov w12, #0x18\n"
      "cmp w11, w12\n"
      "b.ne 1f\n"
      "umov w11, v9.b[0]\n"
      "mov w12, #0x19\n"
      "cmp w11, w12\n"
      "b.ne 1f\n"
      "umov w11, v10.b[0]\n"
      "mov w12, #0x1a\n"
      "cmp w11, w12\n"
      "b.ne 1f\n"
      "umov w11, v11.b[0]\n"
      "mov w12, #0x1b\n"
      "cmp w11, w12\n"
      "b.ne 1f\n"
      "umov w11, v12.b[0]\n"
      "mov w12, #0x1c\n"
      "cmp w11, w12\n"
      "b.ne 1f\n"
      "umov w11, v13.b[0]\n"
      "mov w12, #0x1d\n"
      "cmp w11, w12\n"
      "b.ne 1f\n"
      "umov w11, v14.b[0]\n"
      "mov w12, #0x1e\n"
      "cmp w11, w12\n"
      "b.ne 1f\n"
      "umov w11, v15.b[0]\n"
      "mov w12, #0x1f\n"
      "cmp w11, w12\n"
      "b.ne 1f\n"
      "mov x0, x10\n"
      "b 2f\n"
      "1:\n"
      "mov x0, xzr\n"
      "2:\n"
      "ldp q14, q15, [sp], #32\n"
      "ldp q12, q13, [sp], #32\n"
      "ldp q10, q11, [sp], #32\n"
      "ldp q8, q9, [sp], #32\n"
      "ldp x21, x22, [sp], #16\n"
      "ldp x19, x20, [sp], #16\n"
      "ldp x29, x30, [sp], #16\n"
      "ret\n");
}

extern "C" __attribute__((naked)) u64 test_stack_call_preserves_sentinels_arm64(void*) {
  asm("stp x29, x30, [sp, #-16]!\n"
      "stp x19, x20, [sp, #-16]!\n"
      "stp x21, x22, [sp, #-16]!\n"
      "stp q8, q9, [sp, #-32]!\n"
      "stp q10, q11, [sp, #-32]!\n"
      "stp q12, q13, [sp, #-32]!\n"
      "stp q14, q15, [sp, #-32]!\n"
      "mov x9, x0\n"
      "mov x19, #0x1111\n"
      "mov x20, #0x2222\n"
      "mov x21, #0x3333\n"
      "mov x22, #0x4444\n"
      "movi v8.16b, #0x18\n"
      "movi v9.16b, #0x19\n"
      "movi v10.16b, #0x1a\n"
      "movi v11.16b, #0x1b\n"
      "movi v12.16b, #0x1c\n"
      "movi v13.16b, #0x1d\n"
      "movi v14.16b, #0x1e\n"
      "movi v15.16b, #0x1f\n"
      "mov x0, #1\n"
      "mov x1, #2\n"
      "mov x2, #3\n"
      "mov x3, #4\n"
      "mov x4, #5\n"
      "mov x5, #6\n"
      "mov x6, #7\n"
      "mov x7, #8\n"
      "mov x16, x9\n"
      "bl _stack_call_arm64\n"
      "mov x10, #1\n"
      "mov x11, #0x1111\n"
      "cmp x19, x11\n"
      "b.ne 3f\n"
      "mov x11, #0x2222\n"
      "cmp x20, x11\n"
      "b.ne 3f\n"
      "mov x11, #0x3333\n"
      "cmp x21, x11\n"
      "b.ne 3f\n"
      "mov x11, #0x4444\n"
      "cmp x22, x11\n"
      "b.ne 3f\n"
      "umov w11, v8.b[0]\n"
      "mov w12, #0x18\n"
      "cmp w11, w12\n"
      "b.ne 3f\n"
      "umov w11, v15.b[0]\n"
      "mov w12, #0x1f\n"
      "cmp w11, w12\n"
      "b.ne 3f\n"
      "mov x0, x10\n"
      "b 4f\n"
      "3:\n"
      "mov x0, xzr\n"
      "4:\n"
      "ldp q14, q15, [sp], #32\n"
      "ldp q12, q13, [sp], #32\n"
      "ldp q10, q11, [sp], #32\n"
      "ldp q8, q9, [sp], #32\n"
      "ldp x21, x22, [sp], #16\n"
      "ldp x19, x20, [sp], #16\n"
      "ldp x29, x30, [sp], #16\n"
      "ret\n");
}

extern "C" __attribute__((naked)) u64 test_call_goal_preserves_sentinels_arm64(void*) {
  asm("stp x29, x30, [sp, #-16]!\n"
      "stp x19, x20, [sp, #-16]!\n"
      "stp x21, x22, [sp, #-16]!\n"
      "mov x19, #0x1111\n"
      "mov x20, #0x2222\n"
      "mov x21, #0x3333\n"
      "mov x22, #0x4444\n"
      "mov x9, x0\n"
      "mov x0, #1\n"
      "mov x1, #2\n"
      "mov x2, #3\n"
      "mov x3, x9\n"
      "mov x4, #0x5555\n"
      "mov x5, #0x6666\n"
      "bl _call_goal_asm_arm64\n"
      "mov x10, #1\n"
      "mov x11, #0x1111\n"
      "cmp x19, x11\n"
      "b.ne 5f\n"
      "mov x11, #0x2222\n"
      "cmp x20, x11\n"
      "b.ne 5f\n"
      "mov x11, #0x3333\n"
      "cmp x21, x11\n"
      "b.ne 5f\n"
      "mov x11, #0x4444\n"
      "cmp x22, x11\n"
      "b.ne 5f\n"
      "mov x0, x10\n"
      "b 6f\n"
      "5:\n"
      "mov x0, xzr\n"
      "6:\n"
      "ldp x21, x22, [sp], #16\n"
      "ldp x19, x20, [sp], #16\n"
      "ldp x29, x30, [sp], #16\n"
      "ret\n");
}

extern "C" __attribute__((naked)) u64 test_call_goal8_preserves_sentinels_arm64(void*) {
  asm("stp x29, x30, [sp, #-16]!\n"
      "stp x19, x20, [sp, #-16]!\n"
      "stp x21, x22, [sp, #-16]!\n"
      "sub sp, sp, #64\n"
      "mov x19, #0x1111\n"
      "mov x20, #0x2222\n"
      "mov x21, #0x3333\n"
      "mov x22, #0x4444\n"
      "mov x9, x0\n"
      "mov x0, x9\n"
      "mov x1, sp\n"
      "mov x2, xzr\n"
      "mov x3, #0x5555\n"
      "mov x4, #0x6666\n"
      "mov x5, #0x7777\n"
      "bl _call_goal8_asm_arm64\n"
      "add sp, sp, #64\n"
      "mov x10, #1\n"
      "mov x11, #0x1111\n"
      "cmp x19, x11\n"
      "b.ne 7f\n"
      "mov x11, #0x2222\n"
      "cmp x20, x11\n"
      "b.ne 7f\n"
      "mov x11, #0x3333\n"
      "cmp x21, x11\n"
      "b.ne 7f\n"
      "mov x11, #0x4444\n"
      "cmp x22, x11\n"
      "b.ne 7f\n"
      "mov x0, x10\n"
      "b 8f\n"
      "7:\n"
      "mov x0, xzr\n"
      "8:\n"
      "ldp x21, x22, [sp], #16\n"
      "ldp x19, x20, [sp], #16\n"
      "ldp x29, x30, [sp], #16\n"
      "ret\n");
}

extern "C" __attribute__((naked)) u64 test_call_goal_on_stack_preserves_sentinels_arm64(void*, void*) {
  asm("stp x29, x30, [sp, #-16]!\n"
      "stp x19, x20, [sp, #-16]!\n"
      "stp x21, x22, [sp, #-16]!\n"
      "mov x19, #0x1111\n"
      "mov x20, #0x2222\n"
      "mov x21, #0x3333\n"
      "mov x22, #0x4444\n"
      "mov x9, x0\n"
      "mov x0, x9\n"
      "mov x3, x1\n"
      "mov x4, #0x5555\n"
      "mov x5, #0x6666\n"
      "bl _call_goal_on_stack_asm_arm64\n"
      "mov x10, #1\n"
      "mov x11, #0x1111\n"
      "cmp x19, x11\n"
      "b.ne 9f\n"
      "mov x11, #0x2222\n"
      "cmp x20, x11\n"
      "b.ne 9f\n"
      "mov x11, #0x3333\n"
      "cmp x21, x11\n"
      "b.ne 9f\n"
      "mov x11, #0x4444\n"
      "cmp x22, x11\n"
      "b.ne 9f\n"
      "mov x0, x10\n"
      "b 10f\n"
      "9:\n"
      "mov x0, xzr\n"
      "10:\n"
      "ldp x21, x22, [sp], #16\n"
      "ldp x19, x20, [sp], #16\n"
      "ldp x29, x30, [sp], #16\n"
      "ret\n");
}

TEST(ARM64RuntimeBridge, ArgCallPassesRegistersAndReturns) {
  EXPECT_EQ(test_invoke_arg_call_arm64(reinterpret_cast<void*>(&add4), 1, 2, 3, 4), 10u);
}

TEST(ARM64RuntimeBridge, StackCallPassesPackedArgumentsAndReturns) {
  EXPECT_EQ(test_invoke_stack_call_arm64(reinterpret_cast<void*>(&sum_packed4), 5, 6, 7, 8),
            26u);
}

TEST(ARM64RuntimeBridge, Mips2cCallBuildsContextAndAlignsOddStackSize) {
  EXPECT_EQ(test_invoke_mips2c_call_arm64(reinterpret_cast<void*>(&sum_mips_context), 9, 11, 13,
                                          17, 19),
            60u);
}

TEST(ARM64RuntimeBridge, CallGoalAsmPassesRuntimeRegisters) {
  EXPECT_EQ(call_goal_asm_arm64(10, 20, 30, reinterpret_cast<void*>(&add3),
                                reinterpret_cast<void*>(0x1111),
                                reinterpret_cast<void*>(0x2222)),
            60u);
}

TEST(ARM64RuntimeBridge, CallGoal8AsmPassesAllEightArguments) {
  u64 args[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  EXPECT_EQ(call_goal8_asm_arm64(reinterpret_cast<void*>(&add8), args, 0, 0x1111, 0x2222,
                                 reinterpret_cast<void*>(0x3333)),
            36u);
}

TEST(ARM64RuntimeBridge, CallGoalOnStackRestoresParentStack) {
  void* storage = nullptr;
  ASSERT_EQ(posix_memalign(&storage, 16, 4096), 0);
  auto stack_top = reinterpret_cast<u64>(storage) + 4096;
  stack_top &= ~u64(15);
  EXPECT_EQ(call_goal_on_stack_asm_arm64(stack_top, 0, 0, reinterpret_cast<void*>(&zero_goal),
                                         reinterpret_cast<void*>(0x4444),
                                         reinterpret_cast<void*>(0x5555)),
            0u);
  free(storage);
}

TEST(ARM64RuntimeBridge, ArgCallPreservesGprAndSimdSentinels) {
  EXPECT_EQ(test_arg_call_preserves_sentinels_arm64(
                reinterpret_cast<void*>(&clobber_callers_and_simd)),
            1u);
}

TEST(ARM64RuntimeBridge, StackCallPreservesGprAndSimdSentinels) {
  EXPECT_EQ(test_stack_call_preserves_sentinels_arm64(
                reinterpret_cast<void*>(&clobber_callers_and_simd)),
            1u);
}

TEST(ARM64RuntimeBridge, GoalBridgesPreserveGprSentinels) {
  auto target = reinterpret_cast<void*>(&clobber_goal_saved_registers);
  EXPECT_EQ(test_call_goal_preserves_sentinels_arm64(target), 1u);
  EXPECT_EQ(test_call_goal8_preserves_sentinels_arm64(target), 1u);
}

TEST(ARM64RuntimeBridge, GoalBridgeSupportsArityMatrix) {
  u64 args[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  EXPECT_EQ(call_goal8_asm_arm64(reinterpret_cast<void*>(&zero_args), args, 0, 0x1111, 0x2222,
                                 reinterpret_cast<void*>(0x3333)),
            0x100u);

  args[0] = 0xabcdef0123456789ull;
  EXPECT_EQ(call_goal8_asm_arm64(reinterpret_cast<void*>(&one_arg), args, 0, 0x1111, 0x2222,
                                 reinterpret_cast<void*>(0x3333)),
            args[0] + 0x100);

  for (u64 i = 0; i < 8; i++) {
    args[i] = 0x1000000000000000ull + i * 0x0101010101010101ull;
  }
  EXPECT_EQ(call_goal8_asm_arm64(reinterpret_cast<void*>(&seven_args), args, 0, 0x1111, 0x2222,
                                 reinterpret_cast<void*>(0x3333)),
            args[0] + args[1] + args[2] + args[3] + args[4] + args[5] + args[6]);
  EXPECT_EQ(call_goal8_asm_arm64(reinterpret_cast<void*>(&eight_args), args, 0, 0x1111, 0x2222,
                                 reinterpret_cast<void*>(0x3333)),
            args[0] + args[1] + args[2] + args[3] + args[4] + args[5] + args[6] + args[7]);
}

TEST(ARM64RuntimeBridge, Mips2cCallHandlesStackSizeBoundaries) {
  for (u64 stack_size : {u64(0), u64(1), u64(7), u64(8), u64(9), u64(16)}) {
    EXPECT_EQ(test_invoke_mips2c_call_arm64(reinterpret_cast<void*>(&sum_mips_context), stack_size,
                                            11, 13, 17, 19),
              60u)
        << "stack_size=" << stack_size;
  }
}

TEST(ARM64RuntimeBridge, GoalBridgesSupportNestedCallsAndRepeatedCalls) {
  EXPECT_EQ(call_goal_asm_arm64(10, 20, 30, reinterpret_cast<void*>(&nested_goal),
                                reinterpret_cast<void*>(0x1111),
                                reinterpret_cast<void*>(0x2222)),
            60u);

  u64 actual = 0;
  u64 expected = 0;
  for (u64 i = 0; i < 100000; i++) {
    actual += call_goal_asm_arm64(i, i + 1, i + 2, reinterpret_cast<void*>(&add3),
                                  reinterpret_cast<void*>(0x1111),
                                  reinterpret_cast<void*>(0x2222));
    expected += i + (i + 1) + (i + 2);
  }
  EXPECT_EQ(actual, expected);
}

TEST(ARM64RuntimeBridge, GoalOnStackUsesGuardedAlternateStack) {
  const auto page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  ASSERT_GT(page_size, 0u);
  void* storage = mmap(nullptr, page_size * 2, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
  ASSERT_NE(storage, MAP_FAILED);
  ASSERT_EQ(mprotect(storage, page_size, PROT_NONE), 0);

  auto stack_top = reinterpret_cast<u64>(storage) + page_size * 2;
  stack_top &= ~u64(15);
  EXPECT_EQ(test_call_goal_on_stack_preserves_sentinels_arm64(
                reinterpret_cast<void*>(stack_top),
                reinterpret_cast<void*>(&clobber_goal_saved_registers)),
            1u);
  EXPECT_EQ(munmap(storage, page_size * 2), 0);
}

#else

TEST(ARM64RuntimeBridge, NativeOnly) {
  GTEST_SKIP() << "ARM64 runtime bridge tests require an ARM64 host";
}

#endif
