#include "Relocation.h"

#include <cstring>

#include "common/util/Assert.h"

namespace emitter {

const RelocationFieldInfo& relocation_info(RelocationType type) {
  static const RelocationFieldInfo kInfos[] = {
      // X86Rel32: disp32 at bits [31:0], target = pc + 4 + disp
      {RelocationType::X86Rel32, "x86-rel32", 0, 32, false, 1, true, INT32_MIN, INT32_MAX, 0, 1,
       true},
      // X86Imm32: absolute 32-bit immediate at bits [31:0]
      {RelocationType::X86Imm32, "x86-imm32", 0, 32, false, 1, false, 0, 0xFFFFFFFFLL, 0, 1,
       false},
      // B/BL: imm26 at bits [25:0]
      {RelocationType::Arm64Branch26, "arm64-branch26", 0, 26, false, 4, true, -1LL << 25,
       (1LL << 25) - 1, 0, 4, false},
      // B.cond: imm19 at bits [23:5]
      {RelocationType::Arm64CondBranch19, "arm64-condbranch19", 5, 19, false, 4, true, -1LL << 18,
       (1LL << 18) - 1, 0, 4, false},
      // ADRP: immlo at [30:29], immhi at [23:5]
      {RelocationType::Arm64AdrpPage21, "arm64-adrp-page21", 5, 19, true, 1, true, -1LL << 20,
       (1LL << 20) - 1, 1, 0x1000, false},
      // ADD lo12: imm12 at [21:10]
      {RelocationType::Arm64AddLo12, "arm64-add-lo12", 10, 12, false, 1, false, 0, 0xFFF, 0, 1,
       false},
      // LDR literal: imm19 at [23:5]
      {RelocationType::Arm64LdrLiteral19, "arm64-ldr-literal19", 5, 19, false, 4, true, -1LL << 18,
       (1LL << 18) - 1, 0, 4, false},
      // MOVZ/MOVK: imm16 at [20:5], hw (shift) read from the instruction
      {RelocationType::Arm64MovWide16, "arm64-movwide16", 5, 16, false, 1, false, 0, 0xFFFF, 0, 1,
       false},
  };
  int idx = static_cast<int>(type) - 1;
  if (idx < 0 || idx >= (int)(sizeof(kInfos) / sizeof(kInfos[0]))) {
    ASSERT_MSG(false, "unknown relocation type");
  }
  return kInfos[idx];
}

namespace {

// Sign-extend the low `width` bits of a value.
int64_t sign_extend(u64 value, int width) {
  u64 sign = 1ull << (width - 1);
  return (int64_t)((value ^ sign) - sign);
}

// Extract `width` bits starting at `bit_offset` from a 32-bit word.
u32 extract_field(u32 word, int bit_offset, int width) {
  return (word >> bit_offset) & ((1u << width) - 1);
}

// Place `value` (width bits) at `bit_offset` in a 32-bit word.
u32 insert_field(u32 word, int bit_offset, int width, u32 value) {
  u32 field_mask = (width == 32) ? 0xFFFFFFFFu : ((1u << width) - 1);
  u32 mask = field_mask << bit_offset;
  return (word & ~mask) | ((value << bit_offset) & mask);
}

}  // namespace

std::optional<u32> compute_relocation_field(RelocationType type,
                                            s64 pc,
                                            s64 target,
                                            s64 addend,
                                            std::string* err) {
  const auto& info = relocation_info(type);

  // Resolve the base address the field is relative to.
  s64 base = pc;
  if (info.pc_base == 1) {
    base = pc & ~0xFFFull;
  }

  // Effective target value.
  s64 resolved = target + addend;
  s64 value;
  if (info.rel_end_of_instr) {
    value = resolved - (base + 4);
  } else if (type == RelocationType::X86Imm32) {
    value = resolved;
  } else if (info.pc_base == 1) {
    // ADRP: pages between the target page and the PC page.
    value = (resolved - base) / 0x1000;
  } else if (type == RelocationType::Arm64AddLo12) {
    // The low 12 bits of the page-relative address (the page was resolved by
    // the paired ADRP, so only the low 12 bits matter).
    value = resolved & 0xFFF;
  } else {
    // Branch-like: byte delta from the instruction address.
    value = resolved - base;
  }

  // Alignment check before any division.
  if (info.align > 1 && (resolved % info.align) != 0) {
    if (err) {
      *err = std::string("relocation target not aligned to ") + std::to_string(info.align) +
             " bytes";
    }
    return std::nullopt;
  }

  // Apply scale.
  int64_t scaled;
  if (info.scale > 1) {
    if ((value % info.scale) != 0) {
      if (err) {
        *err = std::string("relocation value not a multiple of ") + std::to_string(info.scale);
      }
      return std::nullopt;
    }
    scaled = value / info.scale;
  } else {
    scaled = value;
  }

  // Range check on the scaled value.
  if (scaled < info.min_value || scaled > info.max_value) {
    if (err) {
      *err = std::string("relocation value ") + std::to_string(scaled) + " out of range [" +
             std::to_string(info.min_value) + ", " + std::to_string(info.max_value) + "]";
    }
    return std::nullopt;
  }

  u32 field_value = (u32)scaled;
  if (info.split) {
    // ADRP: immlo = bits [1:0] of the page delta, immhi = bits [20:2].
    u32 lo = field_value & 0b11;
    u32 hi = (field_value >> 2) & 0x7FFFF;
    return (hi << 5) | (lo << 29);
  }
  if (type == RelocationType::Arm64MovWide16) {
    // The shift selector comes from the instruction; handled in apply.
    return field_value << info.bit_offset;
  }
  return field_value << info.bit_offset;
}

bool apply_relocation(RelocationType type,
                      u8* code,
                      s64 position,
                      s64 pc,
                      s64 target,
                      s64 addend,
                      std::string* err) {
  if (position < 0 || (position % 4) != 0) {
    if (err) {
      *err = "instruction position is not 4-byte aligned";
    }
    return false;
  }
  u32 word;
  std::memcpy(&word, code + position, 4);

  // For MOVZ/MOVK the shift selector lives in the instruction itself.
  if (type == RelocationType::Arm64MovWide16) {
    const auto& info = relocation_info(type);
    u32 hw = extract_field(word, 21, 2);
    s64 shifted = (target + addend) >> (16 * hw);
    if (shifted < 0 || shifted > 0xFFFF) {
      if (err) {
        *err = "movwide16 value out of range for hw " + std::to_string(hw);
      }
      return false;
    }
    word = insert_field(word, info.bit_offset, info.width, (u32)shifted);
    std::memcpy(code + position, &word, 4);
    return true;
  }

  auto field = compute_relocation_field(type, pc, target, addend, err);
  if (!field) {
    return false;
  }
  const auto& info = relocation_info(type);
  if (info.split) {
    // The computed value already carries immlo at [31:30] and immhi at [24:5].
    word = insert_field(word, 29, 2, *field >> 29);
    word = insert_field(word, 5, 19, (*field >> 5) & 0x7FFFF);
  } else {
    word = insert_field(word, info.bit_offset, info.width, *field >> info.bit_offset);
  }
  std::memcpy(code + position, &word, 4);
  return true;
}

std::optional<std::vector<u8>> serialize_relocation(const Relocation& r) {
  if (static_cast<int>(r.type) < 1 || static_cast<int>(r.type) > 8) {
    return std::nullopt;
  }
  std::vector<u8> data;
  data.reserve(13);
  data.push_back(static_cast<u8>(r.type));
  u32 pos = (u32)r.position;
  for (int i = 0; i < 4; i++) {
    data.push_back((pos >> (8 * i)) & 0xFF);
  }
  u64 add = (u64)r.addend;
  for (int i = 0; i < 8; i++) {
    data.push_back((add >> (8 * i)) & 0xFF);
  }
  return data;
}

std::optional<Relocation> deserialize_relocation(const std::vector<u8>& data) {
  if (data.size() != 13) {
    return std::nullopt;
  }
  u8 tag = data[0];
  if (tag < 1 || tag > 8) {
    return std::nullopt;
  }
  u32 pos = 0;
  for (int i = 0; i < 4; i++) {
    pos |= (u32)data[1 + i] << (8 * i);
  }
  u64 add = 0;
  for (int i = 0; i < 8; i++) {
    add |= (u64)data[5 + i] << (8 * i);
  }
  return Relocation{static_cast<RelocationType>(tag), (int32_t)pos, (int64_t)add};
}

}  // namespace emitter
