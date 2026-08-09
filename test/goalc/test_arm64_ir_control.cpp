#include <cstring>
#include <memory>

#include "common/type_system/TypeSystem.h"

#include "goalc/compiler/IR.h"
#include "goalc/compiler/Label.h"
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
    dbg.name = "ctrl-test";
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

  // append a ret instruction (epilogue role) before statics.
  void finish() {
    IR_Record irr = gen.add_ir(func);
    gen.add_instr(Instruction(IGen::ARM64::ret()), irr);
  }

  std::vector<u8> generate() { return gen.generate_data_v3(&ts).segment_data.at(0); }
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

TEST(ARM64IRControl, GotoForward) {
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  Label label(nullptr, 1);  // destination IR id 1
  IR_GotoLabel ir(&label);
  ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
  IR_Record ir1 = h.gen.add_ir(h.func);
  h.gen.add_instr(Instruction(nop()), ir1);
  h.finish();
  auto data = h.generate();
  // [tag 4][B at 4][nop at 8][ret at 12] -> B targets 8: imm26 = 1
  EXPECT_EQ(read_word(data, 4), 0x14000000u | 1u);
}

TEST(ARM64IRControl, GotoBackward) {
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  h.gen.add_instr(Instruction(nop()), ir0);
  IR_Record ir1 = h.gen.add_ir(h.func);
  Label label(nullptr, 0);  // destination IR id 0 (the nop)
  IR_GotoLabel ir(&label);
  ir.do_codegen_arm64(&h.gen, h.allocs, ir1);
  h.finish();
  auto data = h.generate();
  // [tag 4][nop 4][B 8][ret 12] -> B targets 4: imm26 = -1
  EXPECT_EQ(read_word(data, 8) & 0x3FFFFFF, 0x3FFFFFFu);
}

TEST(ARM64IRControl, ConditionalBranchEqual) {
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  auto a = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
  auto b = make_reg(1, RegClass::GPR_64, TypeSpec("int"));
  h.assign(0, X0, 2);
  h.assign(1, X1, 2);
  Condition cond;
  cond.kind = ConditionKind::EQUAL;
  cond.a = a.get();
  cond.b = b.get();
  cond.is_signed = false;
  cond.is_float = false;
  Label label(nullptr, 1);
  IR_ConditionalBranch ir(cond, label);
  ir.mark_as_resolved();
  ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
  IR_Record ir1 = h.gen.add_ir(h.func);
  h.gen.add_instr(Instruction(nop()), ir1);
  h.finish();
  auto data = h.generate();
  // [tag 4][cmp 4][b.eq 8][nop 12][ret 16] -> b.eq at 8 targets 12: imm19 = 1
  u32 beq = read_word(data, 8);
  EXPECT_EQ(beq & 0xFF00001F, 0x54000000u);  // B.EQ with imm19
  EXPECT_EQ((beq >> 5) & 0x7FFFF, 1u);
}

TEST(ARM64IRControl, ConditionalBranchSignedLess) {
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  auto a = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
  auto b = make_reg(1, RegClass::GPR_64, TypeSpec("int"));
  h.assign(0, X0, 2);
  h.assign(1, X1, 2);
  Condition cond;
  cond.kind = ConditionKind::LT;
  cond.a = a.get();
  cond.b = b.get();
  cond.is_signed = true;
  cond.is_float = false;
  Label label(nullptr, 1);
  IR_ConditionalBranch ir(cond, label);
  ir.mark_as_resolved();
  ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
  IR_Record ir1 = h.gen.add_ir(h.func);
  h.gen.add_instr(Instruction(nop()), ir1);
  h.finish();
  auto data = h.generate();
  u32 blt = read_word(data, 8);
  // B.LT cond = 1011
  EXPECT_EQ(blt & 0xF, 0b1011);
}

TEST(ARM64IRControl, ConditionalBranchFloatEqual) {
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  auto a = make_reg(0, RegClass::VECTOR_FLOAT, TypeSpec("float"));
  auto b = make_reg(1, RegClass::VECTOR_FLOAT, TypeSpec("float"));
  h.assign(0, V0, 2);
  h.assign(1, V1, 2);
  Condition cond;
  cond.kind = ConditionKind::EQUAL;
  cond.a = a.get();
  cond.b = b.get();
  cond.is_signed = false;
  cond.is_float = true;
  Label label(nullptr, 1);
  IR_ConditionalBranch ir(cond, label);
  ir.mark_as_resolved();
  ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
  IR_Record ir1 = h.gen.add_ir(h.func);
  h.gen.add_instr(Instruction(nop()), ir1);
  h.finish();
  auto data = h.generate();

  u8 expected_bytes[4] = {};
  auto expected_cmp = cmp_f32_f32(V0, V1);
  expected_cmp.emit(expected_bytes);
  u32 expected_cmp_word;
  memcpy(&expected_cmp_word, expected_bytes, sizeof(expected_cmp_word));
  EXPECT_EQ(read_word(data, 4), expected_cmp_word);
  EXPECT_EQ(read_word(data, 8) & 0xF, 0u);  // B.EQ
}

TEST(ARM64IRControl, ConditionalBranchUnsignedGreaterEqual) {
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  auto a = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
  auto b = make_reg(1, RegClass::GPR_64, TypeSpec("int"));
  h.assign(0, X0, 2);
  h.assign(1, X1, 2);
  Condition cond;
  cond.kind = ConditionKind::GEQ;
  cond.a = a.get();
  cond.b = b.get();
  cond.is_signed = false;
  cond.is_float = false;
  Label label(nullptr, 1);
  IR_ConditionalBranch ir(cond, label);
  ir.mark_as_resolved();
  ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
  IR_Record ir1 = h.gen.add_ir(h.func);
  h.gen.add_instr(Instruction(nop()), ir1);
  h.finish();
  auto data = h.generate();
  EXPECT_EQ(read_word(data, 8) & 0xF, 0b0010);  // B.CS/HS
}

