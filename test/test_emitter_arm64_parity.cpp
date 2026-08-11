#include "emitter_util.h"

#include "goalc/emitter/CodeTester.h"
#include "goalc/emitter/IGen.h"
#include "goalc/emitter/IGenARM64.h"
#include "gtest/gtest.h"

using namespace emitter;
using namespace emitter::IGen;
using namespace emitter::IGen::ARM64;
using namespace emitter::ARM64;

// ARM-008: parity coverage for the ARM64 emitter.  Every public emitter method
// gets at least one observable test here (executable on ARM64 hosts, or an
// encoding check that runs on any host).  Executable tests compare against C++
// models.  All tests are gated so nothing executes on non-ARM64 hosts.

namespace {

// Returns u64 result of executing the emitted buffer as fn(in0, in1, in2, in3).
u64 run_fn(CodeTester& t, u64 in0, u64 in1, u64 in2, u64 in3) {
#ifdef __aarch64__
  return t.execute(in0, in1, in2, in3);
#else
  return 0;
#endif
}

// --- helpers for building a buffer that loads u64 args into v0 (d0=x0, d1=x1)
// and v1 (d0=x2, d1=x3) ---
void load_src_vecs(CodeTester& t) {
  t.emit(ins_vf_d_gpr(V0, 0, X0));
  t.emit(ins_vf_d_gpr(V0, 1, X1));
  t.emit(ins_vf_d_gpr(V1, 0, X2));
  t.emit(ins_vf_d_gpr(V1, 1, X3));
}

// 128-bit float vector built from 4 u32 bit patterns.
struct F4 {
  u64 lo, hi;
};

F4 v4_from_words(u32 a, u32 b, u32 c, u32 d) {
  return F4{(u64)a | ((u64)b << 32), (u64)c | ((u64)d << 32)};
}

}  // namespace

// ------------------------- GPR integer math -------------------------

TEST(ARM64EmitterParity, IntegerMath64) {
#if defined(__aarch64__)
  s64 vals[] = {0, 1, -1, 0x123456789ABCDEFull, INT64_MIN, INT64_MAX};
  struct Op {
    std::function<void(CodeTester&, Register, Register)> emit;
  };
  std::vector<std::pair<std::string, std::function<s64(s64, s64)>>> ops = {
      {"add", [](s64 a, s64 b) { return a + b; }},
      {"sub", [](s64 a, s64 b) { return a - b; }},
      {"and", [](s64 a, s64 b) { return a & b; }},
      {"or", [](s64 a, s64 b) { return a | b; }},
      {"xor", [](s64 a, s64 b) { return a ^ b; }},
  };
  for (auto& [name, model] : ops) {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    if (name == "add") {
      t.emit(IGen::add_gpr64_gpr64(t.generator(), X0, X1));
    } else if (name == "sub") {
      t.emit(IGen::sub_gpr64_gpr64(t.generator(), X0, X1));
    } else if (name == "and") {
      t.emit(IGen::and_gpr64_gpr64(t.generator(), X0, X1));
    } else if (name == "or") {
      t.emit(IGen::or_gpr64_gpr64(t.generator(), X0, X1));
    } else {
      t.emit(IGen::xor_gpr64_gpr64(t.generator(), X0, X1));
    }
    t.emit_return();
    for (auto a : vals) {
      for (auto b : vals) {
        EXPECT_EQ(run_fn(t, (u64)a, (u64)b, 0, 0), (u64)model(a, b)) << name;
      }
    }
  }
  // not
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(256);
  t.emit(IGen::not_gpr64(t.generator(), X0));
  t.emit_return();
  for (auto a : vals) {
    EXPECT_EQ(run_fn(t, (u64)a, 0, 0, 0), (u64)~a);
  }
  // imul 64 and 32
  CodeTester t2(InstructionSet::ARM64);
  t2.init_code_buffer(256);
  t2.emit(IGen::imul_gpr64_gpr64(t2.generator(), X0, X1));
  t2.emit_return();
  for (auto a : vals) {
    for (auto b : vals) {
      EXPECT_EQ(run_fn(t2, (u64)a, (u64)b, 0, 0), (u64)(a * b)) << "imul64";
    }
  }
  CodeTester t3(InstructionSet::ARM64);
  t3.init_code_buffer(256);
  t3.emit(IGen::imul_gpr32_gpr32(t3.generator(), X0, X1));
  t3.emit(IGen::movsx_r64_r32(t3.generator(), X0, X0));
  t3.emit_return();
  for (auto a : vals) {
    for (auto b : vals) {
      s32 ea = (s32)(a & 0xFFFFFFFF), eb = (s32)(b & 0xFFFFFFFF);
      EXPECT_EQ(run_fn(t3, (u64)a, (u64)b, 0, 0), (u64)(s64)(s32)(ea * eb)) << "imul32";
    }
  }
#endif
}

TEST(ARM64EmitterParity, IntegerMathImm) {
#if defined(__aarch64__)
  s64 vals[] = {0, 1, -1, 0x123456789ABCDEFull, INT64_MIN, INT64_MAX};
  s64 imms[] = {0, 1, -1, 8, -8, 127, -128, 20000, -20000, 0x12345678, -0x12345678};
  for (auto imm : imms) {
    for (int kind = 0; kind < 6; kind++) {
      CodeTester t(InstructionSet::ARM64);
      t.init_code_buffer(256);
      switch (kind) {
        case 0:
          t.emit(IGen::add_gpr64_imm8s(t.generator(), X0, imm));
          break;
        case 1:
          t.emit(IGen::sub_gpr64_imm8s(t.generator(), X0, imm));
          break;
        case 2:
          t.emit(IGen::add_gpr64_imm32s(t.generator(), X0, imm));
          break;
        case 3:
          t.emit(IGen::sub_gpr64_imm32s(t.generator(), X0, imm));
          break;
        case 4:
          t.emit(IGen::add_gpr64_imm(t.generator(), X0, imm));
          break;
        case 5:
          t.emit(IGen::sub_gpr64_imm(t.generator(), X0, imm));
          break;
      }
      t.emit_return();
      for (auto a : vals) {
        u64 expected;
        if (kind % 2 == 0) {
          expected = (u64)(a + imm);
        } else {
          expected = (u64)(a - imm);
        }
        EXPECT_EQ(run_fn(t, (u64)a, 0, 0, 0), expected) << "kind " << kind << " imm " << imm;
      }
    }
  }
#endif
}

TEST(ARM64EmitterParity, MovImmediates) {
#if defined(__aarch64__)
  u64 vals[] = {0, 1, 0xFFFF, 0xFFFFFFFF, 0x123456789ABCDEFull, 0xFFFFFFFFFFFFFFFFull,
                0x8000000000000000ull};
  for (auto v : vals) {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    t.emit(IGen::mov_gpr64_u64(t.generator(), X0, v));
    t.emit_return();
    EXPECT_EQ(run_fn(t, 0, 0, 0, 0), v) << "u64 " << std::hex << v;
    CodeTester t2(InstructionSet::ARM64);
    t2.init_code_buffer(256);
    t2.emit(IGen::mov_gpr64_u32(t2.generator(), X0, v));
    t2.emit_return();
    EXPECT_EQ(run_fn(t2, 0, 0, 0, 0), v & 0xFFFFFFFF) << "u32 " << std::hex << v;
    CodeTester t3(InstructionSet::ARM64);
    t3.init_code_buffer(256);
    t3.emit(IGen::mov_gpr64_s32(t3.generator(), X0, (s64)v));
    t3.emit_return();
    s64 sv = (s64)(s32)(u32)(v & 0xFFFFFFFF);
    EXPECT_EQ(run_fn(t3, 0, 0, 0, 0), (u64)sv) << "s32 " << std::hex << v;
  }
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(256);
  t.emit(IGen::mov_gpr64_gpr64(t.generator(), X0, X1));
  t.emit_return();
  EXPECT_EQ(run_fn(t, 0, 0xDEADBEEFCAFEBABEull, 0, 0), 0xDEADBEEFCAFEBABEull);
#endif
}

