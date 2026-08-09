#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>

#include "common/type_system/TypeSystem.h"

#include "goalc/compiler/IR.h"
#include "goalc/debugger/DebugInfo.h"
#include "goalc/emitter/CodeTester.h"
#include "goalc/emitter/IGen.h"
#include "goalc/emitter/IGenARM64.h"
#include "goalc/emitter/ObjectGenerator.h"
#include "gtest/gtest.h"

using namespace emitter;
using namespace emitter::IGen;
using namespace emitter::IGen::ARM64;

namespace {

struct IRHarness {
  ObjectGenerator gen{GameVersion::Jak2, InstructionSet::ARM64};
  TypeSystem ts;
  FunctionDebugInfo dbg;
  FunctionRecord func;
  AllocationResult allocs;

  IRHarness() {
    ts.add_builtin_types(GameVersion::Jak2);
    dbg.name = "math-test";
    func = gen.add_function_to_seg(0, &dbg);
  }

  void assign(int id, Register reg, int n_ir) {
    std::vector<bool> live(n_ir, true);
    std::vector<Assignment> ass(n_ir);
    for (auto& a : ass) {
      a.kind = Assignment::Kind::REGISTER;
      a.reg = reg;
    }
    if (allocs.ass_as_ranges.size() <= size_t(id)) {
      std::vector<bool> dummy_live(1, true);
      std::vector<Assignment> dummy_ass(1);
      allocs.ass_as_ranges.resize(id + 1, AssignmentRange(0, dummy_live, dummy_ass));
    }
    allocs.ass_as_ranges[id] = AssignmentRange(0, live, ass);
  }

