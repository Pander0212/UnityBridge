// SPDX-License-Identifier: GPL-3.0-or-later
// void ub_call_native(void* fn, const uint64_t gpr[8], const double fpr[8], void* out)
//   x0 = fn, x1 = gpr (8 x 8 bytes), x2 = fpr (8 x 8 bytes), x3 = out ({uint64_t x0; double d0;})
// Loads x0-x7 from gpr and d0-d7 from fpr, calls fn (AAPCS64), and writes its integer result (x0)
// and floating result (d0) to out. The reverse half of the il2cpp InternalCall bridge: the engine
// function is native ARM64, its arguments already laid out by IcallBridge::Marshal.
#if defined(__aarch64__)
.text
.align 2
.globl _ub_call_native
_ub_call_native:
    stp x19, x20, [sp, #-16]!      // save callee-saved regs used to survive the call
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x19, x0                    // fn
    mov x20, x3                    // out
    ldp d0, d1, [x2, #0]
    ldp d2, d3, [x2, #16]
    ldp d4, d5, [x2, #32]
    ldp d6, d7, [x2, #48]
    mov x9, x1                     // gpr base (x1 is about to be overwritten)
    ldp x0, x1, [x9, #0]
    ldp x2, x3, [x9, #16]
    ldp x4, x5, [x9, #32]
    ldp x6, x7, [x9, #48]
    blr x19
    str x0, [x20, #0]              // integer result
    str d0, [x20, #8]             // floating result
    ldp x29, x30, [sp], #16
    ldp x19, x20, [sp], #16
    ret
#endif

// void ub_call_native_stack(void* fn, const uint64_t gpr[8], const double fpr[8], void* out,
//                           const uint64_t* stack, uint64_t count)
//   As ub_call_native, with `count` 8-byte stack arguments: AAPCS64 wants them at the callee's sp,
//   in order, sp 16-byte aligned. The area is reserved below this frame and freed through x29.
#if defined(__aarch64__)
.text
.align 2
.globl _ub_call_native_stack
_ub_call_native_stack:
    stp x19, x20, [sp, #-16]!
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x19, x0                    // fn
    mov x20, x3                    // out
    add x9, x5, #1                 // bytes = ceil(count / 2) * 16
    lsr x9, x9, #1
    lsl x9, x9, #4
    sub sp, sp, x9
    mov x10, #0
1:  cmp x10, x5
    b.hs 2f
    ldr x11, [x4, x10, lsl #3]
    str x11, [sp, x10, lsl #3]
    add x10, x10, #1
    b 1b
2:  ldp d0, d1, [x2, #0]
    ldp d2, d3, [x2, #16]
    ldp d4, d5, [x2, #32]
    ldp d6, d7, [x2, #48]
    mov x9, x1                     // gpr base
    ldp x0, x1, [x9, #0]
    ldp x2, x3, [x9, #16]
    ldp x4, x5, [x9, #32]
    ldp x6, x7, [x9, #48]
    blr x19
    str x0, [x20, #0]
    str d0, [x20, #8]
    mov sp, x29                    // frees the stack arguments
    ldp x29, x30, [sp], #16
    ldp x19, x20, [sp], #16
    ret
#endif
