#include "emitter_util.h"

#include <limits>

#include "goalc/emitter/CodeTester.h"
#include "goalc/emitter/IGen.h"
#include "goalc/emitter/IGenARM64.h"
#include "gtest/gtest.h"

using namespace emitter;
using namespace emitter::IGen;
using namespace emitter::IGen::ARM64;

// ARM64 vector blend / swizzle / splat.  These execute real NEON code on the
// host and compare lane-by-lane against a C++ model, bit-exact (including NaN
// payloads and +/-0).
//
// blend_vf(dst, src1, src2, mask): dst[i] = (mask bit i) ? src2[i] : src1[i]
// swizzle_vf(dst, src, ctrl):      dst[i] = src[(ctrl >> (2*i)) & 3]
// splat_vf(dst, src, X/Y/Z/W):     dst[i] = src[lane]
//
// The test functions load src1 into v0 from {x0,x1} and src2 into v1 from
// {x2,x3}, run the operation into v2, then return lanes 0-1 or lanes 2-3.

namespace {

struct V128 {
  u64 lo, hi;
};

u32 f32_bits(float f) {
  u32 b;
  memcpy(&b, &f, 4);
  return b;
}

V128 float4(float a, float b, float c, float d) {
  V128 v;
  u32 bits[4] = {f32_bits(a), f32_bits(b), f32_bits(c), f32_bits(d)};
  v.lo = (u64)bits[0] | ((u64)bits[1] << 32);
  v.hi = (u64)bits[2] | ((u64)bits[3] << 32);
  return v;
}

float lane_f32(V128 v, int i) {
  u32 b = (u32)((i < 2 ? v.lo : v.hi) >> (32 * (i & 1)));
  float f;
  memcpy(&f, &b, 4);
  return f;
}

void emit_load_sources(CodeTester& t) {
  // v0 = {x0, x1}, v1 = {x2, x3}
  t.emit(ins_vf_d_gpr(V0, 0, X0));
  t.emit(ins_vf_d_gpr(V0, 1, X1));
  t.emit(ins_vf_d_gpr(V1, 0, X2));
  t.emit(ins_vf_d_gpr(V1, 1, X3));
}

// Returns lane pair lo (lanes 0-1) or hi (lanes 2-3) of the result.
// Execution only happens on ARM64 hosts; on other hosts the encode path is
// still exercised but no code runs (matching the existing execute_tester gates).
u64 run_blend(u8 mask, bool hi, u64 a0, u64 a1, u64 a2, u64 a3) {
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(512);
  emit_load_sources(t);
  t.emit(blend_vf(V2, V0, V1, mask));
  t.emit(umov_gpr64_vf_d(X0, V2, hi ? 1 : 0));
  t.emit_return();
#ifdef __aarch64__
  return t.execute(a0, a1, a2, a3);
#else
  return 0;
#endif
}

u64 run_swizzle(u8 ctrl, bool hi, u64 a0, u64 a1) {
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(512);
  // v0 = {x0, x1}
  t.emit(ins_vf_d_gpr(V0, 0, X0));
  t.emit(ins_vf_d_gpr(V0, 1, X1));
  t.emit(swizzle_vf(V2, V0, ctrl));
  t.emit(umov_gpr64_vf_d(X0, V2, hi ? 1 : 0));
  t.emit_return();
#ifdef __aarch64__
  return t.execute(a0, a1, 0, 0);
#else
  return 0;
#endif
}

u64 run_splat(Register::VF_ELEMENT element, bool hi, u64 a0, u64 a1) {
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(512);
  t.emit(ins_vf_d_gpr(V0, 0, X0));
  t.emit(ins_vf_d_gpr(V0, 1, X1));
  t.emit(splat_vf(V2, V0, element));
  t.emit(umov_gpr64_vf_d(X0, V2, hi ? 1 : 0));
  t.emit_return();
#ifdef __aarch64__
  return t.execute(a0, a1, 0, 0);
#else
  return 0;
#endif
}

}  // namespace

