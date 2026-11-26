// The fault guard (evr/seh.h).
#include "evr/seh.h"

#include <windows.h>

#include <atomic>

#include "evr/core.h"

// RtlCaptureContext as if the guard's own function called it: a tail jump keeps the caller's return address on the
// stack, so the context saved is the caller's, at the instruction after the call.
asm(".text\n"
    ".globl evr_seh_capture\n"
    "evr_seh_capture:\n"
    "  jmp *__imp_RtlCaptureContext(%rip)\n");

namespace evr {
namespace {

// Win32 TLS, not thread_local: MinGW's thread_local goes through emutls, which may allocate, and the handler runs
// wherever the fault happened (inside the heap, too).
std::atomic<DWORD> g_slot{TLS_OUT_OF_INDEXES};
std::atomic<bool> g_installed{false};

DWORD Slot() {
  DWORD s = g_slot.load(std::memory_order_acquire);
  if (s != TLS_OUT_OF_INDEXES) return s;
  const DWORD mine = TlsAlloc();
  DWORD expected = TLS_OUT_OF_INDEXES;
  if (!g_slot.compare_exchange_strong(expected, mine)) {
    TlsFree(mine);
    return expected;
  }
  return mine;
}

seh::Frame* Top() { return static_cast<seh::Frame*>(TlsGetValue(Slot())); }
void SetTop(seh::Frame* f) { TlsSetValue(Slot(), f); }

bool Catchable(DWORD code) {
  if ((code & 0xF0000000u) != 0xC0000000u) return false;  // errors only (not 0xE06D7363, C++)
  return code != EXCEPTION_STACK_OVERFLOW && code != 0xC0000409u /* fast fail */;
}

LONG CALLBACK OnFault(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (!Catchable(code) || (info->ExceptionRecord->ExceptionFlags & EXCEPTION_NONCONTINUABLE)) return EXCEPTION_CONTINUE_SEARCH;
  const DWORD slot = g_slot.load(std::memory_order_acquire);
  if (slot == TLS_OUT_OF_INDEXES) return EXCEPTION_CONTINUE_SEARCH;
  auto* f = static_cast<seh::Frame*>(TlsGetValue(slot));
  if (f == nullptr || !f->armed) return EXCEPTION_CONTINUE_SEARCH;
  TlsSetValue(slot, f->prev);
  f->armed = false;
  f->code = code;
  f->faulted = true;
  const DWORD flags = info->ContextRecord->ContextFlags;
  *info->ContextRecord = f->ctx;
  info->ContextRecord->ContextFlags = flags & f->ctx.ContextFlags;
  return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

void InstallFaultGuard() {
  bool expected = false;
  if (!g_installed.compare_exchange_strong(expected, true)) return;
  Slot();
  const bool ok = AddVectoredExceptionHandler(1, &OnFault) != nullptr;
  Log(ok ? LogLevel::Info : LogLevel::Warning, "[EVR.SEH] fault guard %s", ok ? "installed" : "NOT installed");
}

namespace seh {

bool Frame::Enter() {
  if (faulted) return false;
  if (!g_installed.load(std::memory_order_acquire)) InstallFaultGuard();
  prev = Top();
  armed = true;
  SetTop(this);
  return true;
}

void Frame::Leave() {
  if (!armed) return;
  armed = false;
  if (Top() == this) SetTop(prev);
}

bool Frame::Accept(long filter) {
  if (filter == 0) RaiseException(code, EXCEPTION_NONCONTINUABLE, 0, nullptr);
  return true;
}

}  // namespace seh
}  // namespace evr
