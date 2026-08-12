.section __TEXT,__text,regular,pure_instructions
.subsections_via_symbols

.globl _aot_fixture_scalar
.p2align 2
_aot_fixture_scalar:
  // x0 = initial value, x1 = increment, x2 = loop count.
  mov x3, x0
  cbz x2, 1f
0:
  add x3, x3, x1
  subs x2, x2, #1
  b.ne 0b
1:
  add x0, x3, #7
  ret

.globl _aot_fixture_callee
.p2align 2
_aot_fixture_callee:
  add x0, x0, x1
  ret

.globl _aot_fixture_caller
.p2align 2
_aot_fixture_caller:
  stp x29, x30, [sp, #-16]!
  bl _aot_fixture_callee
  add x0, x0, #7
  ldp x29, x30, [sp], #16
  ret

.globl _aot_fixture_vector
.p2align 2
_aot_fixture_vector:
  add v0.4s, v0.4s, v1.4s
  ret