TEST(ARM64EmitterParity, Shifts) {
#if defined(__aarch64__)
  s64 vals[] = {0, 1, -1, 0x123456789ABCDEFull, INT64_MIN, INT64_MAX};
  for (int kind = 0; kind < 6; kind++) {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    switch (kind) {
      case 0:
        t.emit(IGen::shl_gpr64_u8(t.generator(), X0, 3));
        break;
      case 1:
        t.emit(IGen::shr_gpr64_u8(t.generator(), X0, 3));
        break;
      case 2:
        t.emit(IGen::sar_gpr64_u8(t.generator(), X0, 3));
        break;
      case 3:
        t.emit(IGen::shl_gpr64_reg(t.generator(), X0, X1));
        break;
      case 4:
        t.emit(IGen::shr_gpr64_reg(t.generator(), X0, X1));
        break;
      case 5:
        t.emit(IGen::sar_gpr64_reg(t.generator(), X0, X1));
        break;
    }
    t.emit_return();
    for (auto a : vals) {
      u64 expected;
      u64 shift = (kind < 3) ? 3 : 1;
      if (kind == 0 || kind == 3) {
        expected = (u64)a << shift;
      } else if (kind == 1 || kind == 4) {
        expected = (u64)a >> shift;
      } else {
        expected = (u64)(a >> (s64)shift);
      }
      EXPECT_EQ(run_fn(t, (u64)a, shift, 0, 0), expected) << "kind " << kind;
    }
  }
#endif
}

TEST(ARM64EmitterParity, Lea) {
#if defined(__aarch64__)
  s64 base = 0x1000;
  s64 offs[] = {0, 1, -1, 127, -128, 30000, -30000, 0x12345678};
  for (auto off : offs) {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    t.emit(IGen::mov_gpr64_u64(t.generator(), X0, base));
    t.emit(IGen::lea_reg_plus_off(t.generator(), X0, X0, off));
    t.emit_return();
    EXPECT_EQ(run_fn(t, 0, 0, 0, 0), (u64)(base + off)) << "off " << off;
  }
  CodeTester t2(InstructionSet::ARM64);
  t2.init_code_buffer(256);
  t2.emit(IGen::mov_gpr64_u64(t2.generator(), X0, base));
  t2.emit(IGen::lea_reg_plus_off8(t2.generator(), X0, X0, 100));
  t2.emit_return();
  EXPECT_EQ(run_fn(t2, 0, 0, 0, 0), (u64)(base + 100));
  CodeTester t3(InstructionSet::ARM64);
  t3.init_code_buffer(256);
  t3.emit(IGen::mov_gpr64_u64(t3.generator(), X0, base));
  t3.emit(IGen::lea_reg_plus_off32(t3.generator(), X0, X0, 70000));
  t3.emit_return();
  EXPECT_EQ(run_fn(t3, 0, 0, 0, 0), (u64)(base + 70000));
#endif
}

TEST(ARM64EmitterParity, CompareGpr64) {
#if defined(__aarch64__)
  // cmp x0, x1 ; b.eq +3 words ; mov x0,#0 ; ret ; mov x0,#1 ; ret
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(256);
  t.emit(IGen::cmp_gpr64_gpr64(t.generator(), X0, X1));
  t.emit(InstructionARM64(Base(0b01010100, 8), Imm19(3), Cond(0b0000)));
  t.emit(IGen::mov_gpr64_u64(t.generator(), X0, 0));
  t.emit_return();
  t.emit(IGen::mov_gpr64_u64(t.generator(), X0, 1));
  t.emit_return();
  EXPECT_EQ(run_fn(t, 5, 5, 0, 0), 1);
  EXPECT_EQ(run_fn(t, 5, 6, 0, 0), 0);
  EXPECT_EQ(run_fn(t, -5, -5, 0, 0), 1);
#endif
}

// ------------------------- Memory -------------------------

