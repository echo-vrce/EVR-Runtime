#include "cdn/cdn_catalog.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

using evr_cdn::kEntryBytes;
using evr_cdn::kListHeader;

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

struct Stock {
  std::string name;
  std::string image, other;  // extra data
};

// A list in the stock layout (synthetic entries; only the fields the patch touches are meaningful).
std::string MakeList(const std::vector<Stock>& items) {
  std::string b(kListHeader, '\0');
  Put<std::uint64_t>(&b, 8, items.size() * kEntryBytes);
  Put<std::uint32_t>(&b, 28, 1);
  Put<std::uint64_t>(&b, 40, items.size());
  Put<std::uint64_t>(&b, 48, items.size());
  std::string ext;
  for (const Stock& s : items) {
    std::string e(kEntryBytes, '\0');
    Put<std::uint64_t>(&e, 0, evr::Symbol64(s.name));
    Put<std::uint64_t>(&e, 32, evr::Symbol64(s.name));
    std::memcpy(&e[40], s.name.data(), s.name.size());
    Put<std::uint64_t>(&e, 104, evr::Symbol64("decal"));
    Put<std::uint64_t>(&e, 112, evr::Symbol64("common"));
    Put<std::uint64_t>(&e, 320, 0x1111);
    for (std::size_t v : {432u, 488u}) {
      Put<std::uint64_t>(&e, v + 24, 0x100000000ULL);
      Put<std::uint64_t>(&e, v + 32, 32);
    }
    Put<std::uint64_t>(&e, 432 + 8, s.image.size());
    Put<std::uint64_t>(&e, 432 + 40, s.image.size() / 8);
    Put<std::uint64_t>(&e, 432 + 48, s.image.size() / 8);
    Put<std::uint64_t>(&e, 488 + 8, s.other.size());
    Put<std::uint64_t>(&e, 488 + 40, s.other.size() / 12);
    Put<std::uint64_t>(&e, 488 + 48, s.other.size() / 12);
    b += e;
    ext += s.image + s.other;
  }
  return b + ext;
}

evr_cdn::CosmeticItem Item(const std::string& json) {
  evr_cdn::CosmeticItem it;
  std::string error;
  EXPECT_TRUE(evr_cdn::ParseCosmeticItem(json, &it, &error)) << error;
  return it;
}

TEST(CdnCatalog, AppendsACopyOfTheTemplateWithOverrides) {
  const std::string list = MakeList({{"rwd_decal_a", "", ""}, {"rwd_pattern_a", std::string(8, '\x07'), ""}});
  std::string out;
  std::vector<std::string> skipped;
  ASSERT_TRUE(evr_cdn::AppendCosmeticItems(
      list,
      {Item(R"({"template":"rwd_pattern_a","internal_name":"rwd_pattern_g_fox","display_name":"Fox",
                "rarity":"3a5ec345159d921d","thumbnail":"00000000000000aa","texture":"00000000000000bb"})")},
      &out, &skipped));
  EXPECT_TRUE(skipped.empty());
  ASSERT_EQ(Get<std::uint64_t>(out, 40), 3u);
  ASSERT_EQ(Get<std::uint64_t>(out, 48), 3u);
  EXPECT_EQ(Get<std::uint64_t>(out, 8), 3 * kEntryBytes);
  const std::size_t e = kListHeader + 2 * kEntryBytes;
  EXPECT_EQ(Get<std::uint64_t>(out, e), evr::Symbol64("rwd_pattern_g_fox"));
  EXPECT_EQ(Get<std::uint64_t>(out, e + 32), evr::Symbol64("rwd_pattern_g_fox"));
  EXPECT_STREQ(&out[e + 40], "rwd_pattern_g_fox");
  EXPECT_STREQ(&out[e + 168], "Fox");
  EXPECT_EQ(Get<std::uint64_t>(out, e + 104), evr::Symbol64("decal"));  // the template's kind
  EXPECT_EQ(Get<std::uint64_t>(out, e + 112), 0x3A5EC345159D921DULL);
  EXPECT_EQ(Get<std::uint64_t>(out, e + 128), 0xAAu);
  EXPECT_EQ(Get<std::uint64_t>(out, e + 320), 0xBBu);
  // The stock part is unchanged and the template's image list is copied after the stock extra data.
  EXPECT_EQ(out.substr(kListHeader, 2 * kEntryBytes), list.substr(kListHeader, 2 * kEntryBytes));
  EXPECT_EQ(out.substr(kListHeader + 3 * kEntryBytes), std::string(8, '\x07') + std::string(8, '\x07'));
}

