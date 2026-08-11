;; GOAL Runtime assembly functions. These exist only in the arm64 version of GOAL.
;; - https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms#Pass-arguments-to-functions-correctly
;; - https://en.wikipedia.org/wiki/Calling_convention#ARM_(A64)
;; - https://student.cs.uwaterloo.ca/~cs452/docs/rpi4b/aapcs64.pdf
;; - s16–s31 (d8–d15, q4–q7) must be preserved
;; - s0–s15 (d0–d7, q0–q3) and d16–d31 (q8–q15) do not need to be preserved
;; - https://devblogs.microsoft.com/oldnewthing/20220728-00/?p=106912
;; - ;; - https://courses.cs.washington.edu/courses/cse469/19wi/arm64.pdf

.text

;; Call C++ code on arm64 systems, from GOAL.
;; Following the macOS documentation which mostly aligns with standard arm64
.global _arg_call_arm64
.align 4
_arg_call_arm64:
  ;; The generated trampoline keeps the target in x16.  This is important because
  ;; GOAL's stack pointer is a virtual offset until this bridge converts it.
  mov x8, x16
  ;; GOAL code represents its stack pointer as an offset from the host memory
  ;; base in x22. Convert it to a host address before touching it natively.
  mov x9, sp
  lsr x9, x9, #32
  cbnz x9, .Larg_call_native_stack
  add sp, sp, x22
  mov x9, #1
  b .Larg_call_save_frame
.Larg_call_native_stack:
  mov x9, #0