TEST(ARM64EmitterParity, MemoryRoundTrip) {
#if defined(__aarch64__)
  // x0 = value, x1 = offset, x2 = pointer.  Store with one addressing mode and
  // load with the matching mode; compare the loaded value against a model.
  struct Case {
    std::string name;
    int bytes;
    bool sign_extend;
    void (*store)(CodeTester&, Register, Register, Register);  // (addr1, addr2, value)
    void (*load)(CodeTester&, Register, Register, Register);   // (dst, addr1, addr2)
  };
  std::vector<Case> cases = {
      {"8+gpr", 1, true, [](CodeTester& t, Register a, Register b, Register v) { t.emit(IGen::store8_gpr64_gpr64_plus_gpr64(t.generator(), a, b, v)); }, [](CodeTester& t, Register d, Register a, Register b) { t.emit(IGen::load8s_gpr64_gpr64_plus_gpr64(t.generator(), d, a, b)); }},
      {"8u+gpr", 1, false, [](CodeTester& t, Register a, Register b, Register v) { t.emit(IGen::store8_gpr64_gpr64_plus_gpr64(t.generator(), a, b, v)); }, [](CodeTester& t, Register d, Register a, Register b) { t.emit(IGen::load8u_gpr64_gpr64_plus_gpr64(t.generator(), d, a, b)); }},
      {"16+gpr", 2, true, [](CodeTester& t, Register a, Register b, Register v) { t.emit(IGen::store16_gpr64_gpr64_plus_gpr64(t.generator(), a, b, v)); }, [](CodeTester& t, Register d, Register a, Register b) { t.emit(IGen::load16s_gpr64_gpr64_plus_gpr64(t.generator(), d, a, b)); }},
      {"16u+gpr", 2, false, [](CodeTester& t, Register a, Register b, Register v) { t.emit(IGen::store16_gpr64_gpr64_plus_gpr64(t.generator(), a, b, v)); }, [](CodeTester& t, Register d, Register a, Register b) { t.emit(IGen::load16u_gpr64_gpr64_plus_gpr64(t.generator(), d, a, b)); }},
      {"32+gpr", 4, true, [](CodeTester& t, Register a, Register b, Register v) { t.emit(IGen::store32_gpr64_gpr64_plus_gpr64(t.generator(), a, b, v)); }, [](CodeTester& t, Register d, Register a, Register b) { t.emit(IGen::load32s_gpr64_gpr64_plus_gpr64(t.generator(), d, a, b)); }},
      {"32u+gpr", 4, false, [](CodeTester& t, Register a, Register b, Register v) { t.emit(IGen::store32_gpr64_gpr64_plus_gpr64(t.generator(), a, b, v)); }, [](CodeTester& t, Register d, Register a, Register b) { t.emit(IGen::load32u_gpr64_gpr64_plus_gpr64(t.generator(), d, a, b)); }},
      {"64+gpr", 8, false, [](CodeTester& t, Register a, Register b, Register v) { t.emit(IGen::store64_gpr64_gpr64_plus_gpr64(t.generator(), a, b, v)); }, [](CodeTester& t, Register d, Register a, Register b) { t.emit(IGen::load64_gpr64_gpr64_plus_gpr64(t.generator(), d, a, b)); }},
      {"8+s8", 1, true, [](CodeTester& t, Register a, Register b, Register v) { t.emit(IGen::store8_gpr64_gpr64_plus_gpr64_plus_s8(t.generator(), a, b, v, 4)); }, [](CodeTester& t, Register d, Register a, Register b) { t.emit(IGen::load8s_gpr64_gpr64_plus_gpr64_plus_s8(t.generator(), d, a, b, 4)); }},
      {"16+s8", 2, true, [](CodeTester& t, Register a, Register b, Register v) { t.emit(IGen::store16_gpr64_gpr64_plus_gpr64_plus_s8(t.generator(), a, b, v, 4)); }, [](CodeTester& t, Register d, Register a, Register b) { t.emit(IGen::load16s_gpr64_gpr64_plus_gpr64_plus_s8(t.generator(), d, a, b, 4)); }},
      {"32+s8", 4, true, [](CodeTester& t, Register a, Register b, Register v) { t.emit(IGen::store32_gpr64_gpr64_plus_gpr64_plus_s8(t.generator(), a, b, v, 4)); }, [](CodeTester& t, Register d, Register a, Register b) { t.emit(IGen::load32s_gpr64_gpr64_plus_gpr64_plus_s8(t.generator(), d, a, b, 4)); }},
      {"64+s8", 8, false, [](CodeTester& t, Register a, Register b, Register v) { t.emit(IGen::store64_gpr64_gpr64_plus_gpr64_plus_s8(t.generator(), a, b, v, 4)); }, [](CodeTester& t, Register d, Register a, Register b) { t.emit(IGen::load64_gpr64_gpr64_plus_gpr64_plus_s8(t.generator(), d, a, b, 4)); }},
      {"8+s32", 1, true, [](CodeTester& t, Register a, Register b, Register v) { t.emit(IGen::store8_gpr64_gpr64_plus_gpr64_plus_s32(t.generator(), a, b, v, 4)); }, [](CodeTester& t, Register d, Register a, Register b) { t.emit(IGen::load8s_gpr64_gpr64_plus_gpr64_plus_s32(t.generator(), d, a, b, 4)); }},
      {"16+s32", 2, true, [](CodeTester& t, Register a, Register b, Register v) { t.emit(IGen::store16_gpr64_gpr64_plus_gpr64_plus_s32(t.generator(), a, b, v, 4)); }, [](CodeTester& t, Register d, Register a, Register b) { t.emit(IGen::load16s_gpr64_gpr64_plus_gpr64_plus_s32(t.generator(), d, a, b, 4)); }},
      {"32+s32", 4, true, [](CodeTester& t, Register a, Register b, Register v) { t.emit(IGen::store32_gpr64_gpr64_plus_gpr64_plus_s32(t.generator(), a, b, v, 4)); }, [](CodeTester& t, Register d, Register a, Register b) { t.emit(IGen::load32s_gpr64_gpr64_plus_gpr64_plus_s32(t.generator(), d, a, b, 4)); }},
      {"64+s32", 8, false, [](CodeTester& t, Register a, Register b, Register v) { t.emit(IGen::store64_gpr64_gpr64_plus_gpr64_plus_s32(t.generator(), a, b, v, 4)); }, [](CodeTester& t, Register d, Register a, Register b) { t.emit(IGen::load64_gpr64_gpr64_plus_gpr64_plus_s32(t.generator(), d, a, b, 4)); }},
      {"32u+s8", 4, false, [](CodeTester& t, Register a, Register b, Register v) { t.emit(IGen::store32_gpr64_gpr64_plus_gpr64_plus_s8(t.generator(), a, b, v, 4)); }, [](CodeTester& t, Register d, Register a, Register b) { t.emit(IGen::load32u_gpr64_gpr64_plus_gpr64_plus_s8(t.generator(), d, a, b, 4)); }},
      {"16u+s32", 2, false, [](CodeTester& t, Register a, Register b, Register v) { t.emit(IGen::store16_gpr64_gpr64_plus_gpr64_plus_s32(t.generator(), a, b, v, 4)); }, [](CodeTester& t, Register d, Register a, Register b) { t.emit(IGen::load16u_gpr64_gpr64_plus_gpr64_plus_s32(t.generator(), d, a, b, 4)); }},
      {"8u+s32", 1, false, [](CodeTester& t, Register a, Register b, Register v) { t.emit(IGen::store8_gpr64_gpr64_plus_gpr64_plus_s32(t.generator(), a, b, v, 4)); }, [](CodeTester& t, Register d, Register a, Register b) { t.emit(IGen::load8u_gpr64_gpr64_plus_gpr64_plus_s32(t.generator(), d, a, b, 4)); }},
      {"64+s32", 8, false, [](CodeTester& t, Register a, Register b, Register v) { t.emit(IGen::store64_gpr64_plus_s32(t.generator(), a, 4, v)); }, [](CodeTester& t, Register d, Register a, Register b) { t.emit(IGen::load64_gpr64_plus_s32(t.generator(), d, 4, a)); }},
  };
  s64 vals[] = {0, 1, -1, 0x12345678, -0x12345678, 0x7FFFFFFF, 0x80000000, -0x80000001};
  for (auto& c : cases) {
    for (auto v : vals) {
      u64 bits = (u64)v;
      u64 mask = (c.bytes == 8) ? ~0ull : ((1ull << (8 * c.bytes)) - 1);
      u64 low = bits & mask;
      u64 expected;
      if (c.sign_extend && (low & (1ull << (8 * c.bytes - 1)))) {
        expected = low | ~mask;
      } else {
        expected = low;
      }
      CodeTester t(InstructionSet::ARM64);
      t.init_code_buffer(256);
      c.store(t, X2, X1, X0);
      c.load(t, X0, X2, X1);
      t.emit_return();
      u64 buf = 0;
      // the +s8/+s32 cases add a +4 offset, so point 4 bytes before buf.
      bool has_offset = c.name.find("+s8") != std::string::npos ||
                        c.name.find("+s32") != std::string::npos;
      u64 ptr = has_offset ? (u64)&buf - 4 : (u64)&buf;
      u64 result = run_fn(t, bits, 0, ptr, 0);
      EXPECT_EQ(result, expected) << c.name << " val " << std::hex << v;
      EXPECT_EQ(buf & mask, low) << c.name << " memory";
    }
  }
#endif
}

// ------------------------- FP scalar -------------------------

TEST(ARM64EmitterParity, FloatMath) {
#if defined(__aarch64__)
  // x0/x1 hold f32 bit patterns in the low 32 bits; the op runs on s0/s1.
  struct Case {
    std::string name;
    void (*emit)(CodeTester&);
    std::function<float(float, float)> model;
  };
  std::vector<Case> cases = {
      {"add", [](CodeTester& t) { t.emit(IGen::add_f32_f32(t.generator(), V0, V1)); },
       [](float a, float b) { return a + b; }},
      {"sub", [](CodeTester& t) { t.emit(IGen::sub_f32_f32(t.generator(), V0, V1)); },
       [](float a, float b) { return a - b; }},
      {"mul", [](CodeTester& t) { t.emit(IGen::mul_f32_f32(t.generator(), V0, V1)); },
       [](float a, float b) { return a * b; }},
      {"div", [](CodeTester& t) { t.emit(IGen::div_f32_f32(t.generator(), V0, V1)); },
       [](float a, float b) { return a / b; }},
      {"min", [](CodeTester& t) { t.emit(IGen::min_f32_f32(t.generator(), V0, V1)); },
       [](float a, float b) { return std::min(a, b); }},
      {"max", [](CodeTester& t) { t.emit(IGen::max_f32_f32(t.generator(), V0, V1)); },
       [](float a, float b) { return std::max(a, b); }},
  };
  float as[] = {1.5f, -2.25f, 0.0f, 100.0f, 0.001f};
  float bs[] = {3.0f, -4.5f, 0.5f, -0.001f, 25.0f};
  for (auto& c : cases) {
    for (auto a : as) {
      for (auto b : bs) {
        CodeTester t(InstructionSet::ARM64);
        t.init_code_buffer(256);
        t.emit(ins_vf_d_gpr(V0, 0, X0));
        t.emit(ins_vf_d_gpr(V1, 0, X1));
        c.emit(t);
        t.emit(umov_gpr64_vf_d(X0, V0, 0));
        t.emit_return();
        u32 in0 = 0;
        u32 in1 = 0;
        memcpy(&in0, &a, 4);
        memcpy(&in1, &b, 4);
        u64 result = run_fn(t, in0, in1, 0, 0);
        u32 out = (u32)result;
        float fout;
        memcpy(&fout, &out, 4);
        float expected = c.model(a, b);
        EXPECT_FLOAT_EQ(fout, expected) << c.name;
      }
    }
  }
  // sqrt
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    t.emit(ins_vf_d_gpr(V0, 0, X0));
    t.emit(sqrt_f32(V1, V0));
    t.emit(umov_gpr64_vf_d(X0, V1, 0));
    t.emit_return();
    for (auto a : as) {
      if (a < 0) {
        continue;  // sqrt of a negative has no defined real result
      }
      u32 in0 = 0;
      memcpy(&in0, &a, 4);
      u64 result = run_fn(t, in0, 0, 0, 0);
      u32 out = (u32)result;
      float fout;
      memcpy(&fout, &out, 4);
      EXPECT_FLOAT_EQ(fout, std::sqrt(a)) << a;
    }
  }
  // mov_f32_f32
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    t.emit(ins_vf_d_gpr(V0, 0, X0));
    t.emit(mov_f32_f32(V1, V0));
    t.emit(umov_gpr64_vf_d(X0, V1, 0));
    t.emit_return();
    for (auto a : as) {
      u32 in0 = 0;
      memcpy(&in0, &a, 4);
      EXPECT_EQ((u32)run_fn(t, in0, 0, 0, 0), in0);
    }
  }
