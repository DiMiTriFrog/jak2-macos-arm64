#include <unordered_set>

#include "goalc/emitter/Register.h"

#include "gtest/gtest.h"

namespace {

// Collect every GPR register reachable from the ARM64 GPR allocation orders.
std::unordered_set<int> arm64_allocatable_gpr_ids() {
  const auto& ri = emitter::get_register_info(emitter::InstructionSet::ARM64);
  std::unordered_set<int> result;
  for (const auto& r : ri.get_gpr_alloc_order()) {
    result.insert(r.id());
  }
  for (const auto& r : ri.get_gpr_temp_alloc_order()) {
    result.insert(r.id());
  }
  for (const auto& r : ri.get_gpr_spill_alloc_order()) {
    result.insert(r.id());
  }
  return result;
}

bool is_reserved_arm64_gpr(int id) {
  return id == emitter::X16 || id == emitter::X17 || id == emitter::X18 ||
         id == emitter::X29 || id == emitter::X30 || id == emitter::SP;
}

}  // namespace

TEST(RegisterInfoARM64, GprNamesAndPolicy) {
  const auto& ri = emitter::get_register_info(emitter::InstructionSet::ARM64);
  EXPECT_EQ(ri.get_info(emitter::X0).name, "x0");
  EXPECT_EQ(ri.get_info(emitter::X18).name, "x18");
  EXPECT_EQ(ri.get_info(emitter::X30).name, "x30");
  EXPECT_EQ(ri.get_info(emitter::SP).name, "sp");
  // x0-x15 are caller-saved temps.
  EXPECT_TRUE(ri.get_info(emitter::X0).temp());
  EXPECT_TRUE(ri.get_info(emitter::X15).temp());
  // x16/x17/x18 are special/reserved, never temps and never saved.
  EXPECT_TRUE(ri.get_info(emitter::X18).special);
  EXPECT_FALSE(ri.get_info(emitter::X18).saved);
  EXPECT_FALSE(ri.get_info(emitter::X18).temp());
  // x19-x28 are callee-saved.
  EXPECT_TRUE(ri.get_info(emitter::X19).saved);
  EXPECT_TRUE(ri.get_info(emitter::X28).saved);
  // x29 FP and x30 LR are special (not allocatable).
  EXPECT_TRUE(ri.get_info(emitter::X29).special);
  EXPECT_TRUE(ri.get_info(emitter::X30).special);
}

TEST(RegisterInfoARM64, SimdNamesAndPolicy) {
  const auto& ri = emitter::get_register_info(emitter::InstructionSet::ARM64);
  EXPECT_EQ(ri.get_simd_info(emitter::V0).name, "v0");
  EXPECT_EQ(ri.get_simd_info(emitter::V31).name, "v31");
  // v0-v7 caller-saved, v8-v15 callee-saved, v16-v31 caller-saved.
  EXPECT_TRUE(ri.get_simd_info(emitter::V0).temp());
  EXPECT_TRUE(ri.get_simd_info(emitter::V8).saved);
  EXPECT_TRUE(ri.get_simd_info(emitter::V15).saved);
  EXPECT_TRUE(ri.get_simd_info(emitter::V16).temp());
  EXPECT_TRUE(ri.get_simd_info(emitter::V31).temp());
}

TEST(RegisterInfoARM64, ArgumentsAndReturn) {
  const auto& ri = emitter::get_register_info(emitter::InstructionSet::ARM64);
  for (int i = 0; i < 8; i++) {
    EXPECT_EQ(ri.get_gpr_arg_reg(i).id(), emitter::X0 + i);
    EXPECT_EQ(ri.get_xmm_arg_reg(i).id(), emitter::V0 + i);
  }
  EXPECT_EQ(ri.get_gpr_ret_reg().id(), emitter::X0);
  EXPECT_EQ(ri.get_xmm_ret_reg().id(), emitter::V0);
}

TEST(RegisterInfoARM64, SavedRegisters) {
  const auto& ri = emitter::get_register_info(emitter::InstructionSet::ARM64);
  EXPECT_EQ(ri.get_saved_gpr(0).id(), emitter::X19);
  EXPECT_EQ(ri.get_saved_gpr(6).id(), emitter::X28);
  EXPECT_EQ(ri.get_saved_xmm(0).id(), emitter::V8);
  EXPECT_EQ(ri.get_saved_xmm(7).id(), emitter::V15);
  // x29 is ABI callee-saved but deliberately not in the GOAL save/restore set.
  for (auto r : ri.get_all_saved()) {
    EXPECT_NE(r.id(), emitter::X29);
    EXPECT_NE(r.id(), emitter::X30);
    EXPECT_NE(r.id(), emitter::SP);
  }
}

