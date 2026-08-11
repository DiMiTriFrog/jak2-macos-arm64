#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
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

struct V128 {
  u64 lo = 0;
  u64 hi = 0;
};

[[maybe_unused]] u32 f32_bits(float value) {
  u32 result;
  memcpy(&result, &value, sizeof(result));
  return result;
}

[[maybe_unused]] float bits_f32(u32 value) {
  float result;
  memcpy(&result, &value, sizeof(result));
  return result;
}

[[maybe_unused]] V128 bits4(u32 a, u32 b, u32 c, u32 d) {
  return {u64(a) | (u64(b) << 32), u64(c) | (u64(d) << 32)};
}

[[maybe_unused]] V128 floats4(float a, float b, float c, float d) {
  return bits4(f32_bits(a), f32_bits(b), f32_bits(c), f32_bits(d));
}

[[maybe_unused]] u32 lane_bits(V128 value, int lane) {
  return u32((lane < 2 ? value.lo : value.hi) >> (32 * (lane & 1)));
}

struct IRHarness {
  ObjectGenerator gen{GameVersion::Jak2, InstructionSet::ARM64};
  TypeSystem ts;
  FunctionDebugInfo dbg;
  FunctionRecord func;
  AllocationResult allocs;

  IRHarness() {
    ts.add_builtin_types(GameVersion::Jak2);
    dbg.name = "vector-test";
    func = gen.add_function_to_seg(0, &dbg);
  }

  void assign(int id, Register reg) {
    std::vector<bool> live(1, true);
    std::vector<Assignment> assignments(1);
    assignments[0].kind = Assignment::Kind::REGISTER;
    assignments[0].reg = reg;
    if (allocs.ass_as_ranges.size() <= size_t(id)) {
      std::vector<bool> dummy_live(1, true);
      std::vector<Assignment> dummy_ass(1);
      allocs.ass_as_ranges.resize(id + 1, AssignmentRange(0, dummy_live, dummy_ass));
    }
    allocs.ass_as_ranges[id] = AssignmentRange(0, live, assignments);
  }

  std::vector<u8> generate() { return gen.generate_data_v3(&ts).segment_data.at(0); }
};

std::unique_ptr<RegVal> make_reg(int id) {
  IRegister ireg;
  ireg.id = id;
  ireg.reg_class = RegClass::VECTOR_FLOAT;
  return std::make_unique<RegVal>(ireg, TypeSpec("vector"));
}

using EmitIR =
    std::function<void(IRHarness&, const std::vector<std::unique_ptr<RegVal>>&, IR_Record)>;

[[maybe_unused]] std::vector<u8> build_three(IR_VFMath3Asm::Kind kind,
                                             Register dst,
                                             Register src1,
                                             Register src2) {
  IRHarness h;
  auto d = make_reg(0);
  auto a = make_reg(1);
  auto b = make_reg(2);
  h.assign(0, dst);
  h.assign(1, src1);
  h.assign(2, src2);
  IR_Record ir = h.gen.add_ir(h.func);
  IR_VFMath3Asm op(true, d.get(), a.get(), b.get(), kind);
  op.do_codegen_arm64(&h.gen, h.allocs, ir);
  return h.generate();
}

[[maybe_unused]] std::vector<u8> build_two(IR_VFMath2Asm::Kind kind,
                                           Register dst,
                                           Register src) {
  IRHarness h;
  auto d = make_reg(0);
  auto a = make_reg(1);
  h.assign(0, dst);
  h.assign(1, src);
  IR_Record ir = h.gen.add_ir(h.func);
  IR_VFMath2Asm op(true, d.get(), a.get(), kind);
  op.do_codegen_arm64(&h.gen, h.allocs, ir);
  return h.generate();
}

[[maybe_unused]] std::vector<u8> build_unary(EmitIR emit) {
  IRHarness h;
  auto d = make_reg(0);
  auto a = make_reg(1);
  h.assign(0, V2);
  h.assign(1, V0);
  IR_Record ir = h.gen.add_ir(h.func);
  std::vector<std::unique_ptr<RegVal>> regs;
  regs.push_back(std::move(d));
  regs.push_back(std::move(a));
  emit(h, regs, ir);
  return h.generate();
}

[[maybe_unused]] std::vector<u8> build_binary(IR_VFMath3Asm::Kind kind,
                                              u8 mask,
                                              int operation) {
  IRHarness h;
  auto d = make_reg(0);
  auto a = make_reg(1);
  auto b = make_reg(2);
  h.assign(0, V2);
  h.assign(1, V0);
  h.assign(2, V1);
  IR_Record ir = h.gen.add_ir(h.func);
  if (operation == 0) {
    IR_VFMath3Asm op(true, d.get(), a.get(), b.get(), kind);
    op.do_codegen_arm64(&h.gen, h.allocs, ir);
  } else {
    IR_BlendVF op(true, d.get(), a.get(), b.get(), mask);
    op.do_codegen_arm64(&h.gen, h.allocs, ir);
  }
  return h.generate();
}

