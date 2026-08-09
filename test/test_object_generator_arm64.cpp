#include <cstring>
#include <memory>

#include "common/type_system/TypeSystem.h"

#include "goalc/debugger/DebugInfo.h"
#include "goalc/emitter/IGen.h"
#include "goalc/emitter/IGenARM64.h"
#include "goalc/emitter/ObjectGenerator.h"

#include "gtest/gtest.h"

using namespace emitter;
using namespace emitter::IGen;
using namespace emitter::IGen::ARM64;

namespace {

u32 read_word(const std::vector<u8>& data, size_t off) {
  u32 w;
  memcpy(&w, data.data() + off, 4);
  return w;
}

// Build an ARM64 object generator with one function: [B target, NOP] plus a
// within-function jump link from the branch to the NOP.
std::unique_ptr<ObjectGenerator> make_arm64_gen() {
  return std::make_unique<ObjectGenerator>(GameVersion::Jak2, InstructionSet::ARM64);
}

}  // namespace

TEST(ObjectGeneratorARM64, SameFunctionJumpForward) {
  ObjectGenerator gen(GameVersion::Jak2, InstructionSet::ARM64);
  TypeSystem ts;
  ts.add_builtin_types(GameVersion::Jak2);
  FunctionDebugInfo dbg;
  dbg.name = "test-func";
  FunctionRecord func = gen.add_function_to_seg(0, &dbg);
  IR_Record ir0 = gen.add_ir(func);
  InstructionRecord jump = gen.add_instr(Instruction(jmp_imm()), ir0);
  IR_Record ir1 = gen.add_ir(func);
  gen.add_instr(Instruction(nop()), ir1);
  gen.link_instruction_jump(jump, ir1);
  auto obj = gen.generate_data_v3(&ts);
  const auto& data = obj.segment_data.at(0);
  // layout: [0-3] type tag, [4-7] B, [8-11] NOP.
  // The B at offset 4 targets offset 8 -> imm26 = (8-4)/4 = 1.
  EXPECT_EQ(read_word(data, 4), 0x14000000u | 1u);
  // nop at 8
  EXPECT_EQ(read_word(data, 8), 0xD503201Fu);
}

TEST(ObjectGeneratorARM64, SameFunctionJumpBackward) {
  ObjectGenerator gen(GameVersion::Jak2, InstructionSet::ARM64);
  TypeSystem ts;
  ts.add_builtin_types(GameVersion::Jak2);
  FunctionDebugInfo dbg;
  dbg.name = "back-func";
  FunctionRecord func = gen.add_function_to_seg(0, &dbg);
  IR_Record ir0 = gen.add_ir(func);
  gen.add_instr(Instruction(nop()), ir0);
  IR_Record ir1 = gen.add_ir(func);
  InstructionRecord jump = gen.add_instr(Instruction(jmp_imm()), ir1);
  IR_Record ir2 = gen.add_ir(func);
  gen.add_instr(Instruction(nop()), ir2);
  gen.link_instruction_jump(jump, ir0);
  auto obj = gen.generate_data_v3(&ts);
  const auto& data = obj.segment_data.at(0);
  // [0-3] type tag; [4-7] nop; [8-11] B; [12-15] nop.
  // B at 8 targets 4 -> imm26 = (4-8)/4 = -1.
  EXPECT_EQ(read_word(data, 8) & 0x3FFFFFF, 0x3FFFFFFu);  // -1 in 26 bits
}

TEST(ObjectGeneratorARM64, SameFunctionJumpZeroOffset) {
  ObjectGenerator gen(GameVersion::Jak2, InstructionSet::ARM64);
  TypeSystem ts;
  ts.add_builtin_types(GameVersion::Jak2);
  FunctionDebugInfo dbg;
  dbg.name = "zero-func";
  FunctionRecord func = gen.add_function_to_seg(0, &dbg);
  IR_Record ir0 = gen.add_ir(func);
  InstructionRecord jump = gen.add_instr(Instruction(jmp_imm()), ir0);
  gen.link_instruction_jump(jump, ir0);
  auto obj = gen.generate_data_v3(&ts);
  EXPECT_EQ(read_word(obj.segment_data.at(0), 4), 0x14000000u);
}

TEST(ObjectGeneratorARM64, OutOfRangeJumpRejected) {
  ObjectGenerator gen(GameVersion::Jak2, InstructionSet::ARM64);
  TypeSystem ts;
  ts.add_builtin_types(GameVersion::Jak2);
  FunctionDebugInfo dbg;
  dbg.name = "far-func";
  FunctionRecord func = gen.add_function_to_seg(0, &dbg);
  IR_Record ir0 = gen.add_ir(func);
  InstructionRecord jump = gen.add_instr(Instruction(jmp_imm()), ir0);
  for (int i = 0; i < (1 << 25); i++) {
    IR_Record irc = gen.add_ir(func);
    gen.add_instr(Instruction(nop()), irc);
  }
  // target the LAST nop: ir_id (1 << 25) -> 2^27 bytes away (out of range).
  gen.link_instruction_jump(jump, gen.get_future_ir_record_in_same_func(ir0, 1 << 25));
  EXPECT_THROW(gen.generate_data_v3(&ts), std::runtime_error);
}

TEST(ObjectGeneratorARM64, X86RipLinkRejected) {
  ObjectGenerator gen(GameVersion::Jak2, InstructionSet::ARM64);
  TypeSystem ts;
  ts.add_builtin_types(GameVersion::Jak2);
  FunctionDebugInfo dbg;
  dbg.name = "rip-func";
  FunctionRecord func = gen.add_function_to_seg(0, &dbg);
  IR_Record ir0 = gen.add_ir(func);
  gen.add_instr(Instruction(jmp_imm()), ir0);
  StaticRecord stat = gen.add_static_to_seg(0);
  // static_addr produces the ADRP+ADD form on ARM64; the rip-style link that
  // used to be emitted for x86 must be rejected for ARM64 objects.
  InstructionRecord rec{0, func.func_id, ir0.ir_id, 0};
  gen.link_instruction_static(rec, stat, 0);
  EXPECT_THROW(gen.generate_data_v3(&ts), std::runtime_error);
}
