// The UPDATES card's level edits and its panel's texts and buttons (updates_level.h, updates_panel.h).
#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <type_traits>

#include "updates/updates_level.h"
#include "updates/updates_panel.h"

namespace ul = evr_updates_level;
namespace up = evr_updates_panel;

namespace {

template <typename T>
void Put(ul::Bytes* b, std::size_t at, const T& v) {
  std::memcpy(b->data() + at, &v, sizeof(v));
}
template <typename C>
std::remove_const_t<C> Get(const ul::Bytes& b, std::size_t at) {
  std::remove_const_t<C> v{};
  std::memcpy(&v, b.data() + at, sizeof(v));
  return v;
}

// A flat table of `actors.size()` entries of `entry` bytes, +8 of each the actor.
ul::Bytes Flat(std::size_t entry, const std::vector<std::uint64_t>& actors) {
  ul::Bytes b(56 + entry * actors.size(), 0);
  Put<std::uint64_t>(&b, 8, entry * actors.size());
  Put<std::uint64_t>(&b, 40, actors.size());
  Put<std::uint64_t>(&b, 48, actors.size());
  for (std::size_t k = 0; k < actors.size(); ++k) Put(&b, 56 + entry * k + 8, actors[k]);
  return b;
}

up::Status Offer() {
  up::Status s;
  s.state = up::State::kOffer;
  s.version = "0.3.0";
  s.title = "EVR runtime 0.3.0";
  s.notes = {"New UPDATES card", "Skip works"};
  s.files = 2;
  s.bytes = 3 * 1024 * 1024;
  s.relaunch = true;
  return s;
}

}  // namespace

TEST(UpdatesLevel, PointerMeshIsTheStockSizeScaled) {
  ul::Bytes in = Flat(80, {1, ul::kCard, 2});
  ul::Bytes out;
  std::string why;
  ASSERT_TRUE(ul::EditPointerMeshes(in, &out, &why)) << why;
  const std::size_t at = 56 + 80 + 32;
  EXPECT_FLOAT_EQ(Get<float>(out, at), 2.1f * ul::kSize);
  EXPECT_FLOAT_EQ(Get<float>(out, at + 4), 1.05f * ul::kSize);
  EXPECT_EQ(Get<float>(out, 56 + 32), 0.0f);  // other actors untouched
}

TEST(UpdatesLevel, TransformPlacesTheCardAtScaleOne) {
  ul::Bytes in = Flat(176, {ul::kCard, 7});
  ul::Bytes out;
  std::string why;
  ASSERT_TRUE(ul::EditTransforms(in, &out, &why)) << why;
  EXPECT_EQ(Get<decltype(ul::kRotation)>(out, 56 + 0x20), ul::kRotation);
  EXPECT_EQ(Get<decltype(ul::kPosition)>(out, 56 + 0x30), ul::kPosition);
  EXPECT_FLOAT_EQ(Get<float>(out, 56 + 0x3C), 1.0f);
}

TEST(UpdatesLevel, RefusesTablesWithoutTheCard) {
  ul::Bytes out;
  std::string why;
  EXPECT_FALSE(ul::EditPointerMeshes(Flat(80, {1, 2}), &out, &why));
  EXPECT_FALSE(ul::EditTransforms(Flat(176, {1}), &out, &why));
  EXPECT_FALSE(ul::EditActorData(ul::Bytes(100, 0), &out, &why));
}

TEST(UpdatesPanel, OfferShowsNewsAndSkipUpdate) {
  const up::Texts t = up::Compose(Offer());
  EXPECT_EQ(t.card, "UPDATE");
  EXPECT_EQ(t.header, "EVR RUNTIME 0.3.0");
  EXPECT_EQ(t.left, "SKIP");
  EXPECT_EQ(t.right, "UPDATE");
  EXPECT_NE(t.news.find("NEW UPDATES CARD / SKIP WORKS"), std::string::npos);
  EXPECT_NE(t.news.find("2 FILES, 3.0 MB"), std::string::npos);
  EXPECT_LE(t.news.size(), up::kNewsChars);
  EXPECT_TRUE(up::CardVisible(Offer()));
  EXPECT_TRUE(up::OpensByItself(Offer()));
  EXPECT_EQ(up::OnRight(Offer()), up::Action::kStart);
  EXPECT_EQ(up::OnLeft(Offer()), up::Action::kSkip);
}

TEST(UpdatesPanel, SkippedOfferStaysOnTheCardButDoesNotOpenItself) {
  up::Status s = Offer();
  s.skipped = true;
  EXPECT_TRUE(up::CardVisible(s));
  EXPECT_FALSE(up::OpensByItself(s));
  EXPECT_EQ(up::Compose(s).left, "LATER");
  EXPECT_EQ(up::OnLeft(s), up::Action::kNone);  // just closes
  EXPECT_EQ(up::OnRight(s), up::Action::kStart);
}

TEST(UpdatesPanel, DownloadShowsProgressAndKeepsThePanelOpen) {
  up::Status s = Offer();
  s.state = up::State::kDownloading;
  s.done = s.bytes / 2;
  s.total = s.bytes;
  const up::Texts t = up::Compose(s);
  EXPECT_EQ(t.right, "DOWNLOADING 50%");
  EXPECT_EQ(up::OnRight(s), up::Action::kNone);
  EXPECT_TRUE(up::RightKeepsOpen(s));
}

TEST(UpdatesPanel, StagedOffersRestartNow) {
  up::Status s = Offer();
  s.state = up::State::kStaged;
  EXPECT_EQ(up::Compose(s).right, "RESTART NOW");
  EXPECT_EQ(up::OnRight(s), up::Action::kRelaunch);
  s.relaunch = false;  // live content: nothing to restart
  EXPECT_EQ(up::OnRight(s), up::Action::kNone);
  EXPECT_FALSE(up::RightKeepsOpen(s));
}

TEST(UpdatesPanel, NothingToOfferHidesTheCard) {
  up::Status s;
  EXPECT_FALSE(up::CardVisible(s));
  s.state = up::State::kNone;
  EXPECT_FALSE(up::CardVisible(s));
  EXPECT_FALSE(up::OpensByItself(s));
}

TEST(UpdatesPanel, TextsAreDrawable) {
  up::Status s = Offer();
  s.title = "caf\xc3\xa9\nnew";
  const up::Texts t = up::Compose(s);
  for (char c : t.header) EXPECT_TRUE(c >= 0x20 && c <= 0x7E);
  EXPECT_EQ(t.header, "CAF?? NEW");
}

TEST(UpdatesPanel, StateNames) {
  EXPECT_EQ(up::StateFromName("offer"), up::State::kOffer);
  EXPECT_EQ(up::StateFromName("staged"), up::State::kStaged);
  EXPECT_EQ(up::StateFromName("???"), up::State::kChecking);
}
