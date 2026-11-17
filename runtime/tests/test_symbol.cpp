#include <gtest/gtest.h>

#include "evr/symbol.h"

// Values the game itself uses: the canvases' on-disk type (CUICanvasResource seeded into "Win10").
TEST(Symbol, MatchesEngineTypes) {
  constexpr auto canvas = evr::Symbol64("CUICanvasResource");
  static_assert(evr::Symbol64("Win10", canvas) == 0x59A9BD6E4525ECC4ULL);
  EXPECT_EQ(evr::Symbol64("MNU_MASTER"), evr::Symbol64("mnu_master"));
  EXPECT_EQ(evr::Symbol64(""), evr::kSymbolSeed);
}
