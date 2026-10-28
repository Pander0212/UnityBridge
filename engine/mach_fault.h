// SPDX-License-Identifier: GPL-3.0-or-later
// Hardware faults on guest threads (iPhone). Under StikDebug a fault in guest code reaches no BSD
// signal handler, so FEXCore's own fixup for unaligned atomics never runs and the app dies silently.
// This registers a thread-level Mach exception port on every host thread that runs guest code, lets
// FEXCore fix up what it can (and resumes the guest), and writes a fault report for what it cannot.
// Shared by the device runners (engine/win32_device.cpp, engine/il2cpp_probe.cpp). See mach_fault.cpp.
#pragma once

#include <cstdint>
#include <string_view>

namespace UnityBridge::MachFault {

// Creates the exception port and its service thread once per process; later calls only update the
// module range. [imageBase, imageEnd) names the main guest module, so a fault report can give the
// guest RIP as an offset into it (0, 0: none).
void Install(uintptr_t imageBase = 0, uintptr_t imageEnd = 0);

// Registers the calling host thread for fault handling: its own saved context, trampoline stack and
// breadcrumb. Call on every host thread before it runs guest code; pair with UnregisterThisThread
// before the thread exits. A no-op before Install, or when registered already.
void RegisterThisThread();
void UnregisterThisThread();

// Per-thread breadcrumb: the Win32 import this thread is dispatching (or dispatched last). A fault
// report prints it, which names the API in progress when a fault leaves no other trace.
void NoteImport(std::string_view dll, std::string_view name);

} // namespace UnityBridge::MachFault
