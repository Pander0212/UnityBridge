/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Host tests only (x86-64 Linux): where HostNative stubs jump. On entry rax holds the operation,
 * r10/rdx/r8/r9 the Windows argument registers (the stub copied rcx to r10), [rsp] the return
 * address into the caller. Builds a CallContext (win32/hle.h; offsets checked there) on the stack
 * and calls ub_win32_host_dispatch with the System V convention.
 *
 * As well as the argument registers it records the full guest integer and XMM state and the return
 * address, so the SEH functions can snapshot a CONTEXT (RtlCaptureContext) and, when the dispatcher
 * asks for a non-local resume (NtContinue), this stub loads the target registers and jumps there
 * instead of returning normally. Windows treats rdi, rsi and xmm6-15 as callee-saved; on the normal
 * path they are restored from the recorded state.
 */
    .intel_syntax noprefix
    .text
    .globl ub_win32_host_entry
    .type ub_win32_host_entry, @function
ub_win32_host_entry:
    push rbp
    mov rbp, rsp
    sub rsp, 0x400              /* room for the whole CallContext (>= sizeof) */
    and rsp, -16
    /* argument-order fields the handlers read through CallContext::Arg */
    mov [rsp], rax             /* id (low 16 bits), tag above */
    mov [rsp + 8], r10         /* gpr[0] = rcx */
    mov [rsp + 16], rdx        /* gpr[1] */
    mov [rsp + 24], r8         /* gpr[2] */
    mov [rsp + 32], r9         /* gpr[3] */
    movdqu [rsp + 40], xmm0
    movdqu [rsp + 56], xmm1
    movdqu [rsp + 72], xmm2
    movdqu [rsp + 88], xmm3
    lea rax, [rbp + 8]
    mov [rsp + 104], rax       /* rsp at the stub's entry: the return address */
    mov qword ptr [rsp + 112], 0   /* ret */
    mov qword ptr [rsp + 120], 0   /* xmmRet lo */
    mov qword ptr [rsp + 128], 0   /* xmmRet hi */
    mov byte ptr [rsp + 136], 0    /* returnsFloat */
    /* full integer state x86[16] at 144 (X86State order RAX..R15) */
    mov [rsp + 144 + 0*8], rax   /* RAX (= op, volatile) */
    mov [rsp + 144 + 1*8], r10   /* RCX (stub moved rcx -> r10) */
    mov [rsp + 144 + 2*8], rdx   /* RDX */
    mov [rsp + 144 + 3*8], rbx   /* RBX */
    lea rax, [rbp + 16]
    mov [rsp + 144 + 4*8], rax   /* RSP (guest rsp = rbp + 16) */
    mov rax, [rbp]
    mov [rsp + 144 + 5*8], rax   /* RBP (saved guest rbp) */
    mov [rsp + 144 + 6*8], rsi   /* RSI */
    mov [rsp + 144 + 7*8], rdi   /* RDI */
    mov [rsp + 144 + 8*8], r8    /* R8 */
    mov [rsp + 144 + 9*8], r9    /* R9 */
    mov [rsp + 144 + 10*8], r10  /* R10 (volatile) */
    mov [rsp + 144 + 11*8], r11  /* R11 (volatile) */
    mov [rsp + 144 + 12*8], r12  /* R12 */
    mov [rsp + 144 + 13*8], r13  /* R13 */
    mov [rsp + 144 + 14*8], r14  /* R14 */
    mov [rsp + 144 + 15*8], r15  /* R15 */
    mov rax, [rbp + 8]
    mov [rsp + 912], rax         /* rip = return address into the caller */
    /* full XMM state xmmFull[16] at 272 */
    movdqu [rsp + 272 + 0*16], xmm0
    movdqu [rsp + 272 + 1*16], xmm1
    movdqu [rsp + 272 + 2*16], xmm2
    movdqu [rsp + 272 + 3*16], xmm3
    movdqu [rsp + 272 + 4*16], xmm4
    movdqu [rsp + 272 + 5*16], xmm5
    movdqu [rsp + 272 + 6*16], xmm6
    movdqu [rsp + 272 + 7*16], xmm7
    movdqu [rsp + 272 + 8*16], xmm8
    movdqu [rsp + 272 + 9*16], xmm9
    movdqu [rsp + 272 + 10*16], xmm10
    movdqu [rsp + 272 + 11*16], xmm11
    movdqu [rsp + 272 + 12*16], xmm12
    movdqu [rsp + 272 + 13*16], xmm13
    movdqu [rsp + 272 + 14*16], xmm14
    movdqu [rsp + 272 + 15*16], xmm15
    mov byte ptr [rsp + 928], 1    /* haveState */
    mov byte ptr [rsp + 929], 0    /* resume */

    mov rdi, rsp
    call ub_win32_host_dispatch

    cmp byte ptr [rsp + 929], 0
    jne .Lresume

    /* normal return: restore Windows callee-saved registers from the recorded state */
    mov rdi, [rsp + 144 + 7*8]
    mov rsi, [rsp + 144 + 6*8]
    movdqu xmm6, [rsp + 272 + 6*16]
    movdqu xmm7, [rsp + 272 + 7*16]
    movdqu xmm8, [rsp + 272 + 8*16]
    movdqu xmm9, [rsp + 272 + 9*16]
    movdqu xmm10, [rsp + 272 + 10*16]
    movdqu xmm11, [rsp + 272 + 11*16]
    movdqu xmm12, [rsp + 272 + 12*16]
    movdqu xmm13, [rsp + 272 + 13*16]
    movdqu xmm14, [rsp + 272 + 14*16]
    movdqu xmm15, [rsp + 272 + 15*16]
    movdqu xmm0, [rsp + 120]    /* xmmRet; zero unless the function returns a float */
    mov rax, [rsp + 112]
    leave
    ret

