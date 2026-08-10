/*!
 * @file CodeGenerator.cpp
 * Generate object files from a FileEnv using an emitter::ObjectGenerator.
 * Populates a DebugInfo.
 * Currently owns the logic for emitting the function prologues/epilogues and stack spill ops.
 */

#include "CodeGenerator.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

#include "IR.h"

#include "goalc/debugger/DebugInfo.h"
#include "goalc/emitter/IGen.h"

#include "fmt/format.h"

using namespace emitter;

namespace {

void record_local_variables(const FunctionEnv* func, FunctionDebugInfo* debug) {
  const auto& allocations = func->allocations();
  if (!allocations.ok || allocations.ass_as_ranges.empty()) {
    return;
  }

  // ireg id -> source name, parameters first, then anything bound in a lexical scope
  std::unordered_map<int, std::pair<std::string, bool>> names;
  for (const auto& [symbol, reg_val] : func->params) {
    if (reg_val) {
      names[reg_val->ireg().id] = {symbol.name_ptr, true};
    }
  }
  for (const auto& env : func->child_envs()) {
    auto* lexical = dynamic_cast<LexicalEnv*>(env.get());
    if (!lexical) {
      continue;
    }
    for (const auto& [symbol, reg_val] : lexical->vars) {
      if (reg_val && names.find(reg_val->ireg().id) == names.end()) {
        names[reg_val->ireg().id] = {symbol.name_ptr, false};
      }
    }
  }

  if (names.empty()) {
    return;
  }

  const int instruction_count = int(func->code().size());

  for (const auto& [ireg_id, name_info] : names) {
    if (ireg_id < 0 || ireg_id >= int(allocations.ass_as_ranges.size()) ||
        ireg_id >= int(func->reg_vals().size())) {
      continue;
    }
    const auto& range = allocations.ass_as_ranges.at(ireg_id);

    LocalVariableDebugInfo local;
    local.name = name_info.first;
    local.is_parameter = name_info.second;
    local.type = func->reg_vals().at(ireg_id)->type();

    for (int instr = 0; instr < instruction_count; instr++) {
      if (!range.is_live_at_instr(instr)) {
        continue;
      }
      const auto& assignment = range.get(instr);
      if (!assignment.is_assigned()) {
        continue;
      }

      VariableLocation here;
      here.start_ir = instr;
      here.end_ir = instr;
      if (assignment.kind == Assignment::Kind::REGISTER) {
        here.kind = VariableLocation::Kind::REGISTER;
        here.reg = assignment.reg.id();
      } else if (assignment.kind == Assignment::Kind::STACK) {
        here.kind = VariableLocation::Kind::STACK;
        // spilled variables sit at rsp + slot * 8, matching how the spill ops address them
        here.stack_offset = allocations.get_slot_for_spill(assignment.stack_slot) * GPR_SIZE;
      } else {
        continue;
      }

      // extend the previous interval instead of starting a new one where nothing changed
      if (!local.locations.empty()) {
        auto& previous = local.locations.back();
        if (previous.end_ir == instr - 1 && previous.kind == here.kind &&
            previous.reg == here.reg && previous.stack_offset == here.stack_offset) {
          previous.end_ir = instr;
          continue;
        }
      }
      local.locations.push_back(here);
    }

    if (!local.locations.empty()) {
      debug->locals.push_back(std::move(local));
    }
  }

  // parameters first, then alphabetically
  std::sort(debug->locals.begin(), debug->locals.end(),
            [](const LocalVariableDebugInfo& a, const LocalVariableDebugInfo& b) {
              if (a.is_parameter != b.is_parameter) {
                return a.is_parameter;
              }
              return a.name < b.name;
            });
}

}  // namespace

CodeGenerator::CodeGenerator(FileEnv* env,
                             DebugInfo* debug_info,
                             GameVersion version,
                             InstructionSet instruction_set)
    : m_gen(version, instruction_set), m_fe(env), m_debug_info(debug_info) {}

/*!
 * Generate an object file.
 */
