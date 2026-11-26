#include <gtest/gtest.h>

#include <cstring>
#include <initializer_list>
#include <vector>

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

TEST(MenuWording, ArenaTiesAreEarlyQuits) {
  EXPECT_STREQ(Reword("Arena Ties"), "Early Quits");
  EXPECT_STREQ(Reword("ARENA TIES"), "EARLY QUITS");
  EXPECT_EQ(Reword("Arena Wins"), nullptr);
}

// Ours never needs more room than the stock text had. A tablet stats row has the room of its page's longest
// stock row, "Highest Win Streak".
TEST(MenuWording, OursIsNeverLonger) {
  for (const auto& r : evr_wording::kRewrites) {
    const bool statsRow = std::strcmp(r.stock, "Arena Ties") == 0 || std::strcmp(r.stock, "ARENA TIES") == 0;
    const std::size_t room = statsRow ? std::strlen("Highest Win Streak") : std::strlen(r.stock);
    EXPECT_LE(std::strlen(r.ours), room) << r.stock;
  }
}

namespace {
std::vector<unsigned char> Bytes(std::initializer_list<const char*> parts, std::size_t pad) {
  std::vector<unsigned char> b;
  for (const char* p : parts) {
    b.insert(b.end(), p, p + std::strlen(p));
    b.insert(b.end(), pad, 0);
  }
  return b;
}
const evr_wording::Rewrite& Ties() { return evr_wording::kRewrites[1]; }
}  // namespace

TEST(MenuWording, FindsOnlyWholeStrings) {
  const auto b = Bytes({"Arena Ties", "xArena Ties", "Arena Tiesx", "Arena Ties"}, 1);
  const auto at = evr_wording::FindWhole(b.data(), b.size(), "Arena Ties");
  ASSERT_EQ(at.size(), 2u);
  EXPECT_EQ(at[0], 0u);
  EXPECT_EQ(at[1], 11u + 12u + 12u);
}

TEST(MenuWording, LongerTextNeedsZeroPadding) {
  const auto padded = Bytes({"Arena Ties", "Wins"}, 2);  // one spare zero after the NUL
  EXPECT_STREQ(evr_wording::InPlace(padded.data(), padded.size(), 0, Ties()), "Early Quits");
  const auto packed = Bytes({"Arena Ties", "Wins"}, 1);  // "Wins" starts right after the NUL
  EXPECT_STREQ(evr_wording::InPlace(packed.data(), packed.size(), 0, Ties()), "Early Quit");
}

TEST(MenuWording, TightTextIsNeverLonger) {
  for (const auto& r : evr_wording::kRewrites)
    if (r.tight != nullptr) EXPECT_LE(std::strlen(r.tight), std::strlen(r.stock)) << r.stock;
}
