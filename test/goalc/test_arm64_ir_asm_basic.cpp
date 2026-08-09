#include <array>
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

struct IRHarness {
  ObjectGenerator gen{GameVersion::Jak2, InstructionSet::ARM64};
  TypeSystem ts;
  FunctionDebugInfo dbg;
  FunctionRecord func;
  AllocationResult allocs;

  IRHarness() {
    ts.add_builtin_types(GameVersion::Jak2);
    dbg.name = "asm-basic-test";
    func = gen.add_function_to_seg(0, &dbg);
  }

  void assign(int id, Register reg, int n_ir) {
    std::vector<bool> live(n_ir, true);
    std::vector<Assignment> ass(n_ir);
    for (auto& a : ass) {
      a.kind = Assignment::Kind::REGISTER;
      a.reg = reg;
    }
    if (allocs.ass_as_ranges.size() <= size_t(id)) {
      std::vector<bool> dummy_live(1, true);
      std::vector<Assignment> dummy_ass(1);
      allocs.ass_as_ranges.resize(id + 1, AssignmentRange(0, dummy_live, dummy_ass));
    }
    allocs.ass_as_ranges[id] = AssignmentRange(0, live, ass);
  }

  void finish() {
    IR_Record ir = gen.add_ir(func);
    gen.add_instr(Instruction(IGen::ARM64::ret()), ir);
  }

  std::vector<u8> generate() { return gen.generate_data_v3(&ts).segment_data.at(0); }
};

[[maybe_unused]] std::unique_ptr<RegVal> make_reg(int id,
                                                  RegClass cls,
                                                  const TypeSpec& ts,
                                                  Register rlet = Register()) {
  IRegister ireg;
  ireg.id = id;
  ireg.reg_class = cls;
  auto result = std::make_unique<RegVal>(ireg, ts);
  if (rlet != Register()) {
    result->set_rlet_constraint(rlet);
  }
  return result;
}

[[maybe_unused]] u64 execute_gpr(const std::vector<u8>& data,
                                 Register capture,
                                 std::array<u64, 4> args) {
#if defined(__aarch64__)
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(512);
  t.append_bytes(data.data() + 4, int(data.size() - 4));
  t.emit(mov_gpr64_gpr64(t.generator(), X0, capture));
  t.emit_return();
  return t.execute(args[0], args[1], args[2], args[3]);
#else
  (void)data;
  (void)capture;
  (void)args;
  return 0;
#endif
}

[[maybe_unused]] u64 execute_with_st(const std::vector<u8>& data, u64 st, std::array<u64, 4> args) {
#if defined(__aarch64__)
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(512);
  t.emit(mov_gpr64_gpr64(t.generator(), X21, X2));
  t.append_bytes(data.data() + 4, int(data.size() - 4));
  t.emit_return();
  return t.execute(args[0], args[1], st, 0);
#else
  (void)data;
  (void)st;
  (void)args;
  return 0;
#endif
}

