#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#include "common/goos/Reader.h"
#include "common/type_system/TypeSystem.h"

#include "goalc/compiler/CodeGenerator.h"
#include "goalc/compiler/Env.h"
#include "goalc/compiler/IR.h"
#include "goalc/debugger/DebugInfo.h"
#include "goalc/emitter/CodeTester.h"
#include "goalc/emitter/InstructionSet.h"
#include "goalc/regalloc/Allocator.h"
#include "goalc/regalloc/Allocator_v2.h"
#include "gtest/gtest.h"

using namespace emitter;

namespace {

extern "C" u64 native_add17(u64 value, u64, u64, u64) {
  return value + 17;
}

struct GeneratedFunction {
  TypeSystem ts;
  DebugInfo debug{"arm64-function-test"};
  std::unique_ptr<GlobalEnv> global = std::make_unique<GlobalEnv>();
  FileEnv* file = nullptr;
  FunctionEnv* function = nullptr;
  std::string name;

  explicit GeneratedFunction(std::string function_name) : name(std::move(function_name)) {
    ts.add_builtin_types(GameVersion::Jak2);
    file = global->add_file("arm64-function-test");
  }

  void add_function(std::unique_ptr<FunctionEnv> fn) {
    function = fn.get();
    file->add_function(std::move(fn));
  }

  void color() {
    AllocationInput input;
    input.instr_set = InstructionSet::ARM64;
    input.max_vars = function->max_vars();
    input.constraints = function->constraints();
    input.function_name = function->name();
    input.is_asm_function = function->is_asm_func;
    for (const auto& reg_val : function->reg_vals()) {
      if (reg_val->forced_on_stack()) {
        input.force_on_stack_regs.insert(reg_val->ireg().id);
      }
    }
    for (const auto& ir : function->code()) {
      input.instructions.push_back(ir->to_rai(InstructionSet::ARM64));
    }

    auto allocation = allocate_registers_v2(input);
    if (!allocation.ok) {
      allocation = allocate_registers(input);
    }
    ASSERT_TRUE(allocation.ok) << "register allocation failed for " << function->name();
    function->set_allocations(std::move(allocation));
  }

  const std::vector<u8>& generate() {
    color();
    CodeGenerator generator(file, &debug, GameVersion::Jak2, InstructionSet::ARM64);
    generator.run(&ts);
    return debug.function_by_name(function->name()).generated_code;
  }

  u64 execute() {
    const auto& bytes = generate();
    CodeTester tester(InstructionSet::ARM64);
    tester.init_code_buffer(4096);
    tester.append_bytes(bytes.data(), int(bytes.size()));
    return tester.execute(0, 0, 0, 0);
  }

  u64 execute(u64 in0, u64 in1, u64 in2, u64 in3) {
    const auto& bytes = generate();
    CodeTester tester(InstructionSet::ARM64);
    tester.init_code_buffer(4096);
    tester.append_bytes(bytes.data(), int(bytes.size()));
    return tester.execute(in0, in1, in2, in3);
  }
};

std::unique_ptr<FunctionEnv> make_function(FileEnv* file,
                                           goos::Reader* reader,
                                           const std::string& name) {
  auto result = std::make_unique<FunctionEnv>(file, name, reader);
  result->set_segment(0);
  return result;
}

goos::Object empty_form() {
  return goos::Object();
}

}  // namespace

TEST(ARM64Function, GoalLeafUsesAppleFrameAndReturns) {
#if defined(__aarch64__)
  goos::Reader reader;
  GeneratedFunction generated("leaf");
  auto fn = make_function(generated.file, &reader, generated.name);
  auto value = fn->make_gpr(generated.ts.make_typespec("int"));
  auto ret = fn->make_gpr(generated.ts.make_typespec("int"));
  auto form = empty_form();
  fn->emit_ir<IR_LoadConstant64>(form, value, 0x1122334455667788ull);
  fn->emit_ir<IR_Return>(form, ret, value, X0);
  fn->finish();
  generated.add_function(std::move(fn));

  EXPECT_EQ(generated.execute(), 0x1122334455667788ull);
  ASSERT_TRUE(generated.debug.function_by_name(generated.name).stack_usage.has_value());
  EXPECT_EQ(*generated.debug.function_by_name(generated.name).stack_usage % 16, 0);
  EXPECT_EQ(*generated.debug.function_by_name(generated.name).stack_usage, 16);
#else
  GTEST_SKIP() << "ARM64 function execution requires an ARM64 host";
#endif
}

