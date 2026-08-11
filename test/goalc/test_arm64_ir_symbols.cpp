#include <cstring>
#include <memory>

#include "common/link_types.h"
#include "common/type_system/TypeSystem.h"

#include "goalc/compiler/IR.h"
#include "goalc/compiler/StaticObject.h"
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

  IRHarness() {
    ts.add_builtin_types(GameVersion::Jak2);
    dbg.name = "sym-test";
    func = gen.add_function_to_seg(0, &dbg);
  }

  void assign(int id, Register reg, int n_ir) {
    std::vector<bool> live(n_ir, true);
    std::vector<Assignment> ass(n_ir);
    for (auto& a : ass) {
      a.kind = Assignment::Kind::REGISTER;
      a.reg = reg;
    }
    if (allocs.ass_as_ranges.size() <= (size_t)id) {
      std::vector<bool> dummy_live(1, true);
      std::vector<Assignment> dummy_ass(1);
      allocs.ass_as_ranges.resize(id + 1, AssignmentRange(0, dummy_live, dummy_ass));
    }
    allocs.ass_as_ranges[id] = AssignmentRange(0, live, ass);
  }

  AllocationResult allocs;

  // Append a ret instruction (the epilogue role) before the static literals so
  // execution does not fall through into the data words.
  void finish() {
    IR_Record irr = gen.add_ir(func);
    gen.add_instr(Instruction(IGen::ARM64::ret()), irr);
  }

  std::vector<u8> generate() { return gen.generate_data_v3(&ts).segment_data.at(0); }
  const std::vector<u8>& link_table() { return m_link; }

  // Run the generated instructions (skip the type tag) with a trailing ret.
  // st (x21) and offset (x22) are set from x2/x3 so that x0/x1 stay free for
  // the generated code's value registers.
  u64 execute_instrs(const std::vector<u8>& data, size_t start, u64 st, u64 offset, u64 in0,
                     u64 in1) {
#ifdef __aarch64__
    CodeTester t(InstructionSet::ARM64);
    // ARM64 object generation page-aligns static data so code pages can be RX
    // while static data remains writable.
    t.init_code_buffer(0x10000);
    t.emit(IGen::mov_gpr64_gpr64(t.generator(), X21, X2));
    t.emit(IGen::mov_gpr64_gpr64(t.generator(), X22, X3));
    // copy the whole tail (instructions + trailing statics) so the patched
    // literal offsets remain valid.
    t.append_bytes(data.data() + start, (int)(data.size() - start));
    t.emit_return();
    return t.execute(in0, in1, st, offset);
#else
    return 0;
#endif
  }

  // Find the literal .quad: with one function and one static it is the last
  // 8 bytes of the segment data.
  size_t literal_offset(const std::vector<u8>& data) const { return data.size() - 8; }

 private:
  std::vector<u8> m_link;
};

std::unique_ptr<RegVal> make_reg(int id, RegClass cls, const TypeSpec& ts) {
  IRegister ireg;
  ireg.id = id;
  ireg.reg_class = cls;
  return std::make_unique<RegVal>(ireg, ts);
}

u32 read_word(const std::vector<u8>& data, size_t off) {
  u32 w;
  memcpy(&w, data.data() + off, 4);
  return w;
}

