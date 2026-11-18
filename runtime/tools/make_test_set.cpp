// Dev tool: writes a package set (set id Symbol64("evr_content_test")) for testing a release's own sets in game.
//   evr_make_test_set <data root>
//       the UPDATE card's default texture ui_mnu_news (86300ac728482baf) as a solid magenta 512 x 256 B8G8R8A8 image
//       (descriptor + DDS payload). Nothing from the game's data.
//   evr_make_test_set <data root> <name hex>=<payload.dds> [<name hex>=<payload.dds> ...]
//       those files as texture payloads (no descriptors: payloads in their stock size and format).
// Writes <data root>\rad15\win10\manifests\<set> and ...\packages\<set>_0.
// The card shows two textures: ui_mnu_news (86300ac728482baf) first, then, once Atlas's main_menu config arrives,
// the one it names (stock config: ui_mnu_news_latest, 4bc2590a55dc5b9e) through DynLoadTexture.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "content/content_sets.h"
#include "friends/friends_package.h"

namespace fp = nevr_friends_package;

namespace {

bool ReadFile(const char* path, fp::Bytes* out) {
  FILE* in = std::fopen(path, "rb");
  if (in == nullptr) return false;
  std::uint8_t buf[65536];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) out->insert(out->end(), buf, buf + n);
  std::fclose(in);
  return !out->empty();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: evr_make_test_set <data root> [<name hex>=<payload.dds> ...]\n");
    return 2;
  }
  constexpr std::uint64_t kCard = 0x86300AC728482BAFULL;  // ui_mnu_news
  constexpr std::uint64_t kSet = evr::Symbol64("evr_content_test");
  std::vector<fp::Resource> res;
  if (argc == 2) {
    constexpr std::uint32_t w = 512, h = 256;
    fp::Bytes pixels(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
      pixels[i] = 0xFF;      // B
      pixels[i + 1] = 0x00;  // G
      pixels[i + 2] = 0xFF;  // R
      pixels[i + 3] = 0xFF;  // A
    }
    fp::Bytes payload = fp::Dds(w, h, pixels);
    res.push_back({fp::kTextureDescriptorType, kCard,
                   fp::TextureDescriptor(w, h, static_cast<std::uint32_t>(payload.size()))});
    res.push_back({fp::kTexturePayloadType, kCard, std::move(payload)});
  }
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    const std::size_t eq = arg.find('=');
    std::uint64_t name = 0;
    fp::Bytes bytes;
    if (eq == std::string::npos || !evr_content::ParseSetName(arg.substr(0, eq), &name) ||
        !ReadFile(arg.c_str() + eq + 1, &bytes)) {
      std::fprintf(stderr, "bad argument %s (want <16 hex digits>=<readable file>)\n", arg.c_str());
      return 2;
    }
    res.push_back({fp::kTexturePayloadType, name, std::move(bytes)});
  }
  fp::PackageSet set;
  if (!fp::BuildPackageSet(std::move(res), &set)) {
    std::fprintf(stderr, "build failed\n");
    return 1;
  }
  const std::filesystem::path root = std::filesystem::path(argv[1]) / "rad15" / "win10";
  std::filesystem::create_directories(root / "manifests");
  std::filesystem::create_directories(root / "packages");
  const std::pair<std::filesystem::path, const fp::Bytes*> files[] = {
      {root / "manifests" / evr_content::SetFileName(kSet), &set.manifest},
      {root / "packages" / evr_content::PackageFileName(kSet, 0), &set.package}};
  for (const auto& f : files) {
    FILE* out = std::fopen(f.first.string().c_str(), "wb");
    if (out == nullptr || std::fwrite(f.second->data(), 1, f.second->size(), out) != f.second->size()) {
      std::fprintf(stderr, "cannot write %s\n", f.first.string().c_str());
      return 1;
    }
    std::fclose(out);
    std::printf("%s (%zu bytes)\n", f.first.string().c_str(), f.second->size());
  }
  return 0;
}
