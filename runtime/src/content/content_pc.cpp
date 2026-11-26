// The release's own package sets on PC (content.h, content_sets.h).
//
// The active version folder is the folder evr-runtime.dll was loaded from (plugins\versions\<ver>\). Its sets sit
// under <version dir>\data\rad15\win10\{manifests,packages}; stock _data is never touched, so a rollback (another
// version folder) leaves no trace of them. Each set is mounted with the engine's AddPackage (evr/packages.h); every
// resource it holds also gets a resource transform that logs, on the first read, whether the game got our bytes or a
// stock copy, and (unless "content_override": false) serves ours in the second case.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "content/content.h"
#include "content/content_sets.h"
#include "evr/core.h"
#include "evr/packages.h"
#include "evr/resources.h"

namespace evr_content {
namespace {

struct Ours {
  std::uint64_t set, type, name;
  std::size_t index;  // in the set's manifest
  std::shared_ptr<const Manifest> manifest;
  std::string package_path;
  std::string label;
  std::mutex mutex;
  bool loaded = false, failed = false, judged = false, won = false;
  Bytes bytes;  // ours, held only while the game may still read a stock copy
};

Options g_options;
std::string g_dataRoot;
std::vector<std::uint64_t> g_sets;
std::deque<Ours> g_ours;  // stable addresses: each is a transform's `user`

bool ReadAll(const std::string& path, Bytes* out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  out->clear();
  std::uint8_t buf[65536];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out->insert(out->end(), buf, buf + n);
  std::fclose(f);
  return true;
}

std::string VersionDir() {
  HMODULE self = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(&VersionDir), &self))
    return {};
  char p[MAX_PATH]{};
  const DWORD n = GetModuleFileNameA(self, p, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  std::string s(p, n);
  return s.substr(0, s.find_last_of("\\/"));
}

std::vector<std::string> ListFiles(const std::string& dir) {
  std::vector<std::string> names;
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return names;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.emplace_back(fd.cFileName);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  return names;
}

// Reads only the compressed block that holds resource `index`.
bool ReadResource(const std::string& path, const Manifest& m, std::size_t index, Bytes* out) {
  const BlockRecord& b = m.blocks[m.resources[index].block];
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  Bytes block(b.compressed);
  const bool ok = _fseeki64(f, b.offset, SEEK_SET) == 0 && std::fread(block.data(), 1, block.size(), f) == block.size();
  std::fclose(f);
  return ok && ExtractResourceFromBlock(m, index, block, out);
}

// Resource-read thread(s): our bytes on first use, the verdict once, ours served when stock was read. Once the game
// is seen reading our own copy the set wins for good: our bytes are dropped and later reads pass straight through.
bool Transform(const void* original, std::uint64_t size, const void** out, std::uint64_t* outSize, void* user) {
  Ours& o = *static_cast<Ours*>(user);
  std::lock_guard<std::mutex> lock(o.mutex);
  if (o.won) return false;
  if (!o.loaded && !o.failed) {
    o.failed = !ReadResource(o.package_path, *o.manifest, o.index, &o.bytes);
    o.loaded = !o.failed;
    if (o.failed)
      evr::Log(evr::LogLevel::Warning, "[EVR.CONTENT] %s: could not read our copy from %s", o.label.c_str(),
               o.package_path.c_str());
  }
  if (o.failed) return false;
  const ReadVerdict v = Judge(original, size, o.bytes);
  const bool serve = v == ReadVerdict::Stock && g_options.override_stock;
  if (!o.judged) {
    o.judged = true;
    evr::Log(evr::LogLevel::Info, "[EVR.CONTENT] %s read: game got %s (read %llu bytes, ours %zu)%s", o.label.c_str(),
             v == ReadVerdict::Ours ? "OUR copy (set wins)" : "ANOTHER copy (stock wins)", (unsigned long long)size,
             o.bytes.size(), serve ? "; serving ours" : "");
  }
  if (v == ReadVerdict::Ours) {
    o.won = true;
    Bytes().swap(o.bytes);
  }
  if (!serve) return false;
  *out = o.bytes.data();
  *outSize = o.bytes.size();
  return true;
}

void Mount(void* manager, const char* when) {
  for (std::uint64_t set : g_sets) {
    const std::uint32_t r = evr::AddPackage(manager, set, g_dataRoot.c_str());
    evr::Log(r == 0 ? evr::LogLevel::Info : evr::LogLevel::Warning, "[EVR.CONTENT] set 0x%016llx %s root=%s when=%s result=%u",
             (unsigned long long)set, r == 0 ? "added" : "NOT added", g_dataRoot.c_str(), when, r);
  }
}

// Reads one set's manifest and registers a transform per resource. False when the manifest is unreadable.
bool Register(std::uint64_t set, const std::string& win10) {
  Bytes file;
  auto manifest = std::make_shared<Manifest>();
  const std::string path = win10 + "\\manifests\\" + SetFileName(set);
  if (!ReadAll(path, &file) || !ParseManifest(file, manifest.get())) {
    evr::Log(evr::LogLevel::Warning, "[EVR.CONTENT] set 0x%016llx skipped: manifest %s unreadable", (unsigned long long)set,
             path.c_str());
    return false;
  }
  for (std::uint32_t p = 0; p < manifest->packages; ++p) {
    const std::string pkg = win10 + "\\packages\\" + PackageFileName(set, p);
    if (GetFileAttributesA(pkg.c_str()) == INVALID_FILE_ATTRIBUTES) {
      evr::Log(evr::LogLevel::Warning, "[EVR.CONTENT] set 0x%016llx skipped: package %s missing", (unsigned long long)set,
               pkg.c_str());
      return false;
    }
  }
  for (std::size_t i = 0; i < manifest->resources.size(); ++i) {
    const ResourceRecord& r = manifest->resources[i];
    Ours& o = g_ours.emplace_back();
    o.set = set;
    o.type = r.type;
    o.name = r.name;
    o.index = i;
    o.manifest = manifest;
    o.package_path = win10 + "\\packages\\" + PackageFileName(set, manifest->blocks[r.block].package);
    char label[96];
    std::snprintf(label, sizeof(label), "content %016llx/%016llx", (unsigned long long)r.type, (unsigned long long)r.name);
    o.label = label;
    evr::RegisterResourceTransform(r.type, r.name, &Transform, &o, o.label.c_str());
  }
  evr::Log(evr::LogLevel::Info, "[EVR.CONTENT] set 0x%016llx: %zu resources in %u package(s)", (unsigned long long)set,
           manifest->resources.size(), manifest->packages);
  return true;
}

// ---- dev probe ("content_probe": true): what the texture deserialiser does with our names ----
// CGTextureResource vtable 0x141c2ff48 (RTTI), slot +0x38 = deserialise(resource, streams) at 0x1405491c0. It returns
// at once when [resource+0x78] (its texture) is already set. Resource: +0x38 name, +0x40/+0x48 buffers, +0x50/+0x58 sizes.
constexpr std::uint64_t kTextureDeserializeVA = 0x1405491C0;
constexpr std::uint8_t kTextureDeserializePrologue[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C,
                                                        0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56};
using DeserializeFn = std::uint32_t (*)(std::uint8_t* resource, void* streams);
DeserializeFn g_deserialize = nullptr;

bool IsOurName(std::uint64_t name) {
  for (const Ours& o : g_ours)
    if (o.name == name) return true;
  return false;
}

std::uint64_t At(const std::uint8_t* p, std::size_t off) {
  std::uint64_t v = 0;
  if (evr::IsReadable(p + off, 8)) std::memcpy(&v, p + off, 8);
  return v;
}

std::uint32_t DeserializeProbe(std::uint8_t* resource, void* streams) {
  const std::uint64_t name = At(resource, 0x38);
  if (!IsOurName(name)) return g_deserialize(resource, streams);
  const std::uint64_t textureBefore = At(resource, 0x78);
  const auto* payload = reinterpret_cast<const std::uint8_t*>(At(resource, 0x48));
  const std::uint64_t firstBlock = payload != nullptr ? At(payload, 148) : 0;
  const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)) - evr::GameBase() + evr::kImageBase;
  const std::uint32_t r = g_deserialize(resource, streams);
  evr::Log(evr::LogLevel::Info,
           "[EVR.CONTENT] probe: texture %016llx deserialise resource=%p texture_before=%llx after=%llx result=%u "
           "buf0=%llx/%llu buf1=%llx/%llu payload_block0=%016llx caller=0x%llx",
           (unsigned long long)name, (void*)resource, (unsigned long long)textureBefore,
           (unsigned long long)At(resource, 0x78), r, (unsigned long long)At(resource, 0x40),
           (unsigned long long)At(resource, 0x50), (unsigned long long)At(resource, 0x48),
           (unsigned long long)At(resource, 0x58), (unsigned long long)firstBlock, (unsigned long long)caller);
  return r;
}

}  // namespace