std::vector<u8> CodeGenerator::run(const TypeSystem* ts) {
  std::unordered_set<std::string> function_names;

  // first, add each function to the ObjectGenerator (but don't add any data)
  for (auto& f : m_fe->functions()) {
    if (function_names.find(f->name()) == function_names.end()) {
      function_names.insert(f->name());
    } else {
      printf("Failed to codegen, there are two functions with internal names [%s]\n",
             f->name().c_str());
      throw std::runtime_error("Failed to codegen.");
    }
    auto rec =
        m_gen.add_function_to_seg(f->segment, &m_debug_info->add_function(f->name(), m_fe->name()));
    for (auto& x : f->code_source()) {
      rec.debug->code_sources.push_back(x.heap_obj);
    }
    for (auto& x : f->code()) {
      rec.debug->ir_strings.push_back(x->print());
    }
    record_local_variables(f.get(), rec.debug);
  }

  // next, add all static objects.
  for (auto& static_obj : m_fe->statics()) {
    static_obj->generate(&m_gen);
  }

  // next, add instructions to functions
  for (size_t i = 0; i < m_fe->functions().size(); i++) {
    do_function(m_fe->functions().at(i).get(), i);
  }

  // generate a v3 object.
  return m_gen.generate_data_v3(ts).to_vector();
}

void CodeGenerator::do_function(FunctionEnv* env, int f_idx) {
  if (env->is_asm_func) {
    if (m_gen.instr_set() == InstructionSet::X86) {
      do_asm_function_x86(env, f_idx, env->asm_func_saved_regs);
    } else if (m_gen.instr_set() == InstructionSet::ARM64) {
      do_asm_function_arm64(env, f_idx, env->asm_func_saved_regs);
    } else {
      throw std::runtime_error("CodeGenerator::do_function, instruction set not supported");
    }
  } else {
    if (m_gen.instr_set() == InstructionSet::X86) {
      do_goal_function_x86(env, f_idx);
    } else if (m_gen.instr_set() == InstructionSet::ARM64) {
      do_goal_function_arm64(env, f_idx);
    } else {
      throw std::runtime_error("CodeGenerator::do_function, instruction set not supported");
    }
  }
}

/*!
 * Add instructions to the function, specified by index.
 * Generates prologues / epilogues.
 */