#endif
}

TEST(ARM64EmitterParity, FloatCompare) {
#if defined(__aarch64__)
  // fcmp s0, s1 ; b.eq +3 ; mov x0,#0 ; ret ; mov x0,#1 ; ret
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(256);
  t.emit(ins_vf_d_gpr(V0, 0, X0));
  t.emit(ins_vf_d_gpr(V1, 0, X1));
  t.emit(cmp_f32_f32(V0, V1));
  t.emit(InstructionARM64(Base(0b01010100, 8), Imm19(3), Cond(0b0000)));
  t.emit(IGen::mov_gpr64_u64(t.generator(), X0, 0));
  t.emit_return();
  t.emit(IGen::mov_gpr64_u64(t.generator(), X0, 1));
  t.emit_return();
  u32 f = 0, g = 0;
  float a = 2.5f, b = 2.5f;
  memcpy(&f, &a, 4);
  memcpy(&g, &b, 4);
  EXPECT_EQ(run_fn(t, f, g, 0, 0), 1);
  float c = 3.5f;
  memcpy(&f, &c, 4);
  EXPECT_EQ(run_fn(t, f, g, 0, 0), 0);
#endif
}

// ------------------------- Conversions -------------------------

TEST(ARM64EmitterParity, Conversions) {
#if defined(__aarch64__)
  // movd gpr <-> f32
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    t.emit(movd_f32_gpr32(V1, X0));  // s1 = w0
    t.emit(movd_gpr32_f32(X0, V1));  // w0 = s1
    t.emit_return();
    u32 bits = 0x3F800000;  // 1.0f
    EXPECT_EQ(run_fn(t, bits, 0, 0, 0), bits);
  }
  // movq gpr <-> f64
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    t.emit(movq_f64_gpr64(V1, X0));  // d1 = x0
    t.emit(movq_gpr64_f64(X0, V1));  // x0 = d1
    t.emit_return();
    u64 bits = 0x3FF0000000000000ull;  // 1.0
    EXPECT_EQ(run_fn(t, bits, 0, 0, 0), bits);
  }
  // int32_to_f32: s1 = (float)w0, then return via movd
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    t.emit(int32_to_f32(V1, X0));
    t.emit(movd_gpr32_f32(X0, V1));
    t.emit_return();
    for (s32 v : {0, 1, -1, 42, -42, INT32_MAX, INT32_MIN}) {
      u64 r = run_fn(t, (u64)(s64)v, 0, 0, 0);
      u32 out = (u32)r;
      float fout;
      memcpy(&fout, &out, 4);
      EXPECT_FLOAT_EQ(fout, (float)v) << v;
    }
  }
  // f32_to_int32: w0 = (int)s1 (with source in s1!)
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    t.emit(ins_vf_d_gpr(V1, 0, X0));  // s1 = low 32 of x0
    t.emit(f32_to_int32(X0, V1));     // w0 = (int)s1
    t.emit_return();
    for (float v : {0.0f, 1.5f, -1.5f, 42.9f, -42.9f}) {
      u32 bits = 0;
      memcpy(&bits, &v, 4);
      s32 expected = (s32)v;  // fcvtzs truncates toward zero
      EXPECT_EQ((s64)(s32)(u32)run_fn(t, bits, 0, 0, 0), (s64)expected) << v;
    }
  }
#endif
}

// ------------------------- Vector math -------------------------

TEST(ARM64EmitterParity, VectorMath) {
#if defined(__aarch64__)
  F4 a = v4_from_words(0x3F800000, 0xC0200000, 0x40700000, 0xC0840000);  // 1,-2.5,3.75,-4.125
  F4 b = v4_from_words(0x41200000, 0xC1A00000, 0x41F00000, 0xC2200000);  // 10,-20,30,-40
  struct Case {
    std::string name;
    void (*emit)(CodeTester&);
    std::function<float(float, float)> model;
  };
  std::vector<Case> cases = {
      {"add", [](CodeTester& t) { t.emit(add_vf(V2, V0, V4)); }, [](float x, float y) { return x + y; }},
      {"sub", [](CodeTester& t) { t.emit(sub_vf(V2, V0, V4)); }, [](float x, float y) { return x - y; }},
      {"mul", [](CodeTester& t) { t.emit(mul_vf(V2, V0, V4)); }, [](float x, float y) { return x * y; }},
      {"div", [](CodeTester& t) { t.emit(div_vf(V2, V0, V4)); }, [](float x, float y) { return x / y; }},
      {"min", [](CodeTester& t) { t.emit(min_vf(V2, V0, V4)); }, [](float x, float y) { return std::min(x, y); }},
      {"max", [](CodeTester& t) { t.emit(max_vf(V2, V0, V4)); }, [](float x, float y) { return std::max(x, y); }},
  };
  for (auto& c : cases) {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    // Exercise an even-numbered Rm. The opcode base must leave all five Rm
    // bits clear before the operand field is inserted.
    t.emit(mov_vf_vf(V4, V1));
    c.emit(t);
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    u64 lo = run_fn(t, a.lo, a.hi, b.lo, b.hi);
    for (int i = 0; i < 2; i++) {
      float fa, fb, expected;
      u32 ba = (u32)((i < 2 ? a.lo : a.hi) >> (32 * (i & 1)));
      u32 bb = (u32)((i < 2 ? b.lo : b.hi) >> (32 * (i & 1)));
      memcpy(&fa, &ba, 4);
      memcpy(&fb, &bb, 4);
      expected = c.model(fa, fb);
      u32 bo = (u32)((lo >> (32 * i)) & 0xFFFFFFFF);
      float fo;
      memcpy(&fo, &bo, 4);
      EXPECT_FLOAT_EQ(fo, expected) << c.name << " lane " << i;
    }
  }
  // xor_vf
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(xor_vf(V2, V0, V1));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    EXPECT_EQ(run_fn(t, a.lo, a.hi, b.lo, b.hi), a.lo ^ b.lo);
  }
  // mov_vf_vf
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(mov_vf_vf(V2, V0));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    EXPECT_EQ(run_fn(t, a.lo, a.hi, b.lo, b.hi), a.lo);
  }
#endif
}

// ------------------------- SIMD / parallel -------------------------

