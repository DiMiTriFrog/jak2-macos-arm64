#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "common/common_types.h"
#include "common/jit_memory.h"
#include "common/util/Assert.h"

// Small, header-only AArch64 encoder for runtime trampolines.  The game target does not link
// against the GOAL emitter library, so this is the central encoder shared by the runtime and the
// MIPS2C registration path until the JIT memory manager owns this interface.
namespace arm64_trampoline {

constexpr u32 kLdrLiteralX16 = 0x58000000;
constexpr u32 kLdrLiteralX17 = 0x58000000 | 17;
constexpr u32 kBrX16 = 0xd61f0200;
constexpr u32 kBrX17 = 0xd61f0220;
constexpr u32 kBrX15 = 0xd61f01e0;
constexpr u32 kMovX3X20 = 0xaa1403e3;
constexpr u32 kMovX0Zero = 0xd2800000;
constexpr u32 kRet = 0xd65f03c0;

constexpr u32 encode_ldr_literal_x16(size_t instruction_offset, size_t literal_offset) {
  const auto delta =
      static_cast<int64_t>(literal_offset) - static_cast<int64_t>(instruction_offset);
  ASSERT(delta % 4 == 0);
  ASSERT(delta >= -(1 << 20) && delta < (1 << 20));
  return kLdrLiteralX16 | ((static_cast<u32>(delta / 4) & 0x7ffff) << 5) | 16;
}

inline void write_word(u8* dst, u32 word) {
  // The GOAL object starts at BASIC_OFFSET (four bytes into a 16-byte allocation), which is
  // instruction-aligned but not necessarily eight-byte aligned.  memcpy is therefore deliberate.
  std::memcpy(dst, &word, sizeof(word));
}

inline void write_literal(u8* dst, size_t offset, u64 value) {
  std::memcpy(dst + offset, &value, sizeof(value));
}

inline void flush(void* start, size_t size) {
  jit_memory::flush_instruction_cache(start, size);
}

constexpr u32 encode_ldr_literal(u32 opcode, size_t instruction_offset, size_t literal_offset) {
  const auto delta = static_cast<int64_t>(literal_offset) - static_cast<int64_t>(instruction_offset);
  ASSERT(delta % 4 == 0);
  ASSERT(delta >= -(1 << 20) && delta < (1 << 20));
  return opcode | ((static_cast<u32>(delta / 4) & 0x7ffff) << 5);
}

constexpr u32 encode_ldr_literal_x17(size_t instruction_offset, size_t literal_offset) {
  return encode_ldr_literal(kLdrLiteralX17, instruction_offset, literal_offset);
}

// The GOAL stack pointer is a virtual offset on ARM64.  These trampolines therefore pass the
// target through x16 and the bridge through x17 instead of reserving a stack slot before the
// bridge has had an opportunity to convert the virtual stack pointer to a host address.
inline size_t emit_c_function(u8* dst, void* target, void* bridge, bool arg3_is_pp) {
  constexpr size_t literal_target = 24;
  constexpr size_t literal_bridge = 32;

  write_word(dst + 0, encode_ldr_literal_x16(0, literal_target));
  write_word(dst + 4, arg3_is_pp ? kMovX3X20 : 0xd503201f);
  write_word(dst + 8, encode_ldr_literal_x17(8, literal_bridge));
  write_word(dst + 12, kBrX17);
  write_word(dst + 16, 0xd503201f);
  write_word(dst + 20, 0xd503201f);
  write_literal(dst, literal_target, static_cast<u64>(reinterpret_cast<uintptr_t>(target)));
  write_literal(dst, literal_bridge, static_cast<u64>(reinterpret_cast<uintptr_t>(bridge)));
  return literal_bridge + sizeof(u64);
}

inline size_t emit_stack_function(u8* dst, void* target, void* bridge) {
  constexpr size_t literal_target = 24;
  constexpr size_t literal_bridge = 32;
  write_word(dst + 0, encode_ldr_literal_x16(0, literal_target));
  write_word(dst + 4, encode_ldr_literal_x17(4, literal_bridge));
  write_word(dst + 8, kBrX17);
  write_word(dst + 12, 0xd503201f);
  write_word(dst + 16, 0xd503201f);
  write_word(dst + 20, 0xd503201f);
  write_literal(dst, literal_target, static_cast<u64>(reinterpret_cast<uintptr_t>(target)));
  write_literal(dst, literal_bridge, static_cast<u64>(reinterpret_cast<uintptr_t>(bridge)));
  return literal_bridge + sizeof(u64);
}

// _mips2c_call_arm64 receives the stack size in x17 and execution address in x16.  As above, no
// stack memory is touched until the bridge has converted the GOAL virtual stack pointer.
inline size_t emit_mips2c(u8* dst, u64 stack_size, void* exec, void* bridge) {
  constexpr size_t literal_stack_size = 32;
  constexpr size_t literal_exec = 40;
  constexpr size_t literal_bridge = 48;
  write_word(dst + 0, encode_ldr_literal_x17(0, literal_stack_size));
  write_word(dst + 4, encode_ldr_literal_x16(4, literal_exec));
  write_word(dst + 8, encode_ldr_literal(0x58000000 | 15, 8, literal_bridge));
  write_word(dst + 12, kBrX15);
  write_word(dst + 16, 0xd503201f);
  write_word(dst + 20, 0xd503201f);
  write_word(dst + 24, 0xd503201f);
  write_word(dst + 28, 0xd503201f);  // keep all literals 4-byte aligned.
  write_literal(dst, literal_stack_size, stack_size);
  write_literal(dst, literal_exec, static_cast<u64>(reinterpret_cast<uintptr_t>(exec)));
  write_literal(dst, literal_bridge, static_cast<u64>(reinterpret_cast<uintptr_t>(bridge)));
  return literal_bridge + sizeof(u64);
}

inline size_t emit_nothing(u8* dst) {
  write_word(dst, kRet);
  return sizeof(u32);
}

inline size_t emit_zero(u8* dst) {
  write_word(dst, kMovX0Zero);
  write_word(dst + sizeof(u32), kRet);
  return sizeof(u32) * 2;
}

}  // namespace arm64_trampoline