TEST(CdnCatalog, ColoursReplaceTheOtherData) {
  const std::string tint(24, '\x01');
  const std::string list = MakeList({{"tint_a", "", tint}});
  std::string out;
  ASSERT_TRUE(evr_cdn::AppendCosmeticItems(
      list, {Item(R"({"template":"tint_a","internal_name":"tint_g_x","colors":[[1,0,0],[0,0.5,1],[0,1,0]]})")}, &out,
      nullptr));
  const std::size_t e = kListHeader + kEntryBytes;
  EXPECT_EQ(Get<std::uint64_t>(out, e + 488 + 8), 36u);
  EXPECT_EQ(Get<std::uint64_t>(out, e + 488 + 40), 3u);
  EXPECT_EQ(Get<std::uint64_t>(out, e + 488 + 48), 3u);
  const std::string ext = out.substr(kListHeader + 2 * kEntryBytes);
  ASSERT_EQ(ext.size(), 24u + 36u);
  EXPECT_EQ(ext.substr(0, 24), tint);
  EXPECT_EQ(Get<float>(ext, 24), 1.0f);
  EXPECT_EQ(Get<float>(ext, 24 + 16), 0.5f);
}

TEST(CdnCatalog, SkipsDuplicatesAndMissingTemplates) {
  const std::string list = MakeList({{"rwd_decal_a", "", ""}});
  std::string out;
  std::vector<std::string> skipped;
  ASSERT_TRUE(evr_cdn::AppendCosmeticItems(
      list,
      {Item(R"({"template":"rwd_decal_a","internal_name":"rwd_decal_a"})"),
       Item(R"({"template":"nope","internal_name":"rwd_x"})"),
       Item(R"({"template":"rwd_decal_a","internal_name":"rwd_y"})"),
       Item(R"({"template":"rwd_decal_a","internal_name":"rwd_y"})")},
      &out, &skipped));
  EXPECT_EQ(Get<std::uint64_t>(out, 40), 2u);
  EXPECT_EQ(skipped, (std::vector<std::string>{"rwd_decal_a", "rwd_x", "rwd_y"}));
}

TEST(CdnCatalog, StockFixesRenameInPlace) {
  const std::string list = MakeList({{"rwd_emissive_0017", "", std::string(24, '')}, {"rwd_decal_a", "", ""}});
  evr_cdn::StockFix f;
  std::string error;
  ASSERT_TRUE(evr_cdn::ParseStockFix(R"({"internal_name":"rwd_emissive_0017","display_name":"Lancer"})", &f, &error));
  evr_cdn::StockFix missing{"rwd_nope", "X"};
  std::string out;
  ASSERT_TRUE(evr_cdn::AppendCosmeticItems(list, {}, &out, nullptr));
  EXPECT_EQ(evr_cdn::ApplyStockFixes(&out, {f, missing}), 1u);
  ASSERT_EQ(out.size(), list.size());
  EXPECT_STREQ(&out[kListHeader + 168], "Lancer");
  EXPECT_EQ(out.substr(kListHeader + kEntryBytes), list.substr(kListHeader + kEntryBytes));  // nothing else moves
  EXPECT_FALSE(evr_cdn::ParseStockFix(R"({"internal_name":"rwd_emissive_0017"})", &f, &error));
  EXPECT_FALSE(evr_cdn::ParseStockFix(R"({"internal_name":"a","display_name":")" + std::string(64, 'x') + "\"}", &f, &error));
  EXPECT_EQ(evr_cdn::kStockFixType, evr::Symbol64("evr_cdn_stock_fix"));
}

