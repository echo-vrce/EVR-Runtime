#pragma once

// A release's own package sets, read back (the inverse of nevr_friends_package::BuildPackageSet).
//
// Convention: <version dir>\data\rad15\win10\manifests\<set id, 16 hex digits> and ...\packages\<set id>_<n> (one
// file per package the manifest names). The runtime mounts each set with AddPackage and also reads it here, so it can
// tell whether the game's read of a (type, name) the set holds got our bytes or a stock copy, and serve ours if not.
// Manifest layout: friends/friends_package.h.
//
// Pure logic, header only (zstd only), no exceptions. Tests: tests/content/test_content_sets.cpp.

#include <zstd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace evr_content {

using Bytes = std::vector<std::uint8_t>;

/// True when `file` is exactly 16 hex digits (either case); *id gets the value.
inline bool ParseSetName(const std::string& file, std::uint64_t* id) {
  if (file.size() != 16) return false;
  std::uint64_t v = 0;
  for (char c : file) {
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return false;
    v = (v << 4) | static_cast<std::uint64_t>(d);
  }
  *id = v;
  return true;
}

/// The set ids among a manifests folder's file names, sorted, without repeats; other names are skipped.
inline std::vector<std::uint64_t> SetIds(const std::vector<std::string>& files) {
  std::vector<std::uint64_t> ids;
  for (const std::string& f : files) {
    std::uint64_t id;
    if (ParseSetName(f, &id)) ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

/// The file names of a set: "<16 hex digits>" and "<16 hex digits>_<n>".
inline std::string SetFileName(std::uint64_t id) {
  char b[17];
  static const char* hex = "0123456789abcdef";
  for (int i = 15; i >= 0; --i, id >>= 4) b[i] = hex[id & 0xF];
  b[16] = '\0';
  return b;
}
inline std::string PackageFileName(std::uint64_t id, std::uint32_t package) {
  return SetFileName(id) + "_" + std::to_string(package);
}

struct ResourceRecord {
  std::uint64_t type, name;
  std::uint32_t block, offset, size;
};
struct BlockRecord {
  std::uint32_t package, offset, compressed, size;
};
struct Manifest {
  std::uint32_t packages = 0;
  std::vector<ResourceRecord> resources;
  std::vector<BlockRecord> blocks;  // as stored: data blocks, then each package's end record and a zero record
};

namespace detail {
template <typename T>
T Get(const std::uint8_t* p) {
  T v{};
  std::memcpy(&v, p, sizeof(v));
  return v;
}
}  // namespace detail

/// Reads a manifest file ("ZSTD" header + one frame). False on any size or layout mismatch.
inline bool ParseManifest(const Bytes& file, Manifest* out) {
  using detail::Get;
  if (file.size() < 24 || std::memcmp(file.data(), "ZSTD", 4) != 0) return false;
  const std::uint64_t raw = Get<std::uint64_t>(file.data() + 8), compressed = Get<std::uint64_t>(file.data() + 16);
  if (raw < 0xC0 || raw > (256u << 20) || compressed != file.size() - 24) return false;
  Bytes m(static_cast<std::size_t>(raw));
  const std::size_t n = ZSTD_decompress(m.data(), m.size(), file.data() + 24, static_cast<std::size_t>(compressed));
  if (ZSTD_isError(n) != 0 || n != m.size()) return false;
  const std::uint64_t nr = Get<std::uint64_t>(m.data() + 0x10 + 0x20), ni = Get<std::uint64_t>(m.data() + 0x50 + 0x20),
                      nb = Get<std::uint64_t>(m.data() + 0x90 + 0x20);
  if (Get<std::uint64_t>(m.data() + 0x10) != 32 * nr || Get<std::uint64_t>(m.data() + 0x50) != 40 * ni ||
      Get<std::uint64_t>(m.data() + 0x90) != 16 * nb || 0xC0 + 32 * nr + 40 * ni + 16 * nb > m.size())
    return false;
  Manifest r;
  r.packages = Get<std::uint32_t>(m.data());
  const std::uint8_t* p = m.data() + 0xC0;
  for (std::uint64_t i = 0; i < nr; ++i, p += 32)
    r.resources.push_back({Get<std::uint64_t>(p), Get<std::uint64_t>(p + 8), Get<std::uint32_t>(p + 16),
                           Get<std::uint32_t>(p + 20), Get<std::uint32_t>(p + 24)});
  p += 40 * ni;
  for (std::uint64_t i = 0; i < nb; ++i, p += 16)
    r.blocks.push_back({Get<std::uint32_t>(p), Get<std::uint32_t>(p + 4), Get<std::uint32_t>(p + 8),
                        Get<std::uint32_t>(p + 12)});
  for (const ResourceRecord& res : r.resources) {
    if (res.block >= r.blocks.size()) return false;
    const BlockRecord& b = r.blocks[res.block];
    if (b.package >= r.packages || b.compressed == 0 || static_cast<std::uint64_t>(res.offset) + res.size > b.size)
      return false;
  }
  *out = std::move(r);
  return true;
}

/// The bytes of manifest resource `index`, cut from `package` (the file of its block's package). False when the
/// block lies outside the file or does not decompress to its recorded size.
inline bool ExtractResource(const Manifest& m, std::size_t index, const Bytes& package, Bytes* out) {
  if (index >= m.resources.size()) return false;
  const ResourceRecord& res = m.resources[index];
  const BlockRecord& b = m.blocks[res.block];
  if (static_cast<std::uint64_t>(b.offset) + b.compressed > package.size()) return false;
  Bytes block(b.size);
  const std::size_t n = ZSTD_decompress(block.data(), block.size(), package.data() + b.offset, b.compressed);
  if (ZSTD_isError(n) != 0 || n != block.size()) return false;
  out->assign(block.begin() + res.offset, block.begin() + res.offset + res.size);
  return true;
}

/// What the game read for one of our resources: our bytes (the set won), or another copy (stock won).
enum class ReadVerdict { Ours, Stock };
inline ReadVerdict Judge(const void* read, std::uint64_t readSize, const Bytes& ours) {
  return readSize == ours.size() && std::memcmp(read, ours.data(), ours.size()) == 0 ? ReadVerdict::Ours
                                                                                       : ReadVerdict::Stock;
}

}  // namespace evr_content
