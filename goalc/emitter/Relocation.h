#pragma once

/*!
 * @file Relocation.h
 *
 * Typed relocation model for the GOAL compiler backends (ARM-009).
 *
 * This is an INTERNAL, in-memory model: it describes how to compute the value
 * of an instruction field from the instruction's own address (PC) and a target
 * address, without reusing the x86 "rel32/immediate" assumptions for ARM64.
 *
 * It is intentionally NOT wired into object generation or the persistent
 * object format yet.  The persistent format change requires an accepted ADR
 * and upstream coordination (see ADR-0004 and the ARM-009 stop clause).
 *
 * Field formulas per type (base, position, width, scale, sign, range):
 *
 *   X86Rel32          disp = target - (pc + 4) - addend
 *                     32-bit signed immediate, range [-2^31, 2^31-1]
 *   X86Imm32          imm  = target + addend
 *                     32-bit signed immediate
 *   Arm64Branch26     imm26 = (target - pc - addend) / 4
 *                     26-bit signed, range [-2^25, 2^25-1] words, target 4-aligned
 *   Arm64CondBranch19 imm19 = (target - pc - addend) / 4
 *                     19-bit signed, range [-2^18, 2^18-1] words, target 4-aligned
 *   Arm64AdrpPage21   imm21 = ((target + addend) - (pc & ~0xFFF)) / 0x1000
 *                     21-bit signed pages; field split immlo[30:29] + immhi[23:5]
 *   Arm64AddLo12      imm12 = (target + addend) & 0xFFF
 *                     12-bit unsigned; pairs with ADRP (same page-relative value)
 *   Arm64LdrLiteral19 imm19 = (target - pc - addend) / 4
 *                     19-bit signed, target 4-aligned
 *   Arm64MovWide16    imm16 = ((target + addend) >> (16 * hw)) & 0xFFFF
 *                     16-bit unsigned; hw (shift selector) read from the
 *                     instruction itself (bits [22:21])
 *   Arm64Adr21        imm21 = target - pc - addend
 *                     21-bit signed byte displacement, split immlo/immhi
 */

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace emitter {

enum class RelocationType : u8 {
  X86Rel32 = 1,
  X86Imm32 = 2,
  Arm64Branch26 = 3,
  Arm64CondBranch19 = 4,
  Arm64AdrpPage21 = 5,
  Arm64AddLo12 = 6,
  Arm64LdrLiteral19 = 7,
  Arm64MovWide16 = 8,
  Arm64Adr21 = 9,
};

/*!
 * Static description of one relocation kind: where the field lives inside the
 * instruction word, how the encoded value maps to a byte delta, and the valid
 * range.  All offsets are in bytes unless stated otherwise.
 */
struct RelocationFieldInfo {
  RelocationType type;
  const char* name;
  // Position of the field within the instruction word:
  //   bit_offset = offset (from the LSB of the word) of the field's LSB.
  //   width      = number of bits in the field.
  //   split      = true for ADRP (immhi + immlo); apply writes both parts.
  int bit_offset = 0;
  int width = 0;
  bool split = false;
  // Effective value = encoded_value * scale, sign-extended first if sign_extend.
  int scale = 1;
  bool sign_extend = true;
  // Range of the EFFECTIVE value (after scaling), inclusive.
  int64_t min_value = 0;
  int64_t max_value = 0;
  // PC base: 0 = the instruction address, 1 = the page (PC & ~0xFFF).
  int pc_base = 0;
  // Required alignment of the resolved target, in bytes.
  int align = 1;
  // For X86Rel32 the effective displacement is relative to the END of the
  // instruction (the standard x86 RIP-relative semantics).
  bool rel_end_of_instr = false;
};

const RelocationFieldInfo& relocation_info(RelocationType type);

/*!
 * A concrete relocation: which kind, where (byte offset of the instruction
 * word within the code buffer) and a constant addend added to the target.
 */
struct Relocation {
  RelocationType type;
  int32_t position = 0;
  int64_t addend = 0;
};

/*!
 * Compute the raw field bits for a relocation.  Returns std::nullopt (and sets
 * *err when non-null) on out-of-range or misaligned targets.  The returned
 * value is the field value already shifted to its bit position within the
 * 32-bit instruction word.
 */
std::optional<u32> compute_relocation_field(RelocationType type,
                                            s64 pc,
                                            s64 target,
                                            s64 addend,
                                            std::string* err = nullptr);

/*!
 * Apply a relocation by patching the instruction word at `position` in `code`.
 * `pc` is the address of that word; `target` is the resolved destination.
 */
bool apply_relocation(RelocationType type,
                      u8* code,
                      s64 position,
                      s64 pc,
                      s64 target,
                      s64 addend,
                      std::string* err = nullptr);

/*!
 * Canonical in-memory serialization of a relocation: [type tag (u8)][position
 * (u32)][addend (s64)].  This is an internal model format for tests and
 * tooling, NOT the persistent object-file format.
 */
std::optional<std::vector<u8>> serialize_relocation(const Relocation& r);
std::optional<Relocation> deserialize_relocation(const std::vector<u8>& data);

}  // namespace emitter
