// Dev/release tool: builds a release package set from loose game resources.
//   evr_make_set <data root> <set name> <tree dir> <list file>
// <tree dir>\<type hex>\<name hex> holds each resource's bytes (the layout of an extracted/staged package tree, e.g.
// NewCosmetics out\input-pcvr). <list file>: one "<type hex> <name hex>" per line (blank lines and # comments skipped).
// The set id is Symbol64(<set name>), e.g. evr_newcosmetics_chassis. Writes <data root>\rad15\win10\manifests\<set>
// and ...\packages\<set>_0; a release carries them as pc/data/rad15/win10/... (content_pc.cpp mounts them).
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "content/content_sets.h"
#include "evr/symbol.h"
#include "friends/friends_package.h"

namespace fp = nevr_friends_package;

namespace {

bool ReadFile(const std::filesystem::path& path, fp::Bytes* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;  // empty files are resources too (zero-byte payloads in the game's own packages)
}

bool WriteFile(const std::filesystem::path& path, const fp::Bytes& b) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  return static_cast<bool>(out);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 5) {
    std::fprintf(stderr, "usage: evr_make_set <data root> <set name> <tree dir> <list file>\n");
    return 2;
  }
  const std::uint64_t set = evr::Symbol64(argv[2]);
  const std::filesystem::path tree = argv[3];
  std::ifstream list(argv[4]);
  if (!list) {
    std::fprintf(stderr, "cannot read %s\n", argv[4]);
    return 2;
  }
  std::vector<fp::Resource> res;
  std::uint64_t total = 0;
  std::size_t empty = 0;
  std::string line;
  while (std::getline(list, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ss(line);
    std::string t, n;
    std::uint64_t type = 0, name = 0;
    if (!(ss >> t >> n) || !evr_content::ParseSetName(t, &type) || !evr_content::ParseSetName(n, &name)) {
      std::fprintf(stderr, "bad line: %s\n", line.c_str());
      return 2;
    }
    fp::Resource r{type, name, {}};
    if (!ReadFile(tree / t / n, &r.bytes)) {
      std::fprintf(stderr, "cannot read %s\n", (tree / t / n).string().c_str());
      return 1;
    }
    total += r.bytes.size();
    if (r.bytes.empty()) ++empty;
    res.push_back(std::move(r));
  }
  const std::size_t count = res.size();
  fp::PackageSet out;
  if (!fp::BuildPackageSet(std::move(res), &out, /*allow_empty=*/true)) {
    std::fprintf(stderr, "build failed (no resources, or a repeated one)\n");
    return 1;
  }
  const std::filesystem::path root = std::filesystem::path(argv[1]) / "rad15" / "win10";
  std::filesystem::create_directories(root / "manifests");
  std::filesystem::create_directories(root / "packages");
  const auto m = root / "manifests" / evr_content::SetFileName(set);
  const auto p = root / "packages" / evr_content::PackageFileName(set, 0);
  if (!WriteFile(m, out.manifest) || !WriteFile(p, out.package)) {
    std::fprintf(stderr, "cannot write the set\n");
    return 1;
  }
  std::printf("set %s = %016llx: %zu resources (%zu empty), %llu bytes raw\n  %s (%zu bytes)\n  %s (%zu bytes)\n", argv[2],
              (unsigned long long)set, count, empty, (unsigned long long)total, m.string().c_str(), out.manifest.size(),
              p.string().c_str(), out.package.size());
  return 0;
}
