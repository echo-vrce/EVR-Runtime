// Resource delivery hook (PC), written for EVR-Runtime from our own reading of echovr.exe
// (sha256 b6d08277e5846900c81004b64b298df6acba834b69700a640b758bda94a52043).
//
// CResourceManager::AsyncResourceIOCallback (0x140fa16d0, named by its profiler string) runs once per finished buffer.
// RCX is the IO block: +0x00 status (4 = read ok), +0x08 resource, +0x10 buffer index, +0x18 data, +0x20 size.
// The resource object: +0x28 class symbol, +0x38 name symbol, +0x40/+0x48 buffer pointers, +0x50/+0x58 sizes.
// Buffers come from the IO allocator ([0x142098f38] + 0x18; vtable +0x08 Alloc(this, size), +0x38 Free(this, ptr)),
// which frees buffer 1 after deserialisation (0x140fa2707). A replacement larger than the stock buffer is allocated
// there too, so the game can free it as its own.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "evr/core.h"
#include "evr/resources.h"
#include "evr/symbol.h"

namespace evr {
namespace {

struct IoBlock {
  std::uint64_t status;
  std::uint8_t* resource;
  std::uint64_t index;
  void* data;
  std::uint64_t size;
};

struct Entry {
  std::uint64_t type, name;
  ResourceTransform fn;  // transform, or null for an override
  void* user;
  const void* data;  // override
  std::size_t size;
  bool pending;  // override not yet served
  std::string label;
};

using CallbackFn = void (*)(IoBlock*);
using AllocFn = void* (*)(void* self, std::uint64_t size);

CallbackFn g_original = nullptr;
std::shared_mutex g_mutex;
std::vector<Entry> g_entries;

constexpr std::uint64_t kCallbackVA = 0x140FA16D0;
constexpr std::uint8_t kCallbackPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x48, 0x83, 0xEC, 0x60,
                                              0x48, 0x8B, 0x41, 0x18, 0x4C, 0x8B, 0xC1};
constexpr std::uint64_t kAllocatorsVA = 0x142098F38;
constexpr std::size_t kIoAllocator = 0x18, kAllocSlot = 0x08;
constexpr std::size_t kClass = 0x28, kName = 0x38;

void* IoAllocator() {
  auto** table = static_cast<std::uint8_t**>(ResolveVA(kAllocatorsVA));
  if (!IsReadable(table, 8) || *table == nullptr || !IsReadable(*table + kIoAllocator, 8)) return nullptr;
  void* a = *reinterpret_cast<void**>(*table + kIoAllocator);
  return IsReadable(a, 8) ? a : nullptr;
}

template <typename Fn>
Fn Slot(void* self, std::size_t slot) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(self))[slot / 8]);
}

/// Copies `bytes` into a buffer the game owns and swaps it into the block (the stock buffer is released).
bool Serve(IoBlock* b, const void* bytes, std::uint64_t size) {
  if (size <= b->size) {
    std::memcpy(b->data, bytes, size);
    b->size = size;
    return true;
  }
  void* a = IoAllocator();
  if (a == nullptr) return false;
  void* fresh = Slot<AllocFn>(a, kAllocSlot)(a, size);
  if (fresh == nullptr) return false;
  std::memcpy(fresh, bytes, size);
  // The stock buffer is left allocated: which path frees buffer 0 is not proven yet, and a one-time leak of a menu
  // resource is harmless next to freeing through the wrong allocator.
  b->data = fresh;
  b->size = size;
  return true;
}

void Apply(IoBlock* b) {
  if (b == nullptr || b->status != 4 || !IsReadable(b->resource + kName, 8)) return;
  const std::uint64_t cls = *reinterpret_cast<std::uint64_t*>(b->resource + kClass);
  const std::uint64_t name = *reinterpret_cast<std::uint64_t*>(b->resource + kName);
  const std::uint64_t t0 = cls, t1 = Symbol64("Win10", cls), tGpu = Symbol64("Win10GPU", cls);
  std::shared_lock<std::shared_mutex> lock(g_mutex);
  // Transforms of one resource chain in registration order: each sees the previous one's output.
  const void* cur = b->data;
  std::uint64_t curSize = b->size;
  std::string served;
  for (Entry& e : g_entries) {
    if (e.name != name) continue;
    const bool match = b->index == 0 ? (e.type == t0 || e.type == t1) : (b->index == 1 && e.type == tGpu);
    if (!match) continue;
    if (e.fn != nullptr) {  // buffer 0, or a texture payload (buffer 1) for a "Win10GPU" type
      const void* out = nullptr;
      std::uint64_t outSize = 0;
      if (e.fn(cur, curSize, &out, &outSize, e.user) && out != nullptr) {
        cur = out;
        curSize = outSize;
        served += served.empty() ? e.label : ", " + e.label;
      }
    } else if (e.pending) {
      const std::uint64_t before = b->size;
      const bool ok = Serve(b, e.data, e.size);
      e.pending = !ok;
      Log(ok ? LogLevel::Info : LogLevel::Error, "[EVR.RES] override %s %s bytes %llu -> %zu", e.label.c_str(),
          ok ? "served" : "NOT served", (unsigned long long)before, e.size);
      return;
    }
  }
  if (cur != b->data) {
    const std::uint64_t before = b->size;
    const bool ok = Serve(b, cur, curSize);
    Log(ok ? LogLevel::Info : LogLevel::Error, "[EVR.RES] [%s] %s bytes %llu -> %llu", served.c_str(),
        ok ? "served" : "NOT served (allocator)", (unsigned long long)before, (unsigned long long)curSize);
  }
}

void Callback(IoBlock* b) {
  Apply(b);
  g_original(b);
}

}  // namespace

bool RegisterResourceTransform(std::uint64_t type, std::uint64_t name, ResourceTransform fn, void* user,
                               const char* label) {
  std::unique_lock<std::shared_mutex> lock(g_mutex);
  g_entries.push_back({type, name, fn, user, nullptr, 0, false, label ? label : "?"});
  return true;
}

bool RegisterResourceOverride(std::uint64_t type, std::uint64_t name, const void* data, std::size_t size,
                              const char* label) {
  std::unique_lock<std::shared_mutex> lock(g_mutex);
  for (Entry& e : g_entries) {
    if (e.type == type && e.name == name && e.fn == nullptr) {
      e.data = data;
      e.size = size;
      e.pending = true;
      return true;
    }
  }
  g_entries.push_back({type, name, nullptr, nullptr, data, size, true, label ? label : "?"});
  return true;
}

bool InstallResourceHook() {
  return HookVA(kCallbackVA, kCallbackPrologue, reinterpret_cast<void*>(&Callback),
                reinterpret_cast<void**>(&g_original), "CResourceManager::AsyncResourceIOCallback");
}

}  // namespace evr
