// SPDX-License-Identifier: GPL-3.0-or-later
// Hardware faults on guest threads (iPhone): see mach_fault.h.
//
// Under StikDebug the debugger holds the TASK-level Mach exception port and tells debugserver to
// IGNORE EXC_BAD_ACCESS/EXC_BAD_INSTRUCTION (QSetIgnoredExceptions, see third_party/madeira). A
// hardware fault therefore reaches no handler -- the FEX port registers none -- and never becomes a
// BSD signal: FEXCore's own fixup never runs and the process vanishes without a trace.
//
// The faults il2cpp hits are FEX translating x86 unaligned atomic / load-acquire accesses (packed
// metadata structs: 8-byte fields on 4-byte offsets) into ARM64 load-acquire/store-release, which
// the CPU rejects when unaligned (EXC_ARM_DA_ALIGN). FEXCore handles exactly this in
// HandleGuestSignal: for a BUS_ADRALN fault inside its code buffer it backpatches the JIT'd
// instruction to a plain access (so that site never faults again) and says where to resume. That
// path reads the FAULTING thread's thread-local FEX state, so it must run on that thread, not on
// the thread servicing the Mach message.
//
// So: every host thread that runs guest code registers a THREAD-level port (consulted before the
// task port, so debugserver's BRK/JIT handling is untouched; EXC_BREAKPOINT is never claimed) and
// gets a slot of its own -- saved context, trampoline stack, breadcrumb. The service thread saves
// the faulting state (GPRs and NEON) into the slot and redirects the thread onto Trampoline(slot)
// on the slot's stack. The trampoline lets FEX fix up the fault; to resume, it touches kResumeMagic
// (in __PAGEZERO), and that second fault makes the service thread load the fixed-up state back into
// the thread with thread_set_state. (setcontext() cannot do it: on Apple arm64 the ucontext functions
// are stubs that fail with ENOTSUP -- device-confirmed.) A fault FEX cannot fix gets a report of the
// guest RIP, guest registers, exact faulting ARM64 words and last Win32 import, then _exit.
//
// Fixups are serialised and remembered per site: when two threads fault on the same instruction,
// the second finds it already patched and simply re-runs it, instead of handing FEX an instruction
// it no longer recognises.
#include "engine/mach_fault.h"

#if defined(__APPLE__)
#include "core/fex/fex_guest_engine.h"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 1  // Darwin <ucontext.h> hard-errors on its deprecated declarations without this
#endif
#include <ucontext.h>    // ucontext_t / _STRUCT_MCONTEXT64, to drive FEX's fault fixup
#include <mach/mach.h>
#include <mach/arm/thread_status.h>

