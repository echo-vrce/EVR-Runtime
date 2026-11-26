#pragma once
// Release-shipped cosmetics and the Story rarity (cosmetics_items.h), PC side: cosmetics_pc.cpp.
#include <cstdint>
#include <map>
#include <string>

namespace evr_cosmetics {
/// Story rarity hooks, the booth's source-line hook, and (when the chassis set is mounted) the chassis records.
/// Call after evr_content::Install and before evr_cdn::Start.
void Install();
/// The source line (under the name in the booth) of the CDN's items, by internal name symbol. Replaces the last set.
void SetSourceLines(std::map<std::uint64_t, std::string> lines);
}  // namespace evr_cosmetics