TEST(ARM64EmitterParity, VectorSimdOps) {
#if defined(__aarch64__)
  // vpsubd: 32-bit lane subtraction.
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(vpsubd(V2, V0, V1));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    u64 a = 0x0004000300020001ull, b = 0x0002000100020001ull;
    EXPECT_EQ(run_fn(t, a, 0, b, 0), 0x0002000200000000ull);
  }
  // parallel_add_byte
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(parallel_add_byte(V2, V0, V1));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    u64 a = 0x0102030405060708ull, b = 0x1010101010101010ull;
    EXPECT_EQ(run_fn(t, a, 0, b, 0), 0x1112131415161718ull);
  }
  // parallel bitwise ops
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(parallel_bitwise_and(V2, V0, V1));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    u64 a = 0x0F0F0F0F0F0F0F0Full, b = 0xFFFF0000FFFF0000ull;
    EXPECT_EQ(run_fn(t, a, 0, b, 0), a & b);
  }
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(parallel_bitwise_or(V2, V0, V1));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    u64 a = 0x0F0F0F0F0F0F0F0Full, b = 0xF0F0F0F0F0F0F0F0ull;
    EXPECT_EQ(run_fn(t, a, 0, b, 0), 0xFFFFFFFFFFFFFFFFull);
  }
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(parallel_bitwise_xor(V2, V0, V1));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    u64 a = 0xFFFFFFFFFFFFFFFFull, b = 0xAAAAAAAAAAAAAAAAull;
    EXPECT_EQ(run_fn(t, a, 0, b, 0), 0x5555555555555555ull);
  }
  // vpunpcklqdq / pcpyld_swapped: zip1 of 64-bit halves.
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(vpunpcklqdq(V2, V0, V1));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit(umov_gpr64_vf_d(X1, V2, 1));
    t.emit_return();
    u64 a = 0x1111222233334444ull, b = 0x5555666677778888ull;
    EXPECT_EQ(run_fn(t, a, 0xDEADBEEFCAFEBABEull, b, 0x0102030405060708ull), a);
  }
  // pcpyud: zip2.
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(pcpyud(V2, V0, V1));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    u64 a = 0x1111222233334444ull, b = 0x5555666677778888ull;
    // ZIP2: v2.d[0] = v0.d[1], v2.d[1] = v1.d[1]
    EXPECT_EQ(run_fn(t, a, 0xDEADBEEFCAFEBABEull, b, 0x0102030405060708ull),
              0xDEADBEEFCAFEBABEull);
  }
  // vpslldq / vpsrldq (16-byte shifts).
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(vpslldq(V2, V0, 8));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    EXPECT_EQ(run_fn(t, 0x1122334455667788ull, 0, 0, 0), 0);
    CodeTester t2(InstructionSet::ARM64);
    t2.init_code_buffer(256);
    load_src_vecs(t2);
    t2.emit(vpsrldq(V2, V0, 8));
    t2.emit(umov_gpr64_vf_d(X0, V2, 0));
    t2.emit_return();
    // EXT #8: v2.d[0] = src.d[1] = 0, v2.d[1] = src.d[0]
    EXPECT_EQ(run_fn(t2, 0x1122334455667788ull, 0, 0, 0), 0);
  }
#endif
}

// ------------------------- Hex encoding checks -------------------------

TEST(ARM64EmitterParity, BranchEncodings) {
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(256);
  t.emit(jmp_imm());
  t.emit(je_imm());
  t.emit(jne_imm());
  t.emit(jle_imm());
  t.emit(jge_imm());
  t.emit(jl_imm());
  t.emit(jg_imm());
  t.emit(jbe_imm());
  t.emit(jae_imm());
  t.emit(jb_imm());
  t.emit(ja_imm());
  EXPECT_EQ(t.dump_to_hex_string(),
            "00 00 00 14 00 00 00 54 01 00 00 54 0d 00 00 54 0a 00 00 54 0b 00 00 54 "
            "0c 00 00 54 09 00 00 54 02 00 00 54 03 00 00 54 08 00 00 54");
}

TEST(ARM64EmitterParity, CallAndJumpReg) {
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(256);
  t.emit(call_r64(X5));
  t.emit(jmp_r64(X6));
  t.emit(ret());
  EXPECT_EQ(t.dump_to_hex_string(), "a0 00 3f d6 c0 00 1f d6 c0 03 5f d6");
}

TEST(ARM64EmitterParity, Nops) {
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(256);
  t.emit(nop());
  t.emit(nop_vf());
  t.emit(wait_vf());
  t.emit(null());  // ARM64 null() is a safe NOP (x86 emits a 1-byte xchg nop)
  EXPECT_EQ(t.dump_to_hex_string(),
            "1f 20 03 d5 1f 20 03 d5 1f 20 03 d5 1f 20 03 d5");
}

TEST(ARM64EmitterParity, StackPushPop) {
#if defined(__aarch64__)
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(256);
  t.emit(push_gpr64(X0));
  t.emit(pop_gpr64(X0));
  t.emit(push_gpr64(X1));
  t.emit(pop_gpr64(X1));
  t.emit_return();
  EXPECT_EQ(run_fn(t, 0x1122334455667788ull, 0x99AABBCCDDEEFF00ull, 0, 0),
            0x1122334455667788ull);
#endif
}

TEST(ARM64EmitterParity, PcRelAndStaticEncodings) {
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(512);
  t.emit(load64_pcRel_s32(X0, 4));
  t.emit(load32s_pcRel_s32(X1, 4));
  t.emit(load32u_pcRel_s32(X2, 4));
  t.emit(load16s_pcRel_s32(X3, 4));
  t.emit(load16u_pcRel_s32(X4, 4));
  t.emit(load8s_pcRel_s32(X5, 4));
  t.emit(load8u_pcRel_s32(X6, 4));
  t.emit(store64_pcRel_s32(X7, 4));
  t.emit(store32_pcRel_s32(X8, 4));
  t.emit(store16_pcRel_s32(X9, 4));
  t.emit(store8_pcRel_s32(X10, 4));
  t.emit(static_addr(X11, 4));
  t.emit(static_load_f32(V0, 4));
  t.emit(static_store_f32(V1, 4));
  // Verified against the disassembler: 64/32-bit sizes use literal loads
  // (offset 4), 16/8-bit sizes use ADRP + scaled offset, static_addr uses LDR
  // literal, static_load_f32 uses LDR literal, static_store_f32 uses
  // ADRP + STR with the fp-scaled pimm.
  EXPECT_EQ(
      t.dump_to_hex_string(),
      "20 00 00 58 21 00 00 98 22 00 00 18 10 00 00 90 03 12 80 79 10 00 00 90 04 12 40 79 "
      "10 00 00 90 05 12 80 39 10 00 00 90 06 12 40 39 10 00 00 90 07 12 00 f9 10 00 00 90 "
      "08 12 00 b9 10 00 00 90 09 12 00 79 10 00 00 90 0a 12 00 39 2b 00 00 58 20 00 00 1c "
      "10 00 00 90 01 12 00 bd");
}

TEST(ARM64EmitterParity, StaticStoreF32) {
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(256);
  t.emit(static_store_f32(V1, 8));
  t.emit(static_load_f32(V2, 8));
  // ADRP x16, #0 ; STR s1, [x16, #0x20] (Imm12 8 scaled by 4) ; LDR s2, #8
  EXPECT_EQ(t.dump_to_hex_string(), "10 00 00 90 01 22 00 bd 42 00 00 1c");
}

// ------------------------- More SIMD / memory coverage -------------------------

