// Main menu CREATE GAME / JOIN VIA CODE cards (PC).
//
// The home page script (991bbec05cde117f) already binds create_card -> create_page and join_card -> join_page, and the
// level already has the two card actors (ui_mnu_card, at x -2.15 and +2.15 beside PLAY and TUTORIALS). What the
// stock build leaves out is their two elements in the mnu_master page resource (CR15UIPage2CR), so they draw nothing.
// We add those elements to each home page variant (pages 24, 26, 27) as the page is read. Left to right the
// stock poses would read JOIN VIA CODE, PLAY, TUTORIALS, CREATE GAME, so we also swap PLAY and JOIN VIA CODE
// (their poses in the level's transform table): PLAY, JOIN VIA CODE, TUTORIALS, CREATE GAME.
#include "menu/create_join.h"

#include <cstdint>
#include <string>
#include <vector>

#include "evr/core.h"
#include "evr/resources.h"
#include "evr/symbol.h"
#include "menu/menu_page.h"
#include "menu/menu_slots.h"

namespace evr_create_join {
namespace {

constexpr std::uint64_t kPageType = evr::Symbol64("CR15UIPage2CR");
constexpr std::uint64_t kMnuMaster = 0xAC360E41E4EDE056ULL;
constexpr std::uint64_t kPlayCard = 0xa3d3ef315cb999dbULL;
constexpr std::uint64_t kCreateCard = 0xff5375e5a4dee29dULL;
constexpr std::uint64_t kJoinCard = 0x097b785350a61c5fULL;
constexpr std::uint64_t kTransformType = evr::Symbol64("CTransformCR");

std::vector<std::uint8_t> g_served;  // the page we serve; stays alive for the game
std::vector<std::uint8_t> g_table;   // the transform table we serve

// PLAY and JOIN VIA CODE trade places, so the menu reads PLAY, JOIN VIA CODE, TUTORIALS, CREATE GAME.
bool SwapPlayJoin(const void* original, std::uint64_t size, const void** out, std::uint64_t* outSize, void*) {
  const auto* p = static_cast<const std::uint8_t*>(original);
  std::string why;
  std::vector<std::uint8_t> result = evr_menu_slots::SwapPoses({p, p + size}, kPlayCard, kJoinCard, &why);
  if (!why.empty()) {
    evr::Log(evr::LogLevel::Warning, "[EVR.MENU] PLAY / JOIN VIA CODE not swapped: %s", why.c_str());
    return false;
  }
  g_table = std::move(result);
  *out = g_table.data();
  *outSize = g_table.size();
  evr::Log(evr::LogLevel::Info, "[EVR.MENU] PLAY and JOIN VIA CODE swapped");
  return true;
}

bool AddCards(const void* original, std::uint64_t size, const void** out, std::uint64_t* outSize, void*) {
  const auto* p = static_cast<const std::uint8_t*>(original);
  const std::vector<std::uint8_t> in(p, p + size);
  const evr_menu_page::Keys keys{evr::Symbol64("default"), evr::Symbol64("sprite"), evr::Symbol64("text"),
                                 evr::Symbol64("subtext"), evr::Symbol64("tip")};
  const std::vector<evr_menu_page::NewCard> cards = {
      {kCreateCard, evr::Symbol64("tile_create"), "CREATE GAME", "", "Create a private match"},
      {kJoinCard, evr::Symbol64("tile_join"), "JOIN VIA CODE", "", "Join a match with an invite code"},
  };
  std::string why;
  int pages = 0;
  std::vector<std::uint8_t> result = evr_menu_page::AddCards(in, kPlayCard, cards, keys, &pages, &why);
  if (!why.empty()) {
    evr::Log(evr::LogLevel::Warning, "[EVR.MENU] create/join cards not added: %s (bytes=%llu)", why.c_str(),
             (unsigned long long)size);
    return false;
  }
  g_served = std::move(result);
  *out = g_served.data();
  *outSize = g_served.size();
  evr::Log(evr::LogLevel::Info, "[EVR.MENU] create/join cards added pages=%d bytes %llu -> %zu", pages,
           (unsigned long long)size, g_served.size());
  return true;
}

}  // namespace

void Install() {
  if (!evr::RegisterResourceTransform(kPageType, kMnuMaster, &AddCards, nullptr, "create/join cards"))
    evr::Log(evr::LogLevel::Warning, "[EVR.MENU] create/join cards: transform not registered");
  if (!evr::RegisterResourceTransform(kTransformType, kMnuMaster, &SwapPlayJoin, nullptr, "create/join card places"))
    evr::Log(evr::LogLevel::Warning, "[EVR.MENU] create/join card places: transform not registered");
}

}  // namespace evr_create_join