TEST(ARM64IRControl, NestedLoopLinks) {
  IRHarness h;
  auto a = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
  auto b = make_reg(1, RegClass::GPR_64, TypeSpec("int"));
  h.assign(0, X0, 4);
  h.assign(1, X1, 4);

  IR_Record ir0 = h.gen.add_ir(h.func);
  Condition cond;
  cond.kind = ConditionKind::EQUAL;
  cond.a = a.get();
  cond.b = b.get();
  cond.is_signed = false;
  cond.is_float = false;
  Label exit_label(nullptr, 3);
  IR_ConditionalBranch branch(cond, exit_label);
  branch.mark_as_resolved();
  branch.do_codegen_arm64(&h.gen, h.allocs, ir0);

  IR_Record ir1 = h.gen.add_ir(h.func);
  Label back_label(nullptr, 0);
  IR_GotoLabel back(&back_label);
  back.do_codegen_arm64(&h.gen, h.allocs, ir1);

  IR_Record ir2 = h.gen.add_ir(h.func);
  h.gen.add_instr(Instruction(nop()), ir2);
  IR_Record ir3 = h.gen.add_ir(h.func);
  h.gen.add_instr(Instruction(nop()), ir3);
  h.finish();
  auto data = h.generate();
  // cmp/branch at 4/8 targets ir3 at 20: imm19 = 3.
  EXPECT_EQ((read_word(data, 8) >> 5) & 0x7FFFF, 3u);
  // B at 12 targets ir0 at 4: imm26 = -2.
  EXPECT_EQ(read_word(data, 12) & 0x3FFFFFF, 0x3FFFFFEu);
}

TEST(ARM64IRControl, FunctionCallArgumentArity) {
  auto function = make_reg(0, RegClass::GPR_64, TypeSpec("function"));
  auto result = make_reg(1, RegClass::GPR_64, TypeSpec("int"));

  for (int arity : {0, 1, 8}) {
    std::vector<std::unique_ptr<RegVal>> owned;
    std::vector<RegVal*> args;
    std::vector<Register> arg_regs;
    for (int i = 0; i < arity; i++) {
      owned.push_back(make_reg(2 + i, RegClass::GPR_64, TypeSpec("int")));
      args.push_back(owned.back().get());
      arg_regs.push_back(Register(X0 + i));
    }
    IR_FunctionCall call(function.get(), result.get(), args, arg_regs, X0, InstructionSet::ARM64);
    std::vector<IRegConstraint> constraints;
    EXPECT_NO_THROW(call.add_constraints(&constraints, 0));
    EXPECT_EQ(constraints.size(), size_t(arity + 1));  // args plus return register
  }

  std::vector<std::unique_ptr<RegVal>> owned;
  std::vector<RegVal*> nine_args;
  std::vector<Register> eight_arg_regs;
  for (int i = 0; i < 9; i++) {
    owned.push_back(make_reg(2 + i, RegClass::GPR_64, TypeSpec("int")));
    nine_args.push_back(owned.back().get());
    if (i < 8) {
      eight_arg_regs.push_back(Register(X0 + i));
    }
  }
  IR_FunctionCall too_many(function.get(), result.get(), nine_args, eight_arg_regs, X0,
                           InstructionSet::ARM64);
  std::vector<IRegConstraint> constraints;
  EXPECT_THROW(too_many.add_constraints(&constraints, 0), std::out_of_range);
}

TEST(ARM64IRControl, MissingJumpTargetRejected) {
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  Label missing(nullptr, 99);
  IR_GotoLabel jump(&missing);
  jump.do_codegen_arm64(&h.gen, h.allocs, ir0);
  h.finish();
  EXPECT_THROW(h.generate(), std::out_of_range);
}

TEST(ARM64IRControl, FunctionCallSequence) {
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  auto fregv = make_reg(0, RegClass::GPR_64, TypeSpec("function"));
  auto retv = make_reg(1, RegClass::GPR_64, TypeSpec("int"));
  h.assign(0, X9, 2);
  h.assign(1, X0, 2);
  std::vector<RegVal*> args;
  std::vector<Register> arg_regs;
  IR_FunctionCall ir(fregv.get(), retv.get(), args, arg_regs, X0);
  ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
  IR_Record ir1 = h.gen.add_ir(h.func);
  h.gen.add_instr(Instruction(nop()), ir1);
  h.finish();
  auto data = h.generate();
  // [tag][add x9, x9, x22][blr x9][nop][ret]
  EXPECT_EQ(read_word(data, 4), 0x8B160129u);  // add x9, x9, x22
  EXPECT_EQ(read_word(data, 8), 0xD63F0120u);  // blr x9
}

TEST(ARM64IRControl, JumpReg) {
  IRHarness h;
  IR_Record ir0 = h.gen.add_ir(h.func);
  auto src = make_reg(0, RegClass::GPR_64, TypeSpec("function"));
  h.assign(0, X7, 1);
  IR_JumpReg ir(true, src.get());
  ir.do_codegen_arm64(&h.gen, h.allocs, ir0);
  h.finish();
  auto data = h.generate();
  // [tag][br x7][ret]
  EXPECT_EQ(read_word(data, 4), 0xD61F00E0u);  // br x7
}