TEST(ARM64EmitterParity, SimdMemoryRoundTrip) {
#if defined(__aarch64__)
  // 32-bit xmm loads/stores: all addressing modes.
  struct Case32 {
    std::string name;
    int offset;
    Instruction (*store)(CodeTester&, Register, Register, Register);
    Instruction (*load)(CodeTester&, Register, Register, Register);
  };
  std::vector<Case32> c32 = {
      {"xmm32+gpr", 0, [](CodeTester& t, Register a, Register b, Register v) { return IGen::store32_xmm32_gpr64_plus_gpr64(t.generator(), a, b, v); }, [](CodeTester& t, Register d, Register a, Register b) { return IGen::load32_xmm32_gpr64_plus_gpr64(t.generator(), d, a, b); }},
      {"xmm32+s8", 4, [](CodeTester& t, Register a, Register b, Register v) { return IGen::store32_xmm32_gpr64_plus_gpr64_plus_s8(t.generator(), a, b, v, 4); }, [](CodeTester& t, Register d, Register a, Register b) { return IGen::load32_xmm32_gpr64_plus_gpr64_plus_s8(t.generator(), d, a, b, 4); }},
      {"xmm32+s32", 4, [](CodeTester& t, Register a, Register b, Register v) { return IGen::store32_xmm32_gpr64_plus_gpr64_plus_s32(t.generator(), a, b, v, 4); }, [](CodeTester& t, Register d, Register a, Register b) { return IGen::load32_xmm32_gpr64_plus_gpr64_plus_s32(t.generator(), d, a, b, 4); }},
      {"xmm32+base", 4, [](CodeTester& t, Register a, Register b, Register v) { return IGen::store32_xmm32_gpr64_plus_s32(t.generator(), a, v, 4); }, [](CodeTester& t, Register d, Register a, Register b) { return IGen::load32_xmm32_gpr64_plus_s32(t.generator(), d, a, 4); }},
  };
  for (auto& c : c32) {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    t.emit(ins_vf_d_gpr(V0, 0, X0));
    t.emit(c.store(t, X2, X1, V0));
    t.emit(c.load(t, V1, X2, X1));
    t.emit(umov_gpr64_vf_d(X0, V1, 0));
    t.emit_return();
    u64 buf = 0;
    u64 ptr = (c.offset ? (u64)&buf - 4 : (u64)&buf);
    u64 r = run_fn(t, 0x12345678, 0, ptr, 0);
    EXPECT_EQ((u32)r, 0x12345678u) << c.name;
    EXPECT_EQ((u32)buf, 0x12345678u) << c.name;
  }

  // 128-bit loads/stores.
  for (int mode = 0; mode < 3; mode++) {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    t.emit(ins_vf_d_gpr(V0, 0, X0));
    t.emit(ins_vf_d_gpr(V0, 1, X1));
    if (mode == 0) {
      t.emit(IGen::store128_gpr64_simd128(t.generator(), X2, V0));
      t.emit(IGen::load128_simd128_gpr64(t.generator(), V1, X2));
    } else if (mode == 1) {
      t.emit(IGen::store128_gpr64_simd128_s8(t.generator(), X2, V0, 4));
      t.emit(IGen::load128_simd128_gpr64_s8(t.generator(), V1, X2, 4));
    } else {
      t.emit(IGen::store128_gpr64_simd128_s32(t.generator(), X2, V0, 4));
      t.emit(IGen::load128_simd128_gpr64_s32(t.generator(), V1, X2, 4));
    }
    t.emit(umov_gpr64_vf_d(X0, V1, 0));
    t.emit_return();
    u64 buf[2] = {0, 0};
    u64 ptr = (mode ? (u64)buf - 4 : (u64)buf);
    u64 lo = 0x1122334455667788ull, hi = 0x99AABBCCDDEEFF00ull;
    u64 r = run_fn(t, lo, hi, ptr, 0);
    EXPECT_EQ(r, lo) << "128 mode " << mode;
    EXPECT_EQ(buf[0], lo) << "128 mode " << mode << " mem";
    EXPECT_EQ(buf[1], hi) << "128 mode " << mode << " mem";
  }

  // vf loads/stores (full 128-bit GOAL vectors).  x1 is the addr2 offset
  // register (always 0), the high value half goes in x3.
  for (int mode = 0; mode < 3; mode++) {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    t.emit(ins_vf_d_gpr(V0, 0, X0));
    t.emit(ins_vf_d_gpr(V0, 1, X3));
    if (mode == 0) {
      t.emit(IGen::storevf_gpr64_plus_gpr64(t.generator(), V0, X2, X1));
      t.emit(IGen::loadvf_gpr64_plus_gpr64(t.generator(), V1, X2, X1));
    } else if (mode == 1) {
      t.emit(IGen::storevf_gpr64_plus_gpr64_plus_s8(t.generator(), V0, X2, X1, 4));
      t.emit(IGen::loadvf_gpr64_plus_gpr64_plus_s8(t.generator(), V1, X2, X1, 4));
    } else {
      t.emit(IGen::storevf_gpr64_plus_gpr64_plus_s32(t.generator(), V0, X2, X1, 4));
      t.emit(IGen::loadvf_gpr64_plus_gpr64_plus_s32(t.generator(), V1, X2, X1, 4));
    }
    t.emit(umov_gpr64_vf_d(X0, V1, 0));
    t.emit_return();
    u64 buf[2] = {0, 0};
    u64 ptr = (mode ? (u64)buf - 4 : (u64)buf);
    u64 lo = 0x1122334455667788ull, hi = 0x99AABBCCDDEEFF00ull;
    u64 r = run_fn(t, lo, 0, ptr, hi);
    EXPECT_EQ(r, lo) << "vf mode " << mode;
    EXPECT_EQ(buf[0], lo) << "vf mode " << mode << " mem";
    EXPECT_EQ(buf[1], hi) << "vf mode " << mode << " mem";
  }

  // reg_offset xmm32.
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    t.emit(ins_vf_d_gpr(V0, 0, X0));
    t.emit(IGen::store_reg_offset_xmm32(t.generator(), X2, V0, 4));
    t.emit(IGen::load_reg_offset_xmm32(t.generator(), V1, X2, 4));
    t.emit(umov_gpr64_vf_d(X0, V1, 0));
    t.emit_return();
    u64 buf = 0;
    u64 r = run_fn(t, 0xCAFEBABE, 0, (u64)&buf - 4, 0);
    EXPECT_EQ((u32)r, 0xCAFEBABEu);
    EXPECT_EQ((u32)buf, 0xCAFEBABEu);
  }

  // goal loads/stores (addr + off + offset).
  for (int kind = 0; kind < 3; kind++) {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    t.emit(IGen::mov_gpr64_u64(t.generator(), X3, 0xDEADBEEFCAFEBABEull));
    if (kind == 0) {
      t.emit(IGen::store_goal_gpr(t.generator(), X2, X0, X1, 4, 8));
      t.emit(IGen::load_goal_gpr(t.generator(), X0, X2, X1, 4, 8, false));
    } else if (kind == 1) {
      t.emit(ins_vf_d_gpr(V0, 0, X0));
      t.emit(IGen::store_goal_xmm32(t.generator(), X2, V0, X1, 4));
      t.emit(IGen::load_goal_xmm32(t.generator(), V1, X2, X1, 4));
      t.emit(umov_gpr64_vf_d(X0, V1, 0));
    } else {
      t.emit(ins_vf_d_gpr(V0, 0, X0));
      t.emit(ins_vf_d_gpr(V0, 1, X1));
      t.emit(IGen::store_goal_vf(t.generator(), X2, V0, X1, 4));
      t.emit(IGen::load_goal_xmm128(t.generator(), V1, X2, X1, 4));
      t.emit(umov_gpr64_vf_d(X0, V1, 0));
    }
    t.emit_return();
    u64 buf = 0;
    u64 ptr = (u64)&buf - 4;
    u64 val = (kind == 0) ? 0x1122334455667788ull : 0x12345678;
    u64 r = run_fn(t, val, 0, ptr, 0);
    EXPECT_EQ(r, val) << "goal kind " << kind;
  }
#endif
}

