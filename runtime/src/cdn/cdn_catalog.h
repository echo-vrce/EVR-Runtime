// cdn/cdn_catalog.h: what the AtlasCDN adds besides replacing stock bytes, pure logic shared by PC and Quest.
//
// 1. New cosmetic items. The game's item list is one resource, CR15NetRewardItemCR (class 6183f3221c4905ae, on-disk
//    type 32f30fe361939dee = Make(class, "Win10"), name 43934c379cf1e366). Layout (read from the stock file):
//      u64 0, u64 entry bytes (= count x 664), 12 zero bytes, u32 1, 8 zero bytes, u64 count, u64 count,
//      count x 664-byte entries, then every entry's extra data, in entry order.
//    An entry carries two vector headers at +432 and +488, seven u64 each: 0 (pointer, set on load), byte size, 0,
//    0x100000000, 32, count, count. The first lists images (a pattern's second texture, an emote's frames), the
//    second other data (a tint's two RGB colours, an emissive's gradient colours: 12 bytes each). The extra data area
//    is those vectors' bytes, one entry after another.
//    A CDN item (index entry type kCosmeticItemType) is a small JSON spec. The client copies a stock TEMPLATE entry
//    of the same kind and overrides its names, rarity, thumbnail and texture (and colours), so every field the game
//    reads but nobody has decoded keeps a stock value. Items are appended when the game reads the list.
// 2. The names set. The game only loads (type, name) pairs some mounted package lists, so a texture name that is new
//    (a cosmetic's texture, the lobby poster) needs a package entry before the CDN can serve its bytes. At boot the
//    client builds one set from the last verified index it cached, plus kLobbyPoster, and mounts it.
// 3. The lobby poster. The stock lobby poster is not level data: it is the texture Atlas's active_battle_pass_season
//    config names in marketing.lobby_poster_start_texture / lobby_poster_upsell_texture, and that was
//    ui_mnu_news_latest, the UPDATE card's texture. Atlas now names kLobbyPoster for EVR-Runtime clients, so a guild's
//    poster and the UPDATE card no longer share a texture. Until the CDN serves a poster, kLobbyPoster shows a neutral
//    built-in default the runtime makes itself (no game art is read), so a guild without one never shows the UPDATE art.
// 4. Stock fixes (index entries of kind kStockFixKind, Atlas's global set). A spec (type kStockFixType) renames a stock
//    item in place: {"internal_name": "rwd_emissive_0017", "display_name": "Lancer"}. Its texture entries replace a
//    stock texture (a placeholder STUB icon) by name; the stock packages already list that name, so it stays out of
//    the names set.
//
// Header only, no platform code, no exceptions escape. Tests: tests/cdn/test_cdn_catalog.cpp.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cdn/cdn_index.h"
#include "evr/symbol.h"
#include "friends/friends_package.h"

