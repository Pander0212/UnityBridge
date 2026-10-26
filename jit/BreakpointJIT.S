/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * UnityBridge's BreakpointJIT.framework: the request channel to StikDebug.
 *
 * FEXCore's iOS allocator (FEXCore/Source/Utils/Allocator.cpp) and AetherPS4's
 * ios_jit_allocator.cpp dlopen "@executable_path/Frameworks/BreakpointJIT.framework/BreakpointJIT"
 * and look up these symbols. The original framework has no stated license, so this one is written
 * from the protocol, as Husk does (src/ios-jit/husk-brk.S): brk #0xf00d with the command in x16
 * (0 = detach, 1 = prepare region; arguments in x0/x1, result in x0), brk #0x69 for the legacy
 * mark call. StikDebug's universal JIT script services the traps.
 *
 * The framework carries no entitlements, so AMFI accepts it in a sideloaded app.
 */

.section __TEXT,__text,regular,pure_instructions

/* void *BreakGetJITMapping(void *addr, size_t len) */
.globl _BreakGetJITMapping
.p2align 2
_BreakGetJITMapping:
    mov     x16, #1
    brk     #0xf00d
    ret

/* void BreakJITDetach(void) */
.globl _BreakJITDetach
.p2align 2
_BreakJITDetach:
    mov     x16, #0
    brk     #0xf00d
    ret

/* void BreakMarkJITMapping(size_t bytes): legacy, kept for completeness */
.globl _BreakMarkJITMapping
.p2align 2
_BreakMarkJITMapping:
    brk     #0x69
    ret