.Larg_call_save_frame:
  stp	x29, x30, [sp, #-16]!
  stp x19, x20, [sp, #-16]!
  ;; Preserve the complete fixed GOAL context across the native call.
  stp x21, x22, [sp, #-16]!
  mov	x29, sp
  mov x19, x9

  ; Putting an exclamation point after the close-bracket 
  ; means that the calculated effective address is written back to the base register. (pre-indexing)
  stp q15, q14, [sp, #-32]!
  stp q13, q12, [sp, #-32]!
  stp q11, q10, [sp, #-32]!
  stp q9, q8, [sp, #-32]!

  blr x8

  ldp q9, q8, [sp], #32
  ldp q11, q10, [sp], #32
  ldp q13, q12, [sp], #32
  ldp q15, q14, [sp], #32

  mov x9, x19
  ldp x21, x22, [sp], #16
  ldp x19, x20, [sp], #16
  ldp	x29, x30, [sp], #16
  ;; Restore the virtual GOAL stack pointer for the caller.
  cbz x9, .Larg_call_return
  sub sp, sp, x22
.Larg_call_return:
  ret


;; Call C++ code on arm64 systems, from GOAL. 
;; 
;; Put arguments on the stack and put a pointer to this array in the first arg.
;; this function pushes all 8 OpenGOAL registers into a stack array.
;; then it calls the function pointed to by x0 (RAX in x86) with a pointer to this array.
;; it returns the return value of the called function.
.global _stack_call_arm64
.align 4
_stack_call_arm64:
  ;; The generated trampoline keeps the target in x16.  Do not touch the virtual
  ;; GOAL stack until it has been converted below.
  mov x8, x16
  ;; The caller's SP is a GOAL offset; use a host address for this C bridge.
  mov x9, sp
  lsr x9, x9, #32
  cbnz x9, .Lstack_call_native_stack
  add sp, sp, x22
  mov x9, #1
  b .Lstack_call_save_frame
.Lstack_call_native_stack:
  mov x9, #0
.Lstack_call_save_frame:
	stp	x29, x30, [sp, #-16]!
  stp x19, x20, [sp, #-16]!
  ;; Preserve the complete fixed GOAL context across the native call.
  stp x21, x22, [sp, #-16]!
  mov	x29, sp
  mov x19, x9

  stp q15, q14, [sp, #-32]!
  stp q13, q12, [sp, #-32]!
  stp q11, q10, [sp, #-32]!
  stp q9, q8, [sp, #-32]!

  ; create a stack array of arguments in GOAL register order.
  ; The C bridge expects args[0]..args[7] to correspond to x0..x7.
  sub sp, sp, #64
  stp x0, x1, [sp, #0]
  stp x2, x3, [sp, #16]
  stp x4, x5, [sp, #32]
  stp x6, x7, [sp, #48]

  ; set first argument to the packed eight-word argument array
  mov x0, sp
  ; call function
  blr x8
  ; Preserve the callee's return value while restoring the packed registers.
  mov x8, x0
  ; restore arguments
  ldp x0, x1, [sp, #0]
  ldp x2, x3, [sp, #16]
  ldp x4, x5, [sp, #32]
  ldp x6, x7, [sp, #48]
  add sp, sp, #64
  mov x0, x8

  ldp q9, q8, [sp], #32
  ldp q11, q10, [sp], #32
  ldp q13, q12, [sp], #32
  ldp q15, q14, [sp], #32

  mov x9, x19
  ldp x21, x22, [sp], #16
  ldp x19, x20, [sp], #16
  ldp	x29, x30, [sp], #16
  ;; Return with the virtual GOAL stack pointer expected by generated code.
  cbz x9, .Lstack_call_return
  sub sp, sp, x22
.Lstack_call_return:
  ; return!
  ret

;; Call c++ code through mips2c.
;; GOAL will call a dynamically generated trampoline.
;; The trampoline will have pushed the exec function and stack offset onto the stack
.global _mips2c_call_arm64
.align 4
_mips2c_call_arm64:
  ;; The trampoline passes stack size in x17 and execution address in x16. Convert
  ;; SP before the native frame prologue; x22 is restored unchanged for the callee.
  mov x10, sp
  lsr x10, x10, #32
  cbnz x10, .Lmips2c_native_stack
  add sp, sp, x22
  mov x10, #1
  b .Lmips2c_save_frame
.Lmips2c_native_stack:
  mov x10, #0
.Lmips2c_save_frame:
	stp	x29, x30, [sp, #-16]!
  stp x19, x20, [sp, #-16]!
  ;; Preserve the complete fixed GOAL context across the native call.
  stp x21, x22, [sp, #-16]!
  mov	x29, sp
  mov x19, x10
  mov x8, x17
  mov x9, x16

  ;; first, save quadword registers
  stp q15, q14, [sp, #-32]!
  stp q13, q12, [sp, #-32]!
  stp q11, q10, [sp, #-32]!
  stp q9, q8, [sp, #-32]!

  ; NOTE - in x86 the 2 special registers are saved (R10 and R11)
  ; we don't need to do that in ARM64, there are plenty of registers to work with

  ;; oof
  sub sp, sp, 1280
  str x0, [sp, #+64] ; arg 0 (RDI in x86) and 
  str x1, [sp, #+80] ; arg 1 (RSI in x86)
  str x2, [sp, #+96] ; arg 2 (RDX in x86) and arg 3 (RCX in x86)
  str x3, [sp, #+112] ; arg 2 (RDX in x86) and arg 3 (RCX in x86)
  str x4, [sp, #+128] ; arg 4 (R8 in x86) and arg 5 (R8 in x86)
  str x5, [sp, #+144] ; arg 4 (R8 in x86) and arg 5 (R8 in x86)
  str x6, [sp, #+160] ; arg 6 (R10 in x86) and arg 7 (R11 in x86)
  str x7, [sp, #+176] ; arg 6 (R10 in x86) and arg 7 (R11 in x86)
  str x20, [sp, #+352] ;; s6 (pp) (R13 in x86) and s7 (st) (R14 in x86)
  str x21, [sp, #+368] ;; s6 (pp) (R13 in x86) and s7 (st) (R14 in x86)

  mov x0, sp ; move the stack pointer to arg 0
  ;; The GOAL call contract supplies the R15-compatible offset in x22.
  sub x0, x0, x22
  str x0, [sp, #+464] ;; mip2c code's MIPS stack

  mov x0, sp ;; move the stack pointer to the new position

  ;; Keep the fake GOAL stack aligned even if a caller supplies a non-aligned
  ;; byte count. Save both the original size and the rounded allocation.
  add x10, x8, #15
  and x10, x10, #-16
  sub sp, sp, x10
  stp x8, x10, [sp, #-16]!
  blr x9 ;; call!

  ;; unallocate
  ldp x8, x10, [sp], #16
  add sp, sp, x10

  add sp, sp, 1280 ; reset the stackpointer back

  ldp q9, q8, [sp], #32
  ldp q11, q10, [sp], #32
  ldp q13, q12, [sp], #32
  ldp q15, q14, [sp], #32

  mov x10, x19
  ldp x21, x22, [sp], #16
  ldp x19, x20, [sp], #16
  ldp	x29, x30, [sp], #16
  ;; Restore the virtual GOAL stack pointer before returning to GOAL.
  cbz x10, .Lmips2c_return
  sub sp, sp, x22
.Lmips2c_return:
  ret

;; The _call_goal_asm function is used to call a GOAL function from C.
;; It calls on the parent stack, which is a bad idea if your stack is not already a GOAL stack.
;; It supports up to 3 arguments and a return value.
;; This should be called with the arguments:
;; - first goal arg
;; - second goal arg
;; - third goal arg
;; - address of function to call
;; - address of the symbol table
;; - GOAL memory space offset
.global _call_goal_asm_arm64
.align 4
_call_goal_asm_arm64:
  stp	x29, x30, [sp, #-16]!
  mov	x29, sp
  ;; saved registers we need to modify for GOAL should be preserved
  ; ARM64 requires 16-byte stack pointer alignment
  stp x20, x21, [sp, #-16]!
  str x22, [sp, #-16]!

  ;; x0 - first arg
  ;; x1 - second arg
  ;; x2 - third arg
  ;; x3 - function pointer
  ;; x4 - st (goes in x20 and x21)
  ;; x5 - off (goes in x22)

  ;; set GOAL process
  mov x20, x4
  ;; symbol table
  mov x21, x4
  ;; offset
  mov x22, x5
  ;; call GOAL by function pointer
  blr x3

  ;; restore saved registers.
  ldr x22, [sp], #16
  ldp x20, x21, [sp], #16
  ldp	x29, x30, [sp], #16
  ret

.global _call_goal8_asm_arm64
.align 4
_call_goal8_asm_arm64:
  stp	x29, x30, [sp, #-16]!
  mov	x29, sp
  ;; saved registers we need to modify for GOAL should be preserved
  ; ARM64 requires 16-byte stack pointer alignment
  stp x20, x21, [sp, #-16]!
  str x22, [sp, #-16]!

  ;; x0 - first arg (func)
  ;; x1 - second arg (arg array)
  ;; x2 - third arg  (0)
  ;; x3 - pp (goes in r13)
  ;; x4  - st (goes in r14)
  ;; x5  - off (goes in r15)

  ;; set GOAL function pointer
  mov x20, x3
  ;; st
  mov x21, x4
  ;; offset
  mov x22, x5
  ;; move function to temp
  mov x8, x0
  ;; extract arguments
  ldr x0, [x1]  ;; 0
  ldr x2, [x1, #+16] ;; 2
  ldr x3, [x1, #+24] ;; 3
  ldr x4, [x1, #+32]  ;; 4
  ldr x5, [x1, #+40]  ;; 5
  ldr x6, [x1, #+48] ;; 6
  ldr x7, [x1, #+56]  ;; 7
  ldr x1, [x1, #+8] ;; 1 (do this last)
  ;; call GOAL by function pointer
  blr x8

  ;; retore registers.
  ldr x22, [sp], #16
  ldp x20, x21, [sp], #16
  ldp	x29, x30, [sp], #16
  ret

;; Call goal, but switch stacks.
.global _call_goal_on_stack_asm_arm64
.align 4
_call_goal_on_stack_asm_arm64:
  stp	x29, x30, [sp, #-16]!
  mov	x29, sp
  ;; x0 - stack pointer
  ;; x1 - unused
  ;; x2 - unused
  ;; x3 - function pointer
  ;; x4  - st (goes in x21 and x20)
  ;; x5  - offset (goes in x22)

  ;; saved registers we need to modify for GOAL should be preserved
  ; ARM64 requires 16-byte stack pointer alignment
  stp x20, x21, [sp, #-16]!
  str x22, [sp, #-16]!
  ;; Save the old stack pointer in the new stack. It must be below the
  ;; callee's entry SP so the callee's own prologue cannot overwrite it.
  mov x9, sp

  ;; switch to new stack
  mov sp, x0
  sub sp, sp, #16
  str x9, [sp]

  mov x20, x4 ;; set GOAL function pointer  
  mov x21, x4 ;; symbol table
  mov x22, x5 ;; offset
  ;; call GOAL by function pointer
  blr x3

  ;; Restore the old stack before loading the saved callee-saved registers.
  ldr x9, [sp], #16
  mov sp, x9
  ldr x22, [sp], #16
  ldp x20, x21, [sp], #16
  ldp	x29, x30, [sp], #16
  ret
