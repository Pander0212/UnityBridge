/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Host tests only (x86-64 Linux): where HostNative stubs jump. On entry rax holds the operation,
 * r10/rdx/r8/r9 the Windows argument registers (the stub copied rcx to r10), [rsp] the return
 * address into the caller. Builds a CallContext (win32/hle.h; offsets checked there) on the
 * stack and calls ub_win32_host_dispatch with the System V convention.
 *
 * Windows treats rdi, rsi and xmm6-xmm15 as callee-saved, System V does not, so they are saved
 * around the call.
 */
    .intel_syntax noprefix
    .text
    .globl ub_win32_host_entry
    .type ub_win32_host_entry, @function
ub_win32_host_entry:
    push rbp
    mov rbp, rsp
    sub rsp, 352
    and rsp, -16
    mov [rsp], rax              /* id (low 16 bits), tag above */
    mov [rsp + 8], r10          /* gpr[0] = rcx */
    mov [rsp + 16], rdx
    mov [rsp + 24], r8
    mov [rsp + 32], r9
    movdqu [rsp + 40], xmm0
    movdqu [rsp + 56], xmm1
    movdqu [rsp + 72], xmm2
    movdqu [rsp + 88], xmm3
    lea rax, [rbp + 8]
    mov [rsp + 104], rax        /* rsp at the stub's entry: the return address */
    mov qword ptr [rsp + 112], 0
    mov qword ptr [rsp + 120], 0
    mov qword ptr [rsp + 128], 0
    mov byte ptr [rsp + 136], 0
    mov [rsp + 144], rdi
    mov [rsp + 152], rsi
    movdqu [rsp + 160], xmm6
    movdqu [rsp + 176], xmm7
    movdqu [rsp + 192], xmm8
    movdqu [rsp + 208], xmm9
    movdqu [rsp + 224], xmm10
    movdqu [rsp + 240], xmm11
    movdqu [rsp + 256], xmm12
    movdqu [rsp + 272], xmm13
    movdqu [rsp + 288], xmm14
    movdqu [rsp + 304], xmm15
    mov rdi, rsp
    call ub_win32_host_dispatch
    mov rdi, [rsp + 144]
    mov rsi, [rsp + 152]
    movdqu xmm6, [rsp + 160]
    movdqu xmm7, [rsp + 176]
    movdqu xmm8, [rsp + 192]
    movdqu xmm9, [rsp + 208]
    movdqu xmm10, [rsp + 224]
    movdqu xmm11, [rsp + 240]
    movdqu xmm12, [rsp + 256]
    movdqu xmm13, [rsp + 272]
    movdqu xmm14, [rsp + 288]
    movdqu xmm15, [rsp + 304]
    movdqu xmm0, [rsp + 120]    /* xmmRet; zero unless the function returns a float */
    mov rax, [rsp + 112]
    leave
    ret
    .size ub_win32_host_entry, . - ub_win32_host_entry
    .section .note.GNU-stack, "", @progbits