[[maybe_unused]] u64 execute_half(const std::vector<u8>& data,
                                  V128 src0,
                                  V128 src1,
                                  Register result,
                                  bool high) {
#if defined(__aarch64__)
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(2048);
  t.emit(ins_vf_d_gpr(V0, 0, X0));
  t.emit(ins_vf_d_gpr(V0, 1, X1));
  t.emit(ins_vf_d_gpr(V1, 0, X2));
  t.emit(ins_vf_d_gpr(V1, 1, X3));
  t.append_bytes(data.data() + 4, int(data.size() - 4));
  t.emit(umov_gpr64_vf_d(X0, result, high ? 1 : 0));
  t.emit_return();
  return t.execute(src0.lo, src0.hi, src1.lo, src1.hi);
#else
  (void)data;
  (void)src0;
  (void)src1;
  (void)result;
  (void)high;
  return 0;
#endif
}

[[maybe_unused]] V128 execute_vector(const std::vector<u8>& data,
                                     V128 src0,
                                     V128 src1,
                                     Register result = V2) {
  return {execute_half(data, src0, src1, result, false),
          execute_half(data, src0, src1, result, true)};
}

}  // namespace

TEST(ARM64IRVector, Math3AllOpcodesAndLegalAliases) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
#else
  V128 a = floats4(1.5f, -2.0f, 3.25f, -4.5f);
  V128 b = floats4(2.0f, 4.0f, -1.25f, 0.5f);
  for (auto kind : {IR_VFMath3Asm::Kind::XOR, IR_VFMath3Asm::Kind::SUB, IR_VFMath3Asm::Kind::ADD,
                    IR_VFMath3Asm::Kind::MUL, IR_VFMath3Asm::Kind::MAX, IR_VFMath3Asm::Kind::MIN,
                    IR_VFMath3Asm::Kind::DIV}) {
    auto data = build_three(kind, V2, V0, V1);
    auto out = execute_vector(data, a, b);
    for (int lane = 0; lane < 4; lane++) {
      float af = bits_f32(lane_bits(a, lane));
      float bf = bits_f32(lane_bits(b, lane));
      u32 expected;
      if (kind == IR_VFMath3Asm::Kind::XOR) {
        expected = lane_bits(a, lane) ^ lane_bits(b, lane);
      } else {
        float value;
        switch (kind) {
          case IR_VFMath3Asm::Kind::SUB:
            value = af - bf;
            break;
          case IR_VFMath3Asm::Kind::ADD:
            value = af + bf;
            break;
          case IR_VFMath3Asm::Kind::MUL:
            value = af * bf;
            break;
          case IR_VFMath3Asm::Kind::MAX:
            value = std::fmax(af, bf);
            break;
          case IR_VFMath3Asm::Kind::MIN:
            value = std::fmin(af, bf);
            break;
          case IR_VFMath3Asm::Kind::DIV:
            value = af / bf;
            break;
          default:
            value = 0;
            break;
        }
        expected = f32_bits(value);
      }
      EXPECT_EQ(lane_bits(out, lane), expected) << "kind=" << int(kind) << " lane=" << lane;
    }

    // Both legal destination aliases must preserve source values before the
    // operation: dst=src1 and dst=src2.
    auto dst_src1 = build_three(kind, V0, V0, V1);
    auto dst_src2 = build_three(kind, V1, V0, V1);
    auto src1_alias_out = execute_vector(dst_src1, a, b, V0);
    auto src2_alias_out = execute_vector(dst_src2, a, b, V1);
    EXPECT_EQ(src1_alias_out.lo, out.lo) << "src1 alias kind=" << int(kind);
    EXPECT_EQ(src1_alias_out.hi, out.hi) << "src1 alias kind=" << int(kind);
    EXPECT_EQ(src2_alias_out.lo, out.lo) << "src2 alias kind=" << int(kind);
    EXPECT_EQ(src2_alias_out.hi, out.hi) << "src2 alias kind=" << int(kind);
  }
#endif
}

