// SPDX-License-Identifier: GPL-3.0-or-later
// Host (x86-64 System V) build of ub_call_native, so the il2cpp InternalCall marshalling can be
// tested natively on CI. Same contract as the ARM64 trampoline (engine/call_native_arm64.S): load
// the integer and floating argument registers from gpr[]/fpr[], call fn, write its integer (rax)
// and floating (xmm0) results to out. System V has six integer argument registers, so a thunk with
// more than six integer arguments would need the stack; the host tests stay within six.
#if defined(__x86_64__)
.text
.globl ub_call_native
ub_call_native:
    pushq %rbx
    movq %rdi, %r10            // fn -> call target (r10: not an argument register, survives the loads;
                               // rax cannot hold it, the varargs vector count below overwrites %al)
    movq %rcx, %rbx            // out (callee-saved across the call)
    movsd   0(%rdx), %xmm0     // fpr[] -> xmm0-7 (read before rdx is reused as an int arg)
    movsd   8(%rdx), %xmm1
    movsd  16(%rdx), %xmm2
    movsd  24(%rdx), %xmm3
    movsd  32(%rdx), %xmm4
    movsd  40(%rdx), %xmm5
    movsd  48(%rdx), %xmm6
    movsd  56(%rdx), %xmm7
    movq %rsi, %r11            // gpr base
    movq   0(%r11), %rdi
    movq   8(%r11), %rsi
    movq  16(%r11), %rdx
    movq  24(%r11), %rcx
    movq  32(%r11), %r8
    movq  40(%r11), %r9
    movb $8, %al               // number of vector registers used (System V varargs rule)
    call *%r10
    movq %rax, 0(%rbx)         // integer result
    movsd %xmm0, 8(%rbx)       // floating result
    popq %rbx
    ret
.section .note.GNU-stack,"",@progbits
#endif
