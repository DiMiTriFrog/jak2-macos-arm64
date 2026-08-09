#include <cstring>
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

// Minimal harness: drive a single IR's ARM64 codegen directly with hand-built
// register assignments, then generate the object and (optionally) execute the
// emitted instruction bytes with a trailing `ret`.
struct IRHarness {
  ObjectGenerator gen{GameVersion::Jak2, InstructionSet::ARM64};
  TypeSystem ts;
  FunctionDebugInfo dbg;
  FunctionRecord func;

  IRHarness() {
    ts.add_builtin_types(GameVersion::Jak2);
    dbg.name = "ir-test";
    func = gen.add_function_to_seg(0, &dbg);
  }

  // Build an AllocationResult where ireg `id` is assigned to `reg` for all
  // instructions in [0, n_ir].
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

  std::vector<u8> generate() { return gen.generate_data_v3(&ts).segment_data.at(0); }

  // Execute the instructions from the generated object (skip the type tag)
  // followed by a return, as a bare C-ABI function.
  u64 execute_instrs(const std::vector<u8>& data, size_t start, u64 in0, u64 in1, u64 in2,
                     u64 in3) {
#ifdef __aarch64__
    CodeTester t(InstructionSet::ARM64);
    t.init_code_buffer(256);
    t.append_bytes(data.data() + start, (int)(data.size() - start));
    t.emit_return();
    return t.execute(in0, in1, in2, in3);
#else
    return 0;
#endif
  }
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

}  // namespace

TEST(ARM64IRCore, LoadConstant64) {
#if defined(__aarch64__)
  u64 values[] = {0, 1, -1ull, 0x12345678ull, 0x123456789ABCDEFull, 0x8000000000000000ull,
                  0xFFFFFFFFFFFFFFFFull};
  for (auto value : values) {
    IRHarness h;
    IR_Record ir0 = h.gen.add_ir(h.func);
    auto dest = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
    h.assign(0, X0, 1);
    IR_LoadConstant64 ir(dest.get(), value);
    ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
    auto data = h.generate();
    // type tag [0-3], mov at 4
    u64 r = h.execute_instrs(data, 4, 0, 0, 0, 0);
    EXPECT_EQ(r, value) << "value " << std::hex << value;
  }
#endif
}

TEST(ARM64IRCore, RegSetGprToGpr) {
#if defined(__aarch64__)
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  auto src = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
  auto dst = make_reg(1, RegClass::GPR_64, TypeSpec("int"));
  h.assign(0, X1, 1);
  h.assign(1, X0, 1);
  IR_RegSet ir(dst.get(), src.get());
  ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
  auto data = h.generate();
  u64 r = h.execute_instrs(data, 4, 0, 0x1122334455667788ull, 0, 0);
  EXPECT_EQ(r, 0x1122334455667788ull);
#endif
}

TEST(ARM64IRCore, RegSetSimdToSimd) {
#if defined(__aarch64__)
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  auto src = make_reg(0, RegClass::VECTOR_FLOAT, TypeSpec("vector"));
  auto dst = make_reg(1, RegClass::VECTOR_FLOAT, TypeSpec("vector"));
  h.assign(0, V0, 1);
  h.assign(1, V1, 1);
  IR_RegSet ir(dst.get(), src.get());
  ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
  auto data = h.generate();
  // mov v1.16b, v0.16b ; ret ; execute with v0 = {in0, in1}
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(256);
  // load the input vector into v0 before the generated move runs.
  t.emit(ins_vf_d_gpr(V0, 0, X0));
  t.emit(ins_vf_d_gpr(V0, 1, X1));
  size_t start = 4;
  t.append_bytes(data.data() + start, (int)(data.size() - start));
  t.emit(umov_gpr64_vf_d(X0, V1, 0));
  t.emit_return();
  u64 r = t.execute(0x1122334455667788ull, 0x99AABBCCDDEEFF00ull, 0, 0);
  EXPECT_EQ(r, 0x1122334455667788ull);
#endif
}

TEST(ARM64IRCore, RegSetSourceEqualsDest) {
#if defined(__aarch64__)
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  auto src = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
  h.assign(0, X1, 1);
  IR_RegSet ir(src.get(), src.get());
  ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
  auto data = h.generate();
  // source == dest: the move is eliminated, only the type tag remains.
  EXPECT_EQ(data.size(), 4);
#endif
}

TEST(ARM64IRCore, ReturnMovesValueToReturnReg) {
#if defined(__aarch64__)
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  auto value = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
  auto ret = make_reg(1, RegClass::GPR_64, TypeSpec("int"));
  h.assign(0, X2, 1);
  h.assign(1, X0, 1);
  IR_Return ir(ret.get(), value.get(), X0);
  ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
  auto data = h.generate();
  // mov x0, x2 ; ret ; input x2 = value
  u64 r = h.execute_instrs(data, 4, 0, 0, 0xDEADBEEFCAFEBABEull, 0);
  EXPECT_EQ(r, 0xDEADBEEFCAFEBABEull);
#endif
}

TEST(ARM64IRCore, NullEmitsNothing) {
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  IR_Null ir;
  ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
  auto data = h.generate();
  EXPECT_EQ(data.size(), 4);  // only the type tag
}

TEST(ARM64IRCore, ValueResetEmitsNothing) {
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  auto a = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
  std::vector<RegVal*> args = {a.get()};
  IR_ValueReset ir(args);
  ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
  auto data = h.generate();
  EXPECT_EQ(data.size(), 4);  // only the type tag
}

TEST(ARM64IRCore, NopEmitsNop) {
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  IR_Nop ir;
  ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
  auto data = h.generate();
  EXPECT_EQ(data.size(), 8);
  EXPECT_EQ(read_word(data, 4), 0xD503201Fu);  // nop
}