TEST(ARM64IRVector, Math2ConversionsAndSqrtContract) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
#else
  V128 integers = bits4(1u, u32(-2), 100u, u32(-101));
  auto itof = build_two(IR_VFMath2Asm::Kind::ITOF, V2, V0);
  V128 float_out = execute_vector(itof, integers, {});
  EXPECT_FLOAT_EQ(bits_f32(lane_bits(float_out, 0)), 1.0f);
  EXPECT_FLOAT_EQ(bits_f32(lane_bits(float_out, 1)), -2.0f);
  EXPECT_FLOAT_EQ(bits_f32(lane_bits(float_out, 2)), 100.0f);
  EXPECT_FLOAT_EQ(bits_f32(lane_bits(float_out, 3)), -101.0f);

  V128 floats = floats4(3.75f, -3.75f, 0.0f, 101.9f);
  auto ftoi = build_two(IR_VFMath2Asm::Kind::FTOI, V2, V0);
  V128 int_out = execute_vector(ftoi, floats, {});
  EXPECT_EQ(s32(u32(lane_bits(int_out, 0))), 3);
  EXPECT_EQ(s32(u32(lane_bits(int_out, 1))), -3);
  EXPECT_EQ(s32(u32(lane_bits(int_out, 2))), 0);
  EXPECT_EQ(s32(u32(lane_bits(int_out, 3))), 101);

  auto sqrt =
      build_unary([](IRHarness& h, const std::vector<std::unique_ptr<RegVal>>& r, IR_Record ir) {
        IR_SqrtVF op(true, r[0].get(), r[1].get());
        op.do_codegen_arm64(&h.gen, h.allocs, ir);
      });
  V128 roots = floats4(0.0f, 4.0f, 9.0f, -1.0f);
  V128 root_out = execute_vector(sqrt, roots, {});
  EXPECT_FLOAT_EQ(bits_f32(lane_bits(root_out, 0)), 0.0f);
  EXPECT_FLOAT_EQ(bits_f32(lane_bits(root_out, 1)), 2.0f);
  EXPECT_FLOAT_EQ(bits_f32(lane_bits(root_out, 2)), 3.0f);
  EXPECT_TRUE(std::isnan(bits_f32(lane_bits(root_out, 3))));
#endif
}

TEST(ARM64IRVector, BlendAllMasksPreserveSpecialBits) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
#else
  V128 a = bits4(0x7FC12345u, 0x80000000u, 0x7F800000u, 0x00000001u);
  V128 b = bits4(0x00000000u, 0x7FC54321u, 0x80000000u, 0xFFFFFFFFu);
  for (u8 mask = 0; mask < 16; mask++) {
    auto data = build_binary(IR_VFMath3Asm::Kind::ADD, mask, 1);
    V128 out = execute_vector(data, a, b);
    for (int lane = 0; lane < 4; lane++) {
      EXPECT_EQ(lane_bits(out, lane),
                (mask & (1 << lane)) ? lane_bits(b, lane) : lane_bits(a, lane))
          << "mask=" << int(mask) << " lane=" << lane;
    }
  }
#endif
}

TEST(ARM64IRVector, SwizzleAllControlsAndSplatLanes) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
#else
  V128 src = bits4(0x7FC00001u, 0x80000000u, 0x12345678u, 0xFFFFFFFFu);
  for (u16 control = 0; control < 256; control++) {
    auto data = build_unary(
        [control](IRHarness& h, const std::vector<std::unique_ptr<RegVal>>& r, IR_Record ir) {
          IR_SwizzleVF op(true, r[0].get(), r[1].get(), u8(control));
          op.do_codegen_arm64(&h.gen, h.allocs, ir);
        });
    V128 out = execute_vector(data, src, {});
    for (int lane = 0; lane < 4; lane++) {
      int selected = (control >> (2 * lane)) & 3;
      EXPECT_EQ(lane_bits(out, lane), lane_bits(src, selected))
          << "control=" << control << " lane=" << lane;
    }
  }

  for (int lane = 0; lane < 4; lane++) {
    auto data = build_unary(
        [lane](IRHarness& h, const std::vector<std::unique_ptr<RegVal>>& r, IR_Record ir) {
          auto element = static_cast<Register::VF_ELEMENT>(lane);
          IR_SplatVF op(true, r[0].get(), r[1].get(), element);
          op.do_codegen_arm64(&h.gen, h.allocs, ir);
        });
    V128 out = execute_vector(data, src, {});
    u32 expected = lane_bits(src, lane);
    for (int out_lane = 0; out_lane < 4; out_lane++) {
      EXPECT_EQ(lane_bits(out, out_lane), expected) << "source lane=" << lane;
    }
  }
#endif
}

TEST(ARM64IRVector, ScratchRegistersExcludedOnlyOnArm64) {
  auto dst = make_reg(0);
  auto src1 = make_reg(1);
  auto src2 = make_reg(2);
  IR_BlendVF blend(true, dst.get(), src1.get(), src2.get(), 0b0101);
  auto arm_blend = blend.to_rai(InstructionSet::ARM64);
  auto x86_blend = blend.to_rai(InstructionSet::X86);
  ASSERT_EQ(arm_blend.exclude.size(), 2);
  EXPECT_EQ(arm_blend.exclude.at(0), X16);
  EXPECT_EQ(arm_blend.exclude.at(1), V16);
  EXPECT_TRUE(x86_blend.exclude.empty());

  IR_SwizzleVF swizzle(true, dst.get(), src1.get(), 0b11100100);
  auto arm_swizzle = swizzle.to_rai(InstructionSet::ARM64);
  auto x86_swizzle = swizzle.to_rai(InstructionSet::X86);
  ASSERT_EQ(arm_swizzle.exclude.size(), 1);
  EXPECT_EQ(arm_swizzle.exclude.at(0), X16);
  EXPECT_TRUE(x86_swizzle.exclude.empty());
}
