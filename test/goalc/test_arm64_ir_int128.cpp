#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

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

bool operator==(const V128& lhs, const V128& rhs) {
  return lhs.lo == rhs.lo && lhs.hi == rhs.hi;
}

using Bytes = std::array<u8, 16>;

u16 get_u16(const Bytes& bytes, int index) {
  return u16(bytes[index * 2]) | (u16(bytes[index * 2 + 1]) << 8);
}

u32 get_u32(const Bytes& bytes, int index) {
  return u32(bytes[index * 4]) | (u32(bytes[index * 4 + 1]) << 8) |
         (u32(bytes[index * 4 + 2]) << 16) | (u32(bytes[index * 4 + 3]) << 24);
}

void put_u16(Bytes& bytes, int index, u16 value) {
  bytes[index * 2] = u8(value);
  bytes[index * 2 + 1] = u8(value >> 8);
}

void put_u32(Bytes& bytes, int index, u32 value) {
  for (int byte = 0; byte < 4; byte++) {
    bytes[index * 4 + byte] = u8(value >> (8 * byte));
  }
}

Bytes to_bytes(V128 value) {
  Bytes result{};
  for (int byte = 0; byte < 8; byte++) {
    result[byte] = u8(value.lo >> (8 * byte));
    result[8 + byte] = u8(value.hi >> (8 * byte));
  }
  return result;
}

V128 from_bytes(const Bytes& bytes) {
  V128 result;
  for (int byte = 0; byte < 8; byte++) {
    result.lo |= u64(bytes[byte]) << (8 * byte);
    result.hi |= u64(bytes[8 + byte]) << (8 * byte);
  }
  return result;
}

s16 signed16(u16 value) {
  return (value & 0x8000) ? s16(-32768 + (value & 0x7FFF)) : s16(value);
}

s32 signed32(u32 value) {
  return (value & 0x80000000) ? s32(-2147483648ll + (value & 0x7FFFFFFF)) : s32(value);
}

u8 saturate_u8(s16 value) {
  return value < 0 ? 0 : value > 255 ? 255 : u8(value);
}

V128 expected_math3(IR_Int128Math3Asm::Kind kind, V128 src1, V128 src2) {
  const auto a = to_bytes(src1);
  const auto b = to_bytes(src2);
  Bytes out{};

  switch (kind) {
    case IR_Int128Math3Asm::Kind::PEXTUB:
    case IR_Int128Math3Asm::Kind::PEXTUH:
    case IR_Int128Math3Asm::Kind::PEXTUW:
    case IR_Int128Math3Asm::Kind::PEXTLB:
    case IR_Int128Math3Asm::Kind::PEXTLH:
    case IR_Int128Math3Asm::Kind::PEXTLW: {
      const bool upper = kind == IR_Int128Math3Asm::Kind::PEXTUB ||
                         kind == IR_Int128Math3Asm::Kind::PEXTUH ||
                         kind == IR_Int128Math3Asm::Kind::PEXTUW;
      const int unit =
          (kind == IR_Int128Math3Asm::Kind::PEXTUB || kind == IR_Int128Math3Asm::Kind::PEXTLB) ? 1
          : (kind == IR_Int128Math3Asm::Kind::PEXTUH || kind == IR_Int128Math3Asm::Kind::PEXTLH)
              ? 2
              : 4;
      const int source_lane = upper ? 1 : 0;
      const int count = 8 / unit;
      // The ARM emitter receives (src2, src1) for the *_swapped operations;
      // UZP therefore contributes src2's selected lanes first.
      for (int lane = 0; lane < count; lane++) {
        for (int byte = 0; byte < unit; byte++) {
          out[lane * unit + byte] = b[(2 * lane + source_lane) * unit + byte];
          out[(count + lane) * unit + byte] = a[(2 * lane + source_lane) * unit + byte];
        }
      }
      break;
    }
    case IR_Int128Math3Asm::Kind::PCPYLD:
      for (int byte = 0; byte < 8; byte++) {
        out[byte] = b[byte];
        out[8 + byte] = a[byte];
      }
      break;
    case IR_Int128Math3Asm::Kind::PCPYUD:
      for (int byte = 0; byte < 8; byte++) {
        out[byte] = a[8 + byte];
        out[8 + byte] = b[8 + byte];
      }
      break;
    case IR_Int128Math3Asm::Kind::PSUBW:
      for (int lane = 0; lane < 4; lane++) {
        put_u32(out, lane, get_u32(a, lane) - get_u32(b, lane));
      }
      break;
    case IR_Int128Math3Asm::Kind::PCEQB:
      for (int lane = 0; lane < 16; lane++) {
        out[lane] = a[lane] == b[lane] ? 0xFF : 0;
      }
      break;
    case IR_Int128Math3Asm::Kind::PCEQH:
      for (int lane = 0; lane < 8; lane++) {
        put_u16(out, lane, get_u16(a, lane) == get_u16(b, lane) ? 0xFFFF : 0);
      }
      break;
    case IR_Int128Math3Asm::Kind::PCEQW:
      for (int lane = 0; lane < 4; lane++) {
        put_u32(out, lane, get_u32(a, lane) == get_u32(b, lane) ? 0xFFFFFFFF : 0);
      }
      break;
    case IR_Int128Math3Asm::Kind::PCGTB:
      for (int lane = 0; lane < 16; lane++) {
        out[lane] = s8(a[lane]) > s8(b[lane]) ? 0xFF : 0;
      }
      break;
    case IR_Int128Math3Asm::Kind::PCGTH:
      for (int lane = 0; lane < 8; lane++) {
        put_u16(out, lane, signed16(get_u16(a, lane)) > signed16(get_u16(b, lane)) ? 0xFFFF : 0);
      }
      break;
    case IR_Int128Math3Asm::Kind::PCGTW:
      for (int lane = 0; lane < 4; lane++) {
        put_u32(out, lane,
                signed32(get_u32(a, lane)) > signed32(get_u32(b, lane)) ? 0xFFFFFFFF : 0);
      }
      break;
    case IR_Int128Math3Asm::Kind::POR:
      for (int byte = 0; byte < 16; byte++) {
        out[byte] = a[byte] | b[byte];
      }
      break;
    case IR_Int128Math3Asm::Kind::PXOR:
      for (int byte = 0; byte < 16; byte++) {
        out[byte] = a[byte] ^ b[byte];
      }
      break;
    case IR_Int128Math3Asm::Kind::PAND:
      for (int byte = 0; byte < 16; byte++) {
        out[byte] = a[byte] & b[byte];
      }
      break;
    case IR_Int128Math3Asm::Kind::PACKUSWB:
      for (int lane = 0; lane < 8; lane++) {
        out[lane] = saturate_u8(signed16(get_u16(a, lane)));
        out[8 + lane] = saturate_u8(signed16(get_u16(b, lane)));
      }
      break;
    case IR_Int128Math3Asm::Kind::PADDB:
      for (int byte = 0; byte < 16; byte++) {
        out[byte] = u8(a[byte] + b[byte]);
      }
      break;
    default:
      ADD_FAILURE() << "unhandled int128 math3 opcode";
  }

  return from_bytes(out);
}