void CodeGenerator::do_goal_function_x86(FunctionEnv* env, int f_idx) {
  bool use_new_xmms = true;
  auto* debug = &m_debug_info->function_by_name(env->name());

  auto f_rec = m_gen.get_existing_function_record(f_idx);
  // todo, extra alignment settings

  auto& ri = emitter::get_register_info(m_gen.instr_set());
  const auto& allocs = env->alloc_result();

  // compute how much stack we will use
  int stack_offset = 0;

  // count how many xmm's we have to backup
  int n_xmm_backups = 0;
  for (auto& saved_reg : allocs.used_saved_regs) {
    if (saved_reg.is_xmm(m_gen.instr_set())) {
      n_xmm_backups++;
    }
  }

  // only for new xmms. if n == 0, we don't use this at all.
  int xmm_backup_stack_offset = 8 + XMM_SIZE * n_xmm_backups;

  if (use_new_xmms) {
    if (n_xmm_backups > 0) {
      // offset the stack
      stack_offset += xmm_backup_stack_offset;
      m_gen.add_instr_no_ir(f_rec, IGen::sub_gpr64_imm(m_gen, RSP, xmm_backup_stack_offset),
                            InstructionInfo::Kind::PROLOGUE);
      // back up xmms
      int i = 0;
      for (auto& saved_reg : allocs.used_saved_regs) {
        if (saved_reg.is_xmm(m_gen.instr_set())) {
          int offset = i * XMM_SIZE;
          m_gen.add_instr_no_ir(f_rec,
                                IGen::store128_xmm128_reg_offset(m_gen, RSP, saved_reg, offset),
                                InstructionInfo::Kind::PROLOGUE);
          i++;
        }
      }
    }
  } else {
    // back up xmms (currently not aligned)
    for (auto& saved_reg : allocs.used_saved_regs) {
      if (saved_reg.is_xmm(m_gen.instr_set())) {
        m_gen.add_instr_no_ir(f_rec, IGen::sub_gpr64_imm8s(m_gen, RSP, XMM_SIZE),
                              InstructionInfo::Kind::PROLOGUE);
        m_gen.add_instr_no_ir(f_rec, IGen::store128_gpr64_simd128(m_gen, RSP, saved_reg),
                              InstructionInfo::Kind::PROLOGUE);
        stack_offset += XMM_SIZE;
      }
    }
  }

  // back up gprs
  for (auto& saved_reg : allocs.used_saved_regs) {
    if (saved_reg.is_gpr(m_gen.instr_set())) {
      m_gen.add_instr_no_ir(f_rec, IGen::push_gpr64(m_gen, saved_reg),
                            InstructionInfo::Kind::PROLOGUE);
      stack_offset += GPR_SIZE;
    }
  }

  // do we include an extra push to get 8 more bytes to keep the stack aligned?
  bool bonus_push = false;

  // the offset to add directly to rsp for stack variables or spills (no push/pop)
  int manually_added_stack_offset =
      GPR_SIZE * (allocs.stack_slots_for_spills + allocs.stack_slots_for_vars);
  stack_offset += manually_added_stack_offset;

  // do we need to align or manually offset?
  if (manually_added_stack_offset || allocs.needs_aligned_stack_for_spills ||
      env->needs_aligned_stack()) {
    if (!(stack_offset & 15)) {
      if (manually_added_stack_offset) {
        // if we're already adding to rsp, just add 8 more.
        manually_added_stack_offset += 8;
      } else {
        // otherwise to an extra push, and remember so we can do an extra pop later on.
        bonus_push = true;
        m_gen.add_instr_no_ir(f_rec, IGen::push_gpr64(m_gen, ri.get_saved_gpr(0)),
                              InstructionInfo::Kind::PROLOGUE);
      }
      stack_offset += 8;
    }

    ASSERT(stack_offset & 15);

    // do manual stack offset.
    if (manually_added_stack_offset) {
      m_gen.add_instr_no_ir(f_rec, IGen::sub_gpr64_imm(m_gen, RSP, manually_added_stack_offset),
                            InstructionInfo::Kind::PROLOGUE);
    }
  }
  debug->stack_usage = stack_offset;

  // emit each IR into x86 instructions.
  for (int ir_idx = 0; ir_idx < int(env->code().size()); ir_idx++) {
    auto& ir = env->code().at(ir_idx);
    // start of IR
    auto i_rec = m_gen.add_ir(f_rec);

    // load anything off the stack that was spilled and is needed.
    auto& bonus = allocs.stack_ops.at(ir_idx);
    for (auto& op : bonus.ops) {
      if (op.load) {
        if (op.reg.is_gpr(m_gen.instr_set()) && op.reg_class == RegClass::GPR_64) {
          // todo, s8 or 0 offset if possible?
          m_gen.add_instr(IGen::load64_gpr64_plus_s32(
                              m_gen, op.reg, allocs.get_slot_for_spill(op.slot) * GPR_SIZE, RSP),
                          i_rec);
        } else if (op.reg.is_xmm(m_gen.instr_set()) && op.reg_class == RegClass::FLOAT) {
          // load xmm32 off of the stack
          m_gen.add_instr(IGen::load_reg_offset_xmm32(
                              m_gen, op.reg, RSP, allocs.get_slot_for_spill(op.slot) * GPR_SIZE),
                          i_rec);
        } else if (op.reg.is_xmm(m_gen.instr_set()) &&
                   (op.reg_class == RegClass::VECTOR_FLOAT || op.reg_class == RegClass::INT_128)) {
          m_gen.add_instr(IGen::load128_xmm128_reg_offset(
                              m_gen, op.reg, RSP, allocs.get_slot_for_spill(op.slot) * GPR_SIZE),
                          i_rec);
        } else {
          ASSERT(false);
        }
      }
    }

    // do the actual op
    ir->do_codegen_x86(&m_gen, allocs, i_rec);

    // store things back on the stack if needed.
    for (auto& op : bonus.ops) {
      if (op.store) {
        if (op.reg.is_gpr(m_gen.instr_set()) && op.reg_class == RegClass::GPR_64) {
          // todo, s8 or 0 offset if possible?
          m_gen.add_instr(IGen::store64_gpr64_plus_s32(
                              m_gen, RSP, allocs.get_slot_for_spill(op.slot) * GPR_SIZE, op.reg),
                          i_rec);
        } else if (op.reg.is_xmm(m_gen.instr_set()) && op.reg_class == RegClass::FLOAT) {
          // store xmm32 on the stack
          m_gen.add_instr(IGen::store_reg_offset_xmm32(
                              m_gen, RSP, op.reg, allocs.get_slot_for_spill(op.slot) * GPR_SIZE),
                          i_rec);
        } else if (op.reg.is_xmm(m_gen.instr_set()) &&
                   (op.reg_class == RegClass::VECTOR_FLOAT || op.reg_class == RegClass::INT_128)) {
          m_gen.add_instr(IGen::store128_xmm128_reg_offset(
                              m_gen, RSP, op.reg, allocs.get_slot_for_spill(op.slot) * GPR_SIZE),
                          i_rec);
        } else {
          ASSERT(false);
        }
      }
    }
  }  // end IR loop

  // EPILOGUE
  if (manually_added_stack_offset || allocs.needs_aligned_stack_for_spills ||
      env->needs_aligned_stack()) {
    if (manually_added_stack_offset) {
      m_gen.add_instr_no_ir(f_rec, IGen::add_gpr64_imm(m_gen, RSP, manually_added_stack_offset),
                            InstructionInfo::Kind::EPILOGUE);
    }

    if (bonus_push) {
      ASSERT(!manually_added_stack_offset);
      m_gen.add_instr_no_ir(f_rec, IGen::pop_gpr64(m_gen, ri.get_saved_gpr(0)),
                            InstructionInfo::Kind::EPILOGUE);
    }
  }

  for (int i = int(allocs.used_saved_regs.size()); i-- > 0;) {
    auto& saved_reg = allocs.used_saved_regs.at(i);
    if (saved_reg.is_gpr(m_gen.instr_set())) {
      m_gen.add_instr_no_ir(f_rec, IGen::pop_gpr64(m_gen, saved_reg),
                            InstructionInfo::Kind::EPILOGUE);
    }
  }

  if (use_new_xmms) {
    if (n_xmm_backups > 0) {
      int j = n_xmm_backups;
      for (int i = int(allocs.used_saved_regs.size()); i-- > 0;) {
        auto& saved_reg = allocs.used_saved_regs.at(i);
        if (saved_reg.is_xmm(m_gen.instr_set())) {
          j--;
          int offset = j * XMM_SIZE;
          m_gen.add_instr_no_ir(f_rec,
                                IGen::load128_xmm128_reg_offset(m_gen, saved_reg, RSP, offset),
                                InstructionInfo::Kind::EPILOGUE);
        }
      }
      ASSERT(j == 0);
      m_gen.add_instr_no_ir(f_rec, IGen::add_gpr64_imm(m_gen, RSP, xmm_backup_stack_offset),
                            InstructionInfo::Kind::EPILOGUE);
    }
  } else {
    for (int i = int(allocs.used_saved_regs.size()); i-- > 0;) {
      auto& saved_reg = allocs.used_saved_regs.at(i);
      if (saved_reg.is_xmm(m_gen.instr_set())) {
        m_gen.add_instr_no_ir(f_rec, IGen::load128_simd128_gpr64(m_gen, saved_reg, RSP),
                              InstructionInfo::Kind::EPILOGUE);
        m_gen.add_instr_no_ir(f_rec, IGen::add_gpr64_imm8s(m_gen, RSP, XMM_SIZE),
                              InstructionInfo::Kind::EPILOGUE);
      }
    }
  }

  m_gen.add_instr_no_ir(f_rec, IGen::ret(m_gen), InstructionInfo::Kind::EPILOGUE);
}

