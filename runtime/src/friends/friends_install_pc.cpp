// Friends panel, stage 1 on PC: the pictures set, the seven menu edits and the AddPackage that mounts the set.
// Addresses are he_is_the_cat's (FriendsMenu, friends_panel_pc.cpp) for echovr.exe sha256 b6d08277...
#include <windows.h>

#include <cstdint>
#include <string>

#include "evr/core.h"
#include "evr/events.h"
#include "evr/frame.h"
#include "evr/packages.h"
#include "friends/friends_install.h"
#include "friends/friends_panel_resources.h"
#include "friends/menu_friends_avatars.h"
#include "friends/menu_friends_pack.h"
#include "social/social.h"

namespace evr_friends {
namespace {

constexpr std::uint64_t kSetId = 0x27974AE8287406F2ULL;  // the pictures set
std::string g_dataRoot;

// Mounted through the shared AddPackage hook (evr/packages.h), after the stock sets.
void AddOurs(void* manager, const char* when) {
  const std::uint32_t r = evr::AddPackage(manager, kSetId, g_dataRoot.c_str());
  if (r == 0) nevr_menu_friends_pack::MarkPackageAdded();
  evr::Log(r == 0 ? evr::LogLevel::Info : evr::LogLevel::Warning,
           "[EVR.FRIENDS] pictures set %s set=0x%016llx root=%s when=%s result=%u", r == 0 ? "added" : "NOT added",
           (unsigned long long)kSetId, g_dataRoot.c_str(), when, r);
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
  if (!evr::OnStockPackagesAdded(&AddOurs)) {
    evr::Log(evr::LogLevel::Warning, "[EVR.FRIENDS] AddPackage unavailable: pictures set not mounted");
    return false;
  }
  return true;
}

}  // namespace

bool Install() {
  if (!InstallSet()) return false;
  InstallPanel();
  return true;
}

}  // namespace evr_friends