namespace UnityBridge::MachFault {
namespace {

constexpr size_t kStackSize = 256 * 1024;     // trampoline stack per thread, safe on a stack overflow
constexpr size_t kMaxThreads = 256;
constexpr size_t kMaxFixedSites = 4096;
constexpr uintptr_t kResumeMagic = 0xfe5c0;   // in __PAGEZERO (< 4 GiB): never mapped, touching it faults
constexpr exception_mask_t kMask = EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION | EXC_MASK_ARITHMETIC;

struct Slot {
  mach_port_t thread = MACH_PORT_NULL;  // our send right to the thread; also the lookup key
  uint8_t* stack = nullptr;
  _STRUCT_MCONTEXT64 mc {};             // state at the fault, fixed up in place by FEX
  uint64_t faultAddr = 0, hostPc = 0, code = 0;
  int type = 0;
  volatile sig_atomic_t resumeReady = 0;   // the trampoline's next fault is the resume request
  volatile sig_atomic_t inTrampoline = 0;  // a fault inside the trampoline itself goes to the report
};

struct FixedSite {
  uint64_t pc;
  int64_t adjustment;  // resume pc - fault pc, as FEX returned it
};

mach_port_t g_port = MACH_PORT_NULL;
uintptr_t g_imageBase = 0, g_imageEnd = 0;
std::mutex g_slotsMutex;  // registration vs. the service thread's lookup
Slot g_slots[kMaxThreads];
std::mutex g_fixMutex;    // serialises FEX fixups and the site table
FixedSite g_fixed[kMaxFixedSites];
size_t g_fixedCount = 0;
volatile sig_atomic_t g_inFatal = 0;
Slot* volatile g_reporter = nullptr;  // the thread writing the fault report
thread_local char t_lastImport[160] = "(none yet)";

void WriteStr(const char* s) {
  if (s) { ssize_t n = write(2, s, std::strlen(s)); (void)n; }
}

#pragma pack(push, 4)
struct ExcRequest {  // EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES message (MIG mach_exception_raise)
  mach_msg_header_t Head;
  mach_msg_body_t msgh_body;
  mach_msg_port_descriptor_t thread;
  mach_msg_port_descriptor_t task;
  NDR_record_t NDR;
  exception_type_t exception;
  mach_msg_type_number_t codeCnt;
  int64_t code[2];
  uint8_t pad[64];  // room for the trailer the kernel appends on receive
};
struct ExcReply {
  mach_msg_header_t Head;
  NDR_record_t NDR;
  kern_return_t RetCode;
};
#pragma pack(pop)

// A fault nothing can fix: report it once, then exit. Any other thread that faults meanwhile parks,
// so it cannot end the process before the report is written.
[[noreturn]] void Report(Slot* slot) {
  if (__atomic_exchange_n(&g_inFatal, 1, __ATOMIC_SEQ_CST)) {
    for (;;) pause();
  }
  g_reporter = slot;
  uint64_t tid = 0;
  pthread_threadid_np(nullptr, &tid);
  char buf[4096];
  std::snprintf(buf, sizeof(buf),
                "\nUB_FAULT ==== FATAL MACH EXCEPTION type=%d code=%#llx fault_addr=%#llx host_pc=%#llx thread=%llu ====\n",
                slot->type, static_cast<unsigned long long>(slot->code),
                static_cast<unsigned long long>(slot->faultAddr), static_cast<unsigned long long>(slot->hostPc),
                static_cast<unsigned long long>(tid));
  WriteStr(buf);
  WriteStr("UB_FAULT last win32 import on this thread: ");
  WriteStr(t_lastImport);
  WriteStr("\n");
  uint64_t rip = 0, syscallNo = 0;
  if (AetherPS4::Fex::BachataQueryGuestRipSyscall(&rip, &syscallNo)) {
    std::snprintf(buf, sizeof(buf), "UB_FAULT guest rip=%#llx rax(syscall)=%#llx\n",
                  static_cast<unsigned long long>(rip), static_cast<unsigned long long>(syscallNo));
    WriteStr(buf);
    if (g_imageBase && rip >= g_imageBase && rip < g_imageEnd) {
      std::snprintf(buf, sizeof(buf), "UB_FAULT   -> inside the main module at +%#llx\n",
                    static_cast<unsigned long long>(rip - g_imageBase));
      WriteStr(buf);
    }
  } else {
    WriteStr("UB_FAULT no active FEX guest thread at the fault\n");
  }
  uint64_t rsp = 0;
  if (AetherPS4::Fex::BachataDumpGuestRegisters(buf, sizeof(buf), &rsp)) { WriteStr(buf); WriteStr("\n"); }
  // The host PC from the Mach message is exact (the guest RIP is only the last checkpoint); the
  // words around it also show whether the fault is inside a live JIT buffer.
  if (slot->hostPc &&
      AetherPS4::Fex::BachataDumpHostCodeWords(reinterpret_cast<void*>(slot->hostPc), buf, sizeof(buf))) {
    WriteStr(buf);
    WriteStr("\n");
  }
  if (slot->faultAddr &&
      AetherPS4::Fex::BachataDescribeHostFaultAddress(reinterpret_cast<void*>(slot->faultAddr), buf, sizeof(buf))) {
    WriteStr(buf);
    WriteStr("\n");
  }
  WriteStr("UB_FAULT ==== end of fault report; exiting ====\n");
  _exit(70);
}

// Runs ON the faulting thread (so FEX's thread-local state is live), on the slot's stack.
[[noreturn]] void Trampoline(Slot* slot) {
  if (g_inFatal) {
    if (slot == g_reporter) {  // faulted again while writing the report: end it here
      WriteStr("\nUB_FAULT (a second fault cut the report short)\n");
      _exit(70);
    }
    for (;;) pause();  // another thread is writing its report
  }
  if (slot->inTrampoline) Report(slot);  // a fault inside the fixup itself
  slot->inTrampoline = 1;

  auto& ss = slot->mc.__ss;
  const auto pc = (uint64_t)(arm_thread_state64_get_pc(ss));  // uint64 (arm64) or void* (arm64e)
  bool fixed = false;
  {
    std::lock_guard lock {g_fixMutex};
    for (size_t i = 0; i < g_fixedCount; ++i) {
      // Patched already by another thread that faulted on the same instruction: FEX re-runs the
      // rewritten code rather than emulating it (adjustment <= 0), so do the same here.
      if (g_fixed[i].pc == pc && g_fixed[i].adjustment <= 0) {
        arm_thread_state64_set_pc_fptr(ss, reinterpret_cast<void*>(pc + g_fixed[i].adjustment));
        fixed = true;
        break;
      }
    }
    if (!fixed) {
      siginfo_t si {};
      si.si_signo = SIGBUS;
      si.si_code = BUS_ADRALN;
      si.si_addr = reinterpret_cast<void*>(slot->faultAddr);
      ucontext_t uc {};
      uc.uc_mcontext = &slot->mc;
      uc.uc_mcsize = sizeof(slot->mc);
      if (AetherPS4::Fex::HandleGuestSignal(SIGBUS, &si, &uc)) {
        fixed = true;
        const auto resumePc = (uint64_t)(arm_thread_state64_get_pc(ss));
        if (g_fixedCount < kMaxFixedSites) g_fixed[g_fixedCount++] = {pc, static_cast<int64_t>(resumePc - pc)};
      }
    }
  }
  if (!fixed) Report(slot);

  // Hand the fixed-up state to the service thread, which loads it into this thread when the
  // deliberate fault below arrives.
  slot->inTrampoline = 0;
  slot->resumeReady = 1;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  *reinterpret_cast<volatile uint32_t*>(kResumeMagic) = 0;
  _exit(72);  // unreachable: the service thread never resumes us here
}

Slot* Find(mach_port_t thread) {
  std::lock_guard lock {g_slotsMutex};
  for (auto& slot : g_slots) {
    if (slot.thread == thread) return &slot;
  }
  return nullptr;
}

void SendReply(const ExcRequest& req, kern_return_t ret) {
  ExcReply reply {};
  reply.Head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(req.Head.msgh_bits), 0);
  reply.Head.msgh_size = sizeof(reply);
  reply.Head.msgh_remote_port = req.Head.msgh_remote_port;
  reply.Head.msgh_local_port = MACH_PORT_NULL;
  reply.Head.msgh_id = req.Head.msgh_id + 100;  // MIG reply id convention
  reply.NDR = NDR_record;
  reply.RetCode = ret;
  mach_msg(&reply.Head, MACH_SEND_MSG, sizeof(reply), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
}

// Redirects a faulting thread onto Trampoline(slot), or resumes one whose fixup is done.
kern_return_t Handle(const ExcRequest& req) {
  const mach_port_t thread = req.thread.name;
  Slot* slot = Find(thread);
  if (slot == nullptr) return KERN_FAILURE;  // not one of ours (unregistered meanwhile)
  const uint64_t faultAddr = req.codeCnt >= 2 ? static_cast<uint64_t>(req.code[1]) : 0;

  if (slot->resumeReady && faultAddr == kResumeMagic) {
    slot->resumeReady = 0;
    thread_set_state(thread, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&slot->mc.__ss),
                     ARM_THREAD_STATE64_COUNT);
    thread_set_state(thread, ARM_NEON_STATE64, reinterpret_cast<thread_state_t>(&slot->mc.__ns),
                     ARM_NEON_STATE64_COUNT);
    return KERN_SUCCESS;
  }

