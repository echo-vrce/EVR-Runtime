#include <gtest/gtest.h>

#include <cstring>

#include "menu/wording.h"

using evr_wording::Reword;

TEST(MenuWording, NoFriendsSaysEcho) {
  EXPECT_STREQ(Reword("You have no Oculus Friends."), "You have no Echo Friends.");
}

TEST(MenuWording, OtherTextsStay) {
  EXPECT_EQ(Reword("FRIENDS"), nullptr);
  EXPECT_EQ(Reword("You have no Oculus Friends"), nullptr);  // whole text only
  EXPECT_EQ(Reword(nullptr), nullptr);
}

TEST(MenuWording, OursIsNeverLonger) {
  for (const auto& r : evr_wording::kRewrites) EXPECT_LE(std::strlen(r.ours), std::strlen(r.stock)) << r.stock;
}