[[maybe_unused]] bool link_contains(const std::vector<u8>& link, const std::string& name) {
  if (name.empty()) {
    return false;
  }
  for (size_t i = 0; i + name.size() <= link.size(); i++) {
    if (memcmp(link.data() + i, name.data(), name.size()) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(ARM64IRAsmBasic, RetLeafAndFpuCompatibilityOps) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
#else
  // .ret is a real ARM64 RET and must return directly to the CodeTester ABI
  // wrapper, without requiring a synthetic epilogue.
  IRHarness leaf;
  IR_Record leaf_ir = leaf.gen.add_ir(leaf.func);
  IR_AsmRet ret(true);
  ret.do_codegen_arm64(&leaf.gen, leaf.allocs, leaf_ir);
  auto leaf_data = leaf.generate();
  CodeTester leaf_t(InstructionSet::ARM64);
  leaf_t.init_code_buffer(64);
  leaf_t.append_bytes(leaf_data.data() + 4, int(leaf_data.size() - 4));
  EXPECT_EQ(leaf_t.execute(0x1122334455667788ull, 0, 0, 0), 0x1122334455667788ull);

  IRHarness compat;
  IR_Record nop_ir = compat.gen.add_ir(compat.func);
  IR_AsmFNop nop;
  nop.do_codegen_arm64(&compat.gen, compat.allocs, nop_ir);
  IR_Record wait_ir = compat.gen.add_ir(compat.func);
  IR_AsmFWait wait;
  wait.do_codegen_arm64(&compat.gen, compat.allocs, wait_ir);
  EXPECT_EQ(execute_gpr(compat.generate(), X0, {0xAABBCCDDEEFF0011ull, 0, 0, 0}),
            0xAABBCCDDEEFF0011ull);
#endif
}

TEST(ARM64IRAsmBasic, PushPopNestedPreservesValuesAndAlignment) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
#else
  IRHarness h;
  auto first = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
  auto second = make_reg(1, RegClass::GPR_64, TypeSpec("int"));
  auto out_first = make_reg(2, RegClass::GPR_64, TypeSpec("int"));
  auto out_second = make_reg(3, RegClass::GPR_64, TypeSpec("int"));
  h.assign(0, X0, 4);
  h.assign(1, X1, 4);
  h.assign(2, X2, 4);
  h.assign(3, X3, 4);

  IR_Record ir0 = h.gen.add_ir(h.func);
  IR_AsmPush push0(true, first.get());
  push0.do_codegen_arm64(&h.gen, h.allocs, ir0);
  IR_Record ir1 = h.gen.add_ir(h.func);
  IR_AsmPush push1(true, second.get());
  push1.do_codegen_arm64(&h.gen, h.allocs, ir1);
  IR_Record ir2 = h.gen.add_ir(h.func);
  IR_AsmPop pop0(true, out_first.get());
  pop0.do_codegen_arm64(&h.gen, h.allocs, ir2);
  IR_Record ir3 = h.gen.add_ir(h.func);
  IR_AsmPop pop1(true, out_second.get());
  pop1.do_codegen_arm64(&h.gen, h.allocs, ir3);
  // Capture both popped values as their sum.  The first pop must see second,
  // and the second pop must see first; each individual operation reserves a
  // 16-byte slot, so SP is aligned throughout the nested sequence.
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(512);
  auto data = h.generate();
  t.append_bytes(data.data() + 4, int(data.size() - 4));
  t.emit(add_gpr64_gpr64(t.generator(), X2, X3));
  t.emit(mov_gpr64_gpr64(t.generator(), X0, X2));
  t.emit_return();
  EXPECT_EQ(t.execute(0x1111222233334444ull, 0x5555666677778888ull, 0, 0), 0x66668888AAAACCCCull);

  // The generated push/pop must not use platform-reserved X18 as an implicit
  // scratch register; the ARM64 emitter uses only SP and the explicit GPR.
  EXPECT_EQ(h.gen.get_stats().moves_eliminated, 0);
#endif
}

TEST(ARM64IRAsmBasic, AddSubAliasesAndNoColorRegisters) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
#else
  IRHarness h;
  auto dst = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
  auto src = make_reg(1, RegClass::GPR_64, TypeSpec("int"));
  h.assign(0, X0, 2);
  h.assign(1, X1, 2);
  IR_Record add_ir = h.gen.add_ir(h.func);
  IR_AsmAdd add(true, dst.get(), src.get());
  add.do_codegen_arm64(&h.gen, h.allocs, add_ir);
  IR_Record sub_ir = h.gen.add_ir(h.func);
  IR_AsmSub sub(true, dst.get(), src.get());
  sub.do_codegen_arm64(&h.gen, h.allocs, sub_ir);
  // (lhs + rhs) - rhs = lhs, including 64-bit wraparound.
  EXPECT_EQ(execute_gpr(h.generate(), X0, {0xFFFFFFFFFFFFFFF0ull, 0x123456789ull, 0, 0}),
            0xFFFFFFFFFFFFFFF0ull);

  IRHarness alias;
  auto same = make_reg(0, RegClass::GPR_64, TypeSpec("int"), X2);
  IR_Record alias_ir = alias.gen.add_ir(alias.func);
  IR_AsmAdd alias_add(false, same.get(), same.get());
  alias_add.do_codegen_arm64(&alias.gen, alias.allocs, alias_ir);
  EXPECT_EQ(execute_gpr(alias.generate(), X2, {0, 0, 0x0102030405060708ull, 0}),
            0x020406080A0C0E10ull);
#endif
}