TEST(ARM64EmitterParity, SimdIndexedAddressUsesEvenRm) {
  // The register-offset LDR/STR encodings must leave Rm clear in the opcode
  // base.  Odd Rm values masked the old bug because the literal had Rm=1
  // pre-encoded; an even register was silently changed to the following one.
  CodeTester encoding(InstructionSet::ARM64);
  encoding.init_code_buffer(256);
  encoding.emit(IGen::storevf_gpr64_plus_gpr64(encoding.generator(), V0, X2, X4));
  encoding.emit(IGen::loadvf_gpr64_plus_gpr64(encoding.generator(), V1, X2, X4));
  EXPECT_EQ(encoding.dump_to_hex_string(), "40 68 a4 3c 41 68 e4 3c");

#if defined(__aarch64__)
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(256);
  t.emit(ins_vf_d_gpr(V0, 0, X0));
  t.emit(ins_vf_d_gpr(V0, 1, X1));
  t.emit(IGen::mov_gpr64_u64(t.generator(), X4, 0));
  t.emit(IGen::mov_gpr64_u64(t.generator(), X5, 16));
  t.emit(IGen::storevf_gpr64_plus_gpr64(t.generator(), V0, X2, X4));
  t.emit(IGen::loadvf_gpr64_plus_gpr64(t.generator(), V1, X2, X4));
  t.emit(umov_gpr64_vf_d(X0, V1, 0));
  t.emit_return();

  alignas(16) u64 buf[4] = {0, 0, 0, 0};
  const u64 lo = 0x1122334455667788ull;
  const u64 hi = 0x99aabbccddeeff00ull;
  EXPECT_EQ(run_fn(t, lo, hi, reinterpret_cast<u64>(buf), 0), lo);
  EXPECT_EQ(buf[0], lo);
  EXPECT_EQ(buf[1], hi);
  EXPECT_EQ(buf[2], 0u);
  EXPECT_EQ(buf[3], 0u);
#endif
}

TEST(ARM64EmitterParity, VectorConversionsAndSqrt) {
#if defined(__aarch64__)
  // itof_vf: convert 4 int32 lanes to floats.
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(itof_vf(V2, V0));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    u64 lo = 0x0000000200000001ull, hi = 0x00000004FFFFFFFFull;
    u64 r = run_fn(t, lo, hi, 0, 0);
    u32 l0 = (u32)r, l1 = (u32)(r >> 32);
    float f0, f1;
    memcpy(&f0, &l0, 4);
    memcpy(&f1, &l1, 4);
    EXPECT_FLOAT_EQ(f0, 1.0f);
    EXPECT_FLOAT_EQ(f1, 2.0f);
  }
  // ftoi_vf: convert 4 floats to int32.
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(ftoi_vf(V2, V0));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    u32 a = 0, b = 0;
    float fa = 3.7f, fb = -3.7f;
    memcpy(&a, &fa, 4);
    memcpy(&b, &fb, 4);
    u64 lo = (u64)b << 32 | a;
    u64 r = run_fn(t, lo, 0, 0, 0);
    EXPECT_EQ((s32)(u32)r, 3);
    EXPECT_EQ((s32)(u32)(r >> 32), -3);
  }
  // sqrt_vf
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(sqrt_vf(V2, V0));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    u32 a = 0;
    float fa = 9.0f;
    memcpy(&a, &fa, 4);
    u64 r = run_fn(t, a, 0, 0, 0);
    u32 l0 = (u32)r;
    float f0;
    memcpy(&f0, &l0, 4);
    EXPECT_FLOAT_EQ(f0, 3.0f);
  }
#endif
}

TEST(ARM64EmitterParity, ParallelCompareAndExtract) {
#if defined(__aarch64__)
  // parallel_compare_e_w: equal per 32-bit lane -> all-ones or all-zeros.
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(parallel_compare_e_w(V2, V0, V1));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    u64 a = 0x0000000200000001ull, b = 0x0000000200000001ull;
    EXPECT_EQ(run_fn(t, a, 0, b, 0), 0xFFFFFFFFFFFFFFFFull);
    u64 c = 0x0000000300000001ull;
    // lane 0 equal (1==1) -> 0xFFFFFFFF, lane 1 unequal (2!=3) -> 0
    EXPECT_EQ(run_fn(t, a, 0, c, 0), 0x00000000FFFFFFFFull);
  }
  // parallel_compare_gt_b: greater-than per byte lane.
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(parallel_compare_gt_b(V2, V0, V1));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    u64 a = 0x0302010000000000ull, b = 0x0101010101010101ull;
    u64 r = run_fn(t, a, 0, b, 0);
    // LE byte order: a bytes are 0..0,1,2,3 at positions 0..7; only bytes 6,7
    // (0x02, 0x03) exceed b's 0x01 -> 0xFFFF in the high half of d[0].
    EXPECT_EQ(r, 0xFFFF000000000000ull);
  }
  // pextlb_swapped == vpunpcklbw (zip1 16B)
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(pextlb_swapped(V2, V0, V1));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    u64 a = 0x1122334455667788ull, b = 0xAABBCCDDEEFF0001ull;
    u64 r = run_fn(t, a, 0, b, 0);
    // ZIP1: a0, b0, a1, b1, a2, b2, a3, b3 in the low 64 bits.
    EXPECT_EQ(r, 0xEE55FF6600770188ull);
  }
  // pcpyld_swapped == vpunpcklqdq (zip1 2D)
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(pcpyld_swapped(V2, V0, V1));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    u64 a = 0x1111222233334444ull, b = 0x5555666677778888ull;
    EXPECT_EQ(run_fn(t, a, 0xDEADBEEFCAFEBABEull, b, 0x0102030405060708ull), a);
  }
#endif
}

TEST(ARM64EmitterParity, WordShiftsAndPack) {
#if defined(__aarch64__)
  // pw_sll / pw_srl / pw_sra: per 32-bit lane shifts.
  for (int kind = 0; kind < 3; kind++) {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    t.emit(ins_vf_d_gpr(V0, 0, X0));
    t.emit(ins_vf_d_gpr(V0, 1, X1));
    if (kind == 0) {
      t.emit(pw_sll(V2, V0, 4));
    } else if (kind == 1) {
      t.emit(pw_srl(V2, V0, 4));
    } else {
      t.emit(pw_sra(V2, V0, 4));
    }
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    u64 lo = 0x0000000100000001ull;  // lanes {1, 1}
    u64 r = run_fn(t, lo, 0, 0, 0);
    if (kind == 0) {
      EXPECT_EQ((u32)r, 0x10u);
    } else {
      EXPECT_EQ((u32)r, 0u);
    }
  }
  // ph_sll / ph_srl: per 16-bit lane shifts.
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    t.emit(ins_vf_d_gpr(V0, 0, X0));
    t.emit(ph_sll(V2, V0, 4));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    u64 lo = 0x0001000100010001ull;
    u64 r = run_fn(t, lo, 0, 0, 0);
    EXPECT_EQ((u16)r, 0x10u);
  }
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    t.emit(ins_vf_d_gpr(V0, 0, X0));
    t.emit(ph_srl(V2, V0, 4));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    u64 lo = 0x0010001000100010ull;
    u64 r = run_fn(t, lo, 0, 0, 0);
    EXPECT_EQ((u16)r, 0x1u);
  }
  // vpackuswb: saturating 16-bit -> 8-bit pack.
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(vpackuswb(V2, V0, V1));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    u64 a = 0x0002000100010001ull, b = 0x0003000200020002ull;
    u64 r = run_fn(t, a, 0, b, 0);
    // bytes: sat(1),sat(1),sat(1),sat(2),sat(2),sat(2),sat(2),sat(3)
    EXPECT_EQ((u8)r, 1u);
    EXPECT_EQ((u8)(r >> 8), 1u);
  }
#endif
}

