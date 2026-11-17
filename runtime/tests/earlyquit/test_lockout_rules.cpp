#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "earlyquit/lockout_rules.h"

using namespace evr_earlyquit;

namespace {

// The bytes Atlas's NewEarlyQuitUpdateNotification writes, built field by field.
void Put64(std::uint8_t* p, std::size_t at, std::int64_t v) { std::memcpy(p + at, &v, 8); }
void Put32(std::uint8_t* p, std::size_t at, std::int32_t v) { std::memcpy(p + at, &v, 4); }

}  // namespace

TEST(EarlyQuitLockout, ReadsAtlasUpdateLayout) {
  std::uint8_t b[kUpdateBytes] = {};
  Put64(b, 0x18, 1'800'000'001);
  Put64(b, 0x20, 1'800'000'900);
  Put32(b, 0x28, 4);
  Put32(b, 0x2c, 20);
  Put32(b, 0x30, 1);
  b[0x34] = 3;
  b[0x35] = 2;
  Update u;
  ASSERT_TRUE(ReadUpdate(b, sizeof(b), u));
  EXPECT_EQ(u.stamp, 1'800'000'001);
  EXPECT_EQ(u.expiry, 1'800'000'900);
  EXPECT_EQ(u.earlyQuits, 4);
  EXPECT_EQ(u.steadyMatches, 20);
  EXPECT_EQ(u.steadyEarlyQuits, 1);
  EXPECT_EQ(u.penaltyLevel, 3);
  EXPECT_EQ(u.steadyLevel, 2);
  EXPECT_FALSE(ReadUpdate(b, kUpdateBytes - 1, u));
  EXPECT_FALSE(ReadUpdate(nullptr, kUpdateBytes, u));
}

TEST(EarlyQuitLockout, GameGateTakesOnlyNewerStamps) {
  EXPECT_TRUE(GameTakes(-1, 5));
  EXPECT_TRUE(GameTakes(4, 5));
  EXPECT_FALSE(GameTakes(5, 5));
  EXPECT_FALSE(GameTakes(6, 5));
}

TEST(EarlyQuitLockout, ExpiryInTheFutureStartsTheCountdown) {
  const Lockout l = AfterUpdate(-1, 100, 1000, 400);
  EXPECT_TRUE(l.take);
  EXPECT_EQ(l.expiry, 1000);
  EXPECT_TRUE(l.running);
}

TEST(EarlyQuitLockout, ZeroOrPastExpiryClears) {
  for (std::int64_t expiry : {std::int64_t{0}, std::int64_t{-1}, std::int64_t{400}, std::int64_t{399}}) {
    const Lockout l = AfterUpdate(99, 100, expiry, 400);
    EXPECT_TRUE(l.take) << expiry;
    EXPECT_EQ(l.expiry, -1) << expiry;
    EXPECT_FALSE(l.running) << expiry;
  }
}

TEST(EarlyQuitLockout, DroppedUpdateLeavesTheCountdownAlone) {
  const Lockout l = AfterUpdate(100, 100, 1000, 400);
  EXPECT_FALSE(l.take);
  EXPECT_FALSE(AfterUpdate(101, 100, 1000, 400).take);
}

TEST(EarlyQuitLockout, ActiveFollowsSecondsLeft) {
  EXPECT_TRUE(CountdownActive(1));
  EXPECT_FALSE(CountdownActive(0));
  EXPECT_FALSE(CountdownActive(-5));
}

TEST(EarlyQuitNotice, DueOnANewLockoutOrANewQuitOnly) {
  EXPECT_TRUE(NoticeDue(true, true, 0, 0));
  EXPECT_TRUE(NoticeDue(true, false, 2, 3));
  EXPECT_FALSE(NoticeDue(true, false, 3, 3));   // a clear, or loyalty counters only
  EXPECT_FALSE(NoticeDue(false, true, 0, 5));   // the game dropped the update
}

TEST(EarlyQuitNotice, OnlyTheNoticeButtonsDismissIt) {
  EXPECT_TRUE(IsNoticeButton(kNoticeOk, kNoticeButtonKey));
  EXPECT_TRUE(IsNoticeButton(kNoticeInfo, kNoticeButtonKey));
  EXPECT_FALSE(IsNoticeButton(kNoticeOk, 0x1234));
  EXPECT_FALSE(IsNoticeButton(0x80432978683E4A9CULL, kNoticeButtonKey));
}

TEST(EarlyQuitNotice, NodePatchSwapsOnlyTheSelect) {
  EXPECT_EQ(std::memcmp(NodeBytes(true), kNodeAsks, 4), 0);
  EXPECT_EQ(std::memcmp(NodeBytes(false), kNodeSaysNo, 4), 0);
  // Same length, same first byte (REX.W): one 4-byte swap in place.
  EXPECT_EQ(kNodeAsks[0], kNodeSaysNo[0]);
}

TEST(EarlyQuitWarning, OnlyAtlasReportedQuitsKeepTheMenuPage) {
  EXPECT_EQ(NextWarningStep(true, false), WarningStep::kClear);         // set by the client on its own
  EXPECT_EQ(NextWarningStep(true, true), WarningStep::kNone);           // Atlas reported a quit: show it
  EXPECT_EQ(NextWarningStep(false, true), WarningStep::kAcknowledged);  // Continue pressed
  EXPECT_EQ(NextWarningStep(false, false), WarningStep::kNone);
  EXPECT_EQ(kWarningBit, 1ULL << 46);
}

TEST(EarlyQuitLockout, OurMaskKeepsTheBitsAtlasSends) {
  const std::uint8_t atlas = (1 << 0) | (1 << 1) | (1 << 5);  // DefaultEarlyQuitFeatureFlags
  EXPECT_EQ(KeptFlags(atlas, kStockFlagMask), 1 << 0);       // the stock client loses the timer and the info pages
  EXPECT_EQ(KeptFlags(atlas, kOurFlagMask), atlas);
}

TEST(EarlyQuitBar, NodesCallTheBit1BoolNodes) {
  EXPECT_EQ(BarCallTarget(kBarNodes[0]), 0x251F0u);
  EXPECT_EQ(BarCallTarget(kBarNodes[1]), 0x25750u);
}

TEST(EarlyQuitBar, PatchIsTheCallsLength) {
  for (const BarNode& n : kBarNodes) EXPECT_EQ(n.call[0], 0xE8);
  EXPECT_EQ(sizeof(kBarSaysNo), 5u);
  EXPECT_EQ(kBarSaysNo[0], 0x31);  // xor eax,eax: al = 0, the stock first pin
}

TEST(EarlyQuitMenuPage, ShownOnlyForAReportedQuit) {
  EXPECT_EQ(std::memcmp(MenuPageBytes(true), kMenuPageShows, 8), 0);
  EXPECT_EQ(std::memcmp(MenuPageBytes(false), kMenuPageHides, 8), 0);
}

TEST(EarlyQuitMenuPage, HideJumpsToTheFalseReturn) {
  // JMP rel8 from kMenuPageAt lands on XOR AL,AL, the last two bytes of kMenuPageAfter.
  const std::size_t target = kMenuPageAt + 2 + kMenuPageHides[1];
  EXPECT_EQ(target, kMenuPageAt + 8 + sizeof(kMenuPageAfter) - 2);
  EXPECT_EQ(kMenuPageAt % 8, 0u);
  EXPECT_EQ(kMenuPageHides[7], kMenuPageShows[7]);  // MOV AL's opcode byte stays
}

TEST(EarlyQuitBar, BounceOnlyAfterALockout) {
  EXPECT_TRUE(BarBounced(true, 1000, 1035));
  EXPECT_FALSE(BarBounced(false, 1000, 1035));        // no lockout shown: stock
  EXPECT_FALSE(BarBounced(true, 1000, 1000 + 5000));  // matchmaking ended later
  EXPECT_FALSE(BarBounced(true, 0, 1035));            // never seen on
}
