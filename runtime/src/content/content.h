#pragma once
#include <cstdint>
// The release's own package sets (PC): mounted from <version dir>\data, and served over a stock copy if the game
// reads one (content_pc.cpp).
namespace evr_content {
struct Options {
  bool before_stock = false;  // plugins\config.json "content_mount": "before_stock" (dev experiment)
  bool override_stock = true;  // "content_override": false only logs which copy the game read
  bool probe = false;          // "content_probe": true logs the texture deserialiser's calls for our names (dev)
};
/// Lists and registers the sets, hooks AddPackage through evr/packages.h. Called from EvrRuntimeStart.
void Install(const Options& options);
/// True when the release set `set` (its id, Symbol64 of its name) was found complete and is mounted. Valid once
/// Install has run.
bool SetMounted(std::uint64_t set);
}  // namespace evr_content
