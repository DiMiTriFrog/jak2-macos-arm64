#include "emitter_util.h"

#include "goalc/emitter/CodeTester.h"
#include "goalc/emitter/IGen.h"
#include "gtest/gtest.h"

using namespace emitter;

// ARM64 integer division.  The x86 contract (dividend in EAX/EDX:EAX, quotient
// in EAX) is mirrored on ARM64 with the dividend in W0, the divisor in the
// operand register, and the quotient in W0.  These tests execute real ARM64
// code on the host (Apple Silicon) and compare against the C++ model.
//
// Note the two documented divergences from x86:
//   - INT_MIN / -1 saturates to INT_MIN on ARM64 (x86 traps with #DE).
//   - division by zero yields 0 on ARM64 (x86 traps with #DE).

namespace {

void emit_signed_quotient(CodeTester& tester, Register divisor) {
  tester.emit(IGen::cdq(tester.generator()));
  tester.emit(IGen::idiv_gpr32(tester.generator(), divisor));
  tester.emit(IGen::movsx_r64_r32(tester.generator(), X0, X0));
  tester.emit_return();
}

void emit_unsigned_quotient(CodeTester& tester, Register divisor) {
  tester.emit(IGen::unsigned_div_gpr32(tester.generator(), divisor));
  tester.emit(IGen::movsx_r64_r32(tester.generator(), X0, X0));
  tester.emit_return();
}

void emit_signed_remainder(CodeTester& tester, Register divisor) {
  // dividend in x0, divisor in divisor-reg, x2 = dividend backup.
  tester.emit(IGen::mov_gpr64_gpr64(tester.generator(), X2, X0));
  tester.emit(IGen::cdq(tester.generator()));
  tester.emit(IGen::idiv_gpr32(tester.generator(), divisor));
  // x1 = divisor * quotient (32-bit).
  tester.emit(IGen::imul_gpr32_gpr32(tester.generator(), X1, X0));
  // x2 = dividend - divisor * quotient = remainder.
  tester.emit(IGen::sub_gpr64_gpr64(tester.generator(), X2, X1));
  tester.emit(IGen::movsx_r64_r32(tester.generator(), X0, X2));
  tester.emit_return();
}

// 32-bit signed division matching ARM64 sdiv: truncates toward zero, and
// saturates INT_MIN / -1 to INT_MIN.  Written without C++ signed-overflow UB.
s32 arm64_sdiv32(s32 n, s32 d) {
  if (n == INT32_MIN && d == -1) {
    return INT32_MIN;
  }
  return n / d;
}

}  // namespace

TEST(ARM64EmitterIntegerDivision, SignedEncoding) {
  CodeTester tester(InstructionSet::ARM64);
  tester.init_code_buffer(256);
  // cdq ; idiv x1 ; movsx x0, w0 ; ret
  tester.emit(IGen::cdq(tester.generator()));
  tester.emit(IGen::idiv_gpr32(tester.generator(), X1));
  tester.emit(IGen::movsx_r64_r32(tester.generator(), X0, X0));
  tester.emit_return();
  EXPECT_EQ(tester.dump_to_hex_string(),
            "00 7c 40 93 00 0c c1 1a 00 7c 40 93 c0 03 5f d6");
}

TEST(ARM64EmitterIntegerDivision, UnsignedEncoding) {
  CodeTester tester(InstructionSet::ARM64);
  tester.init_code_buffer(256);
  // udiv x0, x0, x1 ; movsx x0, w0 ; ret
  tester.emit(IGen::unsigned_div_gpr32(tester.generator(), X1));
  tester.emit(IGen::movsx_r64_r32(tester.generator(), X0, X0));
  tester.emit_return();
  EXPECT_EQ(tester.dump_to_hex_string(), "00 08 c1 1a 00 7c 40 93 c0 03 5f d6");
}

TEST(ARM64EmitterInteger, MovePreservesStackPointer) {
#if defined(__aarch64__)
  CodeTester tester(InstructionSet::ARM64);
  tester.init_code_buffer(256);
  // ORR/MOV interprets register 31 as XZR.  This sequence must preserve SP
  // through both directions of the register move.
  tester.emit(IGen::mov_gpr64_gpr64(tester.generator(), X0, SP));
  tester.emit(IGen::mov_gpr64_gpr64(tester.generator(), SP, X0));
  tester.emit(IGen::mov_gpr64_gpr64(tester.generator(), X0, SP));
  tester.emit_return();
  EXPECT_NE(tester.execute(0, 0, 0, 0), 0u);
#endif
}