bool SetMounted(std::uint64_t set) {
  for (std::uint64_t s : g_sets)
    if (s == set) return true;
  return false;
}

void Install(const Options& options) {
  g_options = options;
  const std::string version = VersionDir();
  if (version.empty()) {
    evr::Log(evr::LogLevel::Warning, "[EVR.CONTENT] version folder unknown: no release sets");
    return;
  }
  g_dataRoot = version + "\\data";
  const std::string win10 = g_dataRoot + "\\rad15\\win10";
  for (std::uint64_t set : SetIds(ListFiles(win10 + "\\manifests")))
    if (Register(set, win10)) g_sets.push_back(set);
  if (g_sets.empty()) {
    evr::Log(evr::LogLevel::Info, "[EVR.CONTENT] no release sets under %s", win10.c_str());
    return;
  }
  if (options.probe)
    evr::HookVA(kTextureDeserializeVA, kTextureDeserializePrologue, reinterpret_cast<void*>(&DeserializeProbe),
                reinterpret_cast<void**>(&g_deserialize), "CGTextureResource deserialise (content probe)");
  const bool hooked = options.before_stock ? evr::OnBeforeStockPackages(&Mount) : evr::OnStockPackagesAdded(&Mount);
  evr::Log(hooked ? evr::LogLevel::Info : evr::LogLevel::Warning, "[EVR.CONTENT] %zu set(s) to mount %s stock sets%s",
           g_sets.size(), options.before_stock ? "before" : "after", hooked ? "" : ": AddPackage unavailable, NOT mounted");
}

}  // namespace evr_content