TEST(RegisterInfoARM64, SavedSimdExcludedFromSuspendableAllocation) {
  const auto& ri = emitter::get_register_info(emitter::InstructionSet::ARM64);
  for (int saved_simd = emitter::V8; saved_simd <= emitter::V15; saved_simd++) {
    for (const auto reg : ri.get_xmm_alloc_order()) {
      EXPECT_NE(reg.id(), saved_simd);
    }
    for (const auto reg : ri.get_xmm_spill_alloc_order()) {
      EXPECT_NE(reg.id(), saved_simd);
    }
  }
  // The ABI description remains intact: these registers are reserved for
  // explicit scalar kernel context handling, not repurposed as caller-saved.
  EXPECT_TRUE(ri.get_simd_info(emitter::V8).saved);
  EXPECT_TRUE(ri.get_simd_info(emitter::V15).saved);
}

TEST(RegisterInfoARM64, ReservedRegistersExcludedFromAllocation) {
  const auto& ri = emitter::get_register_info(emitter::InstructionSet::ARM64);
  EXPECT_FALSE(ri.get_info(emitter::X18).temp());
  EXPECT_FALSE(ri.get_info(emitter::X16).temp());
  EXPECT_FALSE(ri.get_info(emitter::X17).temp());
  const auto allocatable_gpr = arm64_allocatable_gpr_ids();
  EXPECT_EQ(allocatable_gpr.count(emitter::X18), 0);
  EXPECT_EQ(allocatable_gpr.count(emitter::X16), 0);
  EXPECT_EQ(allocatable_gpr.count(emitter::X17), 0);
  EXPECT_EQ(allocatable_gpr.count(emitter::X29), 0);
  EXPECT_EQ(allocatable_gpr.count(emitter::X30), 0);
  EXPECT_EQ(allocatable_gpr.count(emitter::SP), 0);
}

TEST(RegisterInfoARM64, X18NeverInSyntheticAllocations) {
  // Simulate 10,000 greedy GPR allocations drawn uniformly from the GPR
  // allocation orders; the reserved GPRs must never be selected.  SIMD
  // registers share the id space (v29/v30/v31 use ids 29/30/31) so only the
  // GPR selections are checked for reserved status.
  const auto& ri = emitter::get_register_info(emitter::InstructionSet::ARM64);
  const auto& gpr_order = ri.get_gpr_alloc_order();
  unsigned seed = 12345;
  for (int iter = 0; iter < 10000; iter++) {
    int gpr_idx = (int)((seed = seed * 1103515245 + 12345) % gpr_order.size());
    int gpr_id = gpr_order.at(gpr_idx).id();
    EXPECT_FALSE(is_reserved_arm64_gpr(gpr_id)) << "reserved GPR id " << gpr_id
                                                << " selected in iteration " << iter;
  }
}

TEST(RegisterInfoARM64, NoDuplicateRegistersInOrders) {
  const auto& ri = emitter::get_register_info(emitter::InstructionSet::ARM64);
  std::vector<const std::vector<emitter::Register>*> orders = {
      &ri.get_gpr_alloc_order(),
      &ri.get_xmm_alloc_order(),
      &ri.get_gpr_temp_alloc_order(),
      &ri.get_xmm_temp_alloc_order(),
      &ri.get_gpr_spill_alloc_order(),
      &ri.get_xmm_spill_alloc_order(),
  };
  for (auto order : orders) {
    std::unordered_set<int> seen;
    for (auto r : *order) {
      EXPECT_EQ(seen.count(r.id()), 0) << "duplicate register id " << r.id();
      seen.insert(r.id());
    }
  }
  std::unordered_set<int> saved_seen;
  for (auto r : ri.get_all_saved()) {
    EXPECT_EQ(saved_seen.count(r.id()), 0) << "duplicate saved register id " << r.id();
    saved_seen.insert(r.id());
  }
}

TEST(RegisterInfoX86, X86ModelUnchanged) {
  const auto& ri = emitter::get_register_info(emitter::InstructionSet::X86);
  // x86 convention must be byte-for-byte identical to the baseline.
  EXPECT_EQ(ri.get_gpr_arg_reg(0).id(), emitter::RDI);
  EXPECT_EQ(ri.get_gpr_arg_reg(1).id(), emitter::RSI);
  EXPECT_EQ(ri.get_gpr_arg_reg(2).id(), emitter::RDX);
  EXPECT_EQ(ri.get_gpr_arg_reg(3).id(), emitter::RCX);
  EXPECT_EQ(ri.get_xmm_arg_reg(0).id(), emitter::XMM1);
  EXPECT_EQ(ri.get_gpr_ret_reg().id(), emitter::RAX);
  EXPECT_EQ(ri.get_xmm_ret_reg().id(), emitter::XMM0);
  EXPECT_EQ(ri.get_process_reg().id(), emitter::R13);
  EXPECT_EQ(ri.get_st_reg().id(), emitter::R14);
  EXPECT_EQ(ri.get_offset_reg().id(), emitter::R15);
  EXPECT_EQ(ri.get_saved_gpr(0).id(), emitter::RBX);
  EXPECT_EQ(ri.get_all_saved().size(), 13);
  // x86 has no reserved-register concept in the same way; RAX..R15 and XMM0..15
  // are all reachable from some order.
  EXPECT_GT(ri.get_gpr_alloc_order().size(), 0);
}