TEST(ARM64EmitterIntegerDivision, SignedQuotient) {
  CodeTester tester(InstructionSet::ARM64);
  tester.init_code_buffer(256);
  emit_signed_quotient(tester, X1);
  struct Case {
    s32 dividend, divisor;
  };
  // 7/3, -7/3, 7/-3, -7/-3, 0/n, n/1, 32-bit limits.
  std::vector<Case> cases = {
      {7, 3}, {-7, 3}, {7, -3}, {-7, -3}, {0, 5}, {0, -5},
      {INT32_MIN, 1}, {INT32_MAX, 1}, {INT32_MIN, -1}, {INT32_MAX, -1},
      {12345, -67}, {-12345, 67}, {-2147483647, -1}, {INT32_MAX, INT32_MAX},
  };
  for (auto& c : cases) {
    s64 expected = arm64_sdiv32(c.dividend, c.divisor);  // sign-extended
    execute_tester(tester, (u64)(s64)c.dividend, (u64)(s64)c.divisor, 0, 0, (u64)expected);
  }
}

TEST(ARM64EmitterIntegerDivision, UnsignedQuotient) {
  CodeTester tester(InstructionSet::ARM64);
  tester.init_code_buffer(256);
  emit_unsigned_quotient(tester, X1);
  struct Case {
    u32 dividend, divisor;
  };
  // include unsigned values with the high bit set.
  std::vector<Case> cases = {
      {7u, 3u}, {0u, 5u}, {1u, 1u}, {0xFFFFFFFFu, 1u}, {0x80000000u, 2u},
      {0xFFFFFFFFu, 0xFFFFFFFFu}, {0x80000000u, 0x80000000u}, {0xDEADBEEFu, 0x100u},
  };
  for (auto& c : cases) {
    u32 expected_u32 = c.dividend / c.divisor;
    s64 expected = (s64)(s32)expected_u32;  // movsx sign-extends the 32-bit quotient
    execute_tester(tester, c.dividend, c.divisor, 0, 0, (u64)expected);
  }
}

TEST(ARM64EmitterIntegerDivision, SignedRemainder) {
  CodeTester tester(InstructionSet::ARM64);
  tester.init_code_buffer(256);
  emit_signed_remainder(tester, X1);
  struct Case {
    s64 dividend, divisor;
  };
  std::vector<Case> cases = {
      {7, 3}, {-7, 3}, {7, -3}, {-7, -3}, {0, 5}, {INT32_MIN, 3},
      {INT32_MAX, 7}, {-12345, 67}, {12345, -67}, {2147483647, -1000},
  };
  for (auto& c : cases) {
    execute_tester(tester, (u64)(s64)c.dividend, (u64)(s64)c.divisor, (u64)(s64)c.dividend, 0,
                   (u64)(s64)(c.dividend % c.divisor));
  }
}

TEST(ARM64EmitterIntegerDivision, DivisorRegisterAliasing) {
  // "Todos los aliasings permitidos": the divisor register may be X0 (same as
  // the dividend -> division by itself), or a scratch register.
  CodeTester tester(InstructionSet::ARM64);
  tester.init_code_buffer(256);
  // divisor in x2: move the second input into x2, divide w0 by w2.
  tester.emit(IGen::mov_gpr64_gpr64(tester.generator(), X2, X1));
  tester.emit(IGen::cdq(tester.generator()));
  tester.emit(IGen::idiv_gpr32(tester.generator(), X2));
  tester.emit(IGen::movsx_r64_r32(tester.generator(), X0, X0));
  tester.emit_return();
  execute_tester(tester, 20, 4, 0, 0, 5);
  execute_tester(tester, -20, 4, 0, 0, -5);

  // divisor == dividend register: x0 / x0 = 1 (for non-zero).
  CodeTester self(InstructionSet::ARM64);
  self.init_code_buffer(256);
  self.emit(IGen::cdq(self.generator()));
  self.emit(IGen::idiv_gpr32(self.generator(), X0));
  self.emit(IGen::movsx_r64_r32(self.generator(), X0, X0));
  self.emit_return();
  execute_tester(self, 42, 0, 0, 0, 1);
  execute_tester(self, -42, 0, 0, 0, 1);
}

TEST(ARM64EmitterIntegerDivision, DivideByZeroYieldsZero) {
  // ARM64 udiv/sdiv return 0 for a zero divisor instead of trapping like x86.
  CodeTester tester(InstructionSet::ARM64);
  tester.init_code_buffer(256);
  emit_signed_quotient(tester, X1);
  execute_tester(tester, 7, 0, 0, 0, 0);
  execute_tester(tester, -7, 0, 0, 0, 0);

  CodeTester utester(InstructionSet::ARM64);
  utester.init_code_buffer(256);
  emit_unsigned_quotient(utester, X1);
  execute_tester(utester, 7, 0, 0, 0, 0);
  execute_tester(utester, 0xFFFFFFFFu, 0, 0, 0, 0);
}