  std::vector<u8> generate() { return gen.generate_data_v3(&ts).segment_data.at(0); }
};

[[maybe_unused]] std::unique_ptr<RegVal> make_reg(int id, RegClass cls, const TypeSpec& ts) {
  IRegister ireg;
  ireg.id = id;
  ireg.reg_class = cls;
  return std::make_unique<RegVal>(ireg, ts);
}

[[maybe_unused]] u64 execute_gpr(const std::vector<u8>& data,
                                 Register dest,
                                 Register arg,
                                 u64 dest_input,
                                 u64 arg_input) {
#if defined(__aarch64__)
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(512);
  t.append_bytes(data.data() + 4, int(data.size() - 4));
  t.emit(mov_gpr64_gpr64(t.generator(), X0, dest));
  t.emit_return();
  std::array<u64, 4> args = {0, 0, 0, 0};
  args.at(dest.id()) = dest_input;
  if (arg != dest) {
    args.at(arg.id()) = arg_input;
  }
  return t.execute(args[0], args[1], args[2], args[3]);
#else
  (void)data;
  (void)dest;
  (void)arg;
  (void)dest_input;
  (void)arg_input;
  return 0;
#endif
}

[[maybe_unused]] u64 execute_float(const std::vector<u8>& data, u32 lhs, u32 rhs, Register dest) {
#if defined(__aarch64__)
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(512);
  t.emit(movd_f32_gpr32(t.generator(), V0, X0));
  t.emit(movd_f32_gpr32(t.generator(), V1, X1));
  t.append_bytes(data.data() + 4, int(data.size() - 4));
  t.emit(movd_gpr32_f32(t.generator(), X0, dest));
  t.emit_return();
  return t.execute(lhs, rhs, 0, 0) & 0xffffffff;
#else
  (void)data;
  (void)lhs;
  (void)rhs;
  (void)dest;
  return 0;
#endif
}

[[maybe_unused]] u64 execute_float_to_int(const std::vector<u8>& data, u32 input, Register dest) {
#if defined(__aarch64__)
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(512);
  t.emit(movd_f32_gpr32(t.generator(), V0, X1));
  t.append_bytes(data.data() + 4, int(data.size() - 4));
  t.emit(mov_gpr64_gpr64(t.generator(), X0, dest));
  t.emit_return();
  return t.execute(0, input, 0, 0);
#else
  (void)data;
  (void)input;
  (void)dest;
  return 0;
#endif
}

[[maybe_unused]] u32 execute_int_to_float(const std::vector<u8>& data, u32 input, Register dest) {
#if defined(__aarch64__)
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(512);
  t.append_bytes(data.data() + 4, int(data.size() - 4));
  t.emit(movd_gpr32_f32(t.generator(), X0, dest));
  t.emit_return();
  return u32(t.execute(0, input, 0, 0));
#else
  (void)data;
  (void)input;
  (void)dest;
  return 0;
#endif
}

[[maybe_unused]] u32 float_bits(float value) {
  u32 bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

[[maybe_unused]] float bits_float(u32 bits) {
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

[[maybe_unused]] u64 sign_extend_32(u32 value) {
  return u64(s64(s32(value)));
}

[[maybe_unused]] void build_integer(IRHarness& h,
                                    IntegerMathKind kind,
                                    RegVal* dest,
                                    RegVal* arg,
                                    Register dest_reg,
                                    Register arg_reg,
                                    u8 shift = 0) {
  h.assign(0, dest_reg, 1);
  if (arg) {
    h.assign(1, arg_reg, 1);
  }
  IR_Record ir = h.gen.add_ir(h.func);
  if (arg) {
    IR_IntegerMath math(kind, dest, arg);
    math.do_codegen_arm64(&h.gen, h.allocs, ir);
  } else {
    IR_IntegerMath math(kind, dest, shift);
    math.do_codegen_arm64(&h.gen, h.allocs, ir);
  }
}

}  // namespace

TEST(ARM64IRIntegerMath, BinaryBitwiseAndArithmetic) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
#else
  const u64 lhs = 0x8000000012345678ull;
  const u64 rhs = 0x00000000fedcba98ull;
  struct Case {
    IntegerMathKind kind;
    u64 expected;
  };
  const std::array<Case, 7> cases = {
      {{IntegerMathKind::ADD_64, lhs + rhs},
       {IntegerMathKind::SUB_64, lhs - rhs},
       {IntegerMathKind::AND_64, lhs & rhs},
       {IntegerMathKind::OR_64, lhs | rhs},
       {IntegerMathKind::XOR_64, lhs ^ rhs},
       {IntegerMathKind::IMUL_64, lhs * rhs},
       {IntegerMathKind::IMUL_32, sign_extend_32(u32(u32(lhs) * u32(rhs)))}}};
  for (const auto& c : cases) {
    IRHarness h;
    auto dest = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
    auto arg = make_reg(1, RegClass::GPR_64, TypeSpec("int"));
    build_integer(h, c.kind, dest.get(), arg.get(), X0, X1);
    EXPECT_EQ(execute_gpr(h.generate(), X0, X1, lhs, rhs), c.expected) << "kind=" << int(c.kind);
  }

  IRHarness h;
  auto dest = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
  h.assign(0, X0, 1);
  IR_Record ir = h.gen.add_ir(h.func);
  IR_IntegerMath math(IntegerMathKind::NOT_64, dest.get(), u8(0));
  math.do_codegen_arm64(&h.gen, h.allocs, ir);
  EXPECT_EQ(execute_gpr(h.generate(), X0, X1, lhs, 0), ~lhs);
#endif
}

TEST(ARM64IRIntegerMath, VariableAndConstantShiftsMaskCounts) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
#else
  const u64 value = 0x8000000000000001ull;
  for (u64 count : {0ull, 1ull, 63ull, 64ull}) {
    const u64 masked = count & 63;
    for (auto kind : {IntegerMathKind::SHL_64, IntegerMathKind::SHR_64, IntegerMathKind::SAR_64}) {
      IRHarness h;
      auto dest = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
      build_integer(h, kind, dest.get(), nullptr, X0, X1, u8(count));
      u64 expected;
      if (kind == IntegerMathKind::SHL_64) {
        expected = value << masked;
      } else if (kind == IntegerMathKind::SHR_64) {
        expected = value >> masked;
      } else {
        expected = u64(s64(value) >> masked);
      }
      EXPECT_EQ(execute_gpr(h.generate(), X0, X1, value, 0), expected)
          << "kind=" << int(kind) << " count=" << count;
    }

    for (auto kind :
         {IntegerMathKind::SHLV_64, IntegerMathKind::SHRV_64, IntegerMathKind::SARV_64}) {
      IRHarness h;
      auto dest = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
      auto arg = make_reg(1, RegClass::GPR_64, TypeSpec("int"));
      build_integer(h, kind, dest.get(), arg.get(), X0, X1);
      u64 expected;
      if (kind == IntegerMathKind::SHLV_64) {
        expected = value << masked;
      } else if (kind == IntegerMathKind::SHRV_64) {
        expected = value >> masked;
      } else {
        expected = u64(s64(value) >> masked);
      }
      EXPECT_EQ(execute_gpr(h.generate(), X0, X1, value, count), expected)
          << "kind=" << int(kind) << " count=" << count;
    }
  }
#endif
}

