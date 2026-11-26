// cdn/cdn_index.h: the AtlasCDN index (new cosmetic items and lobby posters), pure logic shared by PC and Quest.
//
// Atlas keeps assets per guild (uploaded by admins, shared to other guilds that allow them) and answers
// GET /evr/cdn/index (runtime Bearer token) with the set for the player's active guild, signed with Atlas's own
// Ed25519 key (pinned in the client as label ATLAS, keys/cdn_pinned.txt):
//   {"index": "<index JSON as a string>", "sigs": [{"key": "ATLAS", "sig": "<base64 ed25519 over the index bytes>"}]}
// Index: {"schema":1, "serial":N, "expires":unix_s, "guild_id":"…", "entries":[{"id","kind":"poster"|"cosmetic",
//          "platform":"pc"|"quest", "type":"<16 hex>", "name":"<16 hex>", "sha256":"<64 hex>", "size":N,
//          "asset":"…", "title":"…"}]}
// Textures differ per platform (PC: 256-byte descriptor + DDS; Quest: 248-byte descriptor + raw ASTC), so an entry
// names its platform; a client takes only its own (an entry without one is for every platform).
// A file is fetched from GET /evr/cdn/files/<sha256> and served in place of resource (type, name) when the game reads
// it. `evr-sign cdn` writes the same form from a local spec (dev and offline tests).
//
// Signatures are checked by a caller-supplied verifier (Ed25519 on PC through Monocypher), so this header has no
// crypto and no platform code. No exceptions escape.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace evr_cdn {

struct PinnedKey {
  std::string label;
  std::vector<std::uint8_t> publicKey;  // 32 bytes
};

struct Entry {
  std::string id, kind;
  std::uint64_t type = 0, name = 0;
  std::string sha256;  // lowercase hex
  std::uint64_t size = 0;
  std::string platform;      // "pc", "quest", or empty for both
  std::string asset, title;  // informational
};

struct Index {
  std::uint64_t serial = 0, expires = 0;
  std::string guildId;
  std::vector<Entry> entries;
};

// (signature 64 bytes, public key 32 bytes, message) -> valid
using Verifier = std::function<bool(const std::uint8_t*, const std::uint8_t*, const std::string&)>;

// The lobby posters are one texture array (lobby_poster_array_4k_emi, four posters) of about 130 MiB.
constexpr std::uint64_t kMaxFileBytes = 256ull << 20;
constexpr std::size_t kMaxEntries = 4096;

inline std::vector<std::uint8_t> Base64(const std::string& in) {
  std::vector<std::uint8_t> out;
  std::uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+') v = 62;
    else if (c == '/') v = 63;
    else if (c == '=') break;
    else return {};
    acc = (acc << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>(acc >> bits));
    }
  }
  return out;
}

inline bool ParseHex64(const std::string& s, std::uint64_t* out) {
  if (s.size() != 16) return false;
  std::uint64_t v = 0;
  for (char c : s) {
    int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
    if (d < 0) return false;
    v = (v << 4) | static_cast<std::uint64_t>(d);
  }
  *out = v;
  return true;
}

inline bool IsSha256Hex(const std::string& s) {
  if (s.size() != 64) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

// "LABEL BASE64" lines, '#' comments (keys/pinned.txt).
inline std::vector<PinnedKey> ParsePinned(const std::string& text) {
  std::vector<PinnedKey> keys;
  std::size_t at = 0;
  while (at < text.size()) {
    std::size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    at = end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const std::size_t sp = line.find(' ');
    if (sp == std::string::npos) continue;
    PinnedKey k{line.substr(0, sp), Base64(line.substr(sp + 1))};
    if (k.publicKey.size() == 32) keys.push_back(std::move(k));
  }
  return keys;
}

// Verifies the envelope against the pinned keys (`threshold` distinct keys must sign) and parses the index.
// `error` gets a short reason on failure: malformed, signature, schema, expired, entry.
inline bool Open(const std::string& envelope, const std::vector<PinnedKey>& pinned, std::size_t threshold,
                 const Verifier& verify, std::uint64_t now, Index* out, std::string* error) {
  const nlohmann::json env = nlohmann::json::parse(envelope, nullptr, false);
  if (!env.is_object() || !env.contains("index") || !env["index"].is_string() || !env.contains("sigs") ||
      !env["sigs"].is_array()) {
    *error = "malformed";
    return false;
  }
  const std::string body = env["index"].get<std::string>();
  std::vector<std::string> signers;
  for (const auto& s : env["sigs"]) {
    if (!s.is_object() || !s.contains("key") || !s["key"].is_string() || !s.contains("sig") || !s["sig"].is_string())
      continue;
    const std::string label = s["key"].get<std::string>();
    const std::vector<std::uint8_t> sig = Base64(s["sig"].get<std::string>());
    if (sig.size() != 64) continue;
    for (const PinnedKey& k : pinned) {
      if (k.label != label) continue;
      bool seen = false;
      for (const auto& d : signers) seen = seen || d == label;
      if (!seen && verify(sig.data(), k.publicKey.data(), body)) signers.push_back(label);
    }
  }
  if (threshold == 0 || signers.size() < threshold) {
    *error = "signature";
    return false;
  }
  const nlohmann::json idx = nlohmann::json::parse(body, nullptr, false);
  if (!idx.is_object() || !idx.contains("entries") || !idx["entries"].is_array()) {
    *error = "malformed";
    return false;
  }
  if (idx.value("schema", 0) != 1) {
    *error = "schema";
    return false;
  }
  Index r;
  r.serial = idx.value("serial", 0ull);
  r.expires = idx.value("expires", 0ull);
  if (idx.contains("guild_id") && idx["guild_id"].is_string()) r.guildId = idx["guild_id"].get<std::string>();
  if (r.expires <= now) {
    *error = "expired";
    return false;
  }
  if (idx["entries"].size() > kMaxEntries) {
    *error = "entry";
    return false;
  }
  for (const auto& e : idx["entries"]) {
    Entry x;
    if (!e.is_object()) {
      *error = "entry";
      return false;
    }
    x.id = e.value("id", "");
    x.kind = e.value("kind", "");
    x.sha256 = e.value("sha256", "");
    x.size = e.value("size", 0ull);
    if (e.contains("platform") && e["platform"].is_string()) x.platform = e["platform"].get<std::string>();
    if (!x.platform.empty() && x.platform != "pc" && x.platform != "quest") {
      *error = "entry " + x.id;
      return false;
    }
    if (e.contains("asset") && e["asset"].is_string()) x.asset = e["asset"].get<std::string>();
    if (e.contains("title") && e["title"].is_string()) x.title = e["title"].get<std::string>();
    if (x.id.empty() || (x.kind != "poster" && x.kind != "cosmetic" && x.kind != "stock_fix") || !ParseHex64(e.value("type", ""), &x.type) ||
        !ParseHex64(e.value("name", ""), &x.name) || !IsSha256Hex(x.sha256) || x.size == 0 || x.size > kMaxFileBytes) {
      *error = "entry " + x.id;
      return false;
    }
    r.entries.push_back(std::move(x));
  }
  *out = std::move(r);
  return true;
}

}  // namespace evr_cdn
