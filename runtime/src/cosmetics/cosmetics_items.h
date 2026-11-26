// cosmetics/cosmetics_items.h: cosmetics the release itself ships (models), and the Story rarity. Pure logic shared
// by PC and Quest.
//
// Models (chassis, boosters, bracers) never come from the CDN: their meshes, materials and textures ride in a
// release package set (content/), and their item records are built in here. A record is appended to the game's item
// list only when its set is mounted, so a version without the set never shows an item it cannot draw.
//
// The Lone Echo chassis (from NewCosmetics chassis.json): Delta-13 (Lone Echo II) and Jack (Lone Echo). Set
// kChassisSet = Symbol64("evr_newcosmetics_chassis") holds their 208 resources (models, materials, textures, icons),
// built with tools/make_set.cpp. Players own them through Atlas like any cosmetic (unlock name = internal name).
//
// Story rarity: a rarity the game does not know (hash "story"), drawn by cosmetics_pc.cpp in the booth: label
// "Story", the frame's Common sprite (white) tinted pink, pink text, and the source line "Lone Echo Story Character" (a booster: "Lone Echo Story Item").
#pragma once

#include <cstdint>
#include <vector>

#include "cdn/cdn_catalog.h"
#include "evr/symbol.h"

namespace evr_cosmetics {

inline constexpr const char* kChassisSetName = "evr_newcosmetics_chassis";
inline constexpr std::uint64_t kChassisSet = evr::Symbol64("evr_newcosmetics_chassis");

inline constexpr std::uint64_t kStoryRarity = evr::Symbol64("story");
static_assert(kStoryRarity == 0xE32DC7C9CD8056A9ULL, "NewCosmetics writes this rarity symbol");
inline constexpr const char* kStoryLabel = "Story";
inline constexpr const char* kStorySource = "Lone Echo Story Character";
inline constexpr const char* kStorySourceBooster = "Lone Echo Story Item";
inline constexpr std::uint64_t kBoosterType = evr::Symbol64("booster");
inline constexpr float kStoryTextColour[3] = {0xFC / 255.0f, 0x00 / 255.0f, 0x87 / 255.0f};  // #FC0087
// The frame: the frames' six sprites are not a numbered family (no spare pink sprite; a guessed "7" draws white), so a
// Story frame is the Common sprite, which is white, tinted this colour (EchoBits&Bobs StoryRarity.ini: digit=1,
// colour=#FC0087).
inline constexpr float kStoryFrameColour[3] = {0xFC / 255.0f, 0x00 / 255.0f, 0x87 / 255.0f};

inline constexpr const char* kChassisTemplate = "rwd_chassis_body_s11_a";  // Series-11: NewCosmetics' template

inline evr_cdn::CosmeticItem Chassis(const char* internal, const char* display, const char* description,
                                     std::uint64_t model3p, std::uint64_t model1p, std::uint64_t material3p,
                                     std::uint64_t icon) {
  evr_cdn::CosmeticItem it;
  it.templateName = kChassisTemplate;
  it.internalName = internal;
  it.displayName = display;
  it.description = description;
  it.hasDisplay = it.hasDescription = true;
  it.source = kStorySource;
  it.rarity = kStoryRarity;
  it.thumbnail = icon;
  it.model3p = model3p;
  it.model1p = model1p;
  it.material3p = material3p;
  it.hasRarity = it.hasThumbnail = it.hasModel3p = it.hasModel1p = it.hasMaterial3p = true;
  return it;
}

inline constexpr const char* kBoosterTemplate = "rwd_booster_default";  // NewCosmetics' booster template

/// A booster: one model (AssetSymbol12 only: a booster's UnknownSymbol7 does not mirror it) and its material.
inline evr_cdn::CosmeticItem Booster(const char* internal, const char* display, const char* description,
                                     std::uint64_t model, std::uint64_t material, std::uint64_t icon) {
  evr_cdn::CosmeticItem it = Chassis(internal, display, description, model, 0, material, icon);
  it.templateName = kBoosterTemplate;
  it.source = kStorySourceBooster;
  it.hasModel1p = false;
  it.mirrorModel3p = false;
  return it;
}

/// The records kChassisSet draws: the two chassis and their boosters (the set name predates the boosters).
inline std::vector<evr_cdn::CosmeticItem> ChassisItems() {
  return {
      Chassis("rwd_chassis_le2_delta_a", "Delta-13", "Delta Squad soldier, Lone Echo II", 0x5CBFC8FB292585BFULL,
              0x55517547374D8DC4ULL, 0xB43B3AFCD943A27AULL, 0x57C8BCCE0355AE83ULL),
      Chassis("rwd_chassis_le1_jack_a", "Jack", "Jack, Lone Echo", 0x7029A59D8961CC8FULL, 0x6204FCE0972B9054ULL,
              0x3380067CCC07D6D3ULL, 0xD61EB3DF90C1F617ULL),
      Booster("rwd_booster_le2_delta_a", "Delta-13", "Delta Squad jetpack, Lone Echo II", 0xAE32A9AE46AFD867ULL,
              0xAE32A9AE46AFD867ULL, 0xC4EF139859107C83ULL),
      Booster("rwd_booster_le1_jack_a", "Jack", "Jack's backpack, Lone Echo", 0xACE3CD8F444FF05DULL,
              0xACE3CD8F444FF05DULL, 0xD68D9470C69BB3C5ULL),
  };
}

}  // namespace evr_cosmetics
