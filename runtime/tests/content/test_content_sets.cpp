// content_sets.h: set names, the manifest read back from BuildPackageSet's output, extraction and the read verdict.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "content/content_sets.h"
#include "friends/friends_package.h"

namespace {

namespace fp = nevr_friends_package;
using evr_content::Bytes;

TEST(ContentSets, ParseSetName) {
  std::uint64_t id = 0;
  EXPECT_TRUE(evr_content::ParseSetName("27974ae8287406f2", &id));
  EXPECT_EQ(id, 0x27974AE8287406F2ULL);
  EXPECT_TRUE(evr_content::ParseSetName("27974AE8287406F2", &id));
  EXPECT_EQ(id, 0x27974AE8287406F2ULL);
  EXPECT_FALSE(evr_content::ParseSetName("27974ae8287406f", &id));    // 15 digits
  EXPECT_FALSE(evr_content::ParseSetName("27974ae8287406f20", &id));  // 17
  EXPECT_FALSE(evr_content::ParseSetName("27974ae8287406g2", &id));
  EXPECT_FALSE(evr_content::ParseSetName("27974ae8287406f2_0", &id));
  EXPECT_FALSE(evr_content::ParseSetName("", &id));
}

TEST(ContentSets, SetIdsFiltersSortsAndDedupes) {
  const auto ids = evr_content::SetIds({"ffffffffffffffff", "readme.txt", "0000000000000001", "0000000000000001",
                                        "27974ae8287406f2_0", "27974AE8287406F2"});
  ASSERT_EQ(ids.size(), 3u);
  EXPECT_EQ(ids[0], 1u);
  EXPECT_EQ(ids[1], 0x27974AE8287406F2ULL);
  EXPECT_EQ(ids[2], ~0ULL);
}

TEST(ContentSets, FileNames) {
  EXPECT_EQ(evr_content::SetFileName(0x27974AE8287406F2ULL), "27974ae8287406f2");
  EXPECT_EQ(evr_content::SetFileName(1), "0000000000000001");
  EXPECT_EQ(evr_content::PackageFileName(1, 0), "0000000000000001_0");
}

TEST(ContentSets, ManifestRoundTrip) {
  std::vector<fp::Resource> res;
  for (std::uint64_t i = 0; i < 5; ++i) {
    Bytes b(300000 + i, static_cast<std::uint8_t>(i + 1));  // > one 512 KiB block in all: several blocks
    res.push_back({0x1000 + i, 0x2000 + i, b});
  }
  fp::PackageSet set;
  ASSERT_TRUE(fp::BuildPackageSet(res, &set));
  evr_content::Manifest m;
  ASSERT_TRUE(evr_content::ParseManifest(set.manifest, &m));
  EXPECT_EQ(m.packages, 1u);
  ASSERT_EQ(m.resources.size(), 5u);
  for (std::size_t i = 0; i < m.resources.size(); ++i) {
    Bytes out;
    ASSERT_TRUE(evr_content::ExtractResource(m, i, set.package, &out));
    const std::uint64_t k = m.resources[i].type - 0x1000;
    EXPECT_EQ(m.resources[i].name, 0x2000 + k);
    EXPECT_EQ(out, res[k].bytes);
  }
}

TEST(ContentSets, FriendsSetParses) {
  fp::PackageSet set;
  ASSERT_TRUE(fp::BuildFriendsPictures(fp::kWin10, 2, 3, &set));
  evr_content::Manifest m;
  ASSERT_TRUE(evr_content::ParseManifest(set.manifest, &m));
  EXPECT_EQ(m.resources.size(), 2u * (2 * 3 + 3));
}

TEST(ContentSets, RejectsDamage) {
  fp::PackageSet set;
  ASSERT_TRUE(fp::BuildPackageSet({{1, 2, Bytes(64, 7)}}, &set));
  evr_content::Manifest m;
  Bytes bad = set.manifest;
  bad[0] = 'X';
  EXPECT_FALSE(evr_content::ParseManifest(bad, &m));
  bad = set.manifest;
  bad.pop_back();
  EXPECT_FALSE(evr_content::ParseManifest(bad, &m));
  EXPECT_FALSE(evr_content::ParseManifest(Bytes(10, 0), &m));
  ASSERT_TRUE(evr_content::ParseManifest(set.manifest, &m));
  Bytes out;
  EXPECT_FALSE(evr_content::ExtractResource(m, 0, Bytes(2, 0), &out));  // package too short
  EXPECT_FALSE(evr_content::ExtractResource(m, 1, set.package, &out));  // no such resource
}

TEST(ContentSets, Judge) {
  const Bytes ours = {1, 2, 3};
  const std::uint8_t same[] = {1, 2, 3}, other[] = {1, 2, 4};
  EXPECT_EQ(evr_content::Judge(same, 3, ours), evr_content::ReadVerdict::Ours);
  EXPECT_EQ(evr_content::Judge(other, 3, ours), evr_content::ReadVerdict::Stock);
  EXPECT_EQ(evr_content::Judge(same, 2, ours), evr_content::ReadVerdict::Stock);
}

}  // namespace
