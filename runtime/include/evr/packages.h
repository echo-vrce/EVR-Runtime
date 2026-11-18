// evr/packages.h: mount extra package sets with the engine's own CResourceManager::AddPackage (PC).
//
// One detour on AddPackage (MinHook allows one per target), shared by every feature that mounts a set (the FRIENDS
// panel's pictures set, the release's own sets in content/). Callbacks register before the engine starts
// (EvrRuntimeStart); each runs once, with the resource manager, on the thread that mounts the stock sets.
#pragma once

#include <cstdint>

namespace evr {

using PackageMountFn = void (*)(void* manager, const char* when);

/// `fn` runs once, right after the last stock AddPackage (the locale set). When the manager already exists (the
/// runtime started late), it runs now with when="manager_exists". False (logged) when AddPackage's prologue differs.
bool OnStockPackagesAdded(PackageMountFn fn);

/// `fn` runs once, right before the first stock AddPackage (dev experiment: does mount order matter?). When the
/// manager already exists it runs now. False (logged) when AddPackage's prologue differs.
bool OnBeforeStockPackages(PackageMountFn fn);

/// CResourceManager::AddPackage(manager, set id, data root): 0 when the set was added. Only valid from a callback above.
std::uint32_t AddPackage(void* manager, std::uint64_t set, const char* dataRoot);

}  // namespace evr