TEST(CdnCatalog, RefusesAnUnexpectedLayout) {
  std::string list = MakeList({{"rwd_decal_a", "", ""}});
  std::string out;
  EXPECT_FALSE(evr_cdn::AppendCosmeticItems(list + "x", {}, &out, nullptr));  // unknown tail
  Put<std::uint64_t>(&list, 48, 2);
  EXPECT_FALSE(evr_cdn::AppendCosmeticItems(list, {}, &out, nullptr));
  EXPECT_FALSE(evr_cdn::AppendCosmeticItems("short", {}, &out, nullptr));
}

TEST(CdnCatalog, ParseRefusesBadSpecs) {
  evr_cdn::CosmeticItem it;
  std::string error;
  EXPECT_FALSE(evr_cdn::ParseCosmeticItem(R"({"internal_name":"x"})", &it, &error));
  EXPECT_FALSE(evr_cdn::ParseCosmeticItem(R"({"template":"t","internal_name":"x","rarity":"XYZ"})", &it, &error));
  EXPECT_FALSE(evr_cdn::ParseCosmeticItem(R"({"template":"t","internal_name":"x","colors":[[1,2]]})", &it, &error));
  EXPECT_FALSE(evr_cdn::ParseCosmeticItem(
      R"({"template":"t","internal_name":")" + std::string(64, 'a') + R"("})", &it, &error));
  EXPECT_FALSE(evr_cdn::ParseCosmeticItem("not json", &it, &error));
  EXPECT_FALSE(evr_cdn::ParseCosmeticItem(
      R"({"template":"t","internal_name":"x","source":")" + std::string(64, 'a') + R"("})", &it, &error));
}

TEST(CdnCatalog, SourceLineDefaultsToGuildItem) {
  evr_cdn::CosmeticItem it;
  std::string error;
  ASSERT_TRUE(evr_cdn::ParseCosmeticItem(R"({"template":"t","internal_name":"x"})", &it, &error)) << error;
  EXPECT_EQ(it.source, "Guild Item");
  ASSERT_TRUE(evr_cdn::ParseCosmeticItem(R"({"template":"t","internal_name":"x","source":"Echo Lounge Item"})", &it,
                                         &error));
  EXPECT_EQ(it.source, "Echo Lounge Item");
}

TEST(CdnCatalog, NamesAtlasUses) {
  EXPECT_EQ(evr_cdn::kLobbyPoster, 0x85C8A8EC5D6C774AULL);
  EXPECT_EQ(evr_cdn::kCosmeticItemType, 0x857950DAB19CEAFBULL);
}

}  // namespace

namespace {

TEST(CdnCatalog, NamesSetListsWholeTexturesAndTheLobbyPoster) {
  namespace fp = nevr_friends_package;
  evr_cdn::Index idx;
  auto add = [&](std::uint64_t type, std::uint64_t name, const char* platform) {
    evr_cdn::Entry e;
    e.type = type;
    e.name = name;
    e.platform = platform;
    idx.entries.push_back(e);
  };
  add(fp::kTextureDescriptorType, 0x10, "pc");
  add(fp::kTexturePayloadType, 0x10, "pc");
  add(fp::kTexturePayloadType, 0x20, "pc");  // payload only: a stock-size override, not listed
  add(fp::kTextureDescriptorType, 0x30, "quest");
  add(fp::kTexturePayloadType, 0x30, "quest");
  add(fp::kTextureDescriptorType, 0x40, "");
  add(fp::kTexturePayloadType, 0x40, "");
  add(evr_cdn::kCosmeticItemType, 0x50, "");
  add(fp::kTextureDescriptorType, 0x60, "pc");  // a stock fix replaces a stock name: never in the set
  add(fp::kTexturePayloadType, 0x60, "pc");
  idx.entries[idx.entries.size() - 1].kind = idx.entries[idx.entries.size() - 2].kind = evr_cdn::kStockFixKind;
  const auto names = evr_cdn::TextureNames(idx, fp::kWin10, "pc");
  EXPECT_EQ(names, (std::vector<std::uint64_t>{0x10, 0x40, evr_cdn::kLobbyPoster}));
  fp::PackageSet set;
  ASSERT_TRUE(evr_cdn::BuildNamesSet(names, fp::kWin10, &set));
  EXPECT_FALSE(set.manifest.empty());
  EXPECT_FALSE(set.package.empty());
}

}  // namespace