V128 expected_math2(IR_Int128Math2Asm::Kind kind, V128 src, u8 imm) {
  const auto input = to_bytes(src);
  Bytes out{};

  switch (kind) {
    case IR_Int128Math2Asm::Kind::PW_SLL:
      for (int lane = 0; lane < 4; lane++) {
        put_u32(out, lane, imm >= 32 ? 0 : get_u32(input, lane) << imm);
      }
      break;
    case IR_Int128Math2Asm::Kind::PW_SRL:
      for (int lane = 0; lane < 4; lane++) {
        put_u32(out, lane, imm >= 32 ? 0 : get_u32(input, lane) >> imm);
      }
      break;
    case IR_Int128Math2Asm::Kind::PW_SRA:
      for (int lane = 0; lane < 4; lane++) {
        const u32 value = get_u32(input, lane);
        if (imm == 0) {
          put_u32(out, lane, value);
        } else if (imm >= 32) {
          put_u32(out, lane, value & 0x80000000 ? 0xFFFFFFFF : 0);
        } else {
          const u32 fill = (value & 0x80000000) ? (~u32(0) << (32 - imm)) : 0;
          put_u32(out, lane, (value >> imm) | fill);
        }
      }
      break;
    case IR_Int128Math2Asm::Kind::PH_SLL:
      for (int lane = 0; lane < 8; lane++) {
        put_u16(out, lane, imm >= 16 ? 0 : u16(get_u16(input, lane) << imm));
      }
      break;
    case IR_Int128Math2Asm::Kind::PH_SRL:
      for (int lane = 0; lane < 8; lane++) {
        put_u16(out, lane, imm >= 16 ? 0 : u16(get_u16(input, lane) >> imm));
      }
      break;
    case IR_Int128Math2Asm::Kind::VPSRLDQ:
      for (int byte = 0; byte < 16; byte++) {
        out[byte] = imm < 16 && byte + imm < 16 ? input[byte + imm] : 0;
      }
      break;
    case IR_Int128Math2Asm::Kind::VPSLLDQ:
      for (int byte = 0; byte < 16; byte++) {
        out[byte] = imm < 16 && byte >= imm ? input[byte - imm] : 0;
      }
      break;
    case IR_Int128Math2Asm::Kind::VPSHUFLW:
      out = input;
      for (int lane = 0; lane < 4; lane++) {
        put_u16(out, lane, get_u16(input, (imm >> (2 * lane)) & 3));
      }
      break;
    case IR_Int128Math2Asm::Kind::VPSHUFHW:
      out = input;
      for (int lane = 0; lane < 4; lane++) {
        put_u16(out, 4 + lane, get_u16(input, 4 + ((imm >> (2 * lane)) & 3)));
      }
      break;
    default:
      ADD_FAILURE() << "unhandled int128 math2 opcode";
  }

  return from_bytes(out);
}

