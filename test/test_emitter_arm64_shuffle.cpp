#include "emitter_util.h"

#include "goalc/emitter/CodeTester.h"
#include "goalc/emitter/IGen.h"
#include "goalc/emitter/IGenARM64.h"
#include "gtest/gtest.h"

using namespace emitter;
using namespace emitter::IGen;
using namespace emitter::IGen::ARM64;

// ARM64 vpshuflw / vpshufhw (word shuffles).  These execute real NEON code on
// the host and compare the full 128-bit result against an independent C++
// oracle.
//
// vpshuflw(dst, src, imm): dst words 0-3 = src words (imm >> 2i) & 3,
//                          words 4-7 copied unchanged.
// vpshufhw(dst, src, imm): words 4-7 = src words 4 + ((imm >> 2i) & 3),
//                          words 0-3 copied unchanged.

namespace {

struct V128 {
  u64 lo, hi;
};

V128 word8_to_v128(u16 words[8]) {
  V128 v;
  v.lo = 0;
  v.hi = 0;
  for (int i = 0; i < 8; i++) {
    u64 half = (i < 4) ? v.lo : v.hi;
    half |= (u64)words[i] << (16 * (i & 3));
    if (i < 4) {
      v.lo = half;
    } else {
      v.hi = half;
    }
  }
  return v;
}

// Independent oracle, not using the emitter at all.
V128 oracle_shuflw(const u16 src[8], u8 imm) {
  u16 out[8];
  for (int i = 0; i < 4; i++) {
    out[i] = src[(imm >> (2 * i)) & 3];
  }
  for (int i = 4; i < 8; i++) {
    out[i] = src[i];
  }
  return word8_to_v128(out);
}

V128 oracle_shufhw(const u16 src[8], u8 imm) {
  u16 out[8];
  for (int i = 0; i < 4; i++) {
    out[i] = src[i];
  }
  for (int i = 4; i < 8; i++) {
    out[i] = src[4 + ((imm >> (2 * (i - 4))) & 3)];
  }
  return word8_to_v128(out);
}

// Returns lane pair lo (words 0-3) or hi (words 4-7) of the result.
u64 run_shuflw(u8 imm, bool hi, u64 a0, u64 a1) {
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(512);
  t.emit(ins_vf_d_gpr(V0, 0, X0));
  t.emit(ins_vf_d_gpr(V0, 1, X1));
  t.emit(vpshuflw(V2, V0, imm));
  t.emit(umov_gpr64_vf_d(X0, V2, hi ? 1 : 0));
  t.emit_return();
#ifdef __aarch64__
  return t.execute(a0, a1, 0, 0);
#else
  return 0;
#endif
}

u64 run_shufhw(u8 imm, bool hi, u64 a0, u64 a1) {
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(512);
  t.emit(ins_vf_d_gpr(V0, 0, X0));
  t.emit(ins_vf_d_gpr(V0, 1, X1));
  t.emit(vpshufhw(V2, V0, imm));
  t.emit(umov_gpr64_vf_d(X0, V2, hi ? 1 : 0));
  t.emit_return();
#ifdef __aarch64__
  return t.execute(a0, a1, 0, 0);
#else
  return 0;
#endif
}

}  // namespace

TEST(ARM64EmitterShuffle, VpshuflwAllImmediates) {
#if defined(__aarch64__)
  // unique 16-bit values so every word shuffle is unambiguous.
  u16 src[8] = {0x0001, 0x0002, 0x0003, 0x0004, 0x5555, 0x6666, 0x7777, 0x8888};
  for (u32 imm = 0; imm < 256; imm++) {
    V128 src128 = word8_to_v128(src);
    V128 expected = oracle_shuflw(src, (u8)imm);
    u64 got_lo = run_shuflw((u8)imm, false, src128.lo, src128.hi);
    u64 got_hi = run_shuflw((u8)imm, true, src128.lo, src128.hi);
    EXPECT_EQ(got_lo, expected.lo) << "imm " << imm << " low";
    EXPECT_EQ(got_hi, expected.hi) << "imm " << imm << " high";
  }
#endif
}

TEST(ARM64EmitterShuffle, VpshufhwAllImmediates) {
#if defined(__aarch64__)
  u16 src[8] = {0x1111, 0x2222, 0x3333, 0x4444, 0x0001, 0x0002, 0x0003, 0x0004};
  for (u32 imm = 0; imm < 256; imm++) {
    V128 src128 = word8_to_v128(src);
    V128 expected = oracle_shufhw(src, (u8)imm);
    u64 got_lo = run_shufhw((u8)imm, false, src128.lo, src128.hi);
    u64 got_hi = run_shufhw((u8)imm, true, src128.lo, src128.hi);
    EXPECT_EQ(got_lo, expected.lo) << "imm " << imm << " low";
    EXPECT_EQ(got_hi, expected.hi) << "imm " << imm << " high";
  }
#endif
}

TEST(ARM64EmitterShuffle, VpshuflwUnchangedHalf) {
#if defined(__aarch64__)
  // the high half must be byte-identical regardless of imm.
  u16 src[8] = {0x0001, 0x0002, 0x0003, 0x0004, 0xA55A, 0xB66B, 0xC77C, 0xD88D};
  V128 src128 = word8_to_v128(src);
  for (u32 imm : {0u, 0x1Bu, 0xE4u, 0xFFu}) {
    u64 hi = run_shuflw((u8)imm, true, src128.lo, src128.hi);
    u64 expected_hi = 0xD88DC77CB66BA55Aull;
    EXPECT_EQ(hi, expected_hi) << "imm " << imm;
  }
#endif
}

TEST(ARM64EmitterShuffle, VpshufhwUnchangedHalf) {
#if defined(__aarch64__)
  u16 src[8] = {0xA55A, 0xB66B, 0xC77C, 0xD88D, 0x0001, 0x0002, 0x0003, 0x0004};
  V128 src128 = word8_to_v128(src);
  for (u32 imm : {0u, 0x1Bu, 0xE4u, 0xFFu}) {
    u64 lo = run_shufhw((u8)imm, false, src128.lo, src128.hi);
    u64 expected_lo = 0xD88DC77CB66BA55Aull;
    EXPECT_EQ(lo, expected_lo) << "imm " << imm;
  }
#endif
}

TEST(ARM64EmitterShuffle, AliasedRegisters) {
#if defined(__aarch64__)
  // dst == src must give the same result as the base case.
  u16 src[8] = {0x0001, 0x0002, 0x0003, 0x0004, 0x5555, 0x6666, 0x7777, 0x8888};
  u8 imm = 0xE4;  // reverse of the low four words
  V128 src128 = word8_to_v128(src);
  u64 base_lo = run_shuflw(imm, false, src128.lo, src128.hi);

  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(512);
  t.emit(ins_vf_d_gpr(V0, 0, X0));
  t.emit(ins_vf_d_gpr(V0, 1, X1));
  t.emit(vpshuflw(V0, V0, imm));
  t.emit(umov_gpr64_vf_d(X0, V0, 0));
  t.emit_return();
  EXPECT_EQ(t.execute(src128.lo, src128.hi, 0, 0), base_lo);
#endif
}
