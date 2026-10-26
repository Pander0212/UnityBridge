# Files from Madeira

`madeira-jit.js` is copied unchanged from [willfaust/Madeira](https://github.com/willfaust/Madeira)
at commit `e3a88fddb8daed1f5ac28a45dcb4663d65192629` (`app/Madeira/madeira-jit.js`).

License: Madeira is GPL-3.0-or-later with the "Madeira Converter Exception" (an additional
permission for Apple's Metal Shader Converter, see Madeira's `LICENSE-EXCEPTION.md`). UnityBridge
takes the file under GPL-3.0-or-later; the additional permission is not needed here.

What it is: the debugger script StikDebug runs while attached to the app. It services the
BreakpointJIT protocol (`brk #0xf00d` with the command in x16: 0 detach, 1 prepare an executable
region, 2 map page zero; legacy `brk #0x69`) and hands every other stop back to the app as a unix
signal, so FEXCore's own signal handlers (unaligned access, faults) keep working while a debugger is
attached. UnityBridgeProbe passes it to StikDebug in the `stikdebug://enable-jit` URL.