struct IRHarness {
  ObjectGenerator gen{GameVersion::Jak2, InstructionSet::ARM64};
  TypeSystem ts;
  FunctionDebugInfo dbg;
  FunctionRecord func;
  AllocationResult allocs;

  IRHarness() {
    ts.add_builtin_types(GameVersion::Jak2);
    dbg.name = "int128-test";
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
  ireg.reg_class = RegClass::INT_128;
  return std::make_unique<RegVal>(ireg, TypeSpec("uint128"));
}

std::vector<u8> build_three(IR_Int128Math3Asm::Kind kind,
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
  IR_Int128Math3Asm op(true, d.get(), a.get(), b.get(), kind);
  op.do_codegen_arm64(&h.gen, h.allocs, ir);
  return h.generate();
}

std::vector<u8> build_two(IR_Int128Math2Asm::Kind kind, Register dst, Register src, u8 imm) {
  IRHarness h;
  auto d = make_reg(0);
  auto a = make_reg(1);
  h.assign(0, dst);
  h.assign(1, src);
  IR_Record ir = h.gen.add_ir(h.func);
  IR_Int128Math2Asm op(true, d.get(), a.get(), kind, imm);
  op.do_codegen_arm64(&h.gen, h.allocs, ir);
  return h.generate();
}

V128 execute_vector(const std::vector<u8>& data, V128 src1, V128 src2, Register result = V2) {
#if defined(__aarch64__)
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(4096);
  t.emit(ins_vf_d_gpr(V0, 0, X0));
  t.emit(ins_vf_d_gpr(V0, 1, X1));
  t.emit(ins_vf_d_gpr(V1, 0, X2));
  t.emit(ins_vf_d_gpr(V1, 1, X3));
  t.append_bytes(data.data() + 4, int(data.size() - 4));
  t.emit(umov_gpr64_vf_d(X0, result, 0));
  t.emit(umov_gpr64_vf_d(X1, result, 1));
  t.emit_return();
  const auto lo = t.execute(src1.lo, src1.hi, src2.lo, src2.hi);
  // The generated function returns X0; execute the same body with the high
  // lane moved into X0 to observe the complete 128-bit result.
  CodeTester high(InstructionSet::ARM64);
  high.init_code_buffer(4096);
  high.emit(ins_vf_d_gpr(V0, 0, X0));
  high.emit(ins_vf_d_gpr(V0, 1, X1));
  high.emit(ins_vf_d_gpr(V1, 0, X2));
  high.emit(ins_vf_d_gpr(V1, 1, X3));
  high.append_bytes(data.data() + 4, int(data.size() - 4));
  high.emit(umov_gpr64_vf_d(X0, result, 1));
  high.emit_return();
  return {lo, high.execute(src1.lo, src1.hi, src2.lo, src2.hi)};
#else
  (void)data;
  (void)src1;
  (void)src2;
  (void)result;
  return {};
#endif
}

}  // namespace

