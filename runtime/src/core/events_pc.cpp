// Component events (PC), from our own reading of echovr.exe (sha256 b6d08277...).
//
// The game sends a component event through one of three functions, all (CncaGameSpace* space, const SComponentEvent*
// event, u32 flags):
//   0x14050fb60  to the space itself (and the spaces it holds)
//   0x14050fcd0  to every space, including the ones being loaded
//   0x14050fed0  to every loaded space
// Each posts the event to the space's event DB (0x1404d9e80) and then calls itself for the child spaces of the event's
// sender, so one event can pass through several nested calls. The listeners see it once: only the outermost call on
// the thread dispatches, before the original runs, so a rewrite in place is what the game handles.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <tuple>
#include <vector>

#include "evr/core.h"
#include "evr/events.h"

namespace evr {
namespace {

using SendFn = void (*)(void* space, const void* event, std::uint32_t flags);

constexpr std::uint64_t kSendLocalVA = 0x14050FB60;
constexpr std::uint8_t kSendLocalPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
                                               0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x70};
constexpr std::uint64_t kSendAllVA = 0x14050FCD0;
constexpr std::uint64_t kSendLoadedVA = 0x14050FED0;
constexpr std::uint8_t kSendGlobalPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89,
                                                0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83,
                                                0xEC, 0x70};

SendFn g_original[3] = {nullptr, nullptr, nullptr};
std::vector<EventListener> g_listeners;
thread_local int t_depth = 0;

void Dispatch(void* space, const void* event) {
  if (event == nullptr || !IsReadable(event, event_layout::kEventBytes)) return;
  for (EventListener fn : g_listeners) fn(reinterpret_cast<std::uint64_t>(space), reinterpret_cast<std::uint64_t>(event));
}

template <int I>
void Detour(void* space, const void* event, std::uint32_t flags) {
  if (t_depth == 0) Dispatch(space, event);
  ++t_depth;
  g_original[I](space, event, flags);
  --t_depth;
}

template <typename T>
bool ReadAt(std::uint64_t at, T* out) {
  if (at == 0 || !IsReadable(reinterpret_cast<const void*>(at), sizeof(T))) return false;
  std::memcpy(out, reinterpret_cast<const void*>(at), sizeof(T));
  return true;
}

std::atomic<int> g_logLimit{0};
std::mutex g_seenMutex;
std::set<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>> g_seen;

void LogEvent(std::uint64_t space, std::uint64_t event) {
  std::uint64_t symbol = 0, key = 0;
  if (!ReadAt(event + event_layout::kSymbolOffset, &symbol) || !ReadAt(event + event_layout::kComponentOffset, &key)) return;
  const std::uint64_t sender = EventSender(space, event);
  {
    std::lock_guard<std::mutex> lock(g_seenMutex);
    if (static_cast<int>(g_seen.size()) >= g_logLimit.load()) return;
    if (!g_seen.insert({symbol, sender, key}).second) return;
  }
  Log(LogLevel::Info, "[EVR.EVENT] seen symbol=%016llx sender=%016llx key=%016llx space=%p", (unsigned long long)symbol,
      (unsigned long long)sender, (unsigned long long)key, reinterpret_cast<void*>(space));
}

}  // namespace

std::uint64_t EventSender(std::uint64_t space, std::uint64_t event) {
  std::uint64_t word = 0, table = 0, names = 0, count = 0, name = 0;
  if (!ReadAt(event + event_layout::kSenderOffset, &word)) return 0;
  const auto index = static_cast<std::uint16_t>(word & 0xFFFF);
  if (index == event_layout::kNoSender) return 0;
  if (!ReadAt(space + event_layout::kSpaceActorTableOffset, &table) ||
      !ReadAt(table + event_layout::kTableNamesOffset, &names) ||
      !ReadAt(table + event_layout::kTableCountOffset, &count) || index >= count)
    return 0;
  return ReadAt(names + std::uint64_t{index} * 8, &name) ? name : 0;
}

void LogDistinctEvents(int limit) {
  const bool first = g_logLimit.exchange(limit) == 0;
  if (first && limit > 0) OnComponentEvent(&LogEvent);
}

void OnComponentEvent(EventListener fn) {
  if (fn != nullptr) g_listeners.push_back(fn);
}

bool SendComponentEvent(std::uint64_t space, const void* event) {
  if (g_original[0] == nullptr || space == 0 || event == nullptr) return false;
  Detour<0>(reinterpret_cast<void*>(space), event, 1);
  return true;
}

bool InstallEventHook() {
  const bool a = HookVA(kSendLocalVA, kSendLocalPrologue, reinterpret_cast<void*>(&Detour<0>),
                        reinterpret_cast<void**>(&g_original[0]), "SendComponentEvent (space)");
  const bool b = HookVA(kSendAllVA, kSendGlobalPrologue, reinterpret_cast<void*>(&Detour<1>),
                        reinterpret_cast<void**>(&g_original[1]), "SendComponentEvent (all spaces)");
  const bool c = HookVA(kSendLoadedVA, kSendGlobalPrologue, reinterpret_cast<void*>(&Detour<2>),
                        reinterpret_cast<void**>(&g_original[2]), "SendComponentEvent (loaded spaces)");
  Log(a && b && c ? LogLevel::Info : LogLevel::Warning, "[EVR.EVENT] senders hooked space=%d all=%d loaded=%d listeners=%zu",
      a, b, c, g_listeners.size());
  return a || b || c;
}

}  // namespace evr