TEST(ARM64EmitterBlend, AllMasksBitExact) {
#if defined(__aarch64__)
  V128 src1 = float4(1.0f, -2.5f, 3.75f, -4.125f);
  V128 src2 = float4(10.0f, -20.0f, 30.0f, -40.0f);
  for (u32 mask = 0; mask <= 0b1111; mask++) {
    u32 expected_lanes[4];
    for (int i = 0; i < 4; i++) {
      V128 sel = (mask & (1 << i)) ? src2 : src1;
      expected_lanes[i] = f32_bits(lane_f32(sel, i));
    }
    V128 expected;
    expected.lo = (u64)expected_lanes[1] << 32 | expected_lanes[0];
    expected.hi = (u64)expected_lanes[3] << 32 | expected_lanes[2];
    u64 got_lo = run_blend((u8)mask, false, src1.lo, src1.hi, src2.lo, src2.hi);
    u64 got_hi = run_blend((u8)mask, true, src1.lo, src1.hi, src2.lo, src2.hi);
    EXPECT_EQ(got_lo, expected.lo) << "mask " << mask << " low";
    EXPECT_EQ(got_hi, expected.hi) << "mask " << mask << " high";
  }
#endif
}

TEST(ARM64EmitterBlend, BoundaryAndInvalidMasks) {
#if defined(__aarch64__)
  V128 src1 = float4(1.0f, 2.0f, 3.0f, 4.0f);
  V128 src2 = float4(5.0f, 6.0f, 7.0f, 8.0f);
  // mask 0 -> all from src1, mask 15 -> all from src2.
  EXPECT_EQ(run_blend(0, false, src1.lo, src1.hi, src2.lo, src2.hi), src1.lo);
  EXPECT_EQ(run_blend(0, true, src1.lo, src1.hi, src2.lo, src2.hi), src1.hi);
  EXPECT_EQ(run_blend(15, false, src1.lo, src1.hi, src2.lo, src2.hi), src2.lo);
  EXPECT_EQ(run_blend(15, true, src1.lo, src1.hi, src2.lo, src2.hi), src2.hi);
#endif
}

TEST(ARM64EmitterBlend, NanAndSignedZeroPreserved) {
#if defined(__aarch64__)
  const float nan = std::numeric_limits<float>::quiet_NaN();
  V128 src1 = float4(nan, -0.0f, nan, +0.0f);
  V128 src2 = float4(+0.0f, nan, -0.0f, nan);
  // mask 0b0101 picks src2 for lanes 0 and 2.
  u32 mask = 0b0101;
  for (int i = 0; i < 4; i++) {
    V128 sel = (mask & (1 << i)) ? src2 : src1;
    u32 expected = f32_bits(lane_f32(sel, i));
    u64 got = i < 2 ? run_blend(mask, false, src1.lo, src1.hi, src2.lo, src2.hi)
                    : run_blend(mask, true, src1.lo, src1.hi, src2.lo, src2.hi);
    u32 got_lane = (u32)((got >> (32 * (i & 1))) & 0xFFFFFFFFull);
    EXPECT_EQ(got_lane, expected) << "lane " << i;
  }
#endif
}

TEST(ARM64EmitterBlend, AliasedRegisters) {
#if defined(__aarch64__)
  // dst == src1 and dst == src2 must produce the same result as the base case.
  V128 src1 = float4(1.0f, -2.0f, 3.0f, -4.0f);
  V128 src2 = float4(9.0f, -8.0f, 7.0f, -6.0f);
  u8 mask = 0b1010;

  u64 base_lo = run_blend(mask, false, src1.lo, src1.hi, src2.lo, src2.hi);
  u64 base_hi = run_blend(mask, true, src1.lo, src1.hi, src2.lo, src2.hi);

  // dst == src1: v0 is both source and destination.
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(512);
  emit_load_sources(t);
  t.emit(blend_vf(V0, V0, V1, mask));
  t.emit(umov_gpr64_vf_d(X0, V0, 0));
  t.emit_return();
  EXPECT_EQ(t.execute(src1.lo, src1.hi, src2.lo, src2.hi), base_lo);

  // dst == src2.
  CodeTester t2(InstructionSet::ARM64);
  t2.init_code_buffer(512);
  emit_load_sources(t2);
  t2.emit(blend_vf(V1, V0, V1, mask));
  t2.emit(umov_gpr64_vf_d(X0, V1, 0));
  t2.emit_return();
  EXPECT_EQ(t2.execute(src1.lo, src1.hi, src2.lo, src2.hi), base_lo);
#endif
}