bool has_link_kind(const std::vector<u8>& link, u8 kind) {
  for (size_t i = 0; i < link.size(); i++) {
    if (link[i] == kind) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(ARM64IRSymbols, LoadSymbolPointerFalse) {
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  auto dest = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
  h.assign(0, X0, 1);
  IR_LoadSymbolPointer ir(dest.get(), "#f");
  ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
  auto data = h.generate();
  // mov x0, x21 (st)
  EXPECT_EQ(read_word(data, 4), 0xAA1503E0u);
}

TEST(ARM64IRSymbols, LoadSymbolPointerTrue) {
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  auto dest = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
  h.assign(0, X0, 1);
  IR_LoadSymbolPointer ir(dest.get(), "#t");
  ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
  auto data = h.generate();
  // lea x0, [x21, #offset]: add x0, x21, #imm
  EXPECT_EQ((read_word(data, 4) >> 5) & 0x1F, 21u);
}

TEST(ARM64IRSymbols, LoadSymbolPointerNamed) {
#if defined(__aarch64__)
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  auto dest = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
  h.assign(0, X0, 2);
  IR_LoadSymbolPointer ir(dest.get(), "my-symbol");
  ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
  h.finish();
  auto data = h.generate();
  // The symbol link is signed 32-bit.  Use a negative offset to verify that
  // ARM64 sign-extends it before adding it to the GOAL s7 offset.
  EXPECT_GE(data.size(), 4 + 16 + 8);
  u64 negative_symbol_offset = 0x00000000fffffea0ULL;
  memcpy(data.data() + h.literal_offset(data), &negative_symbol_offset, sizeof(negative_symbol_offset));
  // execute with st = 0x200000: result = 0x200000 - 0x160.
  u64 r = h.execute_instrs(data, 4, 0x200000, 0, 0, 0);
  EXPECT_EQ(r, 0x1ffea0u);
  // the symbol link table must contain a LINK_SYMBOL_OFFSET entry.
  EXPECT_TRUE(has_link_kind(h.gen.generate_data_v3(&h.ts).link_tables.at(0), 1));
#endif
}

TEST(ARM64IRSymbols, SetSymbolValue) {
#if defined(__aarch64__)
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  auto src = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
  h.assign(0, X0, 2);
  SymbolVal sym("the-var", TypeSpec("int"));
  IR_SetSymbolValue ir(&sym, src.get());
  ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
  h.finish();
  auto data = h.generate();
  // The literal starts with the no-offset sentinel for the runtime linker.
  // Simulate the linker result of (symbol offset - 1) == 0 for this buffer.
  EXPECT_EQ(read_word(data, h.literal_offset(data)), LINK_SYM_NO_OFFSET_FLAG);
  u64 zero_symbol_offset = 0;
  memcpy(data.data() + h.literal_offset(data), &zero_symbol_offset, sizeof(zero_symbol_offset));
  // execute: st points to a 4-byte buffer, offset = 0 -> store x0 into [st].
  u32 buf = 0;
  u64 r = h.execute_instrs(data, 4, (u64)&buf, 0, 0xCAFEBABE, 0);
  EXPECT_EQ(buf, 0xCAFEBABEu);
  EXPECT_TRUE(has_link_kind(h.gen.generate_data_v3(&h.ts).link_tables.at(0), 1));
#endif
}

TEST(ARM64IRSymbols, GetSymbolValue) {
#if defined(__aarch64__)
  for (bool sext : {false, true}) {
    IRHarness h;
    IR_Record ir0 = h.gen.add_ir(h.func);
    auto dst = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
    h.assign(0, X0, 2);
    SymbolVal sym("the-var", TypeSpec("int"));
    IR_GetSymbolValue ir(dst.get(), &sym, sext);
    ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
    h.finish();
    auto data = h.generate();
    EXPECT_EQ(read_word(data, h.literal_offset(data)), LINK_SYM_NO_OFFSET_FLAG);
    u64 zero_symbol_offset = 0;
    memcpy(data.data() + h.literal_offset(data), &zero_symbol_offset, sizeof(zero_symbol_offset));
    u32 buf = sext ? 0x80000000u : 0x12345678u;
    u64 r = h.execute_instrs(data, 4, (u64)&buf, 0, 0, 0);
    u32 expected = (u32)r;
    EXPECT_EQ(expected, buf);
    if (sext) {
      EXPECT_EQ((s64)(s32)expected, (s64)(s32)0x80000000);
    }
  }
#endif
}

TEST(ARM64IRSymbols, StaticVarAddr) {
#if defined(__aarch64__)
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  auto dest = make_reg(0, RegClass::GPR_64, TypeSpec("pointer"));
  h.assign(0, X0, 2);
  StaticStructure stat(0);
  stat.generate(&h.gen);
  IR_StaticVarAddr ir(dest.get(), &stat);
  ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
  h.finish();
  auto data = h.generate();
  // sequence: ldr x0, [pc, #4] ; .quad ; sub x0, x0, x22 (offset)
  // execute with offset = 0: result = the literal value.
  size_t lit = h.literal_offset(data);
  u64 fake_addr = 0x12345678;
  memcpy(data.data() + lit, &fake_addr, 8);
  u64 r = h.execute_instrs(data, 4, 0, 0, 0, 0);
  EXPECT_EQ(r, fake_addr);
  // a LINK_PTR entry must exist in the link table.
  EXPECT_TRUE(has_link_kind(h.gen.generate_data_v3(&h.ts).link_tables.at(0), 5));
#endif
}

TEST(ARM64IRSymbols, StaticVarLoadFloat) {
#if defined(__aarch64__)
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  auto dst = make_reg(0, RegClass::FLOAT, TypeSpec("float"));
  h.assign(0, V0, 2);
  StaticFloat stat(1.5f, 0);
  stat.generate(&h.gen);
  IR_StaticVarLoad ir(dst.get(), &stat);
  ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
  h.finish();
  auto data = h.generate();
  size_t lit = h.literal_offset(data);
  float f = 42.25f;
  u64 fake_addr = (u64)&f;  // the loader would patch the literal with the address
  memcpy(data.data() + lit, &fake_addr, 8);
  // Swap the generated ret with an umov that captures the loaded value, and
  // put a ret right after it, before the static data:
  //   [load][umov][ret][statics]  (the statics are never executed).
  u32 ret_word = 0xD65F03C0u;
  for (size_t i = 4; i + 4 < lit; i += 4) {
    if (read_word(data, i) == ret_word) {
      u32 umov = 0x4E083C00u;  // umov x0, v0.d[0]
      memcpy(data.data() + i, &umov, 4);
      memcpy(data.data() + i + 4, &ret_word, 4);
      break;
    }
  }
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(0x10000);
  t.append_bytes(data.data() + 4, (int)(data.size() - 4));
  t.emit_return();
  u64 r = t.execute(0, 0, 0, 0);
  u32 out = (u32)r;
  float fout;
  memcpy(&fout, &out, 4);
  EXPECT_FLOAT_EQ(fout, 42.25f);
  EXPECT_TRUE(has_link_kind(h.gen.generate_data_v3(&h.ts).link_tables.at(0), 5));
#endif
}

TEST(ARM64IRSymbols, LinkTableHasPointerKind) {
  // StaticVarAddr produces a LINK_PTR entry in the link table.
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  auto lit = h.gen.add_static_to_seg(0);
  h.gen.get_static_data(lit).assign(8, 0);
  auto target = h.gen.add_static_to_seg(0);
  h.gen.get_static_data(target).assign(4, 0);
  h.gen.link_static_pointer_to_data(lit, 0, target, 0);
  // generate (needs at least one instruction? no - empty function tolerated now)
  auto obj = h.gen.generate_data_v3(&h.ts);
  EXPECT_TRUE(has_link_kind(obj.link_tables.at(0), 5));
}