.Lresume:
    /* Non-local resume (NtContinue): load the target register file and jump to resumeRip. */
    movdqu xmm0, [rsp + 656 + 0*16]
    movdqu xmm1, [rsp + 656 + 1*16]
    movdqu xmm2, [rsp + 656 + 2*16]
    movdqu xmm3, [rsp + 656 + 3*16]
    movdqu xmm4, [rsp + 656 + 4*16]
    movdqu xmm5, [rsp + 656 + 5*16]
    movdqu xmm6, [rsp + 656 + 6*16]
    movdqu xmm7, [rsp + 656 + 7*16]
    movdqu xmm8, [rsp + 656 + 8*16]
    movdqu xmm9, [rsp + 656 + 9*16]
    movdqu xmm10, [rsp + 656 + 10*16]
    movdqu xmm11, [rsp + 656 + 11*16]
    movdqu xmm12, [rsp + 656 + 12*16]
    movdqu xmm13, [rsp + 656 + 13*16]
    movdqu xmm14, [rsp + 656 + 14*16]
    movdqu xmm15, [rsp + 656 + 15*16]
    mov rax, [rsp + 528 + 4*8]     /* target RSP */
    mov rcx, [rsp + 920]           /* resumeRip */
    mov [rax - 8], rcx             /* stage the return address at target-8 */
    mov rcx, [rsp + 528 + 0*8]     /* target RAX */
    mov [rax - 16], rcx            /* stage target RAX at target-16 */
    mov rbx, [rsp + 528 + 3*8]
    mov rbp, [rsp + 528 + 5*8]
    mov rsi, [rsp + 528 + 6*8]
    mov rdi, [rsp + 528 + 7*8]
    mov r8,  [rsp + 528 + 8*8]
    mov r9,  [rsp + 528 + 9*8]
    mov r10, [rsp + 528 + 10*8]
    mov r11, [rsp + 528 + 11*8]
    mov r12, [rsp + 528 + 12*8]
    mov r13, [rsp + 528 + 13*8]
    mov r14, [rsp + 528 + 14*8]
    mov r15, [rsp + 528 + 15*8]
    mov rcx, [rsp + 528 + 1*8]
    mov rdx, [rsp + 528 + 2*8]
    lea rsp, [rax - 16]
    pop rax                        /* target RAX; rsp -> target-8 */
    ret                            /* jumps to resumeRip, rsp -> target RSP */
    .size ub_win32_host_entry, . - ub_win32_host_entry
    .section .note.GNU-stack, "", @progbits