TEST(ARM64Function, CalleeSavedGprSurvivesGeneratedFrame) {
#if defined(__aarch64__)
  goos::Reader reader;
  GeneratedFunction generated("saved-gpr");
  auto fn = make_function(generated.file, &reader, generated.name);
  auto value = fn->make_gpr(generated.ts.make_typespec("int"));
  auto ret = fn->make_gpr(generated.ts.make_typespec("int"));
  IRegConstraint force_x19;
  force_x19.ireg = value->ireg();
  force_x19.instr_idx = 0;
  force_x19.desired_register = X19;
  fn->constrain(force_x19);
  auto form = empty_form();
  fn->emit_ir<IR_LoadConstant64>(form, value, 0xCAFEBABE12345678ull);
  fn->emit_ir<IR_Return>(form, ret, value, X0);
  fn->finish();
  generated.add_function(std::move(fn));

  EXPECT_EQ(generated.execute(), 0xCAFEBABE12345678ull);
  EXPECT_EQ(*generated.debug.function_by_name(generated.name).stack_usage, 32);
#else
  GTEST_SKIP() << "ARM64 function execution requires an ARM64 host";
#endif
}

TEST(ARM64Function, CalleeSavedSimdSurvivesGeneratedFrame) {
#if defined(__aarch64__)
  goos::Reader reader;
  GeneratedFunction generated("saved-simd");
  auto fn = make_function(generated.file, &reader, generated.name);
  auto input = fn->make_gpr(generated.ts.make_typespec("int"));
  auto value = fn->make_ireg(generated.ts.make_typespec("int128"), RegClass::INT_128);
  auto ret = fn->make_ireg(generated.ts.make_typespec("int128"), RegClass::INT_128);
  IRegConstraint force_v8;
  force_v8.ireg = value->ireg();
  force_v8.instr_idx = 0;
  force_v8.desired_register = V8;
  fn->constrain(force_v8);
  auto form = empty_form();
  fn->emit_ir<IR_RegSet>(form, value, input);
  fn->emit_ir<IR_Return>(form, ret, value, V0);
  fn->finish();
  generated.add_function(std::move(fn));

  generated.color();
  CodeGenerator generator(generated.file, &generated.debug, GameVersion::Jak2,
                          InstructionSet::ARM64);
  generator.run(&generated.ts);
  const auto& bytes = generated.debug.function_by_name(generated.name).generated_code;
  CodeTester tester(InstructionSet::ARM64);
  tester.init_code_buffer(4096);
  tester.append_bytes(bytes.data(), int(bytes.size()));
  EXPECT_EQ(tester.execute(0x8877665544332211ull, 0, 0, 0), 0x8877665544332211ull);
  EXPECT_EQ(*generated.debug.function_by_name(generated.name).stack_usage, 32);
#else
  GTEST_SKIP() << "ARM64 function execution requires an ARM64 host";
#endif
}

TEST(ARM64Function, NonLeafCallPreservesLinkRegisterAndReturns) {
#if defined(__aarch64__)
  goos::Reader reader;
  GeneratedFunction generated("nonleaf");
  auto fn = make_function(generated.file, &reader, generated.name);
  auto offset = fn->make_gpr(generated.ts.make_typespec("int"));
  auto function = fn->make_gpr(generated.ts.make_typespec("int"));
  auto arg = fn->make_gpr(generated.ts.make_typespec("int"));
  auto call_result = fn->make_gpr(generated.ts.make_typespec("int"));
  auto ret = fn->make_gpr(generated.ts.make_typespec("int"));
  IRegConstraint offset_constraint;
  offset_constraint.ireg = offset->ireg();
  offset_constraint.instr_idx = 0;
  offset_constraint.desired_register = X22;
  fn->constrain(offset_constraint);
  IRegConstraint function_constraint;
  function_constraint.ireg = function->ireg();
  function_constraint.instr_idx = 2;
  function_constraint.desired_register = X9;
  fn->constrain(function_constraint);
  auto form = empty_form();
  fn->emit_ir<IR_LoadConstant64>(form, offset, 0);
  fn->emit_ir<IR_LoadConstant64>(form, function,
                                 static_cast<u64>(reinterpret_cast<uintptr_t>(&native_add17)));
  fn->emit_ir<IR_FunctionCall>(form, function, call_result, std::vector<RegVal*>{arg},
                               std::vector<Register>{X0}, X0, InstructionSet::ARM64);
  fn->emit_ir<IR_Return>(form, ret, call_result, X0);
  fn->finish();
  generated.add_function(std::move(fn));

  EXPECT_EQ(generated.execute(), 17u);
  EXPECT_EQ(*generated.debug.function_by_name(generated.name).stack_usage % 16, 0);
#else
  GTEST_SKIP() << "ARM64 function execution requires an ARM64 host";
#endif
}