  arm_thread_state64_t ts {};
  mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
  if (thread_get_state(thread, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&ts), &count) != KERN_SUCCESS) {
    return KERN_FAILURE;
  }
  slot->mc.__ss = ts;
  // NEON now, before the trampoline's C code clobbers it; restored on resume.
  mach_msg_type_number_t neonCount = ARM_NEON_STATE64_COUNT;
  thread_get_state(thread, ARM_NEON_STATE64, reinterpret_cast<thread_state_t>(&slot->mc.__ns), &neonCount);
  slot->type = req.exception;
  slot->code = req.codeCnt >= 1 ? static_cast<uint64_t>(req.code[0]) : 0;
  slot->faultAddr = faultAddr;
  slot->hostPc = (uint64_t)(arm_thread_state64_get_pc(ts));

  arm_thread_state64_t nts = ts;
  nts.__x[0] = reinterpret_cast<uint64_t>(slot);  // Trampoline's argument
  arm_thread_state64_set_sp(nts, (reinterpret_cast<uintptr_t>(slot->stack) + kStackSize) & ~uintptr_t {15});
  arm_thread_state64_set_pc_fptr(nts, &Trampoline);
  thread_set_state(thread, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&nts), ARM_THREAD_STATE64_COUNT);
  return KERN_SUCCESS;
}