TEST(ARM64EmitterParity, StaticLoadStoreEncodings) {
  // static_load/static_store use the adrp+load/store form with placeholder
  // page offsets; encoding check only.
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(256);
  t.emit(IGen::static_load(t.generator(), X0, 4, 8, false));
  t.emit(IGen::static_store(t.generator(), X1, 4, 8));
  // LDR X0, #4 ; ADRP x16, #0 ; STR X1, [x16, #0x20] (pimm units scaled by 8)
  EXPECT_EQ(t.dump_to_hex_string(), "20 00 00 58 10 00 00 90 01 12 00 f9");
}

TEST(ARM64EmitterParity, LoadvfRipEncoding) {
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(256);
  t.emit(loadvf_rip_plus_s32(V0, 4));
  // ADRP x16, #0 ; LDR Q0, [x16, #0x40] (pimm units scaled by 16)
  EXPECT_EQ(t.dump_to_hex_string(), "10 00 00 90 00 12 c0 3d");
}

// ------------------------- Final coverage batch -------------------------

TEST(ARM64EmitterParity, RemainingMemoryModes) {
#if defined(__aarch64__)
  // GPR memory combos not covered above.
  struct Case {
    std::string name;
    int bytes;
    bool sign_extend;
    int offset;
    void (*store)(CodeTester&, Register, Register, Register);
    void (*load)(CodeTester&, Register, Register, Register);
  };
  std::vector<Case> cases = {
      {"8u+s8", 1, false, 4, [](CodeTester& t, Register a, Register b, Register v) { t.emit(IGen::store8_gpr64_gpr64_plus_gpr64_plus_s8(t.generator(), a, b, v, 4)); }, [](CodeTester& t, Register d, Register a, Register b) { t.emit(IGen::load8u_gpr64_gpr64_plus_gpr64_plus_s8(t.generator(), d, a, b, 4)); }},
      {"16u+s8", 2, false, 4, [](CodeTester& t, Register a, Register b, Register v) { t.emit(IGen::store16_gpr64_gpr64_plus_gpr64_plus_s8(t.generator(), a, b, v, 4)); }, [](CodeTester& t, Register d, Register a, Register b) { t.emit(IGen::load16u_gpr64_gpr64_plus_gpr64_plus_s8(t.generator(), d, a, b, 4)); }},
      {"32u+s32", 4, false, 4, [](CodeTester& t, Register a, Register b, Register v) { t.emit(IGen::store32_gpr64_gpr64_plus_gpr64_plus_s32(t.generator(), a, b, v, 4)); }, [](CodeTester& t, Register d, Register a, Register b) { t.emit(IGen::load32u_gpr64_gpr64_plus_gpr64_plus_s32(t.generator(), d, a, b, 4)); }},
  };
  for (auto& c : cases) {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    c.store(t, X2, X1, X0);
    c.load(t, X0, X2, X1);
    t.emit_return();
    u64 buf = 0;
    u64 val = 0x1234;
    u64 mask = (c.bytes == 8) ? ~0ull : ((1ull << (8 * c.bytes)) - 1);
    u64 r = run_fn(t, val, 0, (u64)&buf - c.offset, 0);
    EXPECT_EQ(r & mask, val & mask) << c.name;
    EXPECT_EQ(buf & mask, val & mask) << c.name;
  }
  // xmm32 base+s8
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    t.emit(ins_vf_d_gpr(V0, 0, X0));
    t.emit(IGen::store32_xmm32_gpr64_plus_s8(t.generator(), X2, V0, 4));
    t.emit(IGen::load32_xmm32_gpr64_plus_s8(t.generator(), V1, X2, 4));
    t.emit(umov_gpr64_vf_d(X0, V1, 0));
    t.emit_return();
    u64 buf = 0;
    u64 r = run_fn(t, 0xDEADBEEF, 0, (u64)&buf - 4, 0);
    EXPECT_EQ((u32)r, 0xDEADBEEFu);
  }
  // 128-bit reg-offset
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    t.emit(ins_vf_d_gpr(V0, 0, X0));
    t.emit(ins_vf_d_gpr(V0, 1, X3));
    t.emit(IGen::store128_xmm128_reg_offset(t.generator(), X2, V0, 4));
    t.emit(IGen::load128_xmm128_reg_offset(t.generator(), V1, X2, 4));
    t.emit(umov_gpr64_vf_d(X0, V1, 0));
    t.emit_return();
    u64 buf[2] = {0, 0};
    u64 lo = 0x1111222233334444ull, hi = 0x5555666677778888ull;
    u64 r = run_fn(t, lo, 0, (u64)buf - 4, hi);
    EXPECT_EQ(r, lo);
    EXPECT_EQ(buf[0], lo);
    EXPECT_EQ(buf[1], hi);
  }
#endif
}

TEST(ARM64EmitterParity, ParallelCompareAndExtractMore) {
#if defined(__aarch64__)
  // e_b, e_h, gt_h, gt_w
  struct C {
    std::string name;
    void (*emit)(CodeTester&);
    u64 a, b, expect_lo;
  };
  std::vector<C> cases = {
      {"e_b", [](CodeTester& t) { t.emit(parallel_compare_e_b(V2, V0, V1)); }, 0x0000000000000001ull, 0x0000000000000001ull, 0xFFFFFFFFFFFFFFFFull},
      {"e_h", [](CodeTester& t) { t.emit(parallel_compare_e_h(V2, V0, V1)); }, 0x0001000100010001ull, 0x0001000100010001ull, 0xFFFFFFFFFFFFFFFFull},
      {"gt_h", [](CodeTester& t) { t.emit(parallel_compare_gt_h(V2, V0, V1)); }, 0x0002000200020002ull, 0x0001000100010001ull, 0xFFFFFFFFFFFFFFFFull},
      {"gt_w", [](CodeTester& t) { t.emit(parallel_compare_gt_w(V2, V0, V1)); }, 0x0000000200000002ull, 0x0000000100000001ull, 0xFFFFFFFFFFFFFFFFull},
  };
  for (auto& c : cases) {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    c.emit(t);
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    EXPECT_EQ(run_fn(t, c.a, 0, c.b, 0), c.expect_lo) << c.name;
  }
  // PS2 pext lower/upper operations map to x86 PUNPCK and ARM ZIP.  Keep both
  // inputs observably different: the previous tests checked only the first
  // lane and therefore could not distinguish ZIP from the incorrect UZP.
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(pextlb_swapped(V2, V0, V1));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    EXPECT_EQ(run_fn(t, 0x0807060504030201ull, 0, 0x1817161514131211ull, 0),
              0x1404130312021101ull);
  }
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(pextlh_swapped(V2, V0, V1));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    EXPECT_EQ(run_fn(t, 0x0004000300020001ull, 0, 0x0014001300120011ull, 0),
              0x0012000200110001ull);
  }
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(pextlw_swapped(V2, V0, V1));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    EXPECT_EQ(run_fn(t, 0x0000000200000001ull, 0, 0x0000001200000011ull, 0),
              0x0000001100000001ull);
  }
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(pextub_swapped(V2, V0, V1));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    EXPECT_EQ(run_fn(t, 0, 0x100f0e0d0c0b0a09ull, 0, 0x201f1e1d1c1b1a19ull),
              0x1c0c1b0b1a0a1909ull);
  }
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(pextuh_swapped(V2, V0, V1));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    EXPECT_EQ(run_fn(t, 0, 0x0008000700060005ull, 0, 0x0018001700160015ull),
              0x0016000600150005ull);
  }
  {
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    load_src_vecs(t);
    t.emit(pextuw_swapped(V2, V0, V1));
    t.emit(umov_gpr64_vf_d(X0, V2, 0));
    t.emit_return();
    EXPECT_EQ(run_fn(t, 0, 0x0000000400000003ull, 0, 0x0000001400000013ull),
              0x0000001300000003ull);
  }
#endif
}