TEST(ARM64IRIntegerMath, DivisionAndModuloUseArbitraryRegisters) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
#else
  const s32 signed_lhs = -123456789;
  const s32 signed_rhs = 321;
  for (auto kind : {IntegerMathKind::IDIV_32, IntegerMathKind::IMOD_32}) {
    IRHarness h;
    auto dest = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
    auto arg = make_reg(1, RegClass::GPR_64, TypeSpec("int"));
    build_integer(h, kind, dest.get(), arg.get(), X2, X3);
    s32 result =
        kind == IntegerMathKind::IDIV_32 ? signed_lhs / signed_rhs : signed_lhs % signed_rhs;
    EXPECT_EQ(execute_gpr(h.generate(), X2, X3, u64(s64(signed_lhs)), u64(s64(signed_rhs))),
              sign_extend_32(u32(result)));
  }

  const u32 unsigned_lhs = 0xffffffffu;
  const u32 unsigned_rhs = 3u;
  for (auto kind : {IntegerMathKind::UDIV_32, IntegerMathKind::UMOD_32}) {
    IRHarness h;
    auto dest = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
    auto arg = make_reg(1, RegClass::GPR_64, TypeSpec("int"));
    build_integer(h, kind, dest.get(), arg.get(), X2, X3);
    u32 result = kind == IntegerMathKind::UDIV_32 ? unsigned_lhs / unsigned_rhs
                                                  : unsigned_lhs % unsigned_rhs;
    EXPECT_EQ(execute_gpr(h.generate(), X2, X3, unsigned_lhs, unsigned_rhs),
              sign_extend_32(result));
  }

  for (auto kind : {IntegerMathKind::IDIV_32, IntegerMathKind::UDIV_32, IntegerMathKind::IMOD_32,
                    IntegerMathKind::UMOD_32}) {
    IRHarness h;
    auto dest = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
    auto arg = make_reg(1, RegClass::GPR_64, TypeSpec("int"));
    build_integer(h, kind, dest.get(), arg.get(), X2, X3);
    u64 expected = (kind == IntegerMathKind::IMOD_32 || kind == IntegerMathKind::UMOD_32)
                       ? sign_extend_32(u32(7))
                       : 0;
    EXPECT_EQ(execute_gpr(h.generate(), X2, X3, 7, 0), expected) << "kind=" << int(kind);
  }

  IRHarness alias_h;
  auto alias_dest = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
  auto alias_arg = make_reg(1, RegClass::GPR_64, TypeSpec("int"));
  build_integer(alias_h, IntegerMathKind::ADD_64, alias_dest.get(), alias_arg.get(), X0, X0);
  EXPECT_EQ(execute_gpr(alias_h.generate(), X0, X0, 7, 0), 14u);
#endif
}

TEST(ARM64IRFloatMath, ScalarArithmeticAndSpecialValues) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
#else
  struct Case {
    FloatMathKind kind;
    float lhs;
    float rhs;
    float expected;
  };
  const std::array<Case, 6> cases = {{{FloatMathKind::ADD_SS, 1.25f, -2.5f, -1.25f},
                                      {FloatMathKind::SUB_SS, 1.25f, -2.5f, 3.75f},
                                      {FloatMathKind::MUL_SS, 1.25f, -2.5f, -3.125f},
                                      {FloatMathKind::DIV_SS, 7.5f, 2.5f, 3.0f},
                                      {FloatMathKind::SQRT_SS, 0.0f, 9.0f, 3.0f},
                                      {FloatMathKind::MIN_SS, -0.0f, 0.0f, -0.0f}}};
  for (const auto& c : cases) {
    IRHarness h;
    auto dest = make_reg(0, RegClass::FLOAT, TypeSpec("float"));
    auto arg = make_reg(1, RegClass::FLOAT, TypeSpec("float"));
    h.assign(0, V0, 1);
    h.assign(1, V1, 1);
    IR_Record ir = h.gen.add_ir(h.func);
    IR_FloatMath math(c.kind, dest.get(), arg.get());
    math.do_codegen_arm64(&h.gen, h.allocs, ir);
    u32 actual = u32(execute_float(h.generate(), float_bits(c.lhs), float_bits(c.rhs), V0));
    EXPECT_EQ(actual, float_bits(c.expected)) << "kind=" << int(c.kind);
  }

  for (auto kind : {FloatMathKind::MIN_SS, FloatMathKind::MAX_SS}) {
    IRHarness h;
    auto dest = make_reg(0, RegClass::FLOAT, TypeSpec("float"));
    auto arg = make_reg(1, RegClass::FLOAT, TypeSpec("float"));
    h.assign(0, V0, 1);
    h.assign(1, V1, 1);
    IR_Record ir = h.gen.add_ir(h.func);
    IR_FloatMath math(kind, dest.get(), arg.get());
    math.do_codegen_arm64(&h.gen, h.allocs, ir);
    u32 actual = u32(execute_float(h.generate(), 0x7fc00001u, float_bits(3.0f), V0));
    EXPECT_TRUE(std::isnan(bits_float(actual))) << "kind=" << int(kind);
  }
