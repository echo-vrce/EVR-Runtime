#include "cdn/cdn_index.h"

#include <gtest/gtest.h>

namespace {

using namespace evr_cdn;

const std::vector<std::uint8_t> kKey(32, 7);

std::string Envelope(const std::string& index, const std::string& label = "ATLAS",
                     const std::string& sig = std::string(86, 'A') + "==") {
  return nlohmann::json{{"index", index}, {"sigs", {{{"key", label}, {"sig", sig}}}}}.dump();
}

std::string IndexJson(std::uint64_t expires, const std::string& sha = std::string(64, 'a'), std::uint64_t size = 10) {
  return nlohmann::json{{"schema", 1},
                        {"serial", 3},
                        {"expires", expires},
                        {"guild_id", "g1"},
                        {"entries",
                         {{{"id", "poster/0"},
                           {"kind", "poster"},
                           {"type", "0123456789abcdef"},
                           {"name", "fedcba9876543210"},
                           {"sha256", sha},
                           {"size", size},
                           {"title", "EML poster"}}}}}
      .dump();
}

Verifier Accept(const std::string& expect) {
  return [expect](const std::uint8_t*, const std::uint8_t* key, const std::string& msg) {
    return key[0] == 7 && msg == expect;
  };
}

TEST(CdnIndex, OpensASignedIndex) {
  const std::string idx = IndexJson(2000);
  Index out;
  std::string err;
  ASSERT_TRUE(Open(Envelope(idx), {{"ATLAS", kKey}}, 1, Accept(idx), 1000, &out, &err)) << err;
  EXPECT_EQ(out.serial, 3u);
  EXPECT_EQ(out.guildId, "g1");
  ASSERT_EQ(out.entries.size(), 1u);
  EXPECT_EQ(out.entries[0].type, 0x0123456789abcdefull);
  EXPECT_EQ(out.entries[0].name, 0xfedcba9876543210ull);
  EXPECT_EQ(out.entries[0].title, "EML poster");
}

TEST(CdnIndex, RefusesUnknownKeyBadSignatureAndExpiry) {
  const std::string idx = IndexJson(2000);
  Index out;
  std::string err;
  EXPECT_FALSE(Open(Envelope(idx, "OTHER"), {{"ATLAS", kKey}}, 1, Accept(idx), 1000, &out, &err));
  EXPECT_EQ(err, "signature");
  EXPECT_FALSE(Open(Envelope(idx), {{"ATLAS", kKey}}, 1, Accept("something else"), 1000, &out, &err));
  EXPECT_EQ(err, "signature");
  EXPECT_FALSE(Open(Envelope(idx), {{"ATLAS", kKey}}, 1, Accept(idx), 3000, &out, &err));
  EXPECT_EQ(err, "expired");
}

TEST(CdnIndex, RefusesBadEntries) {
  Index out;
  std::string err;
  const std::string badSha = IndexJson(2000, std::string(64, 'G'));
  EXPECT_FALSE(Open(Envelope(badSha), {{"ATLAS", kKey}}, 1, Accept(badSha), 1000, &out, &err));
  const std::string tooBig = IndexJson(2000, std::string(64, 'a'), kMaxFileBytes + 1);
  EXPECT_FALSE(Open(Envelope(tooBig), {{"ATLAS", kKey}}, 1, Accept(tooBig), 1000, &out, &err));
}

TEST(CdnIndex, ParsesPinnedKeys) {
  const auto keys = ParsePinned("# comment\r\nATLAS tTdtEOzuuXwMTw7/cWAX2ZeyN8W6WDl43xxtDdWwwWo=\r\nBAD xx\n");
  ASSERT_EQ(keys.size(), 1u);
  EXPECT_EQ(keys[0].label, "ATLAS");
  EXPECT_EQ(keys[0].publicKey.size(), 32u);
}

}  // namespace

TEST(CdnIndex, AcceptsEveryKindAtlasServes) {
  // Atlas serves posters, cosmetics and (its global set) stock fixes; an unknown kind refuses the whole index.
  for (const char* kind : {"poster", "cosmetic", "stock_fix", "nonsense"}) {
    nlohmann::json j = nlohmann::json::parse(IndexJson(2000));
    j["entries"][0]["kind"] = kind;
    const std::string idx = j.dump();
    Index out;
    std::string err;
    EXPECT_EQ(Open(Envelope(idx), {{"ATLAS", kKey}}, 1, Accept(idx), 1000, &out, &err), std::string(kind) != "nonsense")
        << kind << " " << err;
  }
}
