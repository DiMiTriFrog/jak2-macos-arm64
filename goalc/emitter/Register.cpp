#include "Register.h"

#include <stdexcept>

namespace emitter {
RegisterInfo RegisterInfo::make_register_info(emitter::InstructionSet instr_set) {
  RegisterInfo info;

  if (instr_set == emitter::InstructionSet::X86) {
    // x86_64: one flat table, GPRs at ids RAX..R15 and XMMs at XMM0..XMM15.
    info.m_info[RAX] = {false, false, "rax"};  // return, temp
    info.m_info[RCX] = {false, false, "rcx"};  // gpr arg 3, temp
    info.m_info[RDX] = {false, false, "rdx"};  // gpr arg 2, temp
    info.m_info[RBX] = {true, false, "rbx"};   // saved
    info.m_info[RSP] = {false, true, "rsp"};   // stack pointer
    info.m_info[RBP] = {true, false, "rbp"};   // saved
    info.m_info[RSI] = {false, false, "rsi"};  // gpr arg 1, temp
    info.m_info[RDI] = {false, false, "rdi"};  // gpr arg 0, temp

    info.m_info[R8] = {false, false, "r8"};   // gpr arg 4, temp
    info.m_info[R9] = {false, false, "r9"};   // gpr arg 5, temp
    info.m_info[R10] = {true, false, "r10"};  // gpr arg 6, saved
    info.m_info[R11] = {true, false, "r11"};  // gpr arg 7, saved
    info.m_info[R12] = {true, false, "r12"};  // saved
    info.m_info[R13] = {false, true, "r13"};  // pp
    info.m_info[R14] = {false, true, "r14"};  // st
    info.m_info[R15] = {false, true, "r15"};  // offset.

    info.m_info[XMM0] = {false, false, "xmm0"};
    info.m_info[XMM1] = {false, false, "xmm1"};
    info.m_info[XMM2] = {false, false, "xmm2"};
    info.m_info[XMM3] = {false, false, "xmm3"};
    info.m_info[XMM4] = {false, false, "xmm4"};
    info.m_info[XMM5] = {false, false, "xmm5"};
    info.m_info[XMM6] = {false, false, "xmm6"};
    info.m_info[XMM7] = {false, false, "xmm7"};
    info.m_info[XMM8] = {true, false, "xmm8"};
    info.m_info[XMM9] = {true, false, "xmm9"};
    info.m_info[XMM10] = {true, false, "xmm10"};
    info.m_info[XMM11] = {true, false, "xmm11"};
    info.m_info[XMM12] = {true, false, "xmm12"};
    info.m_info[XMM13] = {true, false, "xmm13"};
    info.m_info[XMM14] = {true, false, "xmm14"};
    info.m_info[XMM15] = {true, false, "xmm15"};

    info.m_gpr_arg_regs = std::array<Register, N_ARGS>({RDI, RSI, RDX, RCX, R8, R9, R10, R11});
    // skip xmm0 so it can be used for return.
    info.m_xmm_arg_regs =
        std::array<Register, N_ARGS>({XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7, XMM8});
    info.m_saved_gprs = {RBX, RBP, R10, R11, R12};
    info.m_saved_xmms = {XMM8, XMM9, XMM10, XMM11, XMM12, XMM13, XMM14, XMM15};

    for (size_t i = 0; i < N_SAVED_GPRS; i++) {
      info.m_saved_all.push_back(info.m_saved_gprs[i]);
    }
    for (size_t i = 0; i < N_SAVED_XMMS; i++) {
      info.m_saved_all.push_back(info.m_saved_xmms[i]);
    }

    // todo - experiment with better orders for allocation.
    info.m_gpr_alloc_order = {RAX, RCX, RDX, RBX, RBP, RSI, RDI, R8, R9, R10};  // arbitrary
    info.m_xmm_alloc_order = {XMM0, XMM1, XMM2, XMM3,  XMM4,  XMM5,  XMM6,
                              XMM7, XMM8, XMM9, XMM10, XMM11, XMM12, XMM13};

    // these should only be temp registers!
    info.m_gpr_temp_only_alloc_order = {RAX, RCX, RDX, RSI, RDI, R8, R9};
    info.m_xmm_temp_only_alloc_order = {XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7};

    info.m_gpr_spill_temp_alloc_order = {RAX, RCX, RDX, RBX, RBP, RSI,
                                         RDI, R8,  R9,  R10, R11, R12};  // arbitrary
    info.m_xmm_spill_temp_alloc_order = {XMM0, XMM1, XMM2,  XMM3,  XMM4,  XMM5,  XMM6,  XMM7,
                                         XMM8, XMM9, XMM10, XMM11, XMM12, XMM13, XMM14, XMM15};

    info.m_process_reg = R13;
    info.m_st_reg = R14;
    info.m_offset_reg = R15;
    info.m_gpr_ret_reg = RAX;
    info.m_xmm_ret_reg = XMM0;
  } else if (instr_set == emitter::InstructionSet::ARM64) {
    // Apple ARM64.  Reference:
    // https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms
    // Argument GPRs x0-x7, return x0, caller-saved x0-x17, callee-saved x19-x28,
    // x29 frame pointer, x30 LR, sp.  x18 is a platform register reserved by the
    // OS and must never be allocated.  SIMD v0-v7 args/return/temp, v8-v15
    // callee-saved, v16-v31 caller-saved.
    info.m_info[X0] = {false, false, "x0"};
    info.m_info[X1] = {false, false, "x1"};
    info.m_info[X2] = {false, false, "x2"};
    info.m_info[X3] = {false, false, "x3"};
    info.m_info[X4] = {false, false, "x4"};
    info.m_info[X5] = {false, false, "x5"};
    info.m_info[X6] = {false, false, "x6"};
    info.m_info[X7] = {false, false, "x7"};
    info.m_info[X8] = {false, false, "x8"};  // indirect-result return, temp
    info.m_info[X9] = {false, false, "x9"};
    info.m_info[X10] = {false, false, "x10"};
    info.m_info[X11] = {false, false, "x11"};
    info.m_info[X12] = {false, false, "x12"};
    info.m_info[X13] = {false, false, "x13"};
    info.m_info[X14] = {false, false, "x14"};
    info.m_info[X15] = {false, false, "x15"};
    // x16/x17 are reserved for linker veneers; caller-saved but not allocatable.
    info.m_info[X16] = {false, true, "x16"};
    info.m_info[X17] = {false, true, "x17"};
    // x18 is reserved by the platform and must never be allocated.
    info.m_info[X18] = {false, true, "x18"};
    info.m_info[X19] = {true, false, "x19"};
    info.m_info[X20] = {true, true, "x20"};  // pp
    info.m_info[X21] = {true, true, "x21"};  // st
    info.m_info[X22] = {true, true, "x22"};  // offset
    info.m_info[X23] = {true, false, "x23"};
    info.m_info[X24] = {true, false, "x24"};
    info.m_info[X25] = {true, false, "x25"};
    info.m_info[X26] = {true, false, "x26"};
    info.m_info[X27] = {true, false, "x27"};
    info.m_info[X28] = {true, false, "x28"};
    info.m_info[X29] = {true, true, "x29"};  // frame pointer, not allocatable
    info.m_info[X30] = {false, true, "x30"};  // link register
    info.m_info[SP] = {false, true, "sp"};

    for (int i = 0; i <= 31; i++) {
      bool saved = i >= 8 && i <= 15;  // v8-v15 callee-saved per Apple ABI
      info.m_info_simd[i] = {saved, false, "v" + std::to_string(i)};
    }

    info.m_gpr_arg_regs =
        std::array<Register, N_ARGS>({X0, X1, X2, X3, X4, X5, X6, X7});
    info.m_xmm_arg_regs = std::array<Register, N_ARGS>({V0, V1, V2, V3, V4, V5, V6, V7});
    info.m_saved_gprs = {X19, X20, X21, X22, X23, X24, X25, X26, X27, X28};
    info.m_saved_xmms = {V8, V9, V10, V11, V12, V13, V14, V15};

    for (auto sr : info.m_saved_gprs) {
      info.m_saved_all.push_back(sr);
    }
    for (auto sr : info.m_saved_xmms) {
      info.m_saved_all.push_back(sr);
    }

    // x16, x17 and x18 are intentionally absent from every allocation order.
    info.m_gpr_alloc_order = {X0,  X1,  X2,  X3,  X4,  X5,  X6,  X7,  X8,  X9,  X10, X11,
                              X12, X13, X14, X15, X19, X20, X21, X22, X23, X24, X25, X26,
                              X27, X28};
    info.m_xmm_alloc_order = {V0,  V1,  V2,  V3,  V4,  V5,  V6,  V7,  V8,  V9,  V10, V11,
                              V12, V13, V14, V15, V16, V17, V18, V19, V20, V21, V22, V23,
                              V24, V25, V26, V27, V28, V29, V30, V31};

    info.m_gpr_temp_only_alloc_order = {X0, X1,  X2,  X3,  X4, X5, X6, X7,
                                        X8, X9,  X10, X11, X12, X13, X14, X15};
    info.m_xmm_temp_only_alloc_order = {V0, V1, V2, V3, V4, V5, V6, V7,
                                        V16, V17, V18, V19, V20, V21, V22, V23, V24, V25,
                                        V26, V27, V28, V29, V30, V31};

    info.m_gpr_spill_temp_alloc_order = {X0, X1,  X2,  X3,  X4,  X5,  X6,  X7,  X8,  X9,  X10,
                                         X11, X12, X13, X14, X15, X19, X20, X21, X22, X23, X24,
                                         X25, X26, X27, X28};
    info.m_xmm_spill_temp_alloc_order = {V0,  V1,  V2,  V3,  V4,  V5,  V6,  V7,  V8,  V9,  V10,
                                         V11, V12, V13, V14, V15, V16, V17, V18, V19, V20, V21,
                                         V22, V23, V24, V25, V26, V27, V28, V29, V30, V31};

    info.m_process_reg = X20;
    info.m_st_reg = X21;
    info.m_offset_reg = X22;
    info.m_gpr_ret_reg = X0;
    info.m_xmm_ret_reg = V0;
  } else {
    ASSERT_MSG(false, "unknown instruction set");
  }

  return info;
}

RegisterInfo gRegInfo = RegisterInfo::make_register_info(emitter::InstructionSet::X86);
RegisterInfo gRegInfoARM64 = RegisterInfo::make_register_info(emitter::InstructionSet::ARM64);

std::string to_string(HWRegKind kind) {
  switch (kind) {
    case HWRegKind::GPR:
      return "gpr";
    case HWRegKind::XMM:
      return "xmm";
    default:
      throw std::runtime_error("Unsupported HWRegKind");
  }
}

HWRegKind reg_class_to_hw(RegClass reg_class) {
  switch (reg_class) {
    case RegClass::VECTOR_FLOAT:
    case RegClass::FLOAT:
    case RegClass::INT_128:
      return HWRegKind::XMM;
    case RegClass::GPR_64:
      return HWRegKind::GPR;
    default:
      ASSERT(false);
      return HWRegKind::INVALID;
  }
}

std::string Register::print() const {
  return gRegInfo.get_info(*this).name;
}

}  // namespace emitter