void CodeGenerator::do_goal_function_arm64(FunctionEnv* env, int f_idx) {
  auto* debug = &m_debug_info->function_by_name(env->name());
  auto f_rec = m_gen.get_existing_function_record(f_idx);
  const auto& allocs = env->alloc_result();

  // Register is intentionally a single ID type on ARM64: X8 and V8, for
  // example, both have ID 8.  The allocator's saved-register list is ordered
  // as X19..X28 followed by V8..V15, so classify it by the ABI ranges rather
  // than by Register::is_gpr/is_128bit_simd (both are true for ARM IDs).
  std::vector<Register> saved_gprs;
  std::vector<Register> saved_simd;
  for (const auto& saved_reg : allocs.used_saved_regs) {
    if (saved_reg.id() >= X19 && saved_reg.id() <= X28) {
      saved_gprs.push_back(saved_reg);
    } else if (saved_reg.id() >= V8 && saved_reg.id() <= V15) {
      saved_simd.push_back(saved_reg);
    } else {
      throw std::runtime_error(fmt::format("ARM64 function {} has an invalid saved register {}.",
                                           env->name(), saved_reg.id()));
    }
  }

  // X30 is the link register.  Saving it for every GOAL function gives leaf
  // and non-leaf functions one identical ABI frame and makes the epilogue
  // independent of whether a later IR optimization introduces a call.
  m_gen.add_instr_no_ir(f_rec, IGen::push_gpr64(m_gen, X30), InstructionInfo::Kind::PROLOGUE);
  for (const auto& saved_reg : saved_gprs) {
    m_gen.add_instr_no_ir(f_rec, IGen::push_gpr64(m_gen, saved_reg),
                          InstructionInfo::Kind::PROLOGUE);
  }

  const int spill_and_var_bytes =
      GPR_SIZE * (allocs.stack_slots_for_spills + allocs.stack_slots_for_vars);
  // Spill slots are addressed from the final SP.  Round their area up instead
  // of adding an unpaired 8-byte adjustment: SP remains 16-byte aligned at all
  // call boundaries and the unused tail is explicit padding.
  const int spill_area_bytes = (spill_and_var_bytes + 15) & ~15;
  const int simd_save_bytes = int(saved_simd.size()) * 16;
  const int frame_area_bytes = spill_area_bytes + simd_save_bytes;

  if (frame_area_bytes) {
    m_gen.add_instr_no_ir(f_rec, IGen::sub_gpr64_imm(m_gen, SP, frame_area_bytes),
                          InstructionInfo::Kind::PROLOGUE);
  }

  for (size_t i = 0; i < saved_simd.size(); i++) {
    const int offset = spill_area_bytes + int(i) * 16;
    m_gen.add_instr_no_ir(f_rec,
                          IGen::store128_xmm128_reg_offset(m_gen, SP, saved_simd.at(i), offset),
                          InstructionInfo::Kind::PROLOGUE);
  }

  debug->stack_usage = 16 * (1 + int(saved_gprs.size())) + frame_area_bytes;
  ASSERT(debug->stack_usage.value() % 16 == 0);

  auto emit_stack_ops = [&](const StackOp& bonus, IR_Record i_rec, bool load) {
    for (const auto& op : bonus.ops) {
      if ((load && !op.load) || (!load && !op.store)) {
        continue;
      }
      const int offset = allocs.get_slot_for_spill(op.slot) * GPR_SIZE;
      if (op.reg_class == RegClass::GPR_64) {
        if (load) {
          m_gen.add_instr(IGen::load64_gpr64_plus_s32(m_gen, op.reg, offset, SP), i_rec);
        } else {
          m_gen.add_instr(IGen::store64_gpr64_plus_s32(m_gen, SP, offset, op.reg), i_rec);
        }
      } else if (op.reg_class == RegClass::FLOAT) {
        if (load) {
          m_gen.add_instr(IGen::load_reg_offset_xmm32(m_gen, op.reg, SP, offset), i_rec);
        } else {
          m_gen.add_instr(IGen::store_reg_offset_xmm32(m_gen, SP, op.reg, offset), i_rec);
        }
      } else if (op.reg_class == RegClass::VECTOR_FLOAT || op.reg_class == RegClass::INT_128) {
        if (load) {
          m_gen.add_instr(IGen::load128_xmm128_reg_offset(m_gen, op.reg, SP, offset), i_rec);
        } else {
          m_gen.add_instr(IGen::store128_xmm128_reg_offset(m_gen, SP, op.reg, offset), i_rec);
        }
      } else {
        throw std::runtime_error(
            fmt::format("ARM64 function {} has an unsupported spill class.", env->name()));
      }
    }
  };

  for (int ir_idx = 0; ir_idx < int(env->code().size()); ir_idx++) {
    auto& ir = env->code().at(ir_idx);
    auto i_rec = m_gen.add_ir(f_rec);
    const auto& bonus = allocs.stack_ops.at(ir_idx);
    emit_stack_ops(bonus, i_rec, true);
    ir->do_codegen_arm64(&m_gen, allocs, i_rec);
    emit_stack_ops(bonus, i_rec, false);
  }

  for (int i = int(saved_simd.size()); i-- > 0;) {
    const int offset = spill_area_bytes + i * 16;
    m_gen.add_instr_no_ir(f_rec,
                          IGen::load128_xmm128_reg_offset(m_gen, saved_simd.at(i), SP, offset),
                          InstructionInfo::Kind::EPILOGUE);
  }
  if (frame_area_bytes) {
    m_gen.add_instr_no_ir(f_rec, IGen::add_gpr64_imm(m_gen, SP, frame_area_bytes),
                          InstructionInfo::Kind::EPILOGUE);
  }
  for (int i = int(saved_gprs.size()); i-- > 0;) {
    m_gen.add_instr_no_ir(f_rec, IGen::pop_gpr64(m_gen, saved_gprs.at(i)),
                          InstructionInfo::Kind::EPILOGUE);
  }
  m_gen.add_instr_no_ir(f_rec, IGen::pop_gpr64(m_gen, X30), InstructionInfo::Kind::EPILOGUE);
  m_gen.add_instr_no_ir(f_rec, IGen::ret(m_gen), InstructionInfo::Kind::EPILOGUE);
}