void* Serve(void*) {
  for (;;) {
    ExcRequest req {};
    if (mach_msg(&req.Head, MACH_RCV_MSG, 0, sizeof(req), g_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) !=
        KERN_SUCCESS) {
      continue;
    }
    SendReply(req, Handle(req));
    // The message carried a send right to the thread and one to the task; give them back (the
    // slot keeps its own right to the thread, so the name stays valid).
    mach_port_deallocate(mach_task_self(), req.thread.name);
    mach_port_deallocate(mach_task_self(), req.task.name);
  }
  return nullptr;
}

} // namespace

void Install(uintptr_t imageBase, uintptr_t imageEnd) {
  std::lock_guard lock {g_slotsMutex};
  g_imageBase = imageBase;
  g_imageEnd = imageEnd;
  if (g_port != MACH_PORT_NULL) return;
  const mach_port_t task = mach_task_self();
  mach_port_t port = MACH_PORT_NULL;
  if (mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS) return;
  mach_port_insert_right(task, port, port, MACH_MSG_TYPE_MAKE_SEND);
  g_port = port;
  pthread_t thread;
  if (pthread_create(&thread, nullptr, &Serve, nullptr) == 0) pthread_detach(thread);
}

void RegisterThisThread() {
  if (g_port == MACH_PORT_NULL) return;
  const mach_port_t task = mach_task_self();
  const mach_port_t self = mach_thread_self();  // kept in the slot until UnregisterThisThread
  std::lock_guard lock {g_slotsMutex};
  Slot* empty = nullptr;
  for (auto& slot : g_slots) {
    if (slot.thread == self) {  // registered already
      mach_port_deallocate(task, self);
      return;
    }
    if (empty == nullptr && slot.thread == MACH_PORT_NULL) empty = &slot;
  }
  void* stack = empty ? mmap(nullptr, kStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0) : MAP_FAILED;
  if (stack == MAP_FAILED) {
    WriteStr("UB_FAULT cannot register a thread for fault handling (no slot or stack)\n");
    mach_port_deallocate(task, self);
    return;
  }
  empty->stack = static_cast<uint8_t*>(stack);
  empty->resumeReady = 0;
  empty->inTrampoline = 0;
  empty->thread = self;
  thread_set_exception_ports(self, kMask, g_port, EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64);
}

void UnregisterThisThread() {
  if (g_port == MACH_PORT_NULL) return;
  const mach_port_t task = mach_task_self();
  const mach_port_t self = mach_thread_self();
  thread_set_exception_ports(self, kMask, MACH_PORT_NULL, EXCEPTION_DEFAULT, ARM_THREAD_STATE64);
  {
    std::lock_guard lock {g_slotsMutex};
    for (auto& slot : g_slots) {
      if (slot.thread != self) continue;
      munmap(slot.stack, kStackSize);
      slot.stack = nullptr;
      slot.thread = MACH_PORT_NULL;
      mach_port_deallocate(task, self);  // the slot's right
      break;
    }
  }
  mach_port_deallocate(task, self);  // this call's right
}

void NoteImport(std::string_view dll, std::string_view name) {
  std::snprintf(t_lastImport, sizeof(t_lastImport), "%.*s!%.*s", static_cast<int>(dll.size()), dll.data(),
                static_cast<int>(name.size()), name.data());
}

} // namespace UnityBridge::MachFault

#else  // not Apple: no Mach exceptions; the host builds never run guest code on a device

namespace UnityBridge::MachFault {
void Install(uintptr_t, uintptr_t) {}
void RegisterThisThread() {}
void UnregisterThisThread() {}
void NoteImport(std::string_view, std::string_view) {}
} // namespace UnityBridge::MachFault

#endif
