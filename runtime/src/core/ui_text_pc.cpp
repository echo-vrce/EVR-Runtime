// UI text (PC), from our own reading of echovr.exe (sha256 b6d08277...).
//
// SetItemText(pool, componentIndex, itemSymbol, const char* text) at 0x14037d470 is the setter every SetText2 script
// node (0x14126dce0) ends in. `pool` is a canvas component pool and `componentIndex` the actor's component in it.
//
// Reaching an actor's (pool, index), as the game's own script nodes do (e.g. 0x14091e953):
//   game      = *(0x1420a0478)
//   space i   = GetGameSpace(game, i) at 0x1404f35e0: i < game[+0x1f80] ? game[+0x1f50][i*16+8] : 0
//   actor idx = position of the actor's name in the space's actor table: [space+0x370] -> table,
//               [table+0x80] -> u64 names, [table+0xa8] -> count
//   ident     = 0x0000ffffffff0000 | actor idx
//   Find(out{pool, u32 index}, space, ident, sym) at 0x14091daf0: asks each pool of the space for the ident's
//               component (vtable +0x1a8) and keeps the first the pool accepts (vtable +0x1c8); out.pool = 0 if none.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "evr/core.h"
#include "evr/frame.h"
#include "evr/ui_text.h"

namespace evr {
namespace {

using SetItemTextFn = int (*)(void* pool, std::uint32_t index, std::uint64_t item, const char* text);
using GetGameSpaceFn = void* (*)(void* game, std::uint64_t index);
struct Handle {
  void* pool;
  std::uint32_t index;
};
using FindFn = void (*)(Handle* out, void* space, std::uint64_t ident, std::uint64_t sym);

constexpr std::uint64_t kSetItemTextVA = 0x14037D470;
constexpr std::uint8_t kSetItemTextPrologue[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x49, 0x8B, 0xD9, 0x4D, 0x85, 0xC9};
constexpr std::uint64_t kGameVA = 0x1420A0478;
constexpr std::uint64_t kGetGameSpaceVA = 0x1404F35E0;
constexpr std::uint8_t kGetGameSpaceBytes[] = {0x48, 0x3B, 0x91, 0x80, 0x1F, 0x00, 0x00, 0x73, 0x10, 0x48, 0x8B, 0x81,
                                               0x50, 0x1F, 0x00, 0x00, 0x48, 0x03, 0xD2, 0x48, 0x8B, 0x44, 0xD0, 0x08};
constexpr std::uint64_t kFindVA = 0x14091DAF0;
constexpr std::uint8_t kFindPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89,
                                          0x54, 0x24, 0x10, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30};
constexpr std::size_t kSpaceCount = 0x1F80, kActorTable = 0x370, kTableNames = 0x80, kTableCount = 0xA8;
constexpr std::uint64_t kIdentBase = 0x0000FFFFFFFF0000ULL;
constexpr std::uint64_t kAnyComponent = ~0ULL;  // as the game passes at 0x1409eed3a
constexpr std::size_t kMaxText = 127;
constexpr std::uint64_t kMaxActors = 0xFFFE;
constexpr std::uint64_t kRetryMs = 500;

SetItemTextFn g_setItemText = nullptr;  // the original (the trampoline once hooked)
std::vector<UiTextFilter> g_filters;

int SetItemTextHook(void* pool, std::uint32_t index, std::uint64_t item, const char* text) {
  for (UiTextFilter fn : g_filters)
    if (const char* r = fn(item, text)) text = r;
  return g_setItemText(pool, index, item, text);
}
GetGameSpaceFn g_getGameSpace = nullptr;
FindFn g_find = nullptr;

std::mutex g_mutex;
std::map<std::pair<std::uint64_t, std::uint64_t>, std::string> g_pending;  // (actor, item) -> text
std::atomic<bool> g_hasPending{false};
// Game thread only:
std::map<std::uint64_t, bool> g_foundLogged, g_missLogged;
std::uint64_t g_nextTryMs = 0;

template <typename T>
bool Read(std::uintptr_t at, T* out) {
  if (at == 0 || !IsReadable(reinterpret_cast<const void*>(at), sizeof(T))) return false;
  std::memcpy(out, reinterpret_cast<const void*>(at), sizeof(T));
  return true;
}

/// The actor's index in `space`'s actor table, or -1.
std::int64_t ActorIndex(std::uintptr_t space, std::uint64_t actor) {
  std::uintptr_t table = 0, names = 0;
  std::uint64_t count = 0;
  if (!Read(space + kActorTable, &table) || !Read(table + kTableNames, &names) || !Read(table + kTableCount, &count))
    return -1;
  if (names == 0 || count == 0 || count > kMaxActors) return -1;
  if (!IsReadable(reinterpret_cast<const void*>(names), count * 8)) return -1;
  const auto* all = reinterpret_cast<const std::uint64_t*>(names);
  for (std::uint64_t i = 0; i < count; ++i)
    if (all[i] == actor) return static_cast<std::int64_t>(i);
  return -1;
}

/// The actor's canvas component, searched in every game space; pool = nullptr when not there (yet).
Handle Resolve(std::uint64_t actor) {
  Handle h{nullptr, 0};
  void* game = nullptr;
  if (!Read(reinterpret_cast<std::uintptr_t>(ResolveVA(kGameVA)), &game) || game == nullptr) return h;
  std::uint64_t spaces = 0;
  if (!Read(reinterpret_cast<std::uintptr_t>(game) + kSpaceCount, &spaces) || spaces > 64) return h;
  for (std::uint64_t s = 0; s < spaces; ++s) {
    void* space = g_getGameSpace(game, s);
    if (space == nullptr) continue;
    const std::int64_t index = ActorIndex(reinterpret_cast<std::uintptr_t>(space), actor);
    if (index < 0) continue;
    g_find(&h, space, kIdentBase | static_cast<std::uint64_t>(index), kAnyComponent);
    if (h.pool != nullptr) {
      if (!g_foundLogged[actor]) {
        g_foundLogged[actor] = true;
        Log(LogLevel::Info, "[EVR.TEXT] actor %016llx -> space %llu actor #%lld pool=%p component=%u",
            (unsigned long long)actor, (unsigned long long)s, (long long)index, h.pool, h.index);
      }
      return h;
    }
  }
  return h;
}

void Drain() {
  if (!g_hasPending.load(std::memory_order_acquire)) return;
  const std::uint64_t now = GetTickCount64();
  if (now < g_nextTryMs) return;
  std::map<std::pair<std::uint64_t, std::uint64_t>, std::string> work;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    work.swap(g_pending);
    g_hasPending = false;
  }
  std::map<std::uint64_t, Handle> handles;
  std::map<std::pair<std::uint64_t, std::uint64_t>, std::string> retry;
  for (auto& [target, text] : work) {
    auto it = handles.find(target.first);
    if (it == handles.end()) it = handles.emplace(target.first, Resolve(target.first)).first;
    const Handle h = it->second;
    if (h.pool == nullptr) {
      if (!g_missLogged[target.first]) {
        g_missLogged[target.first] = true;
        Log(LogLevel::Info, "[EVR.TEXT] actor %016llx not loaded yet; its texts wait", (unsigned long long)target.first);
      }
      retry.emplace(target, std::move(text));
      continue;
    }
    if (g_setItemText(h.pool, h.index, target.second, text.c_str()) == 0)
      Log(LogLevel::Warning, "[EVR.TEXT] item %016llx of actor %016llx refused \"%s\"", (unsigned long long)target.second,
          (unsigned long long)target.first, text.c_str());
  }
  if (!retry.empty()) {
    g_nextTryMs = now + kRetryMs;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& [target, text] : retry) g_pending.emplace(target, std::move(text));  // a newer post wins
    g_hasPending = true;
  }
}

}  // namespace