void CodeGenerator::do_asm_function_x86(FunctionEnv* env, int f_idx, bool allow_saved_regs) {
  auto f_rec = m_gen.get_existing_function_record(f_idx);
  const auto& allocs = env->alloc_result();

  if (!allow_saved_regs && !allocs.used_saved_regs.empty()) {
    std::string err = fmt::format(
        "ASM Function {}'s coloring using the following callee-saved registers: ", env->name());
    for (auto& x : allocs.used_saved_regs) {
      err += x.print();
      err += " ";
    }
    err.pop_back();
    err.push_back('.');
    throw std::runtime_error(err);
  }

  if (allocs.stack_slots_for_spills) {
    throw std::runtime_error("ASM Function has used the stack for spills.");
  }

  if (allocs.stack_slots_for_vars) {
    throw std::runtime_error("ASM Function has variables on the stack.");
  }

  // emit each IR into x86 instructions.
  for (int ir_idx = 0; ir_idx < int(env->code().size()); ir_idx++) {
    auto& ir = env->code().at(ir_idx);
    // start of IR
    auto i_rec = m_gen.add_ir(f_rec);

    // Make sure we aren't automatically accessing the stack.
    if (!allocs.stack_ops.at(ir_idx).ops.empty()) {
      throw std::runtime_error("ASM Function used a bonus op.");
    }

    // do the actual op
    ir->do_codegen_x86(&m_gen, allocs, i_rec);
  }
}

