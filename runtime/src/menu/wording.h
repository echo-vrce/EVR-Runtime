// menu/wording.h: stock menu texts that name Oculus where the game is now Echo's own, and what we show instead.
// Kept apart from the hook (wording_pc.cpp) so it is tested without the game.
#pragma once

#include <cstring>

namespace evr_wording {

struct Rewrite {
  const char* stock;
  const char* ours;
};

// The party tab's empty friends page (script library 30b4ca0fb0a1ee02, .rdata RVA 0x3eb18).
inline constexpr Rewrite kRewrites[] = {
    {"You have no Oculus Friends.", "You have no Echo Friends."},
};

/// Our text for a stock one, or nullptr to keep it.
inline const char* Reword(const char* text) {
  if (text == nullptr) return nullptr;
  for (const Rewrite& r : kRewrites)
    if (std::strcmp(text, r.stock) == 0) return r.ours;
  return nullptr;
}

}  // namespace evr_wording

namespace evr_wording {
/// Registers the text filter. Call before the game loop runs.
void Install();
}  // namespace evr_wording
