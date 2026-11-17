// Actor enable / disable (PC), from our own reading of echovr.exe (sha256 b6d08277...).
//
//   0x1404eda50  Enable(space, SActor ident, u8 reasons)  clears these disable-reason bits (0x1404eda40: reasons = 1, what scripts use)
//   0x1404ecd40  Disable(space, SActor ident, u8 reasons)  sets them (0x1404ecd30: reasons = 1); an actor is enabled when its mask is 0
// EnableActorNode (0x1410aca70) queues one of the two thunks for the actor its script names.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "evr/actors.h"
#include "evr/core.h"
#include "evr/events.h"

namespace evr {
namespace {

using SetFn = void (*)(void* space, std::uint64_t ident, std::uint8_t reasons);

constexpr std::uint64_t kEnableVA = 0x1404EDA50;
constexpr std::uint64_t kDisableVA = 0x1404ECD40;
// Both bodies start the same way.
constexpr std::uint8_t kPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56,
                                      0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr std::uint64_t kIdentBase = 0x0000FFFFFFFF0000ULL;  // as ui_text_pc.cpp builds idents

SetFn g_enable = nullptr;
SetFn g_disable = nullptr;
std::vector<ActorListener> g_listeners;
std::atomic<int> g_logLeft{0};

template <typename T>
bool ReadAt(std::uint64_t at, T* out) {
  if (at == 0 || !IsReadable(reinterpret_cast<const void*>(at), sizeof(T))) return false;
  std::memcpy(out, reinterpret_cast<const void*>(at), sizeof(T));
  return true;
}

std::uint64_t NameAt(std::uint64_t space, std::uint16_t index) {
  std::uint64_t table = 0, names = 0, count = 0, name = 0;
  if (!ReadAt(space + event_layout::kSpaceActorTableOffset, &table) ||
      !ReadAt(table + event_layout::kTableNamesOffset, &names) ||
      !ReadAt(table + event_layout::kTableCountOffset, &count) || index >= count)
    return 0;
  return ReadAt(names + std::uint64_t{index} * 8, &name) ? name : 0;
}

void After(void* space, std::uint64_t ident, std::uint8_t reasons, bool on) {
  const auto index = static_cast<std::uint16_t>(ident & 0xFFFF);
  const std::uint64_t name = NameAt(reinterpret_cast<std::uint64_t>(space), index);
  if (g_logLeft.load() > 0 && g_logLeft.fetch_sub(1) > 0) {
    Log(LogLevel::Info, "[EVR.ACTOR] %s %016llx (#%u) reasons=%u space=%p", on ? "enable " : "disable",
        (unsigned long long)name, index, reasons, space);
  }
  for (ActorListener fn : g_listeners) fn(reinterpret_cast<std::uint64_t>(space), name, on);
}

void EnableHook(void* space, std::uint64_t ident, std::uint8_t reasons) {
  g_enable(space, ident, reasons);
  After(space, ident, reasons, true);
}

void DisableHook(void* space, std::uint64_t ident, std::uint8_t reasons) {
  g_disable(space, ident, reasons);
  After(space, ident, reasons, false);
}

}  // namespace

void OnActorEnabled(ActorListener fn) {
  if (fn != nullptr) g_listeners.push_back(fn);
}

int ActorIndex(std::uint64_t space, std::uint64_t actor) {
  std::uint64_t table = 0, names = 0, count = 0;
  if (!ReadAt(space + event_layout::kSpaceActorTableOffset, &table) ||
      !ReadAt(table + event_layout::kTableNamesOffset, &names) ||
      !ReadAt(table + event_layout::kTableCountOffset, &count) || count == 0 || count > 0xFFFE ||
      !IsReadable(reinterpret_cast<const void*>(names), count * 8))
    return -1;
  const auto* all = reinterpret_cast<const std::uint64_t*>(names);
  for (std::uint64_t i = 0; i < count; ++i)
    if (all[i] == actor) return static_cast<int>(i);
  return -1;
}

bool SetActorEnabled(std::uint64_t space, std::uint64_t actor, bool on) {
  const int index = ActorIndex(space, actor);
  if (index < 0 || g_enable == nullptr || g_disable == nullptr) return false;
  const std::uint64_t ident = kIdentBase | static_cast<std::uint64_t>(index);
  (on ? EnableHook : DisableHook)(reinterpret_cast<void*>(space), ident, 1);
  return true;
}

bool InstallActorHooks(int logLimit) {
  g_logLeft.store(logLimit);
  const bool a = HookVA(kEnableVA, kPrologue, reinterpret_cast<void*>(&EnableHook), reinterpret_cast<void**>(&g_enable),
                        "Actor enable");
  const bool b = HookVA(kDisableVA, kPrologue, reinterpret_cast<void*>(&DisableHook),
                        reinterpret_cast<void**>(&g_disable), "Actor disable");
  return a && b;
}

}  // namespace evr
