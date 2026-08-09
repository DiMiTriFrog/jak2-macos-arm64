
#include "IGenARM64.h"

#include <tuple>

#include "goalc/emitter/Instruction.h"
#include "goalc/emitter/InstructionSet.h"
#include "goalc/emitter/Register.h"

// https://armconverter.com/?code=ret
// https://developer.arm.com/documentation/ddi0487/latest

// TODO ARM64 - just silencing errors while things are not implemented obviously
#pragma GCC diagnostic ignored "-Wunused-parameter"

namespace emitter {
namespace IGen {
namespace ARM64 {

const auto instr_set = emitter::InstructionSet::ARM64;
using namespace emitter::ARM64;

// Utility functions (not public facing instructions)
// used to encode instructions and match the same API

// Checks whether or not an immediate can be represented in 12 unsigned bits, either:
// - plain [0-4095] immediate
// - imm << 12 (some multiple of 4096)
std::tuple<bool, u16, bool> can_encode_single_imm12(u64 imm) {
  if (imm < 4096) {
    return {true, static_cast<u16>(imm), false};
  }
  if ((imm & 0xFFF) == 0) {  // divisible by 4096
    u64 upper = imm >> 12;
    if (upper < 4096) {
      return {true, static_cast<uint16_t>(upper), true};
    }
  }
  return {false, 0, false};
}

// Given a larger than u12 immediate, decompose it into multiple (shifted or not)
// immediates that can be used to emit multiple instructions to produce the desired outcome
std::vector<std::tuple<u16, bool>> decompose_into_imm12_chunks(u64 imm) {
  std ::vector<std::tuple<u16, bool>> result;
  u64 upper = imm >> 12;
  while (upper > 0) {
    u16 chunk = (upper > 4095) ? 4095 : static_cast<u16>(upper);
    result.emplace_back(chunk, true);
    upper -= chunk;
  }

  u16 lower = imm & 0xFFF;
  if (lower > 0) {
    result.emplace_back(lower, false);
  }
  return result;
}

std::vector<InstructionARM64> construct_multiple_imm12_adds(int64_t imm, u32 register_id) {
  const auto chunks = decompose_into_imm12_chunks(imm);
  std::vector<InstructionARM64> instrs;
  for (const auto& [_imm12, _needs_shift] : chunks) {
    // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_imm.html
    // ADD <Xd|SP>, <Xn|SP>, #<imm>{, <shift>}
    instrs.emplace_back(InstructionARM64(Base(0b100100010, 9), Sh(_needs_shift ? 1 : 0),
                                         Imm12(_imm12), Rd(register_id), Rn(register_id)));
  }
  return instrs;
}

std::vector<InstructionARM64> construct_multiple_imm12_subs(int64_t imm, u32 register_id) {
  const auto chunks = decompose_into_imm12_chunks(imm);
  std::vector<InstructionARM64> instrs;
  for (const auto& [_imm12, _needs_shift] : chunks) {
    // https://www.scs.stanford.edu/~zyedidia/arm64/sub_addsub_imm.html
    // SUB <Xd|SP>, <Xn|SP>, #<imm>{, <shift>}
    instrs.emplace_back(InstructionARM64(Base(0b110100010, 9), Sh(_needs_shift ? 1 : 0),
                                         Imm12(_imm12), Rd(register_id), Rn(register_id)));
  }
  return instrs;
}

//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   MOVES
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

InstructionARM64 mov_gpr64_gpr64(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/mov_orr_log_shift.html
  // MOV <Xd>, <Xm>
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(src.is_gpr(instr_set));
  return InstructionARM64(Base(0b10101010000, 11), Rm(src.id()), Rn(0b11111), Rd(dst.id()),
                          Imm6(0));
}

InstructionARM64 mov_gpr64_u64(Register dst, uint64_t val) {
  // Cannot be done in a single instruction, must combine multiple MOVZ/MOVKs
  std::vector<InstructionARM64> instrs;
  bool emitted_movz = false;
  for (int i = 0; i < 4; i++) {
    u16 chunk = (val >> (i * 16)) & 0xFFFF;
    if (!emitted_movz && chunk != 0) {
      // https://www.scs.stanford.edu/~zyedidia/arm64/movz.html
      // MOVZ <Xd>, #<imm>{, LSL #<shift>/16}
      instrs.emplace_back(
          InstructionARM64(Base(0b110100101, 9), Hw(i), Imm16(chunk), Rd(dst.id())));
      emitted_movz = true;
    } else if (emitted_movz && chunk != 0) {
      // https://www.scs.stanford.edu/~zyedidia/arm64/movk.html
      // MOVK <Xd>, #<imm>{, LSL #<shift>/16}
      instrs.emplace_back(
          InstructionARM64(Base(0b111100101, 9), Hw(i), Imm16(chunk), Rd(dst.id())));
    }
  }
  if (!emitted_movz) {
    // https://www.scs.stanford.edu/~zyedidia/arm64/movz.html
    // MOVZ <Xd>, #<imm>{, LSL #0}
    instrs.emplace_back(InstructionARM64(Base(0b110100101, 9), Hw(0), Imm16(0), Rd(dst.id())));
  }
  return InstructionARM64(instrs);
}

InstructionARM64 mov_gpr64_u32(Register dst, uint64_t val) {
  // x86 mov r64, imm32 zero-extends the 32-bit immediate into the full
  // register.  Replicate that by masking to the low 32 bits.
  return mov_gpr64_u64(dst, (u64)(u32)val);
}

InstructionARM64 mov_gpr64_s32(Register dst, int64_t val) {
  // x86 movsxd-style sign extension of a 32-bit value into the full register.
  return mov_gpr64_u64(dst, (u64)(s64)(s32)(u32)val);
}

InstructionARM64 movd_gpr32_f32(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/fmov_float_gen.html
  // Single-precision to 32-bit (sf == 0 && ftype == 00 && rmode == 00 && opcode == 110)
  // FMOV <Wd>, <Sn>
  ASSERT(dst.is_gpr(instr_set));
  return InstructionARM64(Base(0b0001111000100110000000, 22), Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 movd_f32_gpr32(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/fmov_float_gen.html
  // 32-bit to single-precision (sf == 0 && ftype == 00 && rmode == 00 && opcode == 111)
  // FMOV <Sd>, <Wn>
  ASSERT(src.is_gpr(instr_set));
  return InstructionARM64(Base(0b0001111000100111000000, 22), Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 movq_gpr64_f64(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/fmov_float_gen.html
  // Double-precision to 64-bit (sf == 1 && ftype == 01 && rmode == 00 && opcode == 110)
  // FMOV <Xd>, <Dn>
  ASSERT(dst.is_gpr(instr_set));
  return InstructionARM64(Base(0b1001111001100110000000, 22), Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 movq_f64_gpr64(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/fmov_float_gen.html
  // 64-bit to double-precision (sf == 1 && ftype == 01 && rmode == 00 && opcode == 111)
  // FMOV <Xd>, <Dn>
  ASSERT(src.is_gpr(instr_set));
  return InstructionARM64(Base(0b1001111001100111000000, 22), Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 mov_f32_f32(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/fmov_float.html
  // Single-precision (ftype == 00)
  // FMOV <Sd>, <Sn>
  return InstructionARM64(Base(0b0001111000100000010000, 22), Rn(src.id()), Rd(dst.id()));
}

//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   GOAL Loads and Stores
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

InstructionARM64 load8s_gpr64_gpr64_plus_gpr64(Register dst, Register addr1, Register addr2) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldrsb_reg.html
  // 64-bit with extended register offset (opc == 10 && option != 011)
  // LDRSB <Xt>, [<Xn|SP>, (<Wm>|<Xm>), <extend> {<amount>}]
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  return InstructionARM64(Base(0b0011100010100000011010, 22), Rt(dst.id()), Rn(addr1.id()),
                          Rm(addr2.id()));
}

InstructionARM64 store8_gpr64_gpr64_plus_gpr64(Register addr1, Register addr2, Register value) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/strb_reg.html
  // 64 bit - SXTX
  // strb Wt, [Xn, Xm]
  ASSERT(value.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  return InstructionARM64(Base(0b0011100000100000011010, 22), Rt(value.id()), Rn(addr1.id()),
                          Rm(addr2.id()));
}

// TODO ARM64 - x16 needs to be reserved, started leveraging it here
// yes it would be possible to only reserve it _sometimes_, but keep things simple
// we have SO many more registers already over x86, 1 less isn't going to be that big of a deal

InstructionARM64 load8s_gpr64_gpr64_plus_gpr64_plus_s8(Register dst,
                                                       Register addr1,
                                                       Register addr2,
                                                       s64 offset) {
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT8_MIN && offset <= INT8_MAX);
  return InstructionARM64(
      {// https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_ext.html
       // ADD <Xd|SP>, <Xn|SP>, <R><m>{, <extend> {#<amount>}}
       InstructionARM64(Base(0b1000101100100000111000, 22), Rd(X16), Rn(addr1.id()),
                        Rm(addr2.id())),
       // https://www.scs.stanford.edu/~zyedidia/arm64/ldursb.html
       // LDURSB <Xt>, [<Xn|SP>{, #<simm>}]
       InstructionARM64(Base(0b0011100010000000000000, 22), Imm9s(offset), Rt(dst.id()), Rn(X16))});
}

InstructionARM64 store8_gpr64_gpr64_plus_gpr64_plus_s8(Register addr1,
                                                       Register addr2,
                                                       Register value,
                                                       s64 offset) {
  ASSERT(value.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT8_MIN && offset <= INT8_MAX);
  return InstructionARM64({// https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_ext.html
                           // ADD <Xd|SP>, <Xn|SP>, <R><m>{, <extend> {#<amount>}}
                           InstructionARM64(Base(0b1000101100100000111000, 22), Rd(X16),
                                            Rn(addr1.id()), Rm(addr2.id())),
                           // https://www.scs.stanford.edu/~zyedidia/arm64/sturb.html
                           // STURB <Wt>, [<Xn|SP>{, #<simm>}]
                           InstructionARM64(Base(0b0011100000000000000000, 22), Imm9s(offset),
                                            Rt(value.id()), Rn(X16))});
}

InstructionARM64 load8s_gpr64_gpr64_plus_gpr64_plus_s32(Register dst,
                                                        Register addr1,
                                                        Register addr2,
                                                        s64 offset) {
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT32_MIN && offset <= INT32_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_shift.html
      // ADD <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
      InstructionARM64(Base(0b10001011000, 11), Rd(X16), Imm6(0), Rn(addr1.id()), Rm(addr2.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // finally do the load
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldrsb_imm.html
  // LDRSB <Xt>, [<Xn|SP>], #<simm>
  instrs.emplace_back(InstructionARM64(Base(0b0011100110, 10), Imm12(0), Rt(dst.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 store8_gpr64_gpr64_plus_gpr64_plus_s32(Register addr1,
                                                        Register addr2,
                                                        Register value,
                                                        s64 offset) {
  ASSERT(value.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT32_MIN && offset <= INT32_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_shift.html
      // ADD <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
      InstructionARM64(Base(0b10001011000, 11), Rd(X16), Imm6(0), Rn(addr1.id()), Rm(addr2.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // https://www.scs.stanford.edu/~zyedidia/arm64/strb_imm.html
  // unsigned offset
  // STRB <Wt>, [<Xn|SP>{, #<pimm>}]
  instrs.emplace_back(InstructionARM64(Base(0b0011100100, 10), Imm12(0), Rt(value.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 load8u_gpr64_gpr64_plus_gpr64(Register dst, Register addr1, Register addr2) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldrb_reg.html
  // SXTX extend option
  // LDRB <Wt>, [<Xn|SP>, <Xm>{, LSL <amount>}]
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  return InstructionARM64(Base(0b0011100001100000111010, 22), Rt(dst.id()), Rn(addr1.id()),
                          Rm(addr2.id()));
}

InstructionARM64 load8u_gpr64_gpr64_plus_gpr64_plus_s8(Register dst,
                                                       Register addr1,
                                                       Register addr2,
                                                       s64 offset) {
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT8_MIN && offset <= INT8_MAX);
  std::vector<InstructionARM64> instrs;
  if (offset > 0) {
    instrs = {// https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_ext.html
              // ADD <Xd|SP>, <Xn|SP>, <R><m>{, <extend> {#<amount>}}
              InstructionARM64(Base(0b1000101100100000111000, 22), Rd(X16), Rn(addr1.id()),
                               Rm(addr2.id())),
              // https://www.scs.stanford.edu/~zyedidia/arm64/ldrb_imm.html
              // Unsigned offset mode
              // LDRB <Xt>, [<Xn|SP>], #<simm>
              InstructionARM64(Base(0b0011100101, 10), Imm12(offset), Rt(dst.id()), Rn(X16))};
  } else {
    instrs = {
        // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_ext.html
        // ADD <Xd|SP>, <Xn|SP>, <R><m>{, <extend> {#<amount>}}
        InstructionARM64(Base(0b1000101100100000111000, 22), Rd(X16), Rn(addr1.id()),
                         Rm(addr2.id())),
        // https://www.scs.stanford.edu/~zyedidia/arm64/ldurb.html
        // LDURB <Xt>, [<Xn|SP>{, #<simm>}]
        InstructionARM64(Base(0b0011100001000000000000, 22), Imm9s(offset), Rt(dst.id()), Rn(X16))};
  }
  return InstructionARM64(instrs);
}

InstructionARM64 load8u_gpr64_gpr64_plus_gpr64_plus_s32(Register dst,
                                                        Register addr1,
                                                        Register addr2,
                                                        s64 offset) {
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT32_MIN && offset <= INT32_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_shift.html
      // ADD <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
      InstructionARM64(Base(0b10001011000, 11), Rd(X16), Imm6(0), Rn(addr1.id()), Rm(addr2.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // finally do the load
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldrb_imm.html
  // LDRB <Xt>, [<Xn|SP>], #<simm>
  instrs.emplace_back(
      InstructionARM64(Base(0b0011100101, 10), Imm12(0), Rt(dst.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 load16s_gpr64_gpr64_plus_gpr64(Register dst, Register addr1, Register addr2) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldrsh_reg.html
  // LDRSH <Xt>, [<Xn|SP>, (<Wm>|<Xm>){, <extend> {<amount>}}]
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  return InstructionARM64(Base(0b0111100010100000011010, 22), Rt(dst.id()), Rn(addr1.id()),
                          Rm(addr2.id()));
}

InstructionARM64 store16_gpr64_gpr64_plus_gpr64(Register addr1, Register addr2, Register value) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/strh_reg.html
  // STRH <Wt>, [<Xn|SP>, (<Wm>|<Xm>){, <extend> {<amount>}}]
  ASSERT(value.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  return InstructionARM64(Base(0b0111100000100000011010, 22), Rt(value.id()), Rn(addr1.id()),
                          Rm(addr2.id()));
}

InstructionARM64 store16_gpr64_gpr64_plus_gpr64_plus_s8(Register addr1,
                                                        Register addr2,
                                                        Register value,
                                                        s64 offset) {
  ASSERT(value.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT8_MIN && offset <= INT8_MAX);
  return InstructionARM64({// https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_ext.html
                           // ADD <Xd|SP>, <Xn|SP>, <R><m>{, <extend> {#<amount>}}
                           InstructionARM64(Base(0b1000101100100000111000, 22), Rd(X16),
                                            Rn(addr1.id()), Rm(addr2.id())),
                           // https://www.scs.stanford.edu/~zyedidia/arm64/sturh.html
                           // STURH <Wt>, [<Xn|SP>{, #<simm>}]
                           InstructionARM64(Base(0b0111100000000000000000, 22), Imm9s(offset),
                                            Rt(value.id()), Rn(X16))});
}

InstructionARM64 store16_gpr64_gpr64_plus_gpr64_plus_s32(Register addr1,
                                                         Register addr2,
                                                         Register value,
                                                         s64 offset) {
  ASSERT(value.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT32_MIN && offset <= INT32_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_shift.html
      // ADD <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
      InstructionARM64(Base(0b10001011000, 11), Rd(X16), Imm6(0), Rn(addr1.id()), Rm(addr2.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // finally do the load
  // https://www.scs.stanford.edu/~zyedidia/arm64/strh_imm.html
  // STRH <Wt>, [<Xn|SP>{, #<pimm>}]
  instrs.emplace_back(InstructionARM64(Base(0b0111100100, 10), Imm12(0), Rt(value.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 load16s_gpr64_gpr64_plus_gpr64_plus_s8(Register dst,
                                                        Register addr1,
                                                        Register addr2,
                                                        s64 offset) {
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT8_MIN && offset <= INT8_MAX);
  return InstructionARM64(
      {// https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_ext.html
       // ADD <Xd|SP>, <Xn|SP>, <R><m>{, <extend> {#<amount>}}
       InstructionARM64(Base(0b1000101100100000111000, 22), Rd(X16), Rn(addr1.id()),
                        Rm(addr2.id())),
       // https://www.scs.stanford.edu/~zyedidia/arm64/ldursh.html
       // LDURSH <Xt>, [<Xn|SP>{, #<simm>}]
       InstructionARM64(Base(0b0111100010000000000000, 22), Imm9s(offset), Rt(dst.id()), Rn(X16))});
}

InstructionARM64 load16s_gpr64_gpr64_plus_gpr64_plus_s32(Register dst,
                                                         Register addr1,
                                                         Register addr2,
                                                         s64 offset) {
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT32_MIN && offset <= INT32_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_shift.html
      // ADD <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
      InstructionARM64(Base(0b10001011000, 11), Rd(X16), Imm6(0), Rn(addr1.id()), Rm(addr2.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // finally do the load
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldrsh_imm.html
  // LDRSH <Xt>, [<Xn|SP>], #<simm>
  instrs.emplace_back(InstructionARM64(Base(0b0111100110, 10), Imm12(0), Rt(dst.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 load16u_gpr64_gpr64_plus_gpr64(Register dst, Register addr1, Register addr2) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldrh_reg.html
  // SXTX extend option
  // LDRH <Wt>, [<Xn|SP>, <Xm>{, LSL <amount>}]
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  return InstructionARM64(Base(0b0111100001100000011010, 22), Rt(dst.id()), Rn(addr1.id()),
                          Rm(addr2.id()));
}

InstructionARM64 load16u_gpr64_gpr64_plus_gpr64_plus_s8(Register dst,
                                                        Register addr1,
                                                        Register addr2,
                                                        s64 offset) {
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT8_MIN && offset <= INT8_MAX);
  std::vector<InstructionARM64> instrs;
  if (offset > 0) {
    instrs = {// https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_ext.html
              // ADD <Xd|SP>, <Xn|SP>, <R><m>{, <extend> {#<amount>}}
              InstructionARM64(Base(0b1000101100100000111000, 22), Rd(X16), Rn(addr1.id()),
                               Rm(addr2.id())),
              // LDURH is unscaled, unlike LDRH whose pimm is scaled by 2.
              InstructionARM64(Base(0b0111100001000000000000, 22), Imm9s(offset), Rt(dst.id()),
                               Rn(X16))};
  } else {
    instrs = {
        // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_ext.html
        // ADD <Xd|SP>, <Xn|SP>, <R><m>{, <extend> {#<amount>}}
        InstructionARM64(Base(0b1000101100100000111000, 22), Rd(X16), Rn(addr1.id()),
                         Rm(addr2.id())),
        // https://www.scs.stanford.edu/~zyedidia/arm64/ldurh.html
        // LDURH <Wt>, [<Xn|SP>{, #<simm>}]
        InstructionARM64(Base(0b0111100001000000000000, 22), Imm9s(offset), Rt(dst.id()), Rn(X16))};
  }
  return InstructionARM64(instrs);
}

InstructionARM64 load16u_gpr64_gpr64_plus_gpr64_plus_s32(Register dst,
                                                         Register addr1,
                                                         Register addr2,
                                                         s64 offset) {
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT32_MIN && offset <= INT32_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_shift.html
      // ADD <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
      InstructionARM64(Base(0b10001011000, 11), Rd(X16), Imm6(0), Rn(addr1.id()), Rm(addr2.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // finally do the load
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldrh_imm.html
  // LDRH <Wt>, [<Xn|SP>{, #<pimm>}]
  instrs.emplace_back(
      InstructionARM64(Base(0b0111100101, 10), Imm12(0), Rt(dst.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 load32s_gpr64_gpr64_plus_gpr64(Register dst, Register addr1, Register addr2) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldrsw_reg.html
  // LDRSW <Xt>, [<Xn|SP>, (<Wm>|<Xm>){, <extend> {<amount>}}]
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  return InstructionARM64(Base(0b1011100010100000011010, 22), Rt(dst.id()), Rn(addr1.id()),
                          Rm(addr2.id()));
}

InstructionARM64 store32_gpr64_gpr64_plus_gpr64(Register addr1, Register addr2, Register value) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/str_reg_gen.html
  // STR <Wt>, [<Xn|SP>, (<Wm>|<Xm>){, <extend> {<amount>}}]
  ASSERT(value.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  return InstructionARM64(Base(0b1011100000100000011010, 22), Rt(value.id()), Rn(addr1.id()),
                          Rm(addr2.id()));
}

InstructionARM64 load32s_gpr64_gpr64_plus_gpr64_plus_s8(Register dst,
                                                        Register addr1,
                                                        Register addr2,
                                                        s64 offset) {
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT8_MIN && offset <= INT8_MAX);
  return InstructionARM64(
      {// https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_ext.html
       // ADD <Xd|SP>, <Xn|SP>, <R><m>{, <extend> {#<amount>}}
       InstructionARM64(Base(0b1000101100100000111000, 22), Rd(X16), Rn(addr1.id()),
                        Rm(addr2.id())),
       // https://www.scs.stanford.edu/~zyedidia/arm64/ldursw.html
       // LDURSW <Xt>, [<Xn|SP>{, #<simm>}]
       InstructionARM64(Base(0b1011100010000000000000, 22), Imm9s(offset), Rt(dst.id()), Rn(X16))});
}

InstructionARM64 store32_gpr64_gpr64_plus_gpr64_plus_s8(Register addr1,
                                                        Register addr2,
                                                        Register value,
                                                        s64 offset) {
  ASSERT(value.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT8_MIN && offset <= INT8_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_shift.html
      // ADD <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
      InstructionARM64(Base(0b10001011000, 11), Rd(X16), Imm6(0), Rn(addr1.id()), Rm(addr2.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // https://www.scs.stanford.edu/~zyedidia/arm64/str_imm_gen.html
  // STR <Wt>, [<Xn|SP>{, #<pimm>}]
  instrs.emplace_back(InstructionARM64(Base(0b1011100100, 10), Imm12(0), Rt(value.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 load32s_gpr64_gpr64_plus_gpr64_plus_s32(Register dst,
                                                         Register addr1,
                                                         Register addr2,
                                                         s64 offset) {
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT32_MIN && offset <= INT32_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_shift.html
      // ADD <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
      InstructionARM64(Base(0b10001011000, 11), Rd(X16), Imm6(0), Rn(addr1.id()), Rm(addr2.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // finally do the load
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldrsw_imm.html
  // LDRSW <Xt>, [<Xn|SP>], #<simm>
  instrs.emplace_back(InstructionARM64(Base(0b1011100110, 10), Imm12(0), Rt(dst.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 store32_gpr64_gpr64_plus_gpr64_plus_s32(Register addr1,
                                                         Register addr2,
                                                         Register value,
                                                         s64 offset) {
  ASSERT(value.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT32_MIN && offset <= INT32_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_shift.html
      // ADD <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
      InstructionARM64(Base(0b10001011000, 11), Rd(X16), Imm6(0), Rn(addr1.id()), Rm(addr2.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // https://www.scs.stanford.edu/~zyedidia/arm64/str_imm_gen.html
  // STR <Wt>, [<Xn|SP>{, #<pimm>}]
  instrs.emplace_back(InstructionARM64(Base(0b1011100100, 10), Imm12(0), Rt(value.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 load32u_gpr64_gpr64_plus_gpr64(Register dst, Register addr1, Register addr2) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldr_reg_gen.html
  // 32-bit variant
  // LDR <Wt>, [<Xn|SP>, (<Wm>|<Xm>){, <extend> {<amount>}}]
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  return InstructionARM64(Base(0b1011100001100000011010, 22), Rt(dst.id()), Rn(addr1.id()),
                          Rm(addr2.id()));
}

InstructionARM64 load32u_gpr64_gpr64_plus_gpr64_plus_s8(Register dst,
                                                        Register addr1,
                                                        Register addr2,
                                                        s64 offset) {
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT8_MIN && offset <= INT8_MAX);
  return InstructionARM64(
      {// https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_ext.html
       // ADD <Xd|SP>, <Xn|SP>, <R><m>{, <extend> {#<amount>}}
       InstructionARM64(Base(0b1000101100100000111000, 22), Rd(X16), Rn(addr1.id()),
                        Rm(addr2.id())),
       // https://www.scs.stanford.edu/~zyedidia/arm64/ldur_gen.html
       // 32 bit
       // LDUR <Wt>, [<Xn|SP>{, #<simm>}]
       InstructionARM64(Base(0b1011100001000000000000, 22), Imm9s(offset), Rt(dst.id()), Rn(X16))});
}

InstructionARM64 load32u_gpr64_gpr64_plus_gpr64_plus_s32(Register dst,
                                                         Register addr1,
                                                         Register addr2,
                                                         s64 offset) {
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT32_MIN && offset <= INT32_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_shift.html
      // ADD <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
      InstructionARM64(Base(0b10001011000, 11), Rd(X16), Imm6(0), Rn(addr1.id()), Rm(addr2.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // finally do the load
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldr_imm_gen.html
  // 32-bit variant
  // LDR <Wt>, [<Xn|SP>], #<simm>
  instrs.emplace_back(
      InstructionARM64(Base(0b1011100001000000000001, 22), Imm9s(0), Rt(dst.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 load64_gpr64_gpr64_plus_gpr64(Register dst, Register addr1, Register addr2) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldr_reg_gen.html
  // 64 bit mode
  // LDR <Xt>, [<Xn|SP>, (<Wm>|<Xm>){, <extend> {<amount>}}]
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  return InstructionARM64(Base(0b1111100001100000011010, 22), Rt(dst.id()), Rn(addr1.id()),
                          Rm(addr2.id()));
}

InstructionARM64 store64_gpr64_gpr64_plus_gpr64(Register addr1, Register addr2, Register value) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/str_reg_gen.html
  // STR <Xt>, [<Xn|SP>, (<Wm>|<Xm>){, <extend> {<amount>}}]
  ASSERT(value.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  return InstructionARM64(Base(0b1111100000100000011010, 22), Rt(value.id()), Rn(addr1.id()),
                          Rm(addr2.id()));
}

InstructionARM64 load64_gpr64_gpr64_plus_gpr64_plus_s8(Register dst,
                                                       Register addr1,
                                                       Register addr2,
                                                       s64 offset) {
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT8_MIN && offset <= INT8_MAX);
  return InstructionARM64(
      {// https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_ext.html
       // ADD <Xd|SP>, <Xn|SP>, <R><m>{, <extend> {#<amount>}}
       InstructionARM64(Base(0b1000101100100000111000, 22), Rd(X16), Rn(addr1.id()),
                        Rm(addr2.id())),
       // https://www.scs.stanford.edu/~zyedidia/arm64/ldur_gen.html
       // 64 bit
       // LDUR <Xt>, [<Xn|SP>{, #<simm>}]
       InstructionARM64(Base(0b1111100001000000000000, 22), Imm9s(offset), Rt(dst.id()), Rn(X16))});
}

InstructionARM64 store64_gpr64_gpr64_plus_gpr64_plus_s8(Register addr1,
                                                        Register addr2,
                                                        Register value,
                                                        s64 offset) {
  ASSERT(value.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT8_MIN && offset <= INT8_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_shift.html
      // ADD <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
      InstructionARM64(Base(0b10001011000, 11), Rd(X16), Imm6(0), Rn(addr1.id()), Rm(addr2.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // https://www.scs.stanford.edu/~zyedidia/arm64/str_imm_gen.html
  // STR <Xt>, [<Xn|SP>{, #<pimm>}]
  instrs.emplace_back(InstructionARM64(Base(0b1111100100, 10), Imm12(0), Rt(value.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 load64_gpr64_gpr64_plus_gpr64_plus_s32(Register dst,
                                                        Register addr1,
                                                        Register addr2,
                                                        s64 offset) {
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT32_MIN && offset <= INT32_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_shift.html
      // ADD <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
      InstructionARM64(Base(0b10001011000, 11), Rd(X16), Imm6(0), Rn(addr1.id()), Rm(addr2.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // finally do the load
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldr_imm_gen.html
  // 64-bit variant
  // LDR <Xt>, [<Xn|SP>], #<simm>
  instrs.emplace_back(
      InstructionARM64(Base(0b1111100001000000000001, 22), Imm9s(0), Rt(dst.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 store64_gpr64_gpr64_plus_gpr64_plus_s32(Register addr1,
                                                         Register addr2,
                                                         Register value,
                                                         s64 offset) {
  ASSERT(value.is_gpr(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT32_MIN && offset <= INT32_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_shift.html
      // ADD <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
      InstructionARM64(Base(0b10001011000, 11), Rd(X16), Imm6(0), Rn(addr1.id()), Rm(addr2.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // https://www.scs.stanford.edu/~zyedidia/arm64/str_imm_gen.html
  // STR <Xt>, [<Xn|SP>{, #<pimm>}]
  instrs.emplace_back(InstructionARM64(Base(0b1111100100, 10), Imm12(0), Rt(value.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 load64_gpr64_plus_s32(Register dst_reg, int32_t offset, Register src_reg) {
  ASSERT(dst_reg.is_gpr(instr_set));
  ASSERT(src_reg.is_gpr(instr_set));
  ASSERT(src_reg != SP);
  ASSERT(offset >= INT32_MIN && offset <= INT32_MAX);
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_imm.html
      // ADD <Xd|SP>, <Xn|SP>, #<imm>{, <shift>}
      InstructionARM64(Base(0b100100010, 9), Sh(0), Imm12(0), Rd(X16), Rn(src_reg.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // finally do the load
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldr_imm_gen.html
  // 64-bit variant
  // LDR <Xt>, [<Xn|SP>], #<simm>
  instrs.emplace_back(
      InstructionARM64(Base(0b1111100001000000000001, 22), Imm9s(0), Rt(dst_reg.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 store64_gpr64_plus_s32(Register addr, int32_t offset, Register value) {
  ASSERT(value.is_gpr(instr_set));
  ASSERT(addr.is_gpr(instr_set));
  ASSERT(addr != SP);
  ASSERT(offset >= INT32_MIN && offset <= INT32_MAX);
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_imm.html
      // ADD <Xd|SP>, <Xn|SP>, #<imm>{, <shift>}
      InstructionARM64(Base(0b100100010, 9), Sh(0), Imm12(0), Rd(X16), Rn(addr.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // https://www.scs.stanford.edu/~zyedidia/arm64/str_imm_gen.html
  // STR <Xt>, [<Xn|SP>{, #<pimm>}]
  instrs.emplace_back(InstructionARM64(Base(0b1111100100, 10), Imm12(0), Rt(value.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 store_goal_vf(Register addr, Register value, Register off, s64 offset) {
  if (offset == 0) {
    return storevf_gpr64_plus_gpr64(value, addr, off);
  } else if (offset >= INT8_MIN && offset <= INT8_MAX) {
    return storevf_gpr64_plus_gpr64_plus_s8(value, addr, off, offset);
  } else if (offset >= INT32_MIN && offset <= INT32_MAX) {
    return storevf_gpr64_plus_gpr64_plus_s32(value, addr, off, offset);
  }
  ASSERT(false);
  return {0};
}

InstructionARM64 store_goal_gpr(Register addr, Register value, Register off, int offset, int size) {
  switch (size) {
    case 1:
      if (offset == 0) {
        return store8_gpr64_gpr64_plus_gpr64(addr, off, value);
      } else if (offset >= INT8_MIN && offset <= INT8_MAX) {
        return store8_gpr64_gpr64_plus_gpr64_plus_s8(addr, off, value, offset);
      } else if (offset >= INT32_MIN && offset <= INT32_MAX) {
        return store8_gpr64_gpr64_plus_gpr64_plus_s32(addr, off, value, offset);
      } else {
        ASSERT(false);
      }
    case 2:
      if (offset == 0) {
        return store16_gpr64_gpr64_plus_gpr64(addr, off, value);
      } else if (offset >= INT8_MIN && offset <= INT8_MAX) {
        return store16_gpr64_gpr64_plus_gpr64_plus_s8(addr, off, value, offset);
      } else if (offset >= INT32_MIN && offset <= INT32_MAX) {
        return store16_gpr64_gpr64_plus_gpr64_plus_s32(addr, off, value, offset);
      } else {
        ASSERT(false);
      }
    case 4:
      if (offset == 0) {
        return store32_gpr64_gpr64_plus_gpr64(addr, off, value);
      } else if (offset >= INT8_MIN && offset <= INT8_MAX) {
        return store32_gpr64_gpr64_plus_gpr64_plus_s8(addr, off, value, offset);
      } else if (offset >= INT32_MIN && offset <= INT32_MAX) {
        return store32_gpr64_gpr64_plus_gpr64_plus_s32(addr, off, value, offset);
      } else {
        ASSERT(false);
      }
    case 8:
      if (offset == 0) {
        return store64_gpr64_gpr64_plus_gpr64(addr, off, value);
      } else if (offset >= INT8_MIN && offset <= INT8_MAX) {
        return store64_gpr64_gpr64_plus_gpr64_plus_s8(addr, off, value, offset);
      } else if (offset >= INT32_MIN && offset <= INT32_MAX) {
        return store64_gpr64_gpr64_plus_gpr64_plus_s32(addr, off, value, offset);
      } else {
        ASSERT(false);
      }
    default:
      ASSERT(false);
      return {0};
  }
}

InstructionARM64 load_goal_xmm128(Register dst, Register addr, Register off, int offset) {
  if (offset == 0) {
    return loadvf_gpr64_plus_gpr64(dst, addr, off);
  } else if (offset >= INT8_MIN && offset <= INT8_MAX) {
    return loadvf_gpr64_plus_gpr64_plus_s8(dst, addr, off, offset);
  } else if (offset >= INT32_MIN && offset <= INT32_MAX) {
    return loadvf_gpr64_plus_gpr64_plus_s32(dst, addr, off, offset);
  } else {
    ASSERT(false);
    return {0};
  }
}

InstructionARM64 load_goal_gpr(Register dst,
                               Register addr,
                               Register off,
                               int offset,
                               int size,
                               bool sign_extend) {
  switch (size) {
    case 1:
      if (offset == 0) {
        if (sign_extend) {
          return load8s_gpr64_gpr64_plus_gpr64(dst, addr, off);
        } else {
          return load8u_gpr64_gpr64_plus_gpr64(dst, addr, off);
        }
      } else if (offset >= INT8_MIN && offset <= INT8_MAX) {
        if (sign_extend) {
          return load8s_gpr64_gpr64_plus_gpr64_plus_s8(dst, addr, off, offset);
        } else {
          return load8u_gpr64_gpr64_plus_gpr64_plus_s8(dst, addr, off, offset);
        }
      } else if (offset >= INT32_MIN && offset <= INT32_MAX) {
        if (sign_extend) {
          return load8s_gpr64_gpr64_plus_gpr64_plus_s32(dst, addr, off, offset);
        } else {
          return load8u_gpr64_gpr64_plus_gpr64_plus_s32(dst, addr, off, offset);
        }
      } else {
        ASSERT(false);
      }
    case 2:
      if (offset == 0) {
        if (sign_extend) {
          return load16s_gpr64_gpr64_plus_gpr64(dst, addr, off);
        } else {
          return load16u_gpr64_gpr64_plus_gpr64(dst, addr, off);
        }
      } else if (offset >= INT8_MIN && offset <= INT8_MAX) {
        if (sign_extend) {
          return load16s_gpr64_gpr64_plus_gpr64_plus_s8(dst, addr, off, offset);
        } else {
          return load16u_gpr64_gpr64_plus_gpr64_plus_s8(dst, addr, off, offset);
        }
      } else if (offset >= INT32_MIN && offset <= INT32_MAX) {
        if (sign_extend) {
          return load16s_gpr64_gpr64_plus_gpr64_plus_s32(dst, addr, off, offset);
        } else {
          return load16u_gpr64_gpr64_plus_gpr64_plus_s32(dst, addr, off, offset);
        }
      } else {
        ASSERT(false);
      }
    case 4:
      if (offset == 0) {
        if (sign_extend) {
          return load32s_gpr64_gpr64_plus_gpr64(dst, addr, off);
        } else {
          return load32u_gpr64_gpr64_plus_gpr64(dst, addr, off);
        }
      } else if (offset >= INT8_MIN && offset <= INT8_MAX) {
        if (sign_extend) {
          return load32s_gpr64_gpr64_plus_gpr64_plus_s8(dst, addr, off, offset);
        } else {
          return load32u_gpr64_gpr64_plus_gpr64_plus_s8(dst, addr, off, offset);
        }
      } else if (offset >= INT32_MIN && offset <= INT32_MAX) {
        if (sign_extend) {
          return load32s_gpr64_gpr64_plus_gpr64_plus_s32(dst, addr, off, offset);
        } else {
          return load32u_gpr64_gpr64_plus_gpr64_plus_s32(dst, addr, off, offset);
        }
      } else {
        ASSERT(false);
      }
    case 8:
      if (offset == 0) {
        return load64_gpr64_gpr64_plus_gpr64(dst, addr, off);

      } else if (offset >= INT8_MIN && offset <= INT8_MAX) {
        return load64_gpr64_gpr64_plus_gpr64_plus_s8(dst, addr, off, offset);

      } else if (offset >= INT32_MIN && offset <= INT32_MAX) {
        return load64_gpr64_gpr64_plus_gpr64_plus_s32(dst, addr, off, offset);

      } else {
        ASSERT(false);
      }
    default:
      ASSERT(false);
      return {0};
  }
}

InstructionARM64 lea_reg_plus_off32(Register dest, Register base, s64 offset) {
  ASSERT(dest.is_gpr(instr_set));
  ASSERT(base.is_gpr(instr_set));
  ASSERT(offset >= INT32_MIN && offset <= INT32_MAX);
  // first establish the base value in our destination register
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_imm.html
      // ADD <Xd|SP>, <Xn|SP>, #<imm>{, <shift>}
      InstructionARM64(Base(0b100100010, 9), Sh(0), Imm12(0), Rd(dest.id()), Rn(base.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, dest.id());
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, dest.id());
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  return InstructionARM64(instrs);
}

InstructionARM64 lea_reg_plus_off8(Register dest, Register base, s64 offset) {
  ASSERT(dest.is_gpr(instr_set));
  ASSERT(base.is_gpr(instr_set));
  ASSERT(offset >= INT8_MIN && offset <= INT8_MAX);
  // first establish the base value in our destination register
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_imm.html
      // ADD <Xd|SP>, <Xn|SP>, #<imm>{, <shift>}
      InstructionARM64(Base(0b100100010, 9), Sh(0), Imm12(0), Rd(dest.id()), Rn(base.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, dest.id());
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, dest.id());
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  return InstructionARM64(instrs);
}

InstructionARM64 lea_reg_plus_off(Register dest, Register base, s64 offset) {
  if (offset >= INT8_MIN && offset <= INT8_MAX) {
    return lea_reg_plus_off8(dest, base, offset);
  } else if (offset >= INT32_MIN && offset <= INT32_MAX) {
    return lea_reg_plus_off32(dest, base, offset);
  } else {
    ASSERT(false);
    return {0};
  }
}

//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   LOADS n' STORES - XMM32
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

// TODO - rename these to f32 instead of xmm

InstructionARM64 store32_xmm32_gpr64_plus_gpr64(Register addr1,
                                                Register addr2,
                                                Register xmm_value) {
  ASSERT(xmm_value.is_128bit_simd(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  // https://www.scs.stanford.edu/~zyedidia/arm64/str_reg_fpsimd.html
  // 32-bit variant
  // STR <St>, [<Xn|SP>, (<Wm>|<Xm>){, <extend> {<amount>}}]
  return InstructionARM64(Base(0b1011110000100000011010, 22), Rt(xmm_value.id()), Rn(addr1.id()),
                          Rm(addr2.id()));
}

InstructionARM64 load32_xmm32_gpr64_plus_gpr64(Register simd_dest, Register addr1, Register addr2) {
  ASSERT(simd_dest.is_128bit_simd(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldr_reg_fpsimd.html
  // 32-bit variant
  // LDR <St>, [<Xn|SP>, (<Wm>|<Xm>){, <extend> {<amount>}}]
  return InstructionARM64(Base(0b1011110001100000011010, 22), Rt(simd_dest.id()), Rn(addr1.id()),
                          Rm(addr2.id()));
}

InstructionARM64 store32_xmm32_gpr64_plus_gpr64_plus_s8(Register addr1,
                                                        Register addr2,
                                                        Register xmm_value,
                                                        s64 offset) {
  ASSERT(xmm_value.is_128bit_simd(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(offset >= INT8_MIN && offset <= INT8_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_shift.html
      // ADD <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
      InstructionARM64(Base(0b10001011000, 11), Rd(X16), Imm6(0), Rn(addr1.id()), Rm(addr2.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // https://www.scs.stanford.edu/~zyedidia/arm64/str_imm_fpsimd.html
  // 32-bit variant
  // STR <St>, [<Xn|SP>], #<simm>
  instrs.emplace_back(
      InstructionARM64(Base(0b1011110100000000000000, 22), Imm12(0), Rt(xmm_value.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 load32_xmm32_gpr64_plus_gpr64_plus_s8(Register simd_dest,
                                                       Register addr1,
                                                       Register addr2,
                                                       s64 offset) {
  ASSERT(simd_dest.is_128bit_simd(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(offset >= INT8_MIN && offset <= INT8_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_shift.html
      // ADD <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
      InstructionARM64(Base(0b10001011000, 11), Rd(X16), Imm6(0), Rn(addr1.id()), Rm(addr2.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldr_imm_fpsimd.html
  // 32-bit variant
  // LDR <St>, [<Xn|SP>{, #<pimm>}]
  instrs.emplace_back(
      InstructionARM64(Base(0b1011110101, 10), Imm12(0), Rt(simd_dest.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 store32_xmm32_gpr64_plus_gpr64_plus_s32(Register addr1,
                                                         Register addr2,
                                                         Register xmm_value,
                                                         s64 offset) {
  ASSERT(xmm_value.is_128bit_simd(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(offset >= INT32_MIN && offset <= INT32_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_shift.html
      // ADD <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
      InstructionARM64(Base(0b10001011000, 11), Rd(X16), Imm6(0), Rn(addr1.id()), Rm(addr2.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // https://www.scs.stanford.edu/~zyedidia/arm64/str_imm_fpsimd.html
  // 32-bit variant
  // STR <St>, [<Xn|SP>], #<simm>
  instrs.emplace_back(
      InstructionARM64(Base(0b1011110100000000000000, 22), Imm12(0), Rt(xmm_value.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 store32_xmm32_gpr64_plus_s32(Register base, Register xmm_value, s64 offset) {
  ASSERT(xmm_value.is_128bit_simd(instr_set));
  ASSERT(base.is_gpr(instr_set));
  ASSERT(offset >= INT32_MIN && offset <= INT32_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_imm.html
      // ADD <Xd|SP>, <Xn|SP>, #<imm>{, <shift>}
      InstructionARM64(Base(0b100100010, 9), Sh(0), Imm12(0), Rd(X16), Rn(base.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // https://www.scs.stanford.edu/~zyedidia/arm64/str_imm_fpsimd.html
  // 32-bit variant
  // STR <St>, [<Xn|SP>], #<simm>
  instrs.emplace_back(
      InstructionARM64(Base(0b1011110100000000000000, 22), Imm12(0), Rt(xmm_value.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 store32_xmm32_gpr64_plus_s8(Register base, Register xmm_value, s64 offset) {
  ASSERT(xmm_value.is_128bit_simd(instr_set));
  ASSERT(base.is_gpr(instr_set));
  ASSERT(offset >= INT8_MIN && offset <= INT8_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_imm.html
      // ADD <Xd|SP>, <Xn|SP>, #<imm>{, <shift>}
      InstructionARM64(Base(0b100100010, 9), Sh(0), Imm12(0), Rd(X16), Rn(base.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // https://www.scs.stanford.edu/~zyedidia/arm64/str_imm_fpsimd.html
  // 32-bit variant, unsigned
  // STR <St>, [<Xn|SP>], #<simm>
  instrs.emplace_back(
      InstructionARM64(Base(0b1011110100000000000000, 22), Imm12(0), Rt(xmm_value.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 load32_xmm32_gpr64_plus_gpr64_plus_s32(Register simd_dest,
                                                        Register addr1,
                                                        Register addr2,
                                                        s64 offset) {
  ASSERT(simd_dest.is_128bit_simd(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(offset >= INT32_MIN && offset <= INT32_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_shift.html
      // ADD <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
      InstructionARM64(Base(0b10001011000, 11), Rd(X16), Imm6(0), Rn(addr1.id()), Rm(addr2.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldr_imm_fpsimd.html
  // 32-bit variant
  // LDR <St>, [<Xn|SP>{, #<pimm>}]
  instrs.emplace_back(
      InstructionARM64(Base(0b1011110101, 10), Imm12(0), Rt(simd_dest.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 load32_xmm32_gpr64_plus_s32(Register simd_dest, Register base, s64 offset) {
  ASSERT(simd_dest.is_128bit_simd(instr_set));
  ASSERT(base.is_gpr(instr_set));
  ASSERT(offset >= INT32_MIN && offset <= INT32_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_imm.html
      // ADD <Xd|SP>, <Xn|SP>, #<imm>{, <shift>}
      InstructionARM64(Base(0b100100010, 9), Sh(0), Imm12(0), Rd(X16), Rn(base.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldr_imm_fpsimd.html
  // 32-bit variant
  // LDR <St>, [<Xn|SP>{, #<pimm>}]
  instrs.emplace_back(
      InstructionARM64(Base(0b1011110101, 10), Imm12(0), Rt(simd_dest.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 load32_xmm32_gpr64_plus_s8(Register simd_dest, Register base, s64 offset) {
  ASSERT(simd_dest.is_128bit_simd(instr_set));
  ASSERT(base.is_gpr(instr_set));
  ASSERT(offset >= INT8_MIN && offset <= INT8_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_imm.html
      // ADD <Xd|SP>, <Xn|SP>, #<imm>{, <shift>}
      InstructionARM64(Base(0b100100010, 9), Sh(0), Imm12(0), Rd(X16), Rn(base.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldr_imm_fpsimd.html
  // 32-bit variant
  // LDR <St>, [<Xn|SP>{, #<pimm>}]
  instrs.emplace_back(
      InstructionARM64(Base(0b1011110101, 10), Imm12(0), Rt(simd_dest.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 load_goal_xmm32(Register simd_dest, Register addr, Register off, s64 offset) {
  if (offset == 0) {
    return load32_xmm32_gpr64_plus_gpr64(simd_dest, addr, off);
  } else if (offset >= INT8_MIN && offset <= INT8_MAX) {
    return load32_xmm32_gpr64_plus_gpr64_plus_s8(simd_dest, addr, off, offset);
  } else if (offset >= INT32_MIN && offset <= INT32_MAX) {
    return load32_xmm32_gpr64_plus_gpr64_plus_s32(simd_dest, addr, off, offset);
  } else {
    ASSERT(false);
    return {0};
  }
}

InstructionARM64 store_goal_xmm32(Register addr, Register xmm_value, Register off, s64 offset) {
  if (offset == 0) {
    return store32_xmm32_gpr64_plus_gpr64(addr, off, xmm_value);
  } else if (offset >= INT8_MIN && offset <= INT8_MAX) {
    return store32_xmm32_gpr64_plus_gpr64_plus_s8(addr, off, xmm_value, offset);
  } else if (offset >= INT32_MIN && offset <= INT32_MAX) {
    return store32_xmm32_gpr64_plus_gpr64_plus_s32(addr, off, xmm_value, offset);
  } else {
    ASSERT(false);
    return {0};
  }
}

InstructionARM64 store_reg_offset_xmm32(Register base, Register xmm_value, s64 offset) {
  if (offset >= INT8_MIN && offset <= INT8_MAX) {
    return store32_xmm32_gpr64_plus_s8(base, xmm_value, offset);
  } else if (offset >= INT32_MIN && offset <= INT32_MAX) {
    return store32_xmm32_gpr64_plus_s32(base, xmm_value, offset);
  } else {
    ASSERT(false);
    return {0};
  }
}

InstructionARM64 load_reg_offset_xmm32(Register simd_dest, Register base, s64 offset) {
  if (offset >= INT8_MIN && offset <= INT8_MAX) {
    return load32_xmm32_gpr64_plus_s8(simd_dest, base, offset);
  } else if (offset >= INT32_MIN && offset <= INT32_MAX) {
    return load32_xmm32_gpr64_plus_s32(simd_dest, base, offset);
  } else {
    ASSERT(false);
    return {0};
  }
}

//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   LOADS n' STORES - SIMD (128-bit, QWORDS)
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

InstructionARM64 store128_gpr64_simd128(Register gpr_addr, Register simd_reg) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/str_imm_fpsimd.html
  // - STR Qn, [Xn] (unsigned offset)
  ASSERT(gpr_addr.is_gpr(instr_set));
  ASSERT(
      simd_reg.is_128bit_simd(instr_set));  // TODO ARM64 - this assertion isn't as useful for ARM
                                            // since Q registers are not unique in terms of their id
  return InstructionARM64(Base(0b0011110110, 10), Rn(gpr_addr.id()), Rt(simd_reg.id()), Imm12(0));
}

InstructionARM64 store128_gpr64_simd128_s32(Register gpr_addr, Register xmm_value, s64 offset) {
  ASSERT(gpr_addr.is_gpr(instr_set));
  ASSERT(xmm_value.is_128bit_simd(
      instr_set));  // TODO ARM64 - this assertion isn't as useful for ARM
                    // since Q registers are not unique in terms of their id
  ASSERT(offset >= INT32_MIN && offset <= INT32_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_imm.html
      // ADD <Xd|SP>, <Xn|SP>, #<imm>{, <shift>}
      InstructionARM64(Base(0b100100010, 9), Sh(0), Imm12(0), Rd(X16), Rn(gpr_addr.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // https://www.scs.stanford.edu/~zyedidia/arm64/str_imm_fpsimd.html
  // 128-bit variant, unsigned offset
  // STR <Qt>, [<Xn|SP>{, #<pimm>}]
  instrs.emplace_back(
      InstructionARM64(Base(0b0011110110, 10), Imm12(0), Rt(xmm_value.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 store128_gpr64_simd128_s8(Register gpr_addr, Register xmm_value, s64 offset) {
  ASSERT(gpr_addr.is_gpr(instr_set));
  ASSERT(xmm_value.is_128bit_simd(
      instr_set));  // TODO ARM64 - this assertion isn't as useful for ARM
                    // since Q registers are not unique in terms of their id
  ASSERT(offset >= INT8_MIN && offset <= INT8_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_imm.html
      // ADD <Xd|SP>, <Xn|SP>, #<imm>{, <shift>}
      InstructionARM64(Base(0b100100010, 9), Sh(0), Imm12(0), Rd(X16), Rn(gpr_addr.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // https://www.scs.stanford.edu/~zyedidia/arm64/str_imm_fpsimd.html
  // 128-bit variant, unsigned offset
  // STR <Qt>, [<Xn|SP>{, #<pimm>}]
  instrs.emplace_back(
      InstructionARM64(Base(0b0011110110, 10), Imm12(0), Rt(xmm_value.id()), Rn(X16)));
  return InstructionARM64(instrs);
}

InstructionARM64 load128_simd128_gpr64(Register simd_dest, Register gpr_addr) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldr_imm_fpsimd.html
  // - LDR <Qt>, [<Xn|SP>{, #<pimm>}]
  ASSERT(gpr_addr.is_gpr(instr_set));
  ASSERT(simd_dest.is_128bit_simd(
      instr_set));  // TODO ARM64 - this assertion isn't as useful for ARM
                    // since Q registers are not unique in terms of their id
  return InstructionARM64(Base(0b0011110111, 10), Rn(gpr_addr.id()), Rt(simd_dest.id()), Imm12(0));
}

InstructionARM64 load128_simd128_gpr64_s32(Register simd_dest, Register gpr_addr, s64 offset) {
  ASSERT(gpr_addr.is_gpr(instr_set));
  ASSERT(simd_dest.is_128bit_simd(
      instr_set));  // TODO ARM64 - this assertion isn't as useful for ARM
                    // since Q registers are not unique in terms of their id
  ASSERT(offset >= INT32_MIN && offset <= INT32_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_imm.html
      // ADD <Xd|SP>, <Xn|SP>, #<imm>{, <shift>}
      InstructionARM64(Base(0b100100010, 9), Sh(0), Imm12(0), Rd(X16), Rn(gpr_addr.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldr_imm_fpsimd.html
  // - LDR <Qt>, [<Xn|SP>{, #<pimm>}]
  instrs.emplace_back(
      InstructionARM64(Base(0b0011110111, 10), Rn(X16), Rt(simd_dest.id()), Imm12(0)));
  return InstructionARM64(instrs);
}

InstructionARM64 load128_simd128_gpr64_s8(Register simd_dest, Register gpr_addr, s64 offset) {
  ASSERT(gpr_addr.is_gpr(instr_set));
  ASSERT(simd_dest.is_128bit_simd(
      instr_set));  // TODO ARM64 - this assertion isn't as useful for ARM
                    // since Q registers are not unique in terms of their id
  ASSERT(offset >= INT8_MIN && offset <= INT8_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_imm.html
      // ADD <Xd|SP>, <Xn|SP>, #<imm>{, <shift>}
      InstructionARM64(Base(0b100100010, 9), Sh(0), Imm12(0), Rd(X16), Rn(gpr_addr.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldr_imm_fpsimd.html
  // - LDR <Qt>, [<Xn|SP>{, #<pimm>}]
  instrs.emplace_back(
      InstructionARM64(Base(0b0011110111, 10), Rn(X16), Rt(simd_dest.id()), Imm12(0)));
  return InstructionARM64(instrs);
}

InstructionARM64 load128_xmm128_reg_offset(Register simd_dest, Register base, s64 offset) {
  if (offset == 0) {
    return load128_simd128_gpr64(simd_dest, base);
  } else if (offset >= INT8_MIN && offset <= INT8_MAX) {
    return load128_simd128_gpr64_s8(simd_dest, base, offset);
  } else if (offset >= INT32_MIN && offset <= INT32_MAX) {
    return load128_simd128_gpr64_s32(simd_dest, base, offset);
  } else {
    ASSERT(false);
    return {0};
  }
}

InstructionARM64 store128_xmm128_reg_offset(Register base, Register xmm_val, s64 offset) {
  if (offset == 0) {
    return store128_gpr64_simd128(base, xmm_val);
  } else if (offset >= INT8_MIN && offset <= INT8_MAX) {
    return store128_gpr64_simd128_s8(base, xmm_val, offset);
  } else if (offset >= INT32_MIN && offset <= INT32_MAX) {
    return store128_gpr64_simd128_s32(base, xmm_val, offset);
  } else {
    ASSERT(false);
    return {0};
  }
}

//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   PC relative loads and stores
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

// Implement with LDR but that has a 1MB range limit on ARM (not 2GB like on x86)
// Hopefully this is fine, however it could potentially not be if this is loading static data, which
// may not within 1MB of the current instruction -- that all depends on the linker layout.
//
// But keep it simple at first, add good assertions and we'll see what happens when we
// compile for real.

// TODO ARM64 - the offsets here are always 0 at the time the instruction is made,
// then they are patched later.  That patching also needs an assertion.

// const int ARM64_LDR_MIN = -(1 << 18) * 4;
// const int ARM64_LDR_MAX = ((1 << 18) - 1) * 4;

InstructionARM64 load64_pcRel_s32(Register dest, s64 offset) {
  ASSERT(dest.is_gpr(instr_set));
  ASSERT_MSG(offset != 0,
             "PC Relative offset isn't 0 at encoding time, actually encode it properly!");
  // ASSERT_MSG(offset >= ARM64_LDR_MIN && offset <= ARM64_LDR_MAX,
  //            "PC Relative offset is too large for ARM64, fix it.");
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldr_lit_gen.html
  // LDR <Xt>, <label>
  return InstructionARM64(Base(0b01011000, 8), Imm19(offset / 4), Rt(dest.id()));
}

InstructionARM64 load32s_pcRel_s32(Register dest, s64 offset) {
  ASSERT(dest.is_gpr(instr_set));
  ASSERT_MSG(offset != 0,
             "PC Relative offset isn't 0 at encoding time, actually encode it properly!");
  // ASSERT_MSG(offset >= ARM64_LDR_MIN && offset <= ARM64_LDR_MAX,
  //            "PC Relative offset is too large for ARM64, fix it.");
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldrsw_lit.html
  // LDRSW <Xt>, <label>
  return InstructionARM64(Base(0b10011000, 8), Imm19(offset / 4), Rt(dest.id()));
}

InstructionARM64 load32u_pcRel_s32(Register dest, s64 offset) {
  ASSERT(dest.is_gpr(instr_set));
  ASSERT_MSG(offset != 0,
             "PC Relative offset isn't 0 at encoding time, actually encode it properly!");
  // ASSERT_MSG(offset >= ARM64_LDR_MIN && offset <= ARM64_LDR_MAX,
  //            "PC Relative offset is too large for ARM64, fix it.");
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldr_lit_gen.html
  // LDR <Wt>, <label>
  return InstructionARM64(Base(0b00011000, 8), Imm19(offset / 4), Rt(dest.id()));
}

InstructionARM64 load16u_pcRel_s32(Register dest, s64 offset) {
  ASSERT(dest.is_gpr(instr_set));
  // NOTE - the offsets passed into these functions are always `0` and then later patched
  // so im not going to worry about properly encoding the offset
  ASSERT_MSG(offset != 0,
             "PC Relative offset isn't 0 at encoding time, actually encode it properly!");
  // ASSERT_MSG(offset >= ARM64_LDR_MIN && offset <= ARM64_LDR_MAX,
  //            "PC Relative offset is too large for ARM64, fix it.");
  return InstructionARM64(
      {// https://www.scs.stanford.edu/~zyedidia/arm64/adrp.html
       // ADRP <Xd>, <label>
       InstructionARM64(Base(0b100100000000000000000000000, 27), Rd(X16), Immhi(0), Immlo(0)),
       // https://www.scs.stanford.edu/~zyedidia/arm64/ldrh_imm.html
       // LDRH <Wt>, [<Xn|SP>{, #<pimm>}]
       InstructionARM64(Base(0b0111100101, 10), Imm12(offset), Rt(dest.id()), Rn(X16))});
}

InstructionARM64 load16s_pcRel_s32(Register dest, s64 offset) {
  ASSERT(dest.is_gpr(instr_set));
  // NOTE - the offsets passed into these functions are always `0` and then later patched
  // so im not going to worry about properly encoding the offset
  ASSERT_MSG(offset != 0,
             "PC Relative offset isn't 0 at encoding time, actually encode it properly!");
  // ASSERT_MSG(offset >= ARM64_LDR_MIN && offset <= ARM64_LDR_MAX,
  //            "PC Relative offset is too large for ARM64, fix it.");
  return InstructionARM64(
      {// https://www.scs.stanford.edu/~zyedidia/arm64/adrp.html
       // ADRP <Xd>, <label>
       InstructionARM64(Base(0b100100000000000000000000000, 27), Rd(X16), Immhi(0), Immlo(0)),
       // https://www.scs.stanford.edu/~zyedidia/arm64/ldrsh_imm.html
       // LDRSH <Xt>, [<Xn|SP>{, #<pimm>}]
       InstructionARM64(Base(0b0111100110, 10), Imm12(offset), Rt(dest.id()), Rn(X16))});
}

InstructionARM64 load8u_pcRel_s32(Register dest, s64 offset) {
  ASSERT(dest.is_gpr(instr_set));
  // NOTE - the offsets passed into these functions are always `0` and then later patched
  // so im not going to worry about properly encoding the offset
  ASSERT_MSG(offset != 0,
             "PC Relative offset isn't 0 at encoding time, actually encode it properly!");
  // ASSERT_MSG(offset >= ARM64_LDR_MIN && offset <= ARM64_LDR_MAX,
  //            "PC Relative offset is too large for ARM64, fix it.");
  return InstructionARM64(
      {// https://www.scs.stanford.edu/~zyedidia/arm64/adrp.html
       // ADRP <Xd>, <label>
       InstructionARM64(Base(0b100100000000000000000000000, 27), Rd(X16), Immhi(0), Immlo(0)),
       // https://www.scs.stanford.edu/~zyedidia/arm64/ldrb_imm.html
       // LDRB <Wt>, [<Xn|SP>{, #<pimm>}]
       InstructionARM64(Base(0b0011100101, 10), Imm12(offset), Rt(dest.id()), Rn(X16))});
}

InstructionARM64 load8s_pcRel_s32(Register dest, s64 offset) {
  ASSERT(dest.is_gpr(instr_set));
  // NOTE - the offsets passed into these functions are always `0` and then later patched
  // so im not going to worry about properly encoding the offset
  ASSERT_MSG(offset != 0,
             "PC Relative offset isn't 0 at encoding time, actually encode it properly!");
  // ASSERT_MSG(offset >= ARM64_LDR_MIN && offset <= ARM64_LDR_MAX,
  //            "PC Relative offset is too large for ARM64, fix it.");
  return InstructionARM64(
      {// https://www.scs.stanford.edu/~zyedidia/arm64/adrp.html
       // ADRP <Xd>, <label>
       InstructionARM64(Base(0b100100000000000000000000000, 27), Rd(X16), Immhi(0), Immlo(0)),
       // https://www.scs.stanford.edu/~zyedidia/arm64/ldrsb_imm.html
       // LDRSB <Xt>, [<Xn|SP>{, #<pimm>}]
       InstructionARM64(Base(0b0011100110, 10), Imm12(offset), Rt(dest.id()), Rn(X16))});
}

InstructionARM64 static_load(Register dest, s64 offset, int size, bool sign_extend) {
  switch (size) {
    case 1:
      if (sign_extend) {
        return load8s_pcRel_s32(dest, offset);
      } else {
        return load8u_pcRel_s32(dest, offset);
      }
      break;
    case 2:
      if (sign_extend) {
        return load16s_pcRel_s32(dest, offset);
      } else {
        return load16u_pcRel_s32(dest, offset);
      }
      break;
    case 4:
      if (sign_extend) {
        return load32s_pcRel_s32(dest, offset);
      } else {
        return load32u_pcRel_s32(dest, offset);
      }
      break;
    case 8:
      return load64_pcRel_s32(dest, offset);
    default:
      ASSERT(false);
  }
}

// TODO ARM - no direct store instructions, gotta be two and involve a register

InstructionARM64 store64_pcRel_s32(Register src, s64 offset) {
  ASSERT(src.is_gpr(instr_set));
  ASSERT_MSG(offset != 0,
             "PC Relative offset isn't 0 at encoding time, actually encode it properly!");
  return InstructionARM64(
      {// https://www.scs.stanford.edu/~zyedidia/arm64/adrp.html
       // ADRP <Xd>, <label>
       InstructionARM64(Base(0b100100000000000000000000000, 27), Rd(X16), Immhi(0), Immlo(0)),
       // https://www.scs.stanford.edu/~zyedidia/arm64/str_imm_gen.html
       // STR <Xt>, [<Xn|SP>{, #<pimm>}]
       InstructionARM64(Base(0b1111100100, 10), Imm12(offset), Rt(src.id()), Rn(X16))});
}

InstructionARM64 store32_pcRel_s32(Register src, s64 offset) {
  ASSERT(src.is_gpr(instr_set));
  ASSERT_MSG(offset != 0,
             "PC Relative offset isn't 0 at encoding time, actually encode it properly!");
  return InstructionARM64(
      {// https://www.scs.stanford.edu/~zyedidia/arm64/adrp.html
       // ADRP <Xd>, <label>
       InstructionARM64(Base(0b100100000000000000000000000, 27), Rd(X16), Immhi(0), Immlo(0)),
       // https://www.scs.stanford.edu/~zyedidia/arm64/str_imm_gen.html
       // STR <Wt>, [<Xn|SP>{, #<pimm>}]
       InstructionARM64(Base(0b1011100100, 10), Imm12(offset), Rt(src.id()), Rn(X16))});
}

InstructionARM64 store16_pcRel_s32(Register src, s64 offset) {
  ASSERT(src.is_gpr(instr_set));
  ASSERT_MSG(offset != 0,
             "PC Relative offset isn't 0 at encoding time, actually encode it properly!");
  return InstructionARM64(
      {// https://www.scs.stanford.edu/~zyedidia/arm64/adrp.html
       // ADRP <Xd>, <label>
       InstructionARM64(Base(0b100100000000000000000000000, 27), Rd(X16), Immhi(0), Immlo(0)),
       // https://www.scs.stanford.edu/~zyedidia/arm64/strh_imm.html
       // STRH <Wt>, [<Xn|SP>{, #<pimm>}]
       InstructionARM64(Base(0b0111100100, 10), Imm12(offset), Rt(src.id()), Rn(X16))});
}

InstructionARM64 store8_pcRel_s32(Register src, s64 offset) {
  ASSERT(src.is_gpr(instr_set));
  ASSERT_MSG(offset != 0,
             "PC Relative offset isn't 0 at encoding time, actually encode it properly!");
  return InstructionARM64(
      {// https://www.scs.stanford.edu/~zyedidia/arm64/adrp.html
       // ADRP <Xd>, <label>
       InstructionARM64(Base(0b100100000000000000000000000, 27), Rd(X16), Immhi(0), Immlo(0)),
       // https://www.scs.stanford.edu/~zyedidia/arm64/strb_imm.html
       // STRH <Wt>, [<Xn|SP>{, #<pimm>}]
       InstructionARM64(Base(0b0011100100, 10), Imm12(offset), Rt(src.id()), Rn(X16))});
}

InstructionARM64 static_store(Register value, s64 offset, int size) {
  switch (size) {
    case 1:
      return store8_pcRel_s32(value, offset);
    case 2:
      return store16_pcRel_s32(value, offset);
    case 4:
      return store32_pcRel_s32(value, offset);
    case 8:
      return store64_pcRel_s32(value, offset);
    default:
      ASSERT(false);
  }
}

InstructionARM64 static_addr(Register dest, s64 offset) {
  ASSERT(dest.is_gpr(instr_set));
  ASSERT_MSG(offset != 0,
             "PC Relative offset isn't 0 at encoding time, actually encode it properly!");
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldr_lit_gen.html
  // LDR <Xt>, <label>
  return InstructionARM64(Base(0b01011000, 8), Imm19(offset / 4), Rt(dest.id()));
}

InstructionARM64 static_load_f32(Register simd_dest, s64 offset) {
  ASSERT(simd_dest.is_128bit_simd(instr_set));
  ASSERT_MSG(offset != 0,
             "PC Relative offset isn't 0 at encoding time, actually encode it properly!");
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldr_lit_fpsimd.html
  // LDR <St>, <label>
  return InstructionARM64(Base(0b00011100, 8), Imm19(offset / 4), Rt(simd_dest.id()));
}

InstructionARM64 static_store_f32(Register xmm_value, s64 offset) {
  ASSERT(xmm_value.is_128bit_simd(instr_set));
  ASSERT_MSG(offset != 0,
             "PC Relative offset isn't 0 at encoding time, actually encode it properly!");
  return InstructionARM64(
      {// https://www.scs.stanford.edu/~zyedidia/arm64/adrp.html
       // ADRP <Xd>, <label>
       InstructionARM64(Base(0b100100000000000000000000000, 27), Rd(X16), Immhi(0), Immlo(0)),
       // https://www.scs.stanford.edu/~zyedidia/arm64/str_imm_fpsimd.html
       // STR <St>, [<Xn|SP>{, #<pimm>}]
       InstructionARM64(Base(0b1011110100, 10), Imm12(offset), Rt(xmm_value.id()), Rn(X16))});
}

//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   FUNCTION STUFF
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

InstructionARM64 ret() {
  // https://www.scs.stanford.edu/~zyedidia/arm64/ret.html
  // - defaults to using X30 if Rn is absent
  return InstructionARM64(Base(0b1101011001011111000000, 22), Rn(30));
}

InstructionARM64 push_gpr64(Register reg) {
  // ARM64 stack grows down, so we subtract 16 from SP and store the register
  // Equivalent assembly: STR reg, [SP, #-16]!
  // - https://www.scs.stanford.edu/~zyedidia/arm64/str_imm_gen.html
  // We use 16 because in ARM, the stack must be 16-byte aligned.
  // This does mean we are inefficiently using the stack, there are a few better options:
  // - Push in pairs, two registers at a time
  // - Preallocate stack-space
  // But we can't do either of these at this level, this is an optimization that has to come from
  // higher in the stack.  Here we are concerned with just satisfying the need to push a GPR
  ASSERT(reg.is_gpr(instr_set));
  return InstructionARM64(Base(0b1111100000000000000011, 22), Imm9s(-16), Rn(ARM64_REG::SP),
                          Rt(reg.id()));
}

InstructionARM64 pop_gpr64(Register reg) {
  // ldr reg, [sp], #16
  // - https://www.scs.stanford.edu/~zyedidia/arm64/ldr_imm_gen.html
  ASSERT(reg.is_gpr(instr_set));
  return InstructionARM64(Base(0b1111100001000000000001, 22), Imm9s(16), Rn(ARM64_REG::SP),
                          Rt(reg.id()));
}

InstructionARM64 call_r64(Register reg) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/blr.html
  // BLR <Xn>
  ASSERT(reg.is_gpr(instr_set));
  return InstructionARM64(Base(0b1101011000111111000000, 22), Rn(reg.id()));
}

InstructionARM64 jmp_r64(Register reg) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/br.html
  // BR <Xn>
  ASSERT(reg.is_gpr(instr_set));
  return InstructionARM64(Base(0b1101011000011111000000, 22), Rn(reg.id()));
}

//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   INTEGER MATH
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

InstructionARM64 sub_gpr64_imm8s(Register reg, int64_t imm) {
  return sub_gpr64_imm(reg, imm);
}

InstructionARM64 add_gpr64_imm8s(Register reg, int64_t imm) {
  return add_gpr64_imm(reg, imm);
}

InstructionARM64 sub_gpr64_imm32s(Register reg, int64_t imm) {
  return sub_gpr64_imm(reg, imm);
}

InstructionARM64 add_gpr64_imm32s(Register reg, int64_t imm) {
  return add_gpr64_imm(reg, imm);
}

InstructionARM64 add_gpr64_imm(Register reg, int64_t imm) {
  ASSERT(reg.is_gpr(instr_set));
  if (imm < 0) {
    return sub_gpr64_imm(reg, std::abs(imm));
  }
  // Check to see if we can represent this subtraction in a single instruction
  // if not, then we need to emit multiple partial instructions
  const auto [is_single_instr, imm12, needs_shift] = can_encode_single_imm12(imm);
  if (is_single_instr) {
    // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_imm.html
    // ADD <Xd|SP>, <Xn|SP>, #<imm>{, <shift>}
    return InstructionARM64(Base(0b100100010, 9), Sh(needs_shift ? 1 : 0), Imm12(imm12),
                            Rd(reg.id()), Rn(reg.id()));
  } else {
    std::vector<InstructionARM64> instrs = construct_multiple_imm12_adds(imm, reg.id());
    return InstructionARM64(instrs);
  }
}

InstructionARM64 sub_gpr64_imm(Register reg, int64_t imm) {
  ASSERT(reg.is_gpr(instr_set));
  if (imm < 0) {
    return add_gpr64_imm(reg, std::abs(imm));
  }
  // Check to see if we can represent this subtraction in a single instruction
  // if not, then we need to emit multiple partial instructions
  const auto [is_single_instr, imm12, needs_shift] = can_encode_single_imm12(imm);
  if (is_single_instr) {
    // https://www.scs.stanford.edu/~zyedidia/arm64/sub_addsub_imm.html
    // SUB <Xd|SP>, <Xn|SP>, #<imm>{, <shift>}
    return InstructionARM64(Base(0b110100010, 9), Sh(needs_shift ? 1 : 0), Imm12(imm12),
                            Rd(reg.id()), Rn(reg.id()));
  } else {
    std::vector<InstructionARM64> instrs = construct_multiple_imm12_subs(imm, reg.id());
    return InstructionARM64(instrs);
  }
}

InstructionARM64 add_gpr64_gpr64(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_shift.html
  // ADD <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
  return InstructionARM64(Base(0b10001011000, 11), Rd(dst.id()), Imm6(0), Rn(dst.id()),
                          Rm(src.id()));
}

InstructionARM64 sub_gpr64_gpr64(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/sub_addsub_shift.html
  // SUB <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
  return InstructionARM64(Base(0b11001011000, 11), Rd(dst.id()), Imm6(0), Rn(dst.id()),
                          Rm(src.id()));
}

InstructionARM64 imul_gpr32_gpr32(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/mul_madd.html
  // MUL <Wd>, <Wn>, <Wm>
  return InstructionARM64(Base(0b0001101100000000011111, 22), Rd(dst.id()), Rn(dst.id()),
                          Rm(src.id()));
}

InstructionARM64 imul_gpr64_gpr64(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/mul_madd.html
  // MUL <Xd>, <Xn>, <Xm>
  return InstructionARM64(Base(0b1001101100000000011111, 22), Rd(dst.id()), Rn(dst.id()),
                          Rm(src.id()));
}

InstructionARM64 idiv_gpr32(Register reg) {
  // The x86 idiv divides the implicit EDX:EAX 64-bit dividend by the operand,
  // storing the 32-bit quotient in EAX and remainder in EDX.  On ARM64 a 32-bit
  // divide is a single sdiv taking an explicit dividend register, so the
  // dividend lives in W0 (the ARM64 analog of EAX) and the divisor is the
  // operand.  The quotient is left in W0, mirroring the x86 EAX-in/EAX-out
  // contract.  Note the ARM64 sdiv saturates on INT_MIN / -1 (returns INT_MIN)
  // instead of trapping like x86.
  // https://www.scs.stanford.edu/~zyedidia/arm64/sdiv_sdiv.html
  // SDIV <Wd>, <Wn>, <Wm>
  return InstructionARM64(Base(0b0001101011000000000011, 22), Rd(X0), Rn(X0),
                          Rm(reg.id()));
}

InstructionARM64 unsigned_div_gpr32(Register reg) {
  // Same contract as idiv_gpr32 but unsigned.  UDIV <Wd>, <Wn>, <Wm>.  A zero
  // divisor yields 0 instead of trapping like x86.
  // https://www.scs.stanford.edu/~zyedidia/arm64/udiv_udiv.html
  return InstructionARM64(Base(0b0001101011000000000010, 22), Rd(X0), Rn(X0),
                          Rm(reg.id()));
}

InstructionARM64 cdq() {
  // x86 cdq sign-extends EAX into EDX:EAX to form a 64-bit signed dividend for
  // idiv.  ARM64 has no separate dividend-high register: sdiv takes the 32-bit
  // dividend directly from W0, so the ARM64 translation of cdq is to sign-extend
  // W0 into X0.  This preserves the x86 instruction flow and is harmless for
  // the subsequent sdiv (which only reads the low 32 bits).
  // https://www.scs.stanford.edu/~zyedidia/arm64/sxtw_sbfm.html
  // SXTW <Xd>, <Wn>
  return InstructionARM64(Base(0b1001001101000000011111, 22), Rd(X0), Rn(X0));
}

InstructionARM64 movsx_r64_r32(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/sxtw_sbfm.html
  // SXTW <Xd>, <Wn>
  return InstructionARM64(Base(0b1001001101000000011111, 22), Rd(dst.id()), Rn(src.id()));
}

InstructionARM64 cmp_gpr64_gpr64(Register a, Register b) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/cmp_subs_addsub_ext.html
  // CMP <Xn|SP>, <R><m>{, <extend> {#<amount>}}
  return InstructionARM64(Base(0b11101011000000000000000000011111, 32), Rn(a.id()), Rm(b.id()));
}

//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   BIT STUFF
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

InstructionARM64 or_gpr64_gpr64(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/orr_log_shift.html
  // ORR <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(src.is_gpr(instr_set));
  return InstructionARM64(Base(0b10101010000, 11), Rd(dst.id()), Rn(dst.id()), Rm(src.id()));
}

InstructionARM64 and_gpr64_gpr64(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/and_log_shift.html
  // AND <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(src.is_gpr(instr_set));
  return InstructionARM64(Base(0b10001010000, 11), Rd(dst.id()), Rn(dst.id()), Rm(src.id()));
}

InstructionARM64 xor_gpr64_gpr64(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/eor_log_shift.html
  // EOR <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
  ASSERT(dst.is_gpr(instr_set));
  ASSERT(src.is_gpr(instr_set));
  return InstructionARM64(Base(0b11001010000, 11), Rd(dst.id()), Rn(dst.id()), Rm(src.id()));
}

InstructionARM64 not_gpr64(Register reg) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/mvn_orn_log_shift.html
  // MVN <Xd>, <Xm>{, <shift> #<amount>}
  // ==
  // ORN <Xd>, XZR, <Xm>{, <shift> #<amount>}
  ASSERT(reg.is_gpr(instr_set));
  return InstructionARM64(Base(0b101010100010000000000011111, 27), Rd(reg.id()), Rm(reg.id()));
}

//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   SHIFTS
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

InstructionARM64 shl_gpr64_reg(Register reg, Register shift_reg) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/lsl_lslv.html
  // LSL <Xd>, <Xn>, <Xm>
  ASSERT(reg.is_gpr(instr_set));
  ASSERT(shift_reg.is_gpr(instr_set));
  return InstructionARM64(Base(0b1001101011000000001000, 22), Rd(reg.id()), Rn(reg.id()),
                          Rm(shift_reg.id()));
}

InstructionARM64 shr_gpr64_reg(Register reg, Register shift_reg) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/lsr_lsrv.html
  // LSR <Xd>, <Xn>, <Xm>
  ASSERT(reg.is_gpr(instr_set));
  ASSERT(shift_reg.is_gpr(instr_set));
  return InstructionARM64(Base(0b1001101011000000001001, 22), Rd(reg.id()), Rn(reg.id()),
                          Rm(shift_reg.id()));
}

InstructionARM64 sar_gpr64_reg(Register reg, Register shift_reg) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/asr_asrv.html
  // ASR <Xd>, <Xn>, <Xm>
  ASSERT(reg.is_gpr(instr_set));
  ASSERT(shift_reg.is_gpr(instr_set));
  return InstructionARM64(Base(0b1001101011000000001010, 22), Rd(reg.id()), Rn(reg.id()),
                          Rm(shift_reg.id()));
}

InstructionARM64 shl_gpr64_u8(Register reg, uint8_t sa) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/lsl_ubfm.html
  // LSL <Xd>, <Xn>, #<shift>
  ASSERT(sa < 63);
  ASSERT(reg.is_gpr(instr_set));
  return InstructionARM64(Base(0b1101001101, 10), Rd(reg.id()), Rn(reg.id()), Immr((64 - sa) & 63),
                          Imms(63 - sa));
}

InstructionARM64 shr_gpr64_u8(Register reg, uint8_t sa) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/lsr_ubfm.html
  // LSR <Xd>, <Xn>, #<shift>
  // sf	1	0	1	0	0	1	1	0	N
  ASSERT(sa < 63);
  ASSERT(reg.is_gpr(instr_set));
  return InstructionARM64(Base(0b1101001101000000111111, 22), Rd(reg.id()), Rn(reg.id()), Immr(sa));
}

InstructionARM64 sar_gpr64_u8(Register reg, uint8_t sa) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/asr_sbfm.html
  // ASR <Xd>, <Xn>, #<shift>
  ASSERT(sa < 63);
  ASSERT(reg.is_gpr(instr_set));
  return InstructionARM64(Base(0b1001001101000000111111, 22), Rd(reg.id()), Rn(reg.id()), Immr(sa));
}

//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   CONTROL FLOW
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//
// All of these instructions jump to a target that is zero
// and then its up to the IR to patch the actual target
//
// Critically, on arm these relative targets must be within
// 128MB, which is much less than x86 (~2GB)
//
// However, these functions are only really used for jumps within
// a given function...of which the largest we've seen isn't even in the MB
// of sizes
//
// So for now, keep it simple and don't implement something more
// complicated like veneers, this should be fine.

InstructionARM64 jmp_imm() {
  // https://www.scs.stanford.edu/~zyedidia/arm64/b_uncond.html
  // B <label>
  return InstructionARM64(Base(0b000101, 6), Imm26(0));
}

// Now these instructions in ARM are even more limiting, conditional
// branches must be within 1MB relative to the instruction
//
// However once again, that is still WAY below our biggest functions of a few kb
//
// But still...be aware!  There should be some assertions in place in the patching so that
// said issues don't just fly under the radar
//
// Also, these instructions may have to be patched slightly differently since
// ARM uses a single branch instruction for all
//
// It's worth noting that these x86 instructions also have limitations, they cannot
// jump to far labels (labels in other code segments).  But that's more difficult to
// give a numeric value to like with ARM.

InstructionARM64 je_imm() {
  // https://www.scs.stanford.edu/~zyedidia/arm64/b_cond.html
  // B.<cond> <label>
  // 0000 	EQ
  return InstructionARM64(Base(0b01010100, 8), Imm19(0), Cond(0b0000));
}

InstructionARM64 jne_imm() {
  // https://www.scs.stanford.edu/~zyedidia/arm64/b_cond.html
  // B.<cond> <label>
  // 0001 	NE
  return InstructionARM64(Base(0b01010100, 8), Imm19(0), Cond(0b0001));
}

InstructionARM64 jle_imm() {
  // https://www.scs.stanford.edu/~zyedidia/arm64/b_cond.html
  // B.<cond> <label>
  // 1101 	LE
  return InstructionARM64(Base(0b01010100, 8), Imm19(0), Cond(0b1101));
}

InstructionARM64 jge_imm() {
  // https://www.scs.stanford.edu/~zyedidia/arm64/b_cond.html
  // B.<cond> <label>
  // 1010 	GE
  return InstructionARM64(Base(0b01010100, 8), Imm19(0), Cond(0b1010));
}

InstructionARM64 jl_imm() {
  // https://www.scs.stanford.edu/~zyedidia/arm64/b_cond.html
  // B.<cond> <label>
  // 1011 	LT
  return InstructionARM64(Base(0b01010100, 8), Imm19(0), Cond(0b1011));
}

InstructionARM64 jg_imm() {
  // https://www.scs.stanford.edu/~zyedidia/arm64/b_cond.html
  // B.<cond> <label>
  // 1100 	GT
  return InstructionARM64(Base(0b01010100, 8), Imm19(0), Cond(0b1100));
}

InstructionARM64 jbe_imm() {
  // https://www.scs.stanford.edu/~zyedidia/arm64/b_cond.html
  // B.<cond> <label>
  // 1001 	LS
  return InstructionARM64(Base(0b01010100, 8), Imm19(0), Cond(0b1001));
}

InstructionARM64 jae_imm() {
  // https://www.scs.stanford.edu/~zyedidia/arm64/b_cond.html
  // B.<cond> <label>
  // 0010 	CS
  return InstructionARM64(Base(0b01010100, 8), Imm19(0), Cond(0b0010));
}

InstructionARM64 jb_imm() {
  // https://www.scs.stanford.edu/~zyedidia/arm64/b_cond.html
  // B.<cond> <label>
  // 0011 	CC
  return InstructionARM64(Base(0b01010100, 8), Imm19(0), Cond(0b0011));
}

InstructionARM64 ja_imm() {
  // https://www.scs.stanford.edu/~zyedidia/arm64/b_cond.html
  // B.<cond> <label>
  // 1000 	HI
  return InstructionARM64(Base(0b01010100, 8), Imm19(0), Cond(0b1000));
}

//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   FLOAT MATH
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

InstructionARM64 cmp_f32_f32(Register a, Register b) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/fcmp_float.html
  // Single-precision (ftype == 00 && opc == 00)
  // FCMP <Sn>, <Sm>
  return InstructionARM64(Base(0b00011110001000000010000000000000, 32), Rn(a.id()), Rm(b.id()));
}

InstructionARM64 sqrt_f32(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/fsqrt_float.html
  // Single-precision (ftype == 00)
  // FSQRT <Sd>, <Sn>
  return InstructionARM64(Base(0b0001111000100001110000, 22), Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 mul_f32_f32(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/fmul_float.html
  // Single-precision (ftype == 00)
  // FMUL <Sd>, <Sn>, <Sm>
  return InstructionARM64(Base(0b0001111000100000000010, 22), Rd(dst.id()), Rn(dst.id()),
                          Rm(src.id()));
}

InstructionARM64 div_f32_f32(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/fdiv_float.html
  // Single-precision (ftype == 00)
  // FDIV <Sd>, <Sn>, <Sm>
  return InstructionARM64(Base(0b0001111000100000000110, 22), Rd(dst.id()), Rn(dst.id()),
                          Rm(src.id()));
}

InstructionARM64 sub_f32_f32(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/fsub_float.html
  // Single-precision (ftype == 00)
  // FSUB <Sd>, <Sn>, <Sm>
  return InstructionARM64(Base(0b0001111000100000001110, 22), Rd(dst.id()), Rn(dst.id()),
                          Rm(src.id()));
}

InstructionARM64 add_f32_f32(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/fadd_float.html
  // Single-precision (ftype == 00)
  // FADD <Sd>, <Sn>, <Sm>
  return InstructionARM64(Base(0b0001111000100000001010, 22), Rd(dst.id()), Rn(dst.id()),
                          Rm(src.id()));
}

InstructionARM64 min_f32_f32(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/fmin_float.html
  // Single-precision (ftype == 00)
  // FMIN <Sd>, <Sn>, <Sm>
  return InstructionARM64(Base(0b0001111000100000010110, 22), Rd(dst.id()), Rn(dst.id()),
                          Rm(src.id()));
}

InstructionARM64 max_f32_f32(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/fmax_float.html
  // Single-precision (ftype == 00)
  // FMAX <Sd>, <Sn>, <Sm>
  return InstructionARM64(Base(0b0001111000100000010010, 22), Rd(dst.id()), Rn(dst.id()),
                          Rm(src.id()));
}

InstructionARM64 int32_to_f32(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/scvtf_float_int.html
  // 32-bit to single-precision (sf == 0 && ftype == 00)
  // SCVTF <Sd>, <Wn>
  return InstructionARM64(Base(0b0001111000100010000000, 22), Rd(dst.id()), Rn(src.id()));
}

InstructionARM64 f32_to_int32(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/fcvtzs_float_int.html
  // 32-bit to single-precision (sf == 0 && ftype == 00)
  // FCVTZS <Wd>, <Sn>
  return InstructionARM64(Base(0b0001111000111000000000, 22), Rd(dst.id()), Rn(src.id()));
}

InstructionARM64 nop() {
  // https://www.scs.stanford.edu/~zyedidia/arm64/nop.html
  return InstructionARM64(Base(0b11010101000000110010000000011111, 32));
}

//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
//   UTILITIES
//;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

InstructionARM64 null() {
  // dummy empty byte
  return InstructionARM64(0b0);
}

/////////////////////////////
// AVX (VF - Vector Float) //
/////////////////////////////

InstructionARM64 nop_vf() {
  // Not sure if this one was even needed for x86, but it does not really exist on ARM64
  // just use a normal nop
  return nop();
}

InstructionARM64 wait_vf() {
  // Another instruction that doesnt really map to arm64 because there is no annoying
  // x87 FPU behaviour
  return nop();
}

InstructionARM64 mov_vf_vf(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/mov_orr_advsimd_reg.html
  // MOV <Vd>.<T>, <Vn>.<T>  (alias of ORR <Vd>, <Vn>, <Vn>)
  // Q 	<T>
  // 0 	8B
  // 1 	16B
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));
  return InstructionARM64(Base(0b0100111010100000000111, 22), Rd(dst.id()), Rn(src.id()),
                          Rm(src.id()));
}

InstructionARM64 loadvf_gpr64_plus_gpr64(Register dst, Register addr1, Register addr2) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldr_reg_fpsimd.html
  // 128-bit variant
  // LDR <Qt>, [<Xn|SP>, (<Wm>|<Xm>){, <extend> {<amount>}}]
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  return InstructionARM64(Base(0b0011110011100001011010, 22), Rt(dst.id()), Rn(addr1.id()),
                          Rm(addr2.id()));
}

InstructionARM64 loadvf_gpr64_plus_gpr64_plus_s8(Register dst,
                                                 Register addr1,
                                                 Register addr2,
                                                 s64 offset) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldr_imm_fpsimd.html
  // 128-bit variant
  // LDR <Qt>, [<Xn|SP>], #<simm>
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT8_MIN && offset <= INT8_MAX);
  // compute addr1 + addr2 in x16, then LDUR with the signed offset
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_shift.html
      // ADD <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
      InstructionARM64(Base(0b10001011000, 11), Rd(X16), Imm6(0), Rn(addr1.id()), Rm(addr2.id())),
  };
  instrs.emplace_back(
      InstructionARM64(Base(0b0011110011000000000000, 22), Rt(dst.id()), Rn(X16), Imm9s(offset)));
  return InstructionARM64(instrs);
}

InstructionARM64 loadvf_gpr64_plus_gpr64_plus_s32(Register dst,
                                                  Register addr1,
                                                  Register addr2,
                                                  s64 offset) {
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT32_MIN && offset <= INT32_MAX);
  // first establish the base+index value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_shift.html
      // ADD <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
      InstructionARM64(Base(0b10001011000, 11), Rd(X16), Imm6(0), Rn(addr1.id()), Rm(addr2.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // https://www.scs.stanford.edu/~zyedidia/arm64/ldr_imm_fpsimd.html
  // 128-bit variant
  // LDR <Qt>, [<Xn|SP>], #<simm>
  instrs.emplace_back(
      InstructionARM64(Base(0b0011110011000000000000, 22), Rt(dst.id()), Rn(X16), Imm9s(0)));
  return InstructionARM64(instrs);
}

InstructionARM64 storevf_gpr64_plus_gpr64(Register value, Register addr1, Register addr2) {
  ASSERT(value.is_128bit_simd(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  // https://www.scs.stanford.edu/~zyedidia/arm64/str_reg_fpsimd.html
  // STR <Qt>, [<Xn|SP>, (<Wm>|<Xm>){, <extend> {<amount>}}]
  return InstructionARM64(Base(0b0011110010100001011010, 22), Rt(value.id()), Rn(addr1.id()),
                          Rm(addr2.id()));
}

InstructionARM64 storevf_gpr64_plus_gpr64_plus_s8(Register value,
                                                  Register addr1,
                                                  Register addr2,
                                                  s64 offset) {
  ASSERT(value.is_128bit_simd(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT8_MIN && offset <= INT8_MAX);
  // first establish the base+index+offset value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_shift.html
      // ADD <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
      InstructionARM64(Base(0b10001011000, 11), Rd(X16), Imm6(0), Rn(addr1.id()), Rm(addr2.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // https://www.scs.stanford.edu/~zyedidia/arm64/str_imm_fpsimd.html
  // STR <Qt>, [<Xn|SP>], #<simm>
  instrs.emplace_back(
      InstructionARM64(Base(0b0011110010000000000000, 22), Rt(value.id()), Rn(X16), Imm9s(0)));
  return InstructionARM64(instrs);
}

InstructionARM64 storevf_gpr64_plus_gpr64_plus_s32(Register value,
                                                   Register addr1,
                                                   Register addr2,
                                                   s64 offset) {
  ASSERT(value.is_128bit_simd(instr_set));
  ASSERT(addr1.is_gpr(instr_set));
  ASSERT(addr2.is_gpr(instr_set));
  ASSERT(addr1 != addr2);
  ASSERT(addr1 != SP);
  ASSERT(addr2 != SP);
  ASSERT(offset >= INT32_MIN && offset <= INT32_MAX);
  // first establish the base+index+offset value in x16
  std::vector<InstructionARM64> instrs = {
      // https://www.scs.stanford.edu/~zyedidia/arm64/add_addsub_shift.html
      // ADD <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
      InstructionARM64(Base(0b10001011000, 11), Rd(X16), Imm6(0), Rn(addr1.id()), Rm(addr2.id())),
  };
  if (offset < 0) {
    // we'll subtract instead
    offset = std::abs(offset);
    const auto sub_instrs = construct_multiple_imm12_subs(offset, X16);
    instrs.insert(instrs.end(), sub_instrs.begin(), sub_instrs.end());
  } else {
    const auto add_instrs = construct_multiple_imm12_adds(offset, X16);
    instrs.insert(instrs.end(), add_instrs.begin(), add_instrs.end());
  }
  // https://www.scs.stanford.edu/~zyedidia/arm64/str_imm_fpsimd.html
  // STR <Qt>, [<Xn|SP>], #<simm>
  instrs.emplace_back(
      InstructionARM64(Base(0b0011110010000000000000, 22), Rt(value.id()), Rn(X16), Imm9s(0)));
  return InstructionARM64(instrs);
}

InstructionARM64 loadvf_rip_plus_s32(Register dest, s64 offset) {
  ASSERT(dest.is_128bit_simd(instr_set));
  ASSERT_MSG(offset != 0,
             "PC Relative offset isn't 0 at encoding time, actually encode it properly!");
  return InstructionARM64(
      {// https://www.scs.stanford.edu/~zyedidia/arm64/adrp.html
       // ADRP <Xd>, <label>
       InstructionARM64(Base(0b100100000000000000000000000, 27), Rd(X16), Immhi(0), Immlo(0)),
       // https://www.scs.stanford.edu/~zyedidia/arm64/ldr_imm_fpsimd.html
       // LDR <Qt>, [<Xn|SP>{, #<pimm>}]
       InstructionARM64(Base(0b0011110111, 10), Imm12(offset), Rt(dest.id()), Rn(X16))});
}

// Insert a 64-bit GPR into vector register lane d[index].  Used to build
// scratch index/mask vectors for TBL/BSL.
// https://www.scs.stanford.edu/~zyedidia/arm64/ins_gen_advsimd.html
// INS <Vd>.D[<index>], <Xn>
InstructionARM64 ins_vf_d_gpr(Register vd, u8 index, Register gpr) {
  ASSERT(index <= 1);
  ASSERT(vd.is_128bit_simd(instr_set));
  ASSERT(gpr.is_gpr(instr_set));
  u32 imm5 = (index << 4) | 8;  // 64-bit element index encoding
  return InstructionARM64(Base(0b0100111000000000000111, 22), Rd(vd.id()), Rn(gpr.id()),
                          Rm(imm5));
}

// Extract a 64-bit vector lane into a GPR (UMOV).  Used to return vector
// results from CodeTester test functions.
// https://www.scs.stanford.edu/~zyedidia/arm64/umov_advsimd.html
// UMOV <Xd>, <Vn>.D[<index>]
InstructionARM64 umov_gpr64_vf_d(Register gpr, Register vd, u8 index) {
  ASSERT(index <= 1);
  ASSERT(vd.is_128bit_simd(instr_set));
  ASSERT(gpr.is_gpr(instr_set));
  u32 imm5 = (index << 4) | 8;  // 64-bit element index encoding
  return InstructionARM64(Base(0b0100111000000000001111, 22), Rd(gpr.id()), Rn(vd.id()),
                          Rm(imm5));
}

InstructionARM64 blend_vf(Register dst, Register src1, Register src2, u8 mask) {
  // x86 VBLENDPS: for each 32-bit lane i, take src2[i] when mask bit i is set,
  // otherwise src1[i].  On ARM64 we build the per-lane selector mask (all-ones
  // or all-zeros per lane) in the scratch register v16 and use BSL (bitwise
  // select): v16 = mask ? src2 : src1.
  //
  // Clobbers: v16 (SIMD scratch) and x16 (GPR scratch; never allocated by GOAL
  // per the ARM-004 register model).  The IR backend must treat v16 as clobbered
  // across this instruction (ARM-017).
  ASSERT(!(mask & 0b11110000));
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src1.is_128bit_simd(instr_set));
  ASSERT(src2.is_128bit_simd(instr_set));

  u64 sel[4];
  for (int i = 0; i < 4; i++) {
    sel[i] = (mask & (1 << i)) ? 0xFFFFFFFFull : 0x00000000ull;
  }
  // d[0] = lanes 0,1 ; d[1] = lanes 2,3 (little-endian byte order).
  u64 d0 = (sel[1] << 32) | sel[0];
  u64 d1 = (sel[3] << 32) | sel[2];

  std::vector<InstructionARM64> seq;
  seq.push_back(mov_gpr64_u64(ARM64_REG::X16, d0));
  seq.push_back(ins_vf_d_gpr(ARM64_REG::V16, 0, ARM64_REG::X16));
  seq.push_back(mov_gpr64_u64(ARM64_REG::X16, d1));
  seq.push_back(ins_vf_d_gpr(ARM64_REG::V16, 1, ARM64_REG::X16));
  // https://www.scs.stanford.edu/~zyedidia/arm64/bsl_advsimd.html
  // BSL <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  seq.push_back(InstructionARM64(Base(0b0110111001100000000111, 22), Rn(src2.id()),
                                 Rm(src1.id()), Rd(ARM64_REG::V16)));
  seq.push_back(mov_vf_vf(dst, ARM64_REG::V16));
  return InstructionARM64(seq);
}

InstructionARM64 swizzle_vf(Register dst, Register src, u8 controlBytes) {
  // x86 VSHUFPS with both operands equal to src: dst[i] = src[sel[i]] for each
  // of the four 32-bit lanes, where sel[i] = (controlBytes >> (2*i)) & 3.  On
  // ARM64 we use TBL with a byte index vector built in the scratch register
  // v16: the source is the (single) table and the index vector holds 4*sel[i]+j
  // for byte j of lane i.
  //
  // Clobbers: v16 (SIMD scratch) and x16 (GPR scratch).  The IR backend must
  // treat v16 as clobbered across this instruction (ARM-017).
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));

  u8 sel[4];
  for (int i = 0; i < 4; i++) {
    sel[i] = (controlBytes >> (2 * i)) & 3;
  }
  // Build the 16-byte index vector: byte j of lane i = 4*sel[i] + j.
  u64 d0 = 0, d1 = 0;
  for (int k = 0; k < 4; k++) {
    d0 |= (u64)(4 * sel[0] + k) << (8 * k);
    d0 |= (u64)(4 * sel[1] + k) << (32 + 8 * k);
    d1 |= (u64)(4 * sel[2] + k) << (8 * k);
    d1 |= (u64)(4 * sel[3] + k) << (32 + 8 * k);
  }

  std::vector<InstructionARM64> seq;
  seq.push_back(mov_gpr64_u64(ARM64_REG::X16, d0));
  seq.push_back(ins_vf_d_gpr(ARM64_REG::V16, 0, ARM64_REG::X16));
  seq.push_back(mov_gpr64_u64(ARM64_REG::X16, d1));
  seq.push_back(ins_vf_d_gpr(ARM64_REG::V16, 1, ARM64_REG::X16));
  // https://www.scs.stanford.edu/~zyedidia/arm64/tbl_tbl.html
  // TBL <Vd>.<T>, {<Vn>.<T>}, <Vm>.<T>
  seq.push_back(InstructionARM64(Base(0b0100111000000000000000, 22), Rn(src.id()),
                                 Rm(ARM64_REG::V16), Rd(dst.id())));
  return InstructionARM64(seq);
}

InstructionARM64 shuffle_vf(Register dst, Register src, u8 dx, u8 dy, u8 dz, u8 dw) {
  // x86 packs dx,dy,dz,dw into the VSHUFPS control bytes and delegates to
  // swizzle.  Same here.
  ASSERT(dx < 4);
  ASSERT(dy < 4);
  ASSERT(dz < 4);
  ASSERT(dw < 4);
  u8 imm = dx + (dy << 2) + (dz << 4) + (dw << 6);
  return swizzle_vf(dst, src, imm);
}

InstructionARM64 splat_vf(Register dst, Register src, Register::VF_ELEMENT element) {
  // x86 splat broadcasts one lane of src to all four lanes.  NEON DUP is a
  // single instruction for this.  Lane order is X=0, Y=1, Z=2, W=3.
  // https://www.scs.stanford.edu/~zyedidia/arm64/dup_advsimd_elem.html
  // DUP <Vd>.<T>, <Vn>.S[<index>]
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));
  int lane = 0;
  switch (element) {
    case Register::VF_ELEMENT::X:
      lane = 0;
      break;
    case Register::VF_ELEMENT::Y:
      lane = 1;
      break;
    case Register::VF_ELEMENT::Z:
      lane = 2;
      break;
    case Register::VF_ELEMENT::W:
      lane = 3;
      break;
    default:
      ASSERT(false);
  }
  u32 imm5 = (lane << 3) | 4;  // 32-bit element index encoding
  return InstructionARM64(Base(0b0100111000000000000001, 22), Rd(dst.id()), Rn(src.id()),
                          Rm(imm5));
}

InstructionARM64 xor_vf(Register dst, Register src1, Register src2) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/eor_advsimd.html
  // EOR <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  return InstructionARM64(Base(0b0110111000100001000111, 22), Rn(src1.id()), Rm(src2.id()),
                          Rd(dst.id()));
}

InstructionARM64 sub_vf(Register dst, Register src1, Register src2) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/fsub_advsimd.html
  // FSUB <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  // 4 single precision floats
  return InstructionARM64(Base(0b0100111010100001110101, 22), Rn(src1.id()), Rm(src2.id()),
                          Rd(dst.id()));
}

InstructionARM64 add_vf(Register dst, Register src1, Register src2) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/add_advsimd.html
  // ADD <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  // 4 single precision floats
  return InstructionARM64(Base(0b0100111000100001110101, 22), Rn(src1.id()), Rm(src2.id()),
                          Rd(dst.id()));
}

InstructionARM64 mul_vf(Register dst, Register src1, Register src2) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/fmul_advsimd_vec.html
  // FMUL <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  // 4 single precision floats
  return InstructionARM64(Base(0b0110111000100001110111, 22), Rn(src1.id()), Rm(src2.id()),
                          Rd(dst.id()));
}

InstructionARM64 max_vf(Register dst, Register src1, Register src2) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/famax_advsimd.html
  // FAMAX <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  // 4 single precision floats
  return InstructionARM64(Base(0b0100111000100001111101, 22), Rn(src1.id()), Rm(src2.id()),
                          Rd(dst.id()));
}

InstructionARM64 min_vf(Register dst, Register src1, Register src2) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/famin_advsimd.html
  // FAMIN <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  // 4 single precision floats
  return InstructionARM64(Base(0b0100111010100001111101, 22), Rn(src1.id()), Rm(src2.id()),
                          Rd(dst.id()));
}

InstructionARM64 div_vf(Register dst, Register src1, Register src2) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/fdiv_advsimd.html
  // FDIV <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  // 4 single precision floats
  return InstructionARM64(Base(0b0110111000100001111111, 22), Rn(src1.id()), Rm(src2.id()),
                          Rd(dst.id()));
}

InstructionARM64 sqrt_vf(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/fsqrt_advsimd.html
  // FSQRT <Vd>.<T>, <Vn>.<T>
  // 4 single precision floats
  return InstructionARM64(Base(0b0110111010100001111110, 22), Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 itof_vf(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/scvtf_advsimd_int.html
  // SCVTF <Vd>.<T>, <Vn>.<T>
  // s32 int -> 4 single precision floats
  return InstructionARM64(Base(0b0100111000100001110110, 22), Rn(src.id()), Rd(dst.id()));
}

InstructionARM64 ftoi_vf(Register dst, Register src) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/fcvtzs_advsimd_int.html
  // FCVTZS <Vd>.<T>, <Vn>.<T>
  // 4 single precision floats -> s32 ints
  // TODO - double check rounding mode
  return InstructionARM64(Base(0b0100111010100001101110, 22), Rn(src.id()), Rd(dst.id()));
}

// TODO - rename these instructions

// - arithmetic_shift_right_32bit_vf
InstructionARM64 pw_sra(Register dst, Register src, u8 imm) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/sshr_advsimd.html
  // - vector, 4S
  // SSHR <Vd>.<T>, <Vn>.<T>, #<shift>
  return InstructionARM64(Base(0b0100111100100000000001, 22), Rn(src.id()), Rd(dst.id()),
                          Immb(64 - imm));
}

// - logical_shift_right_32bit_vf
InstructionARM64 pw_srl(Register dst, Register src, u8 imm) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/ushr_advsimd.html
  // - vector, 4S
  // USHR <Vd>.<T>, <Vn>.<T>, #<shift>
  return InstructionARM64(Base(0b0110111100100000000001, 22), Rn(src.id()), Rd(dst.id()),
                          Immb(64 - imm));
}

// - logical_shift_left_32bit_vf
InstructionARM64 pw_sll(Register dst, Register src, u8 imm) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/shl_advsimd.html
  // - vector, 4S
  // SHL <Vd>.<T>, <Vn>.<T>, #<shift>
  return InstructionARM64(Base(0b0100111100100000010101, 22), Rn(src.id()), Rd(dst.id()),
                          Immb(32 + imm));
}

// - logical_shift_right_16bit_vf
InstructionARM64 ph_srl(Register dst, Register src, u8 imm) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/ushr_advsimd.html
  // - vector, 8H
  // USHR <Vd>.<T>, <Vn>.<T>, #<shift>
  return InstructionARM64(Base(0b0110111100010000000001, 22), Rn(src.id()), Rd(dst.id()),
                          Immb(32 - imm));
}

// - logical_shift_left_16bit_vf
InstructionARM64 ph_sll(Register dst, Register src, u8 imm) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/shl_advsimd.html
  // - vector, 8H
  // SHL <Vd>.<T>, <Vn>.<T>, #<shift>
  return InstructionARM64(Base(0b0100111100010000010101, 22), Rn(src.id()), Rd(dst.id()),
                          Immb(16 + imm));
}

InstructionARM64 parallel_add_byte(Register dst, Register src0, Register src1) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/add_advsimd.html
  // - vector, 16B
  // ADD <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  return InstructionARM64(Base(0b0100111000100000100001, 22), Rn(src0.id()), Rm(src1.id()),
                          Rd(dst.id()));
}

InstructionARM64 parallel_bitwise_or(Register dst, Register src0, Register src1) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/orr_advsimd_reg.html
  // - vector, 16B
  // ORR <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  return InstructionARM64(Base(0b0100111010100000000111, 22), Rn(src0.id()), Rm(src1.id()),
                          Rd(dst.id()));
}

InstructionARM64 parallel_bitwise_xor(Register dst, Register src0, Register src1) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/eor_advsimd.html
  // - vector, 16B
  // EOR <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  return InstructionARM64(Base(0b0110111000100001000111, 22), Rn(src0.id()), Rm(src1.id()),
                          Rd(dst.id()));
}

InstructionARM64 parallel_bitwise_and(Register dst, Register src0, Register src1) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/and_advsimd.html
  // - vector, 16B
  // AND <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  return InstructionARM64(Base(0b0100111000100000000111, 22), Rn(src0.id()), Rm(src1.id()),
                          Rd(dst.id()));
}

InstructionARM64 pextub_swapped(Register dst, Register src0, Register src1) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/uzp2_advsimd.html
  // - 16B
  // UZP2 <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  return InstructionARM64(Base(0b0100111000000000010110, 22), Rn(src0.id()), Rm(src1.id()),
                          Rd(dst.id()));
}

InstructionARM64 pextuh_swapped(Register dst, Register src0, Register src1) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/uzp2_advsimd.html
  // - 8H
  // UZP2 <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  return InstructionARM64(Base(0b0100111001000000010110, 22), Rn(src0.id()), Rm(src1.id()),
                          Rd(dst.id()));
}

InstructionARM64 pextuw_swapped(Register dst, Register src0, Register src1) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/uzp2_advsimd.html
  // - 4S
  // UZP2 <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  return InstructionARM64(Base(0b0100111010000000010110, 22), Rn(src0.id()), Rm(src1.id()),
                          Rd(dst.id()));
}

InstructionARM64 pextlb_swapped(Register dst, Register src0, Register src1) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/uzp1_advsimd.html
  // - 16B
  // UZP1 <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  return InstructionARM64(Base(0b0100111000000000000110, 22), Rn(src0.id()), Rm(src1.id()),
                          Rd(dst.id()));
}

InstructionARM64 pextlh_swapped(Register dst, Register src0, Register src1) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/uzp1_advsimd.html
  // - 8H
  // UZP1 <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  return InstructionARM64(Base(0b0100111001000000000110, 22), Rn(src0.id()), Rm(src1.id()),
                          Rd(dst.id()));
}

InstructionARM64 pextlw_swapped(Register dst, Register src0, Register src1) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/uzp1_advsimd.html
  // - 4S
  // UZP1 <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  return InstructionARM64(Base(0b0100111010000000000110, 22), Rn(src0.id()), Rm(src1.id()),
                          Rd(dst.id()));
}

InstructionARM64 parallel_compare_e_b(Register dst, Register src0, Register src1) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/cmeq_advsimd_reg.html
  // - vector, 16B
  // CMEQ <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  return InstructionARM64(Base(0b0110111000100000100011, 22), Rn(src0.id()), Rm(src1.id()),
                          Rd(dst.id()));
}

InstructionARM64 parallel_compare_e_h(Register dst, Register src0, Register src1) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/cmeq_advsimd_reg.html
  // - vector, 8H
  // CMEQ <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  return InstructionARM64(Base(0b0110111001100000100011, 22), Rn(src0.id()), Rm(src1.id()),
                          Rd(dst.id()));
}

InstructionARM64 parallel_compare_e_w(Register dst, Register src0, Register src1) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/cmeq_advsimd_reg.html
  // - vector, 4S
  // CMEQ <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  return InstructionARM64(Base(0b0110111010100000100011, 22), Rn(src0.id()), Rm(src1.id()),
                          Rd(dst.id()));
}

InstructionARM64 parallel_compare_gt_b(Register dst, Register src0, Register src1) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/cmgt_advsimd_reg.html
  // - vector, 16B
  // CMGT <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  return InstructionARM64(Base(0b0100111000100000001101, 22), Rn(src0.id()), Rm(src1.id()),
                          Rd(dst.id()));
}

InstructionARM64 parallel_compare_gt_h(Register dst, Register src0, Register src1) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/cmgt_advsimd_reg.html
  // - vector, 8H
  // CMGT <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  return InstructionARM64(Base(0b0100111001100000001101, 22), Rn(src0.id()), Rm(src1.id()),
                          Rd(dst.id()));
}

InstructionARM64 parallel_compare_gt_w(Register dst, Register src0, Register src1) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/cmgt_advsimd_reg.html
  // - vector, 4S
  // CMGT <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  return InstructionARM64(Base(0b0100111010100000001101, 22), Rn(src0.id()), Rm(src1.id()),
                          Rd(dst.id()));
}

// TODO - rename this monstrosity from x86

InstructionARM64 vpunpcklqdq(Register dst, Register src0, Register src1) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/zip1_advsimd.html
  // - vector, 2D
  // ZIP1 <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  return InstructionARM64(Base(0b0100111011000000001110, 22), Rn(src0.id()), Rm(src1.id()),
                          Rd(dst.id()));
}

InstructionARM64 pcpyld_swapped(Register dst, Register src0, Register src1) {
  return vpunpcklqdq(dst, src0, src1);
}

InstructionARM64 pcpyud(Register dst, Register src0, Register src1) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/zip2_advsimd.html
  // - vector, 2D
  // ZIP2 <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  return InstructionARM64(Base(0b0100111011000000011110, 22), Rn(src0.id()), Rm(src1.id()),
                          Rd(dst.id()));
}

// TODO - more x86 rename candidates

// lane-wise-32bit-substraction
InstructionARM64 vpsubd(Register dst, Register src0, Register src1) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/sub_advsimd.html
  // - vector, 4S
  // SUB <Vd>.<T>, <Vn>.<T>, <Vm>.<T>
  return InstructionARM64(Base(0b0110111010100000100001, 22), Rn(src0.id()), Rm(src1.id()),
                          Rd(dst.id()));
}

// shift-right-logical-entire-simd-reg
InstructionARM64 vpsrldq(Register dst, Register src, u8 imm) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/ext_advsimd.html
  // - 16B
  // EXT <Vd>.<T>, <Vn>.<T>, <Vm>.<T>, #<index>
  return InstructionARM64(Base(0b0110111000000000000000, 22), Rn(src.id()), Rm(src.id()),
                          Rd(dst.id()), Imm4(imm));
}

// shift-left-logical-entire-simd-reg
InstructionARM64 vpslldq(Register dst, Register src, u8 imm) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/ext_advsimd.html
  // - 16B
  // EXT <Vd>.<T>, <Vn>.<T>, <Vm>.<T>, #<index>
  return InstructionARM64(Base(0b0110111000000000000000, 22), Rn(src.id()), Rm(src.id()),
                          Rd(dst.id()), Imm4((16 - imm) & 0xF));
}

InstructionARM64 vpshuflw(Register dst, Register src, u8 imm) {
  // x86 VPSHUFLW: the low four 16-bit words are permuted using 2 bits per
  // word (word i = src word (imm >> 2i) & 3), the high four words are copied
  // unchanged.  On ARM64 we use TBL with a byte index vector built in the
  // scratch registers v16 (SIMD) and x16 (GPR); the index bytes for words 0-3
  // come from the shuffle, and bytes 8-15 are the identity (8..15).
  //
  // Clobbers: v16 (SIMD scratch) and x16 (GPR scratch).  The IR backend must
  // treat v16 as clobbered across this instruction (ARM-017).
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));

  u8 sel[4];
  for (int i = 0; i < 4; i++) {
    sel[i] = (imm >> (2 * i)) & 3;
  }
  u64 d0 = 0;
  for (int k = 0; k < 4; k++) {
    d0 |= (u64)(2 * sel[k]) << (16 * k);
    d0 |= (u64)(2 * sel[k] + 1) << (16 * k + 8);
  }
  const u64 d1 = 0x0F0E0D0C0B0A0908ull;  // bytes 8..15 = words 4..7 identity

  std::vector<InstructionARM64> seq;
  seq.push_back(mov_gpr64_u64(ARM64_REG::X16, d0));
  seq.push_back(ins_vf_d_gpr(ARM64_REG::V16, 0, ARM64_REG::X16));
  seq.push_back(mov_gpr64_u64(ARM64_REG::X16, d1));
  seq.push_back(ins_vf_d_gpr(ARM64_REG::V16, 1, ARM64_REG::X16));
  seq.push_back(InstructionARM64(Base(0b0100111000000000000000, 22), Rn(src.id()),
                                 Rm(ARM64_REG::V16), Rd(dst.id())));
  return InstructionARM64(seq);
}

InstructionARM64 vpshufhw(Register dst, Register src, u8 imm) {
  // x86 VPSHUFHW: the high four 16-bit words are permuted (word 4+i = src word
  // 4 + ((imm >> 2i) & 3)), the low four words are copied unchanged.  On ARM64
  // we use TBL with a byte index vector built in v16/x16: bytes 0-7 are the
  // identity (0..7) and bytes 8-15 come from the shuffle.
  //
  // Clobbers: v16 (SIMD scratch) and x16 (GPR scratch).
  ASSERT(dst.is_128bit_simd(instr_set));
  ASSERT(src.is_128bit_simd(instr_set));

  u8 sel[4];
  for (int i = 0; i < 4; i++) {
    sel[i] = (imm >> (2 * i)) & 3;
  }
  const u64 d0 = 0x0706050403020100ull;  // bytes 0..7 = words 0..3 identity
  u64 d1 = 0;
  for (int k = 0; k < 4; k++) {
    d1 |= (u64)(8 + 2 * sel[k]) << (16 * k);
    d1 |= (u64)(8 + 2 * sel[k] + 1) << (16 * k + 8);
  }

  std::vector<InstructionARM64> seq;
  seq.push_back(mov_gpr64_u64(ARM64_REG::X16, d0));
  seq.push_back(ins_vf_d_gpr(ARM64_REG::V16, 0, ARM64_REG::X16));
  seq.push_back(mov_gpr64_u64(ARM64_REG::X16, d1));
  seq.push_back(ins_vf_d_gpr(ARM64_REG::V16, 1, ARM64_REG::X16));
  seq.push_back(InstructionARM64(Base(0b0100111000000000000000, 22), Rn(src.id()),
                                 Rm(ARM64_REG::V16), Rd(dst.id())));
  return InstructionARM64(seq);
}

InstructionARM64 vpackuswb(Register dst, Register src0, Register src1) {
  // https://www.scs.stanford.edu/~zyedidia/arm64/sqxtun_advsimd.html
  // SQXTUN{2} <Vd>.<Tb>, <Vn>.<Ta>
  return InstructionARM64({
      // sqxtun  vDst.8b,  vSrc0.8h
      InstructionARM64(Base(0b0010111000100001001010, 22), Rn(src0.id()), Rd(dst.id())),
      // sqxtun2 vDst.16b, vSrc1.8h
      InstructionARM64(Base(0b0110111000100001001010, 22), Rn(src1.id()), Rd(dst.id())),
  });
}
}  // namespace ARM64
}  // namespace IGen
}  // namespace emitter
