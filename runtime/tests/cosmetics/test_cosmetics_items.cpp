// The release chassis records and the Story rarity (src/cosmetics/cosmetics_items.h).
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "cosmetics/cosmetics_items.h"

namespace {

template <typename T>
T Get(const std::string& b, std::size_t at) {
  T v{};
  std::memcpy(&v, b.data() + at, sizeof(v));
  return v;
}

// A one-entry stock list: the chassis template, with sentinel model fields.
std::string TemplateList() {
  std::string e(evr_cdn::kEntryBytes, '\0');
  const std::uint64_t sym = evr::Symbol64(evr_cosmetics::kChassisTemplate), old = 0x1111111111111111ULL;
  std::memcpy(&e[0], &sym, 8);
  std::memcpy(&e[32], &sym, 8);
  std::memcpy(&e[40], evr_cosmetics::kChassisTemplate, std::strlen(evr_cosmetics::kChassisTemplate));
  for (std::size_t at : {std::size_t{144}, std::size_t{152}, std::size_t{584}, std::size_t{632}}) std::memcpy(&e[at], &old, 8);
  std::string list(evr_cdn::kListHeader, '\0');
  const std::uint64_t one = 1, bytes = evr_cdn::kEntryBytes;
  const std::uint32_t flag = 1;
  std::memcpy(&list[8], &bytes, 8);
  std::memcpy(&list[28], &flag, 4);
  std::memcpy(&list[40], &one, 8);
  std::memcpy(&list[48], &one, 8);
  return list + e;
}

}  // namespace

TEST(CosmeticsItems, StoryRarityAndSprite) {
  EXPECT_EQ(evr_cosmetics::kStoryRarity, 0xE32DC7C9CD8056A9ULL);
  EXPECT_FLOAT_EQ(evr_cosmetics::kStoryFrameColour[0], 1.0f * 0xFC / 255);
  EXPECT_EQ(evr_cosmetics::kChassisSet, evr::Symbol64(evr_cosmetics::kChassisSetName));
}

TEST(CosmeticsItems, ChassisRecordsCarryTheirModels) {
  const auto items = evr_cosmetics::ChassisItems();
  ASSERT_EQ(items.size(), 4u);  // two chassis, two boosters (their template is not in this list: skipped)
  std::string out;
  std::vector<std::string> skipped;
  ASSERT_TRUE(evr_cdn::AppendCosmeticItems(TemplateList(), items, &out, &skipped));
  EXPECT_EQ(skipped, (std::vector<std::string>{"rwd_booster_le2_delta_a", "rwd_booster_le1_jack_a"}));
  ASSERT_EQ(Get<std::uint64_t>(out, 40), 3u);
  const std::size_t delta = evr_cdn::kListHeader + evr_cdn::kEntryBytes, jack = delta + evr_cdn::kEntryBytes;
  EXPECT_EQ(Get<std::uint64_t>(out, delta), evr::Symbol64("rwd_chassis_le2_delta_a"));
  EXPECT_STREQ(&out[delta + 40], "rwd_chassis_le2_delta_a");
  EXPECT_STREQ(&out[delta + 168], "Delta-13");
  EXPECT_EQ(Get<std::uint64_t>(out, delta + 112), evr_cosmetics::kStoryRarity);
  EXPECT_EQ(Get<std::uint64_t>(out, delta + 128), 0x57C8BCCE0355AE83ULL);  // icon
  EXPECT_EQ(Get<std::uint64_t>(out, delta + 632), 0x5CBFC8FB292585BFULL);  // 3p model
  EXPECT_EQ(Get<std::uint64_t>(out, delta + 152), 0x5CBFC8FB292585BFULL);  // its mirror
  EXPECT_EQ(Get<std::uint64_t>(out, delta + 584), 0x55517547374D8DC4ULL);  // 1p model
  EXPECT_EQ(Get<std::uint64_t>(out, delta + 144), 0xB43B3AFCD943A27AULL);  // 3p material
  EXPECT_STREQ(&out[jack + 168], "Jack");
  EXPECT_EQ(Get<std::uint64_t>(out, jack + 632), 0x7029A59D8961CC8FULL);
  // Appending again changes nothing: the names exist.
  std::string again;
  skipped.clear();
  ASSERT_TRUE(evr_cdn::AppendCosmeticItems(out, items, &again, &skipped));
  EXPECT_EQ(skipped.size(), 4u);
  EXPECT_EQ(again, out);
}

TEST(CosmeticsItems, CdnSpecsMayNameModels) {
  evr_cdn::CosmeticItem it;
  std::string error;
  ASSERT_TRUE(evr_cdn::ParseCosmeticItem(
      R"({"template":"t","internal_name":"x","model_3p":"5cbfc8fb292585bf","model_1p":"55517547374d8dc4"})", &it, &error));
  EXPECT_TRUE(it.hasModel3p && it.hasModel1p && !it.hasMaterial3p);
  EXPECT_EQ(it.model3p, 0x5CBFC8FB292585BFULL);
  EXPECT_FALSE(evr_cdn::ParseCosmeticItem(R"({"template":"t","internal_name":"x","model_3p":"nothex"})", &it, &error));
}

TEST(CosmeticsItems, BoostersSetOnlyTheirModel) {
  const auto items = evr_cosmetics::ChassisItems();
  const evr_cdn::CosmeticItem& b = items[2];
  EXPECT_EQ(b.internalName, "rwd_booster_le2_delta_a");
  EXPECT_EQ(b.templateName, "rwd_booster_default");
  EXPECT_EQ(b.source, evr_cosmetics::kStorySourceBooster);
  EXPECT_TRUE(b.hasModel3p && !b.hasModel1p && !b.mirrorModel3p && b.hasMaterial3p);
  EXPECT_EQ(b.model3p, 0xAE32A9AE46AFD867ULL);
}
