#include <array>
#include <cstring>
#include <memory>
#include <unistd.h>

#include "common/type_system/TypeSystem.h"

#include "goalc/compiler/IR.h"
#include "goalc/debugger/DebugInfo.h"
#include "goalc/emitter/CodeTester.h"
#include "goalc/emitter/IGen.h"
#include "goalc/emitter/IGenARM64.h"
#include "goalc/emitter/ObjectGenerator.h"
#include "gtest/gtest.h"
#include <sys/mman.h>

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
    dbg.name = "memory-test";
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

  void assign_stack(int id, int slot, int n_ir) {
    std::vector<bool> live(n_ir, true);
    std::vector<Assignment> ass(n_ir);
    for (auto& a : ass) {
      a.kind = Assignment::Kind::STACK;
      a.stack_slot = slot;
    }
    if (allocs.ass_as_ranges.size() <= size_t(id)) {
      std::vector<bool> dummy_live(1, true);
      std::vector<Assignment> dummy_ass(1);
      allocs.ass_as_ranges.resize(id + 1, AssignmentRange(0, dummy_live, dummy_ass));
    }
    allocs.ass_as_ranges[id] = AssignmentRange(0, live, ass);
    allocs.stack_slots_for_spills = std::max(allocs.stack_slots_for_spills, slot + 1);
  }

  std::vector<u8> generate() { return gen.generate_data_v3(&ts).segment_data.at(0); }
};

std::unique_ptr<RegVal> make_reg(int id, RegClass cls, const TypeSpec& ts) {
  IRegister ireg;
  ireg.id = id;
  ireg.reg_class = cls;
  return std::make_unique<RegVal>(ireg, ts);
}

struct GuardedPage {
  size_t page_size = size_t(sysconf(_SC_PAGESIZE));
  u8* mapping = nullptr;

  GuardedPage() {
    mapping = static_cast<u8*>(
        mmap(nullptr, page_size * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (mapping != MAP_FAILED) {
      if (mprotect(mapping + page_size, page_size, PROT_NONE) != 0) {
        munmap(mapping, page_size * 2);
        mapping = nullptr;
      }
    } else {
      mapping = nullptr;
    }
  }

  ~GuardedPage() {
    if (mapping) {
      munmap(mapping, page_size * 2);
    }
  }

  bool valid() const { return mapping != nullptr; }
  u8* readable_end() const { return mapping + page_size; }
};

u64 execute_load(const std::vector<u8>& data, u64 base, u64 offset) {
#if defined(__aarch64__)
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(512);
  t.emit(push_gpr64(t.generator(), X22));
  t.emit(mov_gpr64_gpr64(t.generator(), X22, X2));
  t.append_bytes(data.data() + 4, int(data.size() - 4));
  t.emit(pop_gpr64(t.generator(), X22));
  t.emit_return();
  return t.execute(base, 0, offset, 0);
#else
  (void)data;
  (void)base;
  (void)offset;
  return 0;
#endif
}

u64 execute_load_base_x1(const std::vector<u8>& data, u64 base, u64 offset) {
#if defined(__aarch64__)
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(512);
  t.emit(push_gpr64(t.generator(), X22));
  t.emit(mov_gpr64_gpr64(t.generator(), X22, X2));
  t.append_bytes(data.data() + 4, int(data.size() - 4));
  t.emit(pop_gpr64(t.generator(), X22));
  t.emit_return();
  return t.execute(0, base, offset, 0);
#else
  (void)data;
  (void)base;
  (void)offset;
  return 0;
#endif
}

void execute_store(const std::vector<u8>& data, u64 base, u64 value, u64 offset) {
#if defined(__aarch64__)
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(512);
  t.emit(push_gpr64(t.generator(), X22));
  t.emit(mov_gpr64_gpr64(t.generator(), X22, X2));
  t.append_bytes(data.data() + 4, int(data.size() - 4));
  t.emit(pop_gpr64(t.generator(), X22));
  t.emit_return();
  (void)t.execute(base, value, offset, 0);
#else
  (void)data;
  (void)base;
  (void)value;
  (void)offset;
#endif
}

u64 execute_vector_load(const std::vector<u8>& data, u64 base, u64 offset) {
#if defined(__aarch64__)
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(512);
  t.emit(push_gpr64(t.generator(), X22));
  t.emit(mov_gpr64_gpr64(t.generator(), X22, X2));
  t.append_bytes(data.data() + 4, int(data.size() - 4));
  t.emit(umov_gpr64_vf_d(X0, V0, 0));
  t.emit(pop_gpr64(t.generator(), X22));
  t.emit_return();
  return t.execute(base, 0, offset, 0);
#else
  (void)data;
  (void)base;
  (void)offset;
  return 0;
#endif
}

void execute_vector_store(const std::vector<u8>& data, u64 base, u64 low, u64 high, u64 offset) {
#if defined(__aarch64__)
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(512);
  t.emit(push_gpr64(t.generator(), X22));
  t.emit(ins_vf_d_gpr(V0, 0, X1));
  t.emit(ins_vf_d_gpr(V0, 1, X2));
  t.emit(mov_gpr64_gpr64(t.generator(), X22, X3));
  t.append_bytes(data.data() + 4, int(data.size() - 4));
  t.emit(pop_gpr64(t.generator(), X22));
  t.emit_return();
  (void)t.execute(base, low, high, offset);
#else
  (void)data;
  (void)base;
  (void)low;
  (void)high;
  (void)offset;
#endif
}

u64 execute_stack_address(const std::vector<u8>& data, int expected_offset) {
#if defined(__aarch64__)
  CodeTester t(InstructionSet::ARM64);
  t.init_code_buffer(512);
  t.emit(push_gpr64(t.generator(), X22));
  t.emit(mov_gpr64_gpr64(t.generator(), X22, X2));
  t.emit(lea_reg_plus_off(t.generator(), X1, SP, 0));
  t.append_bytes(data.data() + 4, int(data.size() - 4));
  t.emit(sub_gpr64_gpr64(t.generator(), X0, X1));
  t.emit(pop_gpr64(t.generator(), X22));
  t.emit_return();
  EXPECT_EQ(t.execute(0, 0, 0, 0), u64(expected_offset));
  return u64(expected_offset);
#else
  (void)data;
  (void)expected_offset;
  return 0;
#endif
}

}  // namespace

TEST(ARM64IRMemory, GprLoadsGuardedAndExtended) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
#else
  struct Case {
    int size;
    u64 raw;
    u64 sign_extended;
  };
  const std::array<Case, 4> cases = {{{1, 0x80, 0xffffffffffffff80ull},
                                      {2, 0x8001, 0xffffffffffff8001ull},
                                      {4, 0x80000001, 0xffffffff80000001ull},
                                      {8, 0x8000000000000001ull, 0x8000000000000001ull}}};