TEST(ARM64IRInt128, Math3AllOpcodesAndAliases) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
#else
  const V128 src1 = from_bytes({0x80, 0x01, 0x7F, 0xFF, 0x00, 0x10, 0xFE, 0x7F, 0x01, 0x80, 0x34,
                                0x12, 0xFF, 0x00, 0x78, 0x56});
  const V128 src2 = from_bytes({0x01, 0x01, 0x80, 0x01, 0xFF, 0x10, 0x02, 0x80, 0x01, 0x80, 0x78,
                                0x56, 0xFF, 0x00, 0x12, 0x34});
  const std::array kinds = {IR_Int128Math3Asm::Kind::PEXTUB,   IR_Int128Math3Asm::Kind::PEXTUH,
                            IR_Int128Math3Asm::Kind::PEXTUW,   IR_Int128Math3Asm::Kind::PEXTLB,
                            IR_Int128Math3Asm::Kind::PEXTLH,   IR_Int128Math3Asm::Kind::PEXTLW,
                            IR_Int128Math3Asm::Kind::PCPYUD,   IR_Int128Math3Asm::Kind::PCPYLD,
                            IR_Int128Math3Asm::Kind::PSUBW,    IR_Int128Math3Asm::Kind::PCEQB,
                            IR_Int128Math3Asm::Kind::PCEQH,    IR_Int128Math3Asm::Kind::PCEQW,
                            IR_Int128Math3Asm::Kind::PCGTB,    IR_Int128Math3Asm::Kind::PCGTH,
                            IR_Int128Math3Asm::Kind::PCGTW,    IR_Int128Math3Asm::Kind::POR,
                            IR_Int128Math3Asm::Kind::PXOR,     IR_Int128Math3Asm::Kind::PAND,
                            IR_Int128Math3Asm::Kind::PACKUSWB, IR_Int128Math3Asm::Kind::PADDB};
  for (auto kind : kinds) {
    const auto expected = expected_math3(kind, src1, src2);
    const auto data = build_three(kind, V2, V0, V1);
    EXPECT_EQ(execute_vector(data, src1, src2), expected) << "opcode=" << int(kind);

    const auto src1_alias = build_three(kind, V0, V0, V1);
    const auto src2_alias = build_three(kind, V1, V0, V1);
    EXPECT_EQ(execute_vector(src1_alias, src1, src2, V0), expected)
        << "src1 alias opcode=" << int(kind);
    EXPECT_EQ(execute_vector(src2_alias, src1, src2, V1), expected)
        << "src2 alias opcode=" << int(kind);
  }
#endif
}

TEST(ARM64IRInt128, Math2AllOpcodesBoundariesAndAliases) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
#else
  const V128 src = from_bytes({0x01, 0x80, 0xFF, 0x7F, 0x02, 0x00, 0x00, 0x80, 0xFF, 0xFF, 0x01,
                               0x00, 0x34, 0x12, 0xCD, 0xAB});
  const std::array kinds = {IR_Int128Math2Asm::Kind::PW_SLL,  IR_Int128Math2Asm::Kind::PW_SRL,
                            IR_Int128Math2Asm::Kind::PW_SRA,  IR_Int128Math2Asm::Kind::PH_SLL,
                            IR_Int128Math2Asm::Kind::PH_SRL,  IR_Int128Math2Asm::Kind::VPSRLDQ,
                            IR_Int128Math2Asm::Kind::VPSLLDQ, IR_Int128Math2Asm::Kind::VPSHUFLW,
                            IR_Int128Math2Asm::Kind::VPSHUFHW};
  const std::array<u8, 8> immediates = {0, 1, 4, 15, 16, 31, 32, 255};
  for (auto kind : kinds) {
    for (u8 imm : immediates) {
      const auto expected = expected_math2(kind, src, imm);
      const auto data = build_two(kind, V2, V0, imm);
      EXPECT_EQ(execute_vector(data, src, {}), expected)
          << "opcode=" << int(kind) << " imm=" << int(imm);

      const auto alias = build_two(kind, V0, V0, imm);
      EXPECT_EQ(execute_vector(alias, src, {}, V0), expected)
          << "alias opcode=" << int(kind) << " imm=" << int(imm);
    }
  }
#endif
}

TEST(ARM64IRInt128, ScratchRegistersExcludedOnlyForArmShuffles) {
  auto dst = make_reg(0);
  auto src = make_reg(1);
  IR_Int128Math2Asm shuffle(true, dst.get(), src.get(), IR_Int128Math2Asm::Kind::VPSHUFLW, 0xE4);
  const auto arm = shuffle.to_rai(InstructionSet::ARM64);
  const auto x86 = shuffle.to_rai(InstructionSet::X86);
  ASSERT_EQ(arm.exclude.size(), 1);
  EXPECT_EQ(arm.exclude.at(0), X16);
  EXPECT_TRUE(x86.exclude.empty());

  IR_Int128Math2Asm shift(true, dst.get(), src.get(), IR_Int128Math2Asm::Kind::PW_SLL, 4);
  EXPECT_TRUE(shift.to_rai(InstructionSet::ARM64).exclude.empty());

  IR_Int128Math2Asm byte_shift(true, dst.get(), src.get(), IR_Int128Math2Asm::Kind::VPSRLDQ, 4);
  ASSERT_EQ(byte_shift.to_rai(InstructionSet::ARM64).exclude.size(), 1);
  EXPECT_EQ(byte_shift.to_rai(InstructionSet::ARM64).exclude.at(0), X16);

  IR_Int128Math3Asm pack(true, dst.get(), src.get(), src.get(), IR_Int128Math3Asm::Kind::PACKUSWB);
  ASSERT_EQ(pack.to_rai(InstructionSet::ARM64).exclude.size(), 1);
  EXPECT_EQ(pack.to_rai(InstructionSet::ARM64).exclude.at(0), X16);
  EXPECT_TRUE(pack.to_rai(InstructionSet::X86).exclude.empty());
}
