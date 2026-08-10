/*!
 * @file CodeTester.cpp
 * The CodeTester is a utility to run the output of the compiler as part of a unit test.
 * This is effective for tests which try all combinations of registers, etc.
 *
 * The CodeTester can't be used for tests requiring the full GOAL language/linking.
 */

#include "CodeTester.h"

#include <cstdio>
#include <stdexcept>

#include "IGen.h"

#include "common/common_types.h"

#include "goalc/emitter/Instruction.h"
#include "goalc/emitter/InstructionSet.h"
#include "goalc/emitter/Register.h"

namespace emitter {

CodeTester::CodeTester() : m_info(RegisterInfo::make_register_info()), m_gen(GameVersion::Jak1) {}

CodeTester::CodeTester(InstructionSet instruction_set)
    : m_info(RegisterInfo::make_register_info()), m_gen(GameVersion::Jak1, instruction_set) {}

/*!
 * Convert to a string for comparison against an assembler or tests.
 */
std::string CodeTester::dump_to_hex_string(bool nospace) {
  std::string result;
  char buff[32];
  for (int i = 0; i < code_buffer_size; i++) {
    if (nospace) {
      sprintf(buff, "%02X", code_buffer[i]);
    } else {
      sprintf(buff, "%02x ", code_buffer[i]);
    }

    result += buff;
  }

  // remove trailing space
  if (!nospace && !result.empty()) {
    result.pop_back();
  }
  return result;
}

/*!
 * Add an instruction to the buffer.
 */
void CodeTester::emit(const emitter::Instruction& instr) {
  u8* start = code_buffer + code_buffer_size;
  code_buffer_size += instr.emit(start);
  ASSERT(code_buffer_size <= code_buffer_capacity);
}
/*!
 * Append raw bytes to the code buffer.
 */
void CodeTester::append_bytes(const u8* data, int size) {
  ASSERT(size + code_buffer_size <= code_buffer_capacity);
  memcpy(code_buffer + code_buffer_size, data, size);
  code_buffer_size += size;
}

/*!
 * Add a return instruction to the buffer.
 */
void CodeTester::emit_return() {
  emit(IGen::ret(m_gen));
}

/*!
 * Pop all GPRs off of the stack. Optionally exclude rax.
 * Pops RSP always, which is weird, but doesn't cause issues.
 */
void CodeTester::emit_pop_all_gprs(bool exclude_return_register) {
  if (m_gen.instr_set() == InstructionSet::X86) {
    for (int i = 16; i-- > 0;) {
      if (i != RAX || !exclude_return_register) {
        emit(IGen::pop_gpr64(m_gen, i));
      }
    }
  } else if (m_gen.instr_set() == InstructionSet::ARM64) {
    for (int i = 31; i-- > 0;) {
      if (i != X0 || !exclude_return_register) {
        emit(IGen::pop_gpr64(m_gen, i));
      }
    }
  } else {
    throw std::runtime_error("CodeTester::emit_pop_all_gprs unhandled instruction set");
  }
}

/*!
 * Push all GPRs onto the stack. Optionally exclude RAX.
 * Pushes RSP always, which is weird, but doesn't cause issues.
 */
void CodeTester::emit_push_all_gprs(bool exclude_return_register) {
  if (m_gen.instr_set() == InstructionSet::X86) {
    for (int i = 0; i < 16; i++) {
      if (i != RAX || !exclude_return_register) {
        emit(IGen::push_gpr64(m_gen, i));
      }
    }
  } else if (m_gen.instr_set() == InstructionSet::ARM64) {
    for (int i = 0; i < 31; i++) {
      if (i != X0 || !exclude_return_register) {
        emit(IGen::push_gpr64(m_gen, i));
      }
    }
  } else {
    throw std::runtime_error("CodeTester::emit_push_all_gprs unhandled instruction set");
  }
}

/*!
 * Push all xmm registers (all 128-bits) to the stack.
 */
void CodeTester::emit_push_all_simd() {
  if (m_gen.instr_set() == InstructionSet::X86) {
    emit(IGen::sub_gpr64_imm8s(m_gen, RSP, 8));
    for (int i = 0; i < 16; i++) {
      emit(IGen::sub_gpr64_imm8s(m_gen, RSP, 16));
      emit(IGen::store128_gpr64_simd128(m_gen, RSP, XMM0 + i));
    }
  } else if (m_gen.instr_set() == InstructionSet::ARM64) {
    for (int i = 0; i < 16; i++) {
      emit(IGen::sub_gpr64_imm8s(m_gen, SP, 16));
      emit(IGen::store128_gpr64_simd128(m_gen, SP, V0 + i));
    }
  } else {
    throw std::runtime_error("CodeTester::emit_push_all_simd unhandled instruction set");
  }
}

/*!
 * Pop all xmm registers (all 128-bits) from the stack
 */
void CodeTester::emit_pop_all_simd() {
  if (m_gen.instr_set() == InstructionSet::X86) {
    for (int i = 0; i < 16; i++) {
      emit(IGen::load128_simd128_gpr64(m_gen, XMM0 + i, RSP));
      emit(IGen::add_gpr64_imm8s(m_gen, RSP, 16));
    }
    emit(IGen::add_gpr64_imm8s(m_gen, RSP, 8));
  } else if (m_gen.instr_set() == InstructionSet::ARM64) {
    for (int i = 0; i < 16; i++) {
      emit(IGen::load128_simd128_gpr64(m_gen, V0 + i, SP));
      emit(IGen::add_gpr64_imm8s(m_gen, SP, 16));
    }
  } else {
    throw std::runtime_error("CodeTester::emit_pop_all_simd unhandled instruction set");
  }
}

/*!
 * Remove everything from the code buffer
 */
void CodeTester::clear() {
  code_buffer_size = 0;
}

/*!
 * Execute the buffered code with no arguments, return the value of RAX.
 */
u64 CodeTester::execute() {
  jit_memory::make_executable(code_buffer, code_buffer_capacity);
  auto ret = ((u64 (*)())code_buffer)();
  jit_memory::make_writable(code_buffer, code_buffer_capacity);
  return ret;
}

/*!
 * Execute code buffer with arguments. Use get_c_abi_arg to figure out which registers the
 * arguments will appear in (will handle windows/linux differences)
 */
u64 CodeTester::execute(u64 in0, u64 in1, u64 in2, u64 in3) {
  jit_memory::make_executable(code_buffer, code_buffer_capacity);
  auto ret = ((u64 (*)(u64, u64, u64, u64))code_buffer)(in0, in1, in2, in3);
  jit_memory::make_writable(code_buffer, code_buffer_capacity);
  return ret;
}

/*!
 * Allocate a code buffer of the given size.
 */
void CodeTester::init_code_buffer(int capacity) {
  code_region = std::make_unique<jit_memory::JitRegion>(jit_memory::JitRegion::allocate(capacity));
  code_buffer = static_cast<u8*>(code_region->data());

  code_buffer_capacity = capacity;
  code_buffer_size = 0;
}

CodeTester::~CodeTester() {
  code_region.reset();
}
}  // namespace emitter