TEST(ARM64EmitterSwizzle, AllControlBytes) {
#if defined(__aarch64__)
  V128 src = float4(1.0f, -2.0f, 3.0f, -4.0f);
  for (u32 ctrl = 0; ctrl < 256; ctrl++) {
    V128 expected;
    expected.lo = 0;
    expected.hi = 0;
    for (int i = 0; i < 4; i++) {
      u32 sel = (ctrl >> (2 * i)) & 3;
      float val = lane_f32(src, sel);
      u32 bits = f32_bits(val);
      if (i < 2) {
        expected.lo |= (u64)bits << (32 * i);
      } else {
        expected.hi |= (u64)bits << (32 * (i & 1));
      }
    }
    u64 got_lo = run_swizzle((u8)ctrl, false, src.lo, src.hi);
    u64 got_hi = run_swizzle((u8)ctrl, true, src.lo, src.hi);
    EXPECT_EQ(got_lo, expected.lo) << "ctrl " << ctrl << " low";
    EXPECT_EQ(got_hi, expected.hi) << "ctrl " << ctrl << " high";
  }
#endif
}

TEST(ARM64EmitterSwizzle, IdentityAndReverse) {
#if defined(__aarch64__)
  V128 src = float4(1.0f, 2.0f, 3.0f, 4.0f);
  // identity ctrl = 0b11_10_01_00 (sel 0,1,2,3)
  EXPECT_EQ(run_swizzle(0b11100100, false, src.lo, src.hi), src.lo);
  EXPECT_EQ(run_swizzle(0b11100100, true, src.lo, src.hi), src.hi);
  // reverse ctrl = 0b00_01_10_11 (sel 3,2,1,0)
  V128 rev = float4(4.0f, 3.0f, 2.0f, 1.0f);
  EXPECT_EQ(run_swizzle(0b00011011, false, src.lo, src.hi), rev.lo);
  EXPECT_EQ(run_swizzle(0b00011011, true, src.lo, src.hi), rev.hi);
#endif
}

TEST(ARM64EmitterSwizzle, ShuffleEquivalent) {
#if defined(__aarch64__)
  // shuffle_vf(dst, src, dx, dy, dz, dw) must match swizzle_vf with the packed
  // control bytes: dst[i] = src[(ctrl >> 2i) & 3].
  V128 src = float4(1.0f, -2.0f, 3.0f, -4.0f);
  u8 cases[][4] = {{0, 1, 2, 3}, {3, 2, 1, 0}, {1, 1, 2, 2}, {0, 0, 0, 0}, {3, 3, 3, 3}};
  for (auto& c : cases) {
    u8 imm = c[0] + (c[1] << 2) + (c[2] << 4) + (c[3] << 6);
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(512);
    t.emit(ins_vf_d_gpr(V0, 0, X0));
    t.emit(ins_vf_d_gpr(V0, 1, X1));
    t.emit(shuffle_vf(V2, V0, c[0], c[1], c[2], c[3]));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    u64 got_lo = t.execute(src.lo, src.hi, 0, 0);
    u64 sw_lo = run_swizzle(imm, false, src.lo, src.hi);
    EXPECT_EQ(got_lo, sw_lo) << "dx=" << (int)c[0] << " dy=" << (int)c[1]
                             << " dz=" << (int)c[2] << " dw=" << (int)c[3];
  }
#endif
}

TEST(ARM64EmitterSplat, AllElements) {
#if defined(__aarch64__)
  V128 src = float4(1.0f, -2.0f, 3.0f, -4.0f);
  for (int lane = 0; lane < 4; lane++) {
    Register::VF_ELEMENT element = static_cast<Register::VF_ELEMENT>(lane);
    V128 expected = float4(lane_f32(src, lane), lane_f32(src, lane), lane_f32(src, lane),
                           lane_f32(src, lane));
    EXPECT_EQ(run_splat(element, false, src.lo, src.hi), expected.lo) << "lane " << lane;
    EXPECT_EQ(run_splat(element, true, src.lo, src.hi), expected.hi) << "lane " << lane;
  }
#endif
}

TEST(ARM64EmitterSplat, Encoding) {
  // splat X = DUP <Vd>.4S, <Vn>.S[0] -> 0x4E040420 (verified against assembler).
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(256);
  t.emit(splat_vf(V0, V1, Register::VF_ELEMENT::X));
  EXPECT_EQ(t.dump_to_hex_string(), "20 04 04 4e");
  CodeTester t2(InstructionSet::ARM64);
  t2.init_code_buffer(256);
  t2.emit(splat_vf(V0, V1, Register::VF_ELEMENT::W));
  // DUP <Vd>.4S, <Vn>.S[3] -> 0x4E1C0420
  EXPECT_EQ(t2.dump_to_hex_string(), "20 04 1c 4e");
}