  for (const auto& c : cases) {
    for (bool sign_extend : {false, true}) {
      GuardedPage page;
      ASSERT_TRUE(page.valid());
      u8* target = page.readable_end() - c.size;
      memcpy(target, &c.raw, size_t(c.size));

      IRHarness h;
      auto dest = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
      auto base = make_reg(1, RegClass::GPR_64, TypeSpec("pointer"));
      h.assign(0, X0, 1);
      h.assign(1, X0, 1);
      MemLoadInfo info;
      info.reg = RegClass::GPR_64;
      info.size = c.size;
      info.sign_extend = sign_extend;
      IR_LoadConstOffset load(dest.get(), 0, base.get(), info);
      IR_Record ir = h.gen.add_ir(h.func);
      load.do_codegen_arm64(&h.gen, h.allocs, ir);
      auto data = h.generate();
      u64 result = execute_load(data, u64(target), 0);
      EXPECT_EQ(result, sign_extend ? c.sign_extended : c.raw)
          << "size=" << c.size << " sign_extend=" << sign_extend;
    }
  }
#endif
}

TEST(ARM64IRMemory, GprStoresGuarded) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
#else
  const std::array<int, 4> sizes = {1, 2, 4, 8};
  const u64 value = 0x8877665544332211ull;
  for (int size : sizes) {
    GuardedPage page;
    ASSERT_TRUE(page.valid());
    u8* target = page.readable_end() - size;
    memset(target, 0, size_t(size));

    IRHarness h;
    auto value_reg = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
    auto base = make_reg(1, RegClass::GPR_64, TypeSpec("pointer"));
    h.assign(0, X1, 1);
    h.assign(1, X0, 1);
    IR_StoreConstOffset store(value_reg.get(), 0, base.get(), size);
    IR_Record ir = h.gen.add_ir(h.func);
    store.do_codegen_arm64(&h.gen, h.allocs, ir);
    auto data = h.generate();
    execute_store(data, u64(target), value, 0);

    u64 actual = 0;
    memcpy(&actual, target, size_t(size));
    u64 expected = value & ((size == 8) ? ~0ull : ((1ull << (size * 8)) - 1));
    EXPECT_EQ(actual, expected) << "size=" << size;
  }
#endif
}

