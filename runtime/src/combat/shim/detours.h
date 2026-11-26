// The part of Microsoft Detours' API EchoCombat's code uses, on the runtime's MinHook.
//
// DetourAttach(&p, detour): p holds the target; on commit it gets the trampoline. DetourDetach(&p, detour): p holds
// the trampoline; on commit the hook comes out and p gets the target back. A commit applies its queued operations in
// order and, if one fails, undoes the ones it had done (Detours' all-or-nothing) and returns that error.
#pragma once

#include <windows.h>

namespace evr_detours {
LONG Begin();
LONG Queue(PVOID* pointer, PVOID detour, bool attach);
LONG Commit();
LONG Abort();
}  // namespace evr_detours

inline LONG DetourTransactionBegin() { return evr_detours::Begin(); }
inline LONG DetourTransactionCommit() { return evr_detours::Commit(); }
inline LONG DetourTransactionAbort() { return evr_detours::Abort(); }
inline LONG DetourUpdateThread(HANDLE) { return NO_ERROR; }  // MinHook suspends the other threads itself

template <typename D>
inline LONG DetourAttach(PVOID* pointer, D detour) {
  return evr_detours::Queue(pointer, reinterpret_cast<PVOID>(detour), true);
}
template <typename D>
inline LONG DetourDetach(PVOID* pointer, D detour) {
  return evr_detours::Queue(pointer, reinterpret_cast<PVOID>(detour), false);
}