#endif
}

TEST(ARM64IRFloatMath, SourceDestinationAlias) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
#else
  IRHarness h;
  auto dest = make_reg(0, RegClass::FLOAT, TypeSpec("float"));
  auto arg = make_reg(1, RegClass::FLOAT, TypeSpec("float"));
  h.assign(0, V0, 1);
  h.assign(1, V0, 1);
  IR_Record ir = h.gen.add_ir(h.func);
  IR_FloatMath math(FloatMathKind::ADD_SS, dest.get(), arg.get());
  math.do_codegen_arm64(&h.gen, h.allocs, ir);
  EXPECT_EQ(execute_float(h.generate(), float_bits(1.5f), float_bits(0.0f), V0), float_bits(3.0f));
#endif
}

TEST(ARM64IRConversion, FloatToIntTruncatesAndHandlesInvalid) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
#else
  const std::array<u32, 8> inputs = {float_bits(1.9f),
                                     float_bits(-1.9f),
                                     float_bits(2147483520.0f),
                                     float_bits(-2147483648.0f),
                                     float_bits(-2147483904.0f),
                                     0x7f800000u,
                                     0xff800000u,
                                     0x7fc00001u};
  for (u32 input : inputs) {
    IRHarness h;
    auto dest = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
    auto src = make_reg(1, RegClass::FLOAT, TypeSpec("float"));
    h.assign(0, X0, 1);
    h.assign(1, V0, 1);
    IR_Record ir = h.gen.add_ir(h.func);
    IR_FloatToInt conversion(dest.get(), src.get());
    conversion.do_codegen_arm64(&h.gen, h.allocs, ir);
    s32 expected;
    float value = bits_float(input);
    if (std::isnan(value)) {
      // ARM64 FCVTZS returns zero for a NaN input.
      expected = 0;
    } else if (value >= 2147483648.0f) {
      // ARM64 saturates positive overflow and +infinity to INT_MAX.
      expected = std::numeric_limits<s32>::max();
    } else if (value < -2147483648.0f || value == -std::numeric_limits<float>::infinity()) {
      // ARM64 saturates negative overflow and -infinity to INT_MIN.
      expected = std::numeric_limits<s32>::min();
    } else {
      expected = s32(std::trunc(value));
    }
    EXPECT_EQ(execute_float_to_int(h.generate(), input, X0), sign_extend_32(u32(expected)))
        << "input=0x" << std::hex << input;
  }
#endif
}

TEST(ARM64IRConversion, IntToFloatRoundsLikeFloat32) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
#else
  for (s32 input :
       {0, 1, -1, 16777217, std::numeric_limits<s32>::max(), std::numeric_limits<s32>::min()}) {
    IRHarness h;
    auto dest = make_reg(0, RegClass::FLOAT, TypeSpec("float"));
    auto src = make_reg(1, RegClass::GPR_64, TypeSpec("int"));
    h.assign(0, V0, 1);
    h.assign(1, X1, 1);
    IR_Record ir = h.gen.add_ir(h.func);
    IR_IntToFloat conversion(dest.get(), src.get());
    conversion.do_codegen_arm64(&h.gen, h.allocs, ir);
    EXPECT_EQ(execute_int_to_float(h.generate(), u32(input), V0), float_bits(float(input)))
        << "input=" << input;
  }
#endif
}
