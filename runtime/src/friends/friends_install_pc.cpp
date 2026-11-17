// Friends panel, stage 1 on PC: the pictures set, the seven menu edits and the AddPackage that mounts the set.
// Addresses are he_is_the_cat's (FriendsMenu, friends_panel_pc.cpp) for echovr.exe sha256 b6d08277...
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "evr/core.h"
#include "evr/events.h"
#include "evr/frame.h"
#include "friends/friends_install.h"
#include "friends/friends_panel_resources.h"
#include "friends/menu_friends_avatars.h"
#include "friends/menu_friends_pack.h"
#include "social/social.h"

namespace evr_friends {
namespace {

// CResourceManager::AddPackage(manager, CSymbol64 set, const char* dataRoot) -> 0 on success.
constexpr std::uint64_t kAddPackageVA = 0x140FA1680;
constexpr std::uint8_t kAddPackagePrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                                0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20};
constexpr std::uint64_t kManagerVA = 0x1420A2AB8;          // the resource manager InitGlobals builds
constexpr std::uint64_t kLastStockReturnVA = 0x1415497AC;  // return address of the last stock AddPackage (locale set)
constexpr std::uint64_t kSetId = 0x27974AE8287406F2ULL;    // the pictures set

using AddPackageFn = std::uint32_t (*)(void* manager, std::uint64_t set, const char* dataRoot);
AddPackageFn g_addPackage = nullptr;
std::string g_dataRoot;
std::atomic<bool> g_added{false};

void AddOurs(void* manager, const char* when) {
  if (g_added.exchange(true)) return;
  const std::uint32_t r = g_addPackage(manager, kSetId, g_dataRoot.c_str());
  if (r == 0) nevr_menu_friends_pack::MarkPackageAdded();
  evr::Log(r == 0 ? evr::LogLevel::Info : evr::LogLevel::Warning,
           "[EVR.FRIENDS] pictures set %s set=0x%016llx root=%s when=%s result=%u", r == 0 ? "added" : "NOT added",
           (unsigned long long)kSetId, g_dataRoot.c_str(), when, r);
}

std::uint32_t AddPackageHook(void* manager, std::uint64_t set, const char* dataRoot) {
  const std::uint32_t r = g_addPackage(manager, set, dataRoot);
  const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
  if (r == 0 && caller == reinterpret_cast<std::uintptr_t>(evr::ResolveVA(kLastStockReturnVA)))
    AddOurs(manager, "after_stock_sets");
  return r;
}

// Game-loop thread, once per frame: the newest roster, then the rows, the pop-up and the pictures.
void PanelFrame() {
  evr_social::Adopt();
  nevr_menu_friends_pack::OnFrame();
  nevr_menu_friends_avatars::OnFrame();
}

void PanelEvent(std::uint64_t space, std::uint64_t event) { nevr_menu_friends_pack::OnComponentEvent(space, event); }

// Stage 2: rows from the Atlas roster, clicks, the profile pop-up and the avatars. Needs the set mounted.
void InstallPanel() {
  nevr_menu_friends_avatars::Install(evr::GameBase());
  evr::OnFrame(&PanelFrame);
  evr::OnComponentEvent(&PanelEvent);
  evr_social::Start();
}

bool InstallSet() {
  g_dataRoot = evr::ContentDir() + "\\friends_panel";
  if (!nevr_friends_panel_resources::WritePicturesSet(g_dataRoot)) return false;
  nevr_friends_panel_resources::RegisterTransforms();
  void* manager = *static_cast<void**>(evr::ResolveVA(kManagerVA));
  void* target = evr::ResolveVA(kAddPackageVA);
  if (!evr::CheckBytes(target, kAddPackagePrologue, sizeof(kAddPackagePrologue))) {
    evr::Log(evr::LogLevel::Warning, "[EVR.FRIENDS] AddPackage prologue mismatch: pictures set not mounted");
    return false;
  }
  g_addPackage = reinterpret_cast<AddPackageFn>(target);
  if (manager != nullptr) {
    AddOurs(manager, "manager_exists");
    return true;
  }
  return evr::Hook(target, reinterpret_cast<void*>(&AddPackageHook), reinterpret_cast<void**>(&g_addPackage),
                   "CResourceManager::AddPackage");
}

}  // namespace

bool Install() {
  if (!InstallSet()) return false;
  InstallPanel();
  return true;
}

}  // namespace evr_friends
