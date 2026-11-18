// The shared AddPackage hook (evr/packages.h). Addresses for echovr.exe sha256 b6d08277...; moved here from
// friends_install_pc.cpp so the friends set and the release's sets share one detour.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <vector>

#include "evr/core.h"
#include "evr/packages.h"

namespace evr {
namespace {

// CResourceManager::AddPackage(manager, CSymbol64 set, const char* dataRoot) -> 0 on success.
constexpr std::uint64_t kAddPackageVA = 0x140FA1680;
constexpr std::uint8_t kAddPackagePrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                                0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20};
constexpr std::uint64_t kManagerVA = 0x1420A2AB8;          // the resource manager InitGlobals builds
constexpr std::uint64_t kLastStockReturnVA = 0x1415497AC;  // return address of the last stock AddPackage (locale set)

using AddPackageFn = std::uint32_t (*)(void* manager, std::uint64_t set, const char* dataRoot);
AddPackageFn g_addPackage = nullptr;  // the trampoline once hooked, else the function itself
bool g_ready = false, g_hooked = false;
std::vector<PackageMountFn> g_before, g_after;
std::atomic<bool> g_beforeRan{false}, g_afterRan{false};

void Run(const std::vector<PackageMountFn>& fns, void* manager, const char* when) {
  for (PackageMountFn fn : fns) fn(manager, when);
}

std::uint32_t AddPackageHook(void* manager, std::uint64_t set, const char* dataRoot) {
  if (!g_beforeRan.exchange(true)) Run(g_before, manager, "before_stock_sets");
  const std::uint32_t r = g_addPackage(manager, set, dataRoot);
  const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
  if (r == 0 && caller == reinterpret_cast<std::uintptr_t>(ResolveVA(kLastStockReturnVA)) && !g_afterRan.exchange(true))
    Run(g_after, manager, "after_stock_sets");
  return r;
}

// Resolves AddPackage once; returns the manager when it already exists (then no hook is needed).
bool Prepare(void** existing) {
  *existing = nullptr;
  void* target = ResolveVA(kAddPackageVA);
  if (!g_ready) {
    if (!CheckBytes(target, kAddPackagePrologue, sizeof(kAddPackagePrologue))) {
      Log(LogLevel::Warning, "[EVR.PKG] AddPackage prologue mismatch: no package sets mounted");
      return false;
    }
    g_addPackage = reinterpret_cast<AddPackageFn>(target);
    g_ready = true;
  }
  if (void** slot = static_cast<void**>(ResolveVA(kManagerVA)); slot != nullptr && *slot != nullptr) {
    *existing = *slot;
    return true;
  }
  if (!g_hooked)
    g_hooked = Hook(target, reinterpret_cast<void*>(&AddPackageHook), reinterpret_cast<void**>(&g_addPackage),
                    "CResourceManager::AddPackage");
  return g_hooked;
}

bool Register(std::vector<PackageMountFn>* list, PackageMountFn fn) {
  void* manager = nullptr;
  if (!Prepare(&manager)) return false;
  if (manager != nullptr) {
    fn(manager, "manager_exists");
    return true;
  }
  list->push_back(fn);
  return true;
}

}  // namespace

bool OnStockPackagesAdded(PackageMountFn fn) { return Register(&g_after, fn); }
bool OnBeforeStockPackages(PackageMountFn fn) { return Register(&g_before, fn); }

std::uint32_t AddPackage(void* manager, std::uint64_t set, const char* dataRoot) {
  return g_addPackage != nullptr ? g_addPackage(manager, set, dataRoot) : 0xFFFFFFFFu;
}

}  // namespace evr