TEST(ARM64IRMemory, VectorLoadStoreGuarded) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
#else
  const std::array<u8, 16> expected = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                       0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
  u64 low, high;
  memcpy(&low, expected.data(), sizeof(low));
  memcpy(&high, expected.data() + sizeof(low), sizeof(high));

  GuardedPage store_page;
  ASSERT_TRUE(store_page.valid());
  u8* store_target = store_page.readable_end() - expected.size();
  memset(store_target, 0, expected.size());
  IRHarness store_h;
  auto value = make_reg(0, RegClass::VECTOR_FLOAT, TypeSpec("vector"));
  auto base = make_reg(1, RegClass::GPR_64, TypeSpec("pointer"));
  store_h.assign(0, V0, 1);
  store_h.assign(1, X0, 1);
  IR_StoreConstOffset store(value.get(), 0, base.get(), 16);
  IR_Record store_ir = store_h.gen.add_ir(store_h.func);
  store.do_codegen_arm64(&store_h.gen, store_h.allocs, store_ir);
  execute_vector_store(store_h.generate(), u64(store_target), low, high, 0);
  EXPECT_EQ(memcmp(store_target, expected.data(), expected.size()), 0);

  GuardedPage load_page;
  ASSERT_TRUE(load_page.valid());
  u8* load_target = load_page.readable_end() - expected.size();
  memcpy(load_target, expected.data(), expected.size());
  IRHarness load_h;
  auto dest = make_reg(0, RegClass::VECTOR_FLOAT, TypeSpec("vector"));
  auto load_base = make_reg(1, RegClass::GPR_64, TypeSpec("pointer"));
  load_h.assign(0, V0, 1);
  load_h.assign(1, X0, 1);
  MemLoadInfo info;
  info.reg = RegClass::VECTOR_FLOAT;
  info.size = 16;
  IR_LoadConstOffset load(dest.get(), 0, load_base.get(), info);
  IR_Record load_ir = load_h.gen.add_ir(load_h.func);
  load.do_codegen_arm64(&load_h.gen, load_h.allocs, load_ir);
  EXPECT_EQ(execute_vector_load(load_h.generate(), u64(load_target), 0), low);
#endif
}

TEST(ARM64IRMemory, OffsetsAndBaseEqualsDestination) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
#else
  std::array<u8, 16384> storage{};
  u8* target = storage.data() + 8192;
  const std::array<int, 5> offsets = {0, 127, -128, 4096, -4096};
  for (int offset : offsets) {
    u64 expected = 0x1122334455667788ull;
    memcpy(target, &expected, sizeof(expected));
    IRHarness h;
    auto dest = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
    auto base = make_reg(1, RegClass::GPR_64, TypeSpec("pointer"));
    h.assign(0, X0, 1);
    h.assign(1, X1, 1);
    MemLoadInfo info;
    info.reg = RegClass::GPR_64;
    info.size = 8;
    IR_LoadConstOffset load(dest.get(), offset, base.get(), info);
    IR_Record ir = h.gen.add_ir(h.func);
    load.do_codegen_arm64(&h.gen, h.allocs, ir);
    EXPECT_EQ(execute_load_base_x1(h.generate(), u64(target - offset), 0), expected)
        << "offset=" << offset;
  }

  IRHarness large_h;
  auto large_dest = make_reg(0, RegClass::GPR_64, TypeSpec("int"));
  auto large_base = make_reg(1, RegClass::GPR_64, TypeSpec("pointer"));
  large_h.assign(0, X0, 1);
  large_h.assign(1, X0, 1);
  MemLoadInfo large_info;
  large_info.reg = RegClass::GPR_64;
  large_info.size = 8;
  IR_LoadConstOffset large_load(large_dest.get(), INT32_MAX, large_base.get(), large_info);
  IR_Record large_ir = large_h.gen.add_ir(large_h.func);
  large_load.do_codegen_arm64(&large_h.gen, large_h.allocs, large_ir);
  EXPECT_GT(large_h.generate().size(), size_t(8));
#endif
}

TEST(ARM64IRStack, StackAddressesPreserveSP) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "ARM64 JIT execution requires an ARM64 host";
#else
  for (int slot : {0, 2}) {
    IRHarness h;
    h.allocs.stack_slots_for_vars = 3;
    auto dest = make_reg(0, RegClass::GPR_64, TypeSpec("pointer"));
    h.assign(0, X0, 1);
    IR_GetStackAddr addr(dest.get(), slot);
    IR_Record ir = h.gen.add_ir(h.func);
    addr.do_codegen_arm64(&h.gen, h.allocs, ir);
    execute_stack_address(h.generate(), slot * 8);
  }

  IRHarness h;
  auto dest = make_reg(0, RegClass::GPR_64, TypeSpec("pointer"));
  auto source = make_reg(1, RegClass::GPR_64, TypeSpec("int"));
  source->force_on_stack();
  h.assign(0, X0, 1);
  h.assign_stack(1, 1, 1);
  IR_RegValAddr addr(dest.get(), source.get());
  IR_Record ir = h.gen.add_ir(h.func);
  addr.do_codegen_arm64(&h.gen, h.allocs, ir);
  execute_stack_address(h.generate(), 8);
#endif
}

TEST(ARM64IRMemory, UnsupportedShapeRejected) {
  IRHarness h;
  auto dest = make_reg(0, RegClass::VECTOR_FLOAT, TypeSpec("vector"));
  auto base = make_reg(1, RegClass::GPR_64, TypeSpec("pointer"));
  h.assign(0, V0, 1);
  h.assign(1, X0, 1);
  MemLoadInfo info;
  info.reg = RegClass::VECTOR_FLOAT;
  info.size = 4;
  IR_LoadConstOffset load(dest.get(), 0, base.get(), info);
  IR_Record ir = h.gen.add_ir(h.func);
  EXPECT_THROW(load.do_codegen_arm64(&h.gen, h.allocs, ir), std::runtime_error);
}