TEST(ARM64IRAsmBasic, RegSetGprSimdAndCalleeSavedRegister) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
#else
  IRHarness gpr;
  auto src = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
  auto dst = make_reg(1, RegClass::GPR_64, TypeSpec("int"));
  gpr.assign(0, X0, 1);
  gpr.assign(1, X1, 1);
  IR_Record gpr_ir = gpr.gen.add_ir(gpr.func);
  IR_RegSetAsm mov(true, dst.get(), src.get());
  mov.do_codegen_arm64(&gpr.gen, gpr.allocs, gpr_ir);
  EXPECT_EQ(execute_gpr(gpr.generate(), X1, {0x0123456789ABCDEFull, 0, 0, 0}),
            0x0123456789ABCDEFull);

  IRHarness simd;
  auto vsrc = make_reg(0, RegClass::VECTOR_FLOAT, TypeSpec("vector"));
  auto vdst = make_reg(1, RegClass::VECTOR_FLOAT, TypeSpec("vector"));
  simd.assign(0, V0, 1);
  simd.assign(1, V1, 1);
  IR_Record simd_ir = simd.gen.add_ir(simd.func);
  IR_RegSetAsm vmov(true, vdst.get(), vsrc.get());
  vmov.do_codegen_arm64(&simd.gen, simd.allocs, simd_ir);
  auto simd_data = simd.generate();
  CodeTester simd_t(InstructionSet::ARM64);
  simd_t.init_code_buffer(512);
  simd_t.emit(ins_vf_d_gpr(V0, 0, X0));
  simd_t.emit(ins_vf_d_gpr(V0, 1, X1));
  simd_t.append_bytes(simd_data.data() + 4, int(simd_data.size() - 4));
  simd_t.emit(umov_gpr64_vf_d(X0, V1, 0));
  simd_t.emit_return();
  EXPECT_EQ(simd_t.execute(0x1122334455667788ull, 0x99AABBCCDDEEFF00ull, 0, 0),
            0x1122334455667788ull);

  // X19 is callee-saved on the Apple ARM64 ABI.  The asm push/pop pair must
  // round-trip it without using X18 or disturbing the caller's value.
  IRHarness saved;
  auto saved_reg = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
  saved.assign(0, X19, 2);
  IR_Record saved_ir = saved.gen.add_ir(saved.func);
  IR_AsmPush save(true, saved_reg.get());
  save.do_codegen_arm64(&saved.gen, saved.allocs, saved_ir);
  IR_Record restore_ir = saved.gen.add_ir(saved.func);
  IR_AsmPop restore(true, saved_reg.get());
  restore.do_codegen_arm64(&saved.gen, saved.allocs, restore_ir);
  auto saved_data = saved.generate();
  CodeTester saved_t(InstructionSet::ARM64);
  saved_t.init_code_buffer(512);
  saved_t.emit(mov_gpr64_gpr64(saved_t.generator(), X19, X0));
  saved_t.append_bytes(saved_data.data() + 4, int(saved_data.size() - 4));
  saved_t.emit(mov_gpr64_gpr64(saved_t.generator(), X0, X19));
  saved_t.emit_return();
  EXPECT_EQ(saved_t.execute(0xCAFEBABE12345678ull, 0, 0, 0), 0xCAFEBABE12345678ull);
#endif
}

TEST(ARM64IRAsmBasic, GetSymbolValueSignedUnsignedAndDeferredMissingSymbol) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
#else
  for (bool sext : {false, true}) {
    IRHarness h;
    auto dst = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
    h.assign(0, X0, 1);
    IR_Record ir = h.gen.add_ir(h.func);
    IR_GetSymbolValueAsm load(true, dst.get(), "the-var", sext);
    load.do_codegen_arm64(&h.gen, h.allocs, ir);
    // The ARM symbol literal is static data appended after the function; keep
    // execution from falling through into it.
    h.finish();
    auto data = h.generate();
    u32 value = 0x80000000u;
    u64 fake_symbol_offset = 0;
    memcpy(data.data() + data.size() - 8, &fake_symbol_offset, sizeof(fake_symbol_offset));
    u32 storage = value;
    u64 result = execute_with_st(data, (u64)&storage, {0, 0, 0, 0});
    EXPECT_EQ(result, sext ? u64(s64(s32(value))) : u64(value));
    EXPECT_TRUE(link_contains(h.gen.generate_data_v3(&h.ts).link_tables.at(0), "the-var"));
  }

  // Symbol existence is resolved by the GOAL runtime linker, not by codegen.
  // A missing name must still produce an explicit symbol link; silently
  // treating it as #f would corrupt the generated program.
  IRHarness missing;
  auto dst = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
  missing.assign(0, X0, 1);
  IR_Record missing_ir = missing.gen.add_ir(missing.func);
  IR_GetSymbolValueAsm load_missing(true, dst.get(), "not-present-at-link", false);
  load_missing.do_codegen_arm64(&missing.gen, missing.allocs, missing_ir);
  auto missing_obj = missing.gen.generate_data_v3(&missing.ts);
  EXPECT_TRUE(link_contains(missing_obj.link_tables.at(0), "not-present-at-link"));
#endif
}
