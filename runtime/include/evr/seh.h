// evr/seh.h: a fault guard for MinGW, where MSVC's __try/__except does not exist.
//
//   EVR_TRY { ...reads or calls that may fault... }
//   EVR_EXCEPT (EXCEPTION_EXECUTE_HANDLER) { ...after a fault... }
//
// EVR_TRY saves the CPU context at its start (evr_seh_capture, which returns twice like setjmp). A vectored
// exception handler that runs before any other (InstallFaultGuard) sees a fault on a thread with a guard armed,
// puts the saved context back and lets the thread go on: the capture returns again, this time "faulted", and the
// EVR_EXCEPT block runs. Nothing is unwound: objects built inside the guarded block are not destroyed (MSVC's
// /EHsc behaves the same). Only error-class exceptions (0xC.......) are caught, never C++ exceptions, breakpoints
// or a stack overflow. A filter that evaluates to EXCEPTION_CONTINUE_SEARCH raises the fault again (it is fatal).
#pragma once

#include <windows.h>

#include <cstdint>

extern "C" __attribute__((returns_twice)) void evr_seh_capture(CONTEXT* out);

namespace evr {

/// Registers the fault guard's handler first in line. Call after InstallCrashLog, so a caught fault is not logged
/// as a crash. Guards armed before it work too (it installs itself on the first EVR_TRY).
void InstallFaultGuard();

namespace seh {

struct Frame {
  alignas(16) CONTEXT ctx;
  Frame* prev = nullptr;
  volatile DWORD code = 0;
  volatile bool faulted = false;
  bool armed = false;

  Frame() = default;
  Frame(const Frame&) = delete;
  Frame& operator=(const Frame&) = delete;
  ~Frame() { Leave(); }

  /// After the capture: true the first time (the guard is armed), false once a fault brought the thread back.
  bool Enter();
  void Leave();
  /// The EVR_EXCEPT filter's verdict: EXCEPTION_EXECUTE_HANDLER (1) runs the handler; 0 re-raises; -1 cannot be
  /// honoured (the thread is already back at the guard) and also runs the handler.
  bool Accept(long filter);
};

}  // namespace seh
}  // namespace evr

#define EVR_TRY if (::evr::seh::Frame evr_seh_frame_; evr_seh_capture(&evr_seh_frame_.ctx), evr_seh_frame_.Enter())
#define EVR_EXCEPT(filter) else if (evr_seh_frame_.Accept(static_cast<long>(filter)); true)
#define EVR_EXCEPTION_CODE() (evr_seh_frame_.code)