namespace evr_cdn {

inline constexpr std::uint64_t kRewardItemClass = 0x6183F3221C4905AEULL;  // CR15NetRewardItemCR
inline constexpr std::uint64_t kRewardItemType = 0x32F30FE361939DEEULL;   // Make(class, "Win10")
inline constexpr std::uint64_t kRewardItemName = 0x43934C379CF1E366ULL;
inline constexpr std::uint64_t kCosmeticItemType = evr::Symbol64("evr_cdn_cosmetic");  // index entries holding a spec
inline constexpr std::uint64_t kStockFixType = evr::Symbol64("evr_cdn_stock_fix");  // index entries holding a rename
inline constexpr const char* kStockFixKind = "stock_fix";
inline constexpr std::uint64_t kLobbyPoster = evr::Symbol64("evr_lobby_poster");
static_assert(kRewardItemType == evr::Symbol64("Win10", kRewardItemClass), "the list's on-disk type");
static_assert(kLobbyPoster == 0x85C8A8EC5D6C774AULL, "Atlas names this texture");

inline constexpr std::size_t kEntryBytes = 664;
inline constexpr std::size_t kListHeader = 56;
inline constexpr std::size_t kMaxItems = 1024;  // per index

namespace catalog_detail {
inline constexpr std::size_t kInternalSym = 0, kInternalSym2 = 32, kInternalName = 40, kType = 104, kRarity = 112,
                             kThumbnail = 128, kDisplayName = 168, kDescription = 232, kTexture = 320,
                             kImageVec = 432, kOtherVec = 488, kVecBytes = 8, kVecCount = 40, kVecCount2 = 48;
// Model fields (NewCosmetics cosmeticdb): UnknownSymbol6 = the 3p body material, UnknownSymbol7 mirrors the 3p model,
// AssetSymbol6 = the 1p model, AssetSymbol12 = the 3p model. AssetSymbol<k> sits at 544 + 8 (k - 1).
inline constexpr std::size_t kMaterial3p = 144, kModel3pMirror = 152, kModel1p = 584, kModel3p = 632;

template <typename T>
T Get(const std::string& b, std::size_t at) {
  T v{};
  std::memcpy(&v, b.data() + at, sizeof(v));
  return v;
}
template <typename T>
void Put(std::string* b, std::size_t at, T v) {
  std::memcpy(&(*b)[at], &v, sizeof(v));
}
inline void PutText(std::string* b, std::size_t at, const std::string& s) {
  std::memset(&(*b)[at], 0, 64);
  std::memcpy(&(*b)[at], s.data(), s.size() < 63 ? s.size() : 63);
}
inline bool Hex64(const nlohmann::json& j, const char* key, std::uint64_t* out) {
  if (!j.contains(key) || !j[key].is_string()) return false;
  const std::string s = j[key].get<std::string>();
  if (s.size() != 16) return false;
  std::uint64_t v = 0;
  for (char c : s) {
    const int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
    if (d < 0) return false;
    v = (v << 4) | static_cast<std::uint64_t>(d);
  }
  *out = v;
  return true;
}
inline bool Name(const nlohmann::json& j, const char* key, std::string* out, bool required) {
  if (!j.contains(key)) return !required;
  if (!j[key].is_string()) return false;
  *out = j[key].get<std::string>();
  return out->size() <= 63 && (!required || !out->empty());
}
}  // namespace catalog_detail

// One item to add, from its CDN spec:
//   {"template": "rwd_decal_ramen_a", "internal_name": "rwd_decal_g1234abcd_fox", "display_name": "Fox",
//    "description": "...", "rarity": "<16 hex>", "thumbnail": "<16 hex>", "texture": "<16 hex>",
//    "colors": [[r, g, b], ...]}            (tint: exactly 2; emissive: 1-16; 0..1 floats)
// template and internal_name are required; any other key absent keeps the template's value.
//   optional "model_3p", "model_1p", "material_3p" (16 hex): a model item (chassis) whose resources a release set
//   ships (models come with updates, never from the CDN).
//   optional "source": the line under the name in the booth ("<guild> Item"; the game's own are "Level 12", "Store
//   Item"...). Absent: kDefaultSource. The game has no field for it: cdn_pc.cpp hooks the function that writes it.
inline constexpr const char* kDefaultSource = "Guild Item";

struct CosmeticItem {
  std::string templateName, internalName, displayName, description, source = kDefaultSource;
  bool hasDisplay = false, hasDescription = false;
  std::uint64_t rarity = 0, thumbnail = 0, texture = 0;
  bool hasRarity = false, hasThumbnail = false, hasTexture = false;
  std::uint64_t model3p = 0, model1p = 0, material3p = 0;
  bool hasModel3p = false, hasModel1p = false, hasMaterial3p = false;
  bool mirrorModel3p = true;  // a chassis' UnknownSymbol7 mirrors its 3p model; a booster's does not
  std::vector<float> colors;  // 3 per colour
  bool hasColors = false;
};

inline bool ParseCosmeticItem(const std::string& text, CosmeticItem* out, std::string* error) {
  using namespace catalog_detail;
  const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
  CosmeticItem it;
  if (!j.is_object() || !Name(j, "template", &it.templateName, true) ||
      !Name(j, "internal_name", &it.internalName, true)) {
    *error = "template and internal_name are required (1-63 bytes)";
    return false;
  }
  it.hasDisplay = j.contains("display_name");
  it.hasDescription = j.contains("description");
  if (!Name(j, "display_name", &it.displayName, false) || !Name(j, "description", &it.description, false) ||
      !Name(j, "source", &it.source, false)) {
    *error = "display_name, description and source are at most 63 bytes";
    return false;
  }
  if (it.source.empty()) it.source = kDefaultSource;
  it.hasRarity = j.contains("rarity");
  it.hasThumbnail = j.contains("thumbnail");
  it.hasTexture = j.contains("texture");
  if ((it.hasRarity && !Hex64(j, "rarity", &it.rarity)) || (it.hasThumbnail && !Hex64(j, "thumbnail", &it.thumbnail)) ||
      (it.hasTexture && !Hex64(j, "texture", &it.texture))) {
    *error = "rarity, thumbnail and texture are 16 lowercase hex";
    return false;
  }
  it.hasModel3p = j.contains("model_3p");
  it.hasModel1p = j.contains("model_1p");
  it.hasMaterial3p = j.contains("material_3p");
  if ((it.hasModel3p && !Hex64(j, "model_3p", &it.model3p)) || (it.hasModel1p && !Hex64(j, "model_1p", &it.model1p)) ||
      (it.hasMaterial3p && !Hex64(j, "material_3p", &it.material3p))) {
    *error = "model_3p, model_1p and material_3p are 16 lowercase hex";
    return false;
  }
  if (j.contains("colors")) {
    const auto& c = j["colors"];
    if (!c.is_array() || c.empty() || c.size() > 16) {
      *error = "colors holds 1-16 [r, g, b]";
      return false;
    }
    for (const auto& rgb : c) {
      if (!rgb.is_array() || rgb.size() != 3) {
        *error = "colors holds 1-16 [r, g, b]";
        return false;
      }
      for (const auto& v : rgb) {
        if (!v.is_number() || v.get<double>() < 0.0 || v.get<double>() > 4.0) {
          *error = "a colour channel is a number in 0..4";
          return false;
        }
        it.colors.push_back(v.get<float>());
      }
    }
    it.hasColors = true;
  }
  *out = std::move(it);
  return true;
}

// The item list with `items` appended (each a copy of its template with the overrides). An item whose internal name
// the list already has, or whose template it lacks, is skipped and named in `skipped`. False (list unchanged) when
// the list is not the expected layout.
inline bool AppendCosmeticItems(const std::string& list, const std::vector<CosmeticItem>& items, std::string* out,
                                std::vector<std::string>* skipped) {
  using namespace catalog_detail;
  if (list.size() < kListHeader) return false;
  const std::uint64_t count = Get<std::uint64_t>(list, 40);
  if (count != Get<std::uint64_t>(list, 48) || count > 65536 || Get<std::uint64_t>(list, 8) != count * kEntryBytes ||
      list.size() < kListHeader + count * kEntryBytes)
    return false;
  // Where each entry's extra data starts (they follow the entries in order).
  std::vector<std::size_t> extAt(count + 1);
  std::size_t at = kListHeader + count * kEntryBytes;
  for (std::uint64_t i = 0; i < count; ++i) {
    const std::size_t e = kListHeader + i * kEntryBytes;
    const std::uint64_t n = Get<std::uint64_t>(list, e + kImageVec + kVecBytes) +
                            Get<std::uint64_t>(list, e + kOtherVec + kVecBytes);
    extAt[i] = at;
    if (n > list.size() - at) return false;
    at += n;
  }
  extAt[count] = at;
  if (at != list.size()) return false;  // an unknown tail: leave the list alone

  std::string entries = list.substr(kListHeader, count * kEntryBytes);
  std::string ext = list.substr(kListHeader + count * kEntryBytes);
  std::uint64_t added = 0;
  auto find = [&](std::uint64_t sym) -> std::int64_t {
    for (std::uint64_t i = 0; i < count + added; ++i)
      if (Get<std::uint64_t>(entries, i * kEntryBytes + kInternalSym) == sym) return static_cast<std::int64_t>(i);
    return -1;
  };
  for (const CosmeticItem& it : items) {
    if (added >= kMaxItems) break;
    const std::uint64_t sym = evr::Symbol64(it.internalName);
    const std::int64_t tpl = find(evr::Symbol64(it.templateName));
    if (find(sym) >= 0 || tpl < 0 || static_cast<std::uint64_t>(tpl) >= count) {
      if (skipped) skipped->push_back(it.internalName);
      continue;
    }
    std::string e = entries.substr(static_cast<std::size_t>(tpl) * kEntryBytes, kEntryBytes);
    std::string img = list.substr(extAt[tpl], Get<std::uint64_t>(e, kImageVec + kVecBytes));
    std::string other = list.substr(extAt[tpl] + img.size(), Get<std::uint64_t>(e, kOtherVec + kVecBytes));
    Put<std::uint64_t>(&e, kInternalSym, sym);
    Put<std::uint64_t>(&e, kInternalSym2, sym);
    PutText(&e, kInternalName, it.internalName);
    if (it.hasDisplay) PutText(&e, kDisplayName, it.displayName);
    if (it.hasDescription) PutText(&e, kDescription, it.description);
    if (it.hasRarity) Put<std::uint64_t>(&e, kRarity, it.rarity);
    if (it.hasThumbnail) Put<std::uint64_t>(&e, kThumbnail, it.thumbnail);
    if (it.hasTexture) Put<std::uint64_t>(&e, kTexture, it.texture);
    if (it.hasModel3p) {
      Put<std::uint64_t>(&e, kModel3p, it.model3p);
      if (it.mirrorModel3p) Put<std::uint64_t>(&e, kModel3pMirror, it.model3p);
    }
    if (it.hasModel1p) Put<std::uint64_t>(&e, kModel1p, it.model1p);
    if (it.hasMaterial3p) Put<std::uint64_t>(&e, kMaterial3p, it.material3p);
    if (it.hasColors) {
      other.assign(it.colors.size() * 4, '\0');
      std::memcpy(&other[0], it.colors.data(), other.size());
      const std::uint64_t n = it.colors.size() / 3;
      Put<std::uint64_t>(&e, kOtherVec + kVecBytes, other.size());
      Put<std::uint64_t>(&e, kOtherVec + kVecCount, n);
      Put<std::uint64_t>(&e, kOtherVec + kVecCount2, n);
    }
    entries += e;
    ext += img;
    ext += other;
    ++added;
  }
  std::string r = list.substr(0, kListHeader);
  const std::uint64_t total = count + added;
  Put<std::uint64_t>(&r, 8, total * kEntryBytes);
  Put<std::uint64_t>(&r, 40, total);
  Put<std::uint64_t>(&r, 48, total);
  r += entries;
  r += ext;
  *out = std::move(r);
  return true;
}

// A stock item's new display name, from a stock fix spec.
struct StockFix {
  std::string internalName, displayName;
};

inline bool ParseStockFix(const std::string& text, StockFix* out, std::string* error) {
  using namespace catalog_detail;
  const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
  StockFix f;
  if (!j.is_object() || !Name(j, "internal_name", &f.internalName, true) ||
      !Name(j, "display_name", &f.displayName, true)) {
    *error = "internal_name and display_name are required (1-63 bytes)";
    return false;
  }
  *out = std::move(f);
  return true;
}

// Renames the list's stock items in place (same size, nothing moves). Returns how many entries changed; a fix whose
// item the list lacks changes nothing. The list must already be one AppendCosmeticItems accepts.
inline std::size_t ApplyStockFixes(std::string* list, const std::vector<StockFix>& fixes) {
  using namespace catalog_detail;
  if (list->size() < kListHeader) return 0;
  const std::uint64_t count = Get<std::uint64_t>(*list, 40);
  if (list->size() < kListHeader + count * kEntryBytes) return 0;
  std::size_t changed = 0;
  for (const StockFix& f : fixes) {
    const std::uint64_t sym = evr::Symbol64(f.internalName);
    for (std::uint64_t i = 0; i < count; ++i) {
      const std::size_t e = kListHeader + i * kEntryBytes;
      if (Get<std::uint64_t>(*list, e + kInternalSym) != sym) continue;
      PutText(list, e + kDisplayName, f.displayName);
      ++changed;
      break;
    }
  }
  return changed;
}

// ---- the names set ----

inline constexpr std::uint64_t kNamesSetId = evr::Symbol64("evr_cdn_names");

// The texture names the names set must list for `platform` ("pc" or "quest"): every name the index has BOTH a
// descriptor and a payload entry for (one alone is a stock-size override of a stock texture: the stock entry must
// stay the one the game finds), plus kLobbyPoster. Stock fixes are left out: they replace names the stock packages
// list. Sorted, no repeats.
inline std::vector<std::uint64_t> TextureNames(const Index& idx, const nevr_friends_package::Platform& platform,
                                               const std::string& platformName) {
  std::vector<std::uint64_t> descriptors, payloads, out{kLobbyPoster};
  for (const Entry& e : idx.entries) {
    if (!e.platform.empty() && e.platform != platformName) continue;
    if (e.kind == kStockFixKind) continue;
    if (e.type == platform.descriptor_type) descriptors.push_back(e.name);
    if (e.type == platform.payload_type) payloads.push_back(e.name);
  }
  for (std::uint64_t n : descriptors)
    if (std::find(payloads.begin(), payloads.end(), n) != payloads.end()) out.push_back(n);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// A package set with a placeholder texture (128 x 128 slate) under each name. The game finds the names in it; the
// CDN (or the lobby poster's stock copy) serves the real descriptor and payload as they are read.
inline bool BuildNamesSet(const std::vector<std::uint64_t>& names, const nevr_friends_package::Platform& platform,
                          nevr_friends_package::PackageSet* out) {
  std::vector<nevr_friends_package::Resource> resources;
  for (std::uint64_t n : names) nevr_friends_package::AddPlaceholderPicture(platform, n, &resources);
  return nevr_friends_package::BuildPackageSet(std::move(resources), out);
}

}  // namespace evr_cdn