TEST(ARM64Function, ForcedSpillUsesAlignedSpAreaAndExecutes) {
#if defined(__aarch64__)
  goos::Reader reader;
  GeneratedFunction generated("spill");
  auto fn = make_function(generated.file, &reader, generated.name);
  auto value = fn->make_gpr(generated.ts.make_typespec("int"));
  auto ret = fn->make_gpr(generated.ts.make_typespec("int"));
  value->force_on_stack();
  auto form = empty_form();
  fn->emit_ir<IR_LoadConstant64>(form, value, 0xA5A5A5A55A5A5A5Aull);
  fn->emit_ir<IR_Return>(form, ret, value, X0);
  fn->finish();
  generated.add_function(std::move(fn));

  EXPECT_EQ(generated.execute(), 0xA5A5A5A55A5A5A5Aull);
  EXPECT_EQ(*generated.debug.function_by_name(generated.name).stack_usage % 16, 0);
  EXPECT_GE(*generated.debug.function_by_name(generated.name).stack_usage, 32);
#else
  GTEST_SKIP() << "ARM64 function execution requires an ARM64 host";
#endif
}

TEST(ARM64Function, ForcedSimdSpillUsesAlignedSpAreaAndExecutes) {
#if defined(__aarch64__)
  goos::Reader reader;
  GeneratedFunction generated("simd-spill");
  auto fn = make_function(generated.file, &reader, generated.name);
  auto input = fn->make_gpr(generated.ts.make_typespec("int"));
  auto value = fn->make_ireg(generated.ts.make_typespec("int128"), RegClass::INT_128);
  auto ret = fn->make_ireg(generated.ts.make_typespec("int128"), RegClass::INT_128);
  value->force_on_stack();
  auto form = empty_form();
  fn->emit_ir<IR_RegSet>(form, value, input);
  fn->emit_ir<IR_Return>(form, ret, value, V0);
  fn->finish();
  generated.add_function(std::move(fn));

  EXPECT_EQ(generated.execute(0x0123456789ABCDEFull, 0, 0, 0), 0x0123456789ABCDEFull);
  EXPECT_EQ(*generated.debug.function_by_name(generated.name).stack_usage % 16, 0);
  EXPECT_GE(*generated.debug.function_by_name(generated.name).stack_usage, 32);
#else
  GTEST_SKIP() << "ARM64 function execution requires an ARM64 host";
#endif
}

TEST(ARM64Function, FloatReturnAbiRegisterRoundTrips) {
#if defined(__aarch64__)
  goos::Reader reader;
  GeneratedFunction generated("float-return");
  auto fn = make_function(generated.file, &reader, generated.name);
  auto input = fn->make_gpr(generated.ts.make_typespec("int"));
  auto float_value = fn->make_ireg(generated.ts.make_typespec("float"), RegClass::FLOAT);
  auto float_return = fn->make_ireg(generated.ts.make_typespec("float"), RegClass::FLOAT);
  auto gpr_value = fn->make_gpr(generated.ts.make_typespec("int"));
  auto ret = fn->make_gpr(generated.ts.make_typespec("int"));
  IRegConstraint input_constraint;
  input_constraint.ireg = input->ireg();
  input_constraint.instr_idx = 0;
  input_constraint.desired_register = X0;
  fn->constrain(input_constraint);
  auto form = empty_form();
  fn->emit_ir<IR_RegSet>(form, float_value, input);
  fn->emit_ir<IR_Return>(form, float_return, float_value, V0);
  fn->emit_ir<IR_RegSet>(form, gpr_value, float_return);
  fn->emit_ir<IR_Return>(form, ret, gpr_value, X0);
  fn->finish();
  generated.add_function(std::move(fn));

  EXPECT_EQ(generated.execute(0x3FC00000u, 0, 0, 0), 0x3FC00000u);
#else
  GTEST_SKIP() << "ARM64 function execution requires an ARM64 host";
#endif
}

