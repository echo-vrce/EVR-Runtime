// Detours' transactions on MinHook (shim/detours.h).
#include "shim/detours.h"

#include <MinHook.h>

#include <map>
#include <mutex>
#include <vector>

#include "evr/core.h"

namespace evr_detours {
namespace {

struct Op {
  PVOID* pointer;
  PVOID detour;
  bool attach;
};

std::recursive_mutex g_mutex;
std::vector<Op> g_ops;
bool g_open = false;
std::map<PVOID, PVOID> g_targetOf;  // trampoline -> target, for DetourDetach
struct Chained {
  void** slot;     // the runtime's original pointer, which now holds our detour
  PVOID original;  // what it held before (and what our pointer holds)
};
std::map<PVOID, Chained> g_chained;  // detour -> where it was chained in

LONG Error(MH_STATUS s) { return s == MH_OK ? NO_ERROR : static_cast<LONG>(ERROR_INVALID_OPERATION); }

}  // namespace

LONG Begin() {
  g_mutex.lock();
  if (g_open) {
    g_mutex.unlock();
    return ERROR_INVALID_OPERATION;
  }
  g_open = true;
  g_ops.clear();
  return NO_ERROR;
}

LONG Queue(PVOID* pointer, PVOID detour, bool attach) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (!g_open || pointer == nullptr || *pointer == nullptr || detour == nullptr) return ERROR_INVALID_PARAMETER;
  g_ops.push_back({pointer, detour, attach});
  return NO_ERROR;
}

LONG Abort() {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (!g_open) return ERROR_INVALID_OPERATION;
  g_ops.clear();
  g_open = false;
  g_mutex.unlock();  // Begin's lock
  return NO_ERROR;
}

LONG Commit() {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (!g_open) return ERROR_INVALID_OPERATION;
  static const MH_STATUS init = MH_Initialize();
  (void)init;
  struct Done {
    PVOID* pointer;
    PVOID before;  // what *pointer held
    PVOID target;
    PVOID detour;
    bool attach;
  };
  std::vector<Done> done;
  LONG result = NO_ERROR;
  for (const Op& op : g_ops) {
    if (op.attach) {
      PVOID target = *op.pointer, trampoline = nullptr;
      MH_STATUS s = MH_CreateHook(target, op.detour, &trampoline);
      if (s == MH_ERROR_ALREADY_CREATED) {
        // The runtime hooked it first (evr::Hook): chain in behind its detour (core.h HookOriginalSlot).
        if (void** slot = evr::HookOriginalSlot(target)) {
          done.push_back({op.pointer, target, target, op.detour, true});
          g_chained[op.detour] = {slot, *slot};
          *op.pointer = *slot;
          *slot = op.detour;
          evr::Log(evr::LogLevel::Info, "[EVR.COMBAT] hook at %p chained behind the runtime's", target);
          continue;
        }
      }
      if (s == MH_OK) s = MH_EnableHook(target);
      if (s != MH_OK) {
        evr::Log(evr::LogLevel::Warning, "[EVR.COMBAT] hook at %p failed: %s", target, MH_StatusToString(s));
        if (trampoline != nullptr) MH_RemoveHook(target);
        result = Error(s);
        break;
      }
      done.push_back({op.pointer, target, target, op.detour, true});
      *op.pointer = trampoline;
      g_targetOf[trampoline] = target;
    } else {
      auto ch = g_chained.find(op.detour);
      if (ch != g_chained.end() && *ch->second.slot == op.detour) {
        *ch->second.slot = ch->second.original;
        *op.pointer = ch->second.original;  // still the way to the original
        g_chained.erase(ch);
        continue;
      }
      auto it = g_targetOf.find(*op.pointer);
      if (it == g_targetOf.end()) {
        result = ERROR_INVALID_HANDLE;
        break;
      }
      const PVOID target = it->second;
      MH_STATUS s = MH_DisableHook(target);
      if (s == MH_OK) s = MH_RemoveHook(target);
      if (s != MH_OK) {
        result = Error(s);
        break;
      }
      done.push_back({op.pointer, *op.pointer, target, op.detour, false});
      g_targetOf.erase(it);
      *op.pointer = target;
    }
  }
  if (result != NO_ERROR) {  // undo, newest first
    for (auto d = done.rbegin(); d != done.rend(); ++d) {
      auto ch = d->attach ? g_chained.find(d->detour) : g_chained.end();
      if (ch != g_chained.end()) {
        *ch->second.slot = ch->second.original;
        *d->pointer = d->before;
        g_chained.erase(ch);
      } else if (d->attach) {
        g_targetOf.erase(*d->pointer);
        MH_DisableHook(d->target);
        MH_RemoveHook(d->target);
        *d->pointer = d->before;
      } else {
        PVOID trampoline = nullptr;
        if (MH_CreateHook(d->target, d->detour, &trampoline) == MH_OK && MH_EnableHook(d->target) == MH_OK) {
          *d->pointer = trampoline;
          g_targetOf[trampoline] = d->target;
        }
      }
    }
  }
  g_ops.clear();
  g_open = false;
  g_mutex.unlock();  // Begin's lock
  return result;
}

}  // namespace evr_detours