void CodeGenerator::do_asm_function_arm64(FunctionEnv* env, int f_idx, bool allow_saved_regs) {
  auto f_rec = m_gen.get_existing_function_record(f_idx);
  const auto& allocs = env->alloc_result();

  // r13/r14/r15 are the GOAL process, symbol-table, and memory-offset
  // registers.  ARM maps them to x20/x21/x22; asm-funcs are allowed to use
  // them directly because context-switch routines deliberately own that
  // state.  Other callee-saved registers still require the explicit
  // allow-saved-regs declaration.
  const auto is_goal_context_reg = [](Register reg) {
    return reg == X20 || reg == X21 || reg == X22;
  };
  const bool has_unapproved_saved_reg =
      std::any_of(allocs.used_saved_regs.begin(), allocs.used_saved_regs.end(),
                  [&](Register reg) { return !is_goal_context_reg(reg); });
  if (!allow_saved_regs && has_unapproved_saved_reg) {
    std::string err = fmt::format(
        "ASM Function {}'s coloring using the following callee-saved registers: ", env->name());
    for (const auto& x : allocs.used_saved_regs) {
      if (!is_goal_context_reg(x)) {
        err += fmt::format("{}(id={}) ", x.print(), x.id());
      }
    }
    err.pop_back();
    err.push_back('.');
    throw std::runtime_error(err);
  }
  if (allocs.stack_slots_for_spills) {
    throw std::runtime_error("ASM Function has used the stack for spills.");
  }
  if (allocs.stack_slots_for_vars) {
    throw std::runtime_error("ASM Function has variables on the stack.");
  }

  // asm-func has always owned its own prologue/epilogue.  In particular, an
  // asm body may contain .ret and .push/.pop directives, so do not surround it
  // with an implicit frame here; this mirrors the x86 contract exactly.
  for (int ir_idx = 0; ir_idx < int(env->code().size()); ir_idx++) {
    auto& ir = env->code().at(ir_idx);
    auto i_rec = m_gen.add_ir(f_rec);
    if (!allocs.stack_ops.at(ir_idx).ops.empty()) {
      throw std::runtime_error("ASM Function used a bonus op.");
    }
    try {
      ir->do_codegen_arm64(&m_gen, allocs, i_rec);
    } catch (const std::exception& e) {
      throw std::runtime_error(
          fmt::format("ASM Function {} IR {} ({}): {}", env->name(), ir_idx, ir->print(),
                      e.what()));
    }
  }
}