TEST(ARM64Function, BranchesShareGeneratedEpilogue) {
#if defined(__aarch64__)
  goos::Reader reader;
  GeneratedFunction generated("branching");
  auto fn = make_function(generated.file, &reader, generated.name);
  auto input = fn->make_gpr(generated.ts.make_typespec("int"));
  auto zero = fn->make_gpr(generated.ts.make_typespec("int"));
  auto value = fn->make_gpr(generated.ts.make_typespec("int"));
  auto ret = fn->make_gpr(generated.ts.make_typespec("int"));
  IRegConstraint input_constraint;
  input_constraint.ireg = input->ireg();
  input_constraint.instr_idx = 0;
  input_constraint.desired_register = X0;
  fn->constrain(input_constraint);
  auto form = empty_form();
  fn->emit_ir<IR_LoadConstant64>(form, zero, 0);
  Condition condition;
  condition.kind = ConditionKind::EQUAL;
  condition.a = input;
  condition.b = zero;
  condition.is_signed = false;
  condition.is_float = false;
  Label equal_label(nullptr, 4);
  IR_ConditionalBranch branch(condition, equal_label);
  branch.mark_as_resolved();
  fn->emit(form, std::make_unique<IR_ConditionalBranch>(branch), fn.get());
  fn->emit_ir<IR_LoadConstant64>(form, value, 111);
  Label end_label(nullptr, 6);
  fn->emit(form, std::make_unique<IR_GotoLabel>(&end_label), fn.get());
  fn->emit_ir<IR_LoadConstant64>(form, value, 222);
  fn->emit(form, std::make_unique<IR_GotoLabel>(&end_label), fn.get());
  fn->emit_ir<IR_Return>(form, ret, value, X0);
  fn->finish();
  generated.add_function(std::move(fn));

  const auto& bytes = generated.generate();
  CodeTester tester(InstructionSet::ARM64);
  tester.init_code_buffer(4096);
  tester.append_bytes(bytes.data(), int(bytes.size()));
  EXPECT_EQ(tester.execute(0, 0, 0, 0), 222u);
  EXPECT_EQ(tester.execute(1, 0, 0, 0), 111u);
#else
  GTEST_SKIP() << "ARM64 function execution requires an ARM64 host";
#endif
}

TEST(ARM64Function, AsmFunctionKeepsExplicitReturnContract) {
#if defined(__aarch64__)
  goos::Reader reader;
  GeneratedFunction generated("asm-leaf");
  auto fn = make_function(generated.file, &reader, generated.name);
  fn->is_asm_func = true;
  fn->asm_func_return_type = generated.ts.make_typespec("none");
  auto form = empty_form();
  fn->emit_ir<IR_AsmRet>(form, false);
  fn->finish();
  generated.add_function(std::move(fn));

  EXPECT_EQ(generated.execute(), 0u);
#else
  GTEST_SKIP() << "ARM64 function execution requires an ARM64 host";
#endif
}

TEST(ARM64Function, AsmFunctionRejectsImplicitSavedRegisterUse) {
#if defined(__aarch64__)
  goos::Reader reader;
  GeneratedFunction generated("asm-invalid-saved");
  auto fn = make_function(generated.file, &reader, generated.name);
  fn->is_asm_func = true;
  AllocationResult allocation;
  allocation.ok = true;
  allocation.used_saved_regs.push_back(X19);
  allocation.stack_ops.resize(1);
  fn->set_allocations(std::move(allocation));
  auto form = empty_form();
  fn->emit_ir<IR_AsmRet>(form, false);
  fn->finish();
  generated.add_function(std::move(fn));

  EXPECT_THROW(
      {
        CodeGenerator generator(generated.file, &generated.debug, GameVersion::Jak2,
                                InstructionSet::ARM64);
        generator.run(&generated.ts);
      },
      std::runtime_error);
#else
  GTEST_SKIP() << "ARM64 function execution requires an ARM64 host";
#endif
}