void OnUiTextSet(UiTextFilter fn) {
  if (fn != nullptr) g_filters.push_back(fn);
}

UiTextResult PostUiText(std::uint64_t actor, std::uint64_t item, const std::string& text) {
  if (text.size() > kMaxText) return UiTextResult::Refused;
  for (unsigned char c : text)
    if (c < 0x20 || c > 0x7E) return UiTextResult::Refused;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_pending[{actor, item}] = text;
  g_hasPending.store(true, std::memory_order_release);
  return UiTextResult::Queued;
}

bool InstallUiText() {
  void* set = ResolveVA(kSetItemTextVA);
  void* get = ResolveVA(kGetGameSpaceVA);
  void* find = ResolveVA(kFindVA);
  if (!CheckBytes(set, kSetItemTextPrologue, sizeof(kSetItemTextPrologue)) ||
      !CheckBytes(get, kGetGameSpaceBytes, sizeof(kGetGameSpaceBytes)) ||
      !CheckBytes(find, kFindPrologue, sizeof(kFindPrologue))) {
    Log(LogLevel::Warning, "[EVR.TEXT] game bytes differ: menu text writes off");
    return false;
  }
  g_setItemText = reinterpret_cast<SetItemTextFn>(set);
  // Our own writes call the original directly; only the game's go through the filters.
  if (!Hook(set, reinterpret_cast<void*>(&SetItemTextHook), reinterpret_cast<void**>(&g_setItemText),
            "SetItemText (text filters)"))
    g_setItemText = reinterpret_cast<SetItemTextFn>(set);
  g_getGameSpace = reinterpret_cast<GetGameSpaceFn>(get);
  g_find = reinterpret_cast<FindFn>(find);
  OnFrame(&Drain);
  Log(LogLevel::Info, "[EVR.TEXT] writer ready");
  return true;
}

}  // namespace evr
