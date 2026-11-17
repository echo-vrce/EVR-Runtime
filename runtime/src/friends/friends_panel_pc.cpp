// friends_panel_pc.cpp: the FRIENDS panel (#1535) on PC: the platform functions the shared logic calls
// (friends_panel_platform.h) and the avatars' behaviour capture. The pictures set's AddPackage is friends_install_pc.cpp.

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "evr/core.h"
#include "evr/symbol.h"
#include "friends/evr_compat.h"
#include "friends/friends_panel_platform.h"
#include "friends/menu_friends_avatars.h"
#include "social/social.h"

namespace nevr_friends_pc {
namespace {

// ---- the avatars ----

// Make(CGTextureResource, "Win10GPU"): a texture's payload, buffer 1 (resource_override.cpp TYPE_DDS_TEXTURE).
constexpr std::uint64_t kTextureGpuType = 0xBEAC1969CB7B8861ULL;

// CR15DynLoadTextureBehavior::SetTexture(behaviour, CSymbol64 texture) (echovr.exe 0x140c42a20): unloads the current
// texture, stores the name at +0x68 and, through the customization system at +0x60, loads it ("[DynLoadTexture] '%s'
// Loading texture '%s'"). The element name is the symbol at *(behaviour + 0x50).
constexpr std::uint64_t kSetTextureVA = 0x140C42A20;
constexpr std::array<std::uint8_t, 16> kSetTexturePrologue = {0x48, 0x89, 0x54, 0x24, 0x10, 0x56, 0x48, 0x83,
                                                               0xEC, 0x30, 0x48, 0x8B, 0xF1, 0x48, 0x8B, 0x49};
constexpr std::size_t kBehaviourElement = 0x50;
using SetTextureFn = void (*)(void* behaviour, std::uint64_t texture);
// CR15DynLoadTextureBehavior's initialization (echovr.exe 0x140c3ebd0, "[DynLoadTexture] Behavior initialized for
// element '%s', texturename '%s'"): RCX the behaviour, four more arguments (the fifth on the stack, the element record it
// stores at +0x50); it stores the customization system SetTexture uses at +0x60. Returns 0 on success. It does not call
// SetTexture for the default texture, so the behaviour is captured here.
constexpr std::uint64_t kInitVA = 0x140C3EBD0;
constexpr std::array<std::uint8_t, 16> kInitPrologue = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                                        0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48};
using InitFn = std::uint64_t (*)(void* behaviour, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                 std::uint64_t);

// The DDS header of the stock 128x128 B8G8R8A8 texture the slots are made from (84b9a06506af66fe): DX10, DXGI 87.
constexpr std::array<std::uint8_t, 148> kDdsHeader = {
    0x44, 0x44, 0x53, 0x20, 0x7C, 0x00, 0x00, 0x00, 0x0F, 0x10, 0x02, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x20, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x44, 0x58, 0x31, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x57, 0x00, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr int kSide = nevr_friends_platform::kPictureSide;

// The game keeps every CR15DynLoadTextureBehavior in one array of 0x78-byte entries and moves them: a canvas that becomes
// active puts its own in, and the ones after them shift (seen in game: two frames after the friend profile pop-up first
// opened, the entry captured for row 2 held row 0's texture). An address captured at initialization is a hint, never the
// behaviour: before each SetTexture the hint is checked by its element's name and, when it no longer matches, the
// entries around it are searched.
constexpr std::size_t kBehaviourStride = 0x78;
constexpr std::size_t kBehaviourCustomization = 0x60;
constexpr int kSearchEntries = 512;

struct Captured {
  std::uint64_t element = 0;
  void* hint = nullptr;
  std::uint64_t generation = 0;  // counts captures: each is a new canvas instance of the element
};

SetTextureFn g_setTexture = nullptr;
InitFn g_originalInit = nullptr;
std::atomic<bool> g_picturesReady{false};
std::mutex g_captureMutex;
std::vector<Captured> g_captured;
std::deque<std::shared_ptr<const std::vector<std::uint8_t>>> g_keep;  // every DDS served stays alive

bool HoldsElement(const std::uint8_t* b, std::uint64_t element) {
  if (!evr::IsReadable(b, kBehaviourStride)) return false;
  void* record = nullptr;
  std::uint64_t customization = 0, name = 0;
  std::memcpy(&record, b + kBehaviourElement, sizeof(record));
  std::memcpy(&customization, b + kBehaviourCustomization, sizeof(customization));
  if (record == nullptr || customization == 0 || !evr::IsReadable(record, sizeof(name))) return false;
  std::memcpy(&name, record, sizeof(name));
  return name == element;
}

// The behaviour of `element` near `hint`, or nullptr (reads game memory only).
void* Resolve(void* hint, std::uint64_t element) {
  if (hint == nullptr) return nullptr;
  const auto* at = static_cast<const std::uint8_t*>(hint);
  if (HoldsElement(at, element)) return hint;
  for (int d = 1; d <= kSearchEntries; ++d) {
    for (const int sign : {1, -1}) {
      const auto* probe = at + static_cast<std::ptrdiff_t>(sign * d) * static_cast<std::ptrdiff_t>(kBehaviourStride);
      if (HoldsElement(probe, element)) return const_cast<std::uint8_t*>(probe);
    }
  }
  return nullptr;
}

void Capture(void* behaviour) {
  std::uint64_t element = 0;
  void* const* field = reinterpret_cast<void* const*>(static_cast<std::uint8_t*>(behaviour) + kBehaviourElement);
  if (!evr::IsReadable(field, sizeof(*field)) || *field == nullptr ||
      !evr::IsReadable(*field, sizeof(element))) {
    return;
  }
  std::memcpy(&element, *field, sizeof(element));
  std::lock_guard<std::mutex> lock(g_captureMutex);
  for (Captured& c : g_captured) {
    if (c.element != element) continue;
    if (c.hint != behaviour) {
      c.hint = behaviour;
      ++c.generation;
    }
    return;
  }
  g_captured.push_back({element, behaviour, 1});
}

std::uint64_t InitHook(void* behaviour, std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d,
                       std::uint64_t e) {
  const std::uint64_t result = g_originalInit(behaviour, a, b, c, d, e);
  Capture(behaviour);  // the element and the customization system are stored before any failure return
  return result;
}

}  // namespace
}  // namespace nevr_friends_pc

namespace nevr_friends_platform {

void Log(Level level, const char* format, ...) {
  char line[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  ::Log(level == Level::Warning ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info, "%s", line);
}

std::uint64_t NowMs() { return GetTickCount64(); }

bool Read(std::uint64_t address, void* out, std::size_t length) {
  const auto* at = reinterpret_cast<const void*>(address);
  if (!evr::IsReadable(at, length)) return false;
  std::memcpy(out, at, length);
  return true;
}

bool RewriteWord(std::uint64_t address, std::uint64_t value) {
  auto* at = reinterpret_cast<void*>(address);
  if (!evr::IsReadable(at, sizeof(value))) return false;
  std::memcpy(at, &value, sizeof(value));
  return true;
}

HttpReply Http(const char* method, const std::string& url, const std::string& bearer, const std::string& payload) {
  const evr::HttpReply r = evr::Http(method, url, bearer, payload);
  HttpReply reply;
  reply.ok = r.ok;
  reply.status = r.status;
  reply.body = r.body;
  return reply;
}

std::string ServiceBase() { return evr_social::ServiceBase(); }

std::string Token() { return evr_social::Token(); }
Picture FindPicture(std::uint64_t /*actor*/, std::uint64_t element) {
  using namespace nevr_friends_pc;
  std::lock_guard<std::mutex> lock(g_captureMutex);
  for (Captured& c : g_captured) {
    if (c.element != element) continue;
    void* behaviour = Resolve(c.hint, element);
    if (behaviour != nullptr) c.hint = behaviour;
    return {behaviour, c.generation};
  }
  return {};
}

void ApplyTexture(void* behaviour, std::uint64_t texture) {
  if (nevr_friends_pc::g_setTexture != nullptr && behaviour != nullptr) nevr_friends_pc::g_setTexture(behaviour, texture);
}

void ServePicture(std::uint64_t name, const std::vector<std::uint8_t>& rgba, const std::string& label) {
  using namespace nevr_friends_pc;
  if (rgba.size() != static_cast<std::size_t>(kSide) * kSide * 4) return;
  auto dds = std::make_shared<std::vector<std::uint8_t>>(kDdsHeader.size() + rgba.size());
  std::memcpy(dds->data(), kDdsHeader.data(), kDdsHeader.size());
  std::uint8_t* out = dds->data() + kDdsHeader.size();
  for (std::size_t i = 0; i < rgba.size(); i += 4) {  // RGBA to the slot's B, G, R, A
    out[i] = rgba[i + 2];
    out[i + 1] = rgba[i + 1];
    out[i + 2] = rgba[i];
    out[i + 3] = rgba[i + 3];
  }
  g_keep.push_back(dds);
  // The payload (buffer 1) is served by a one-shot override written over the game's own buffer, same size; a transform
  // is offered buffer 0 only. g_keep holds the bytes for the rest of the process.
  RegisterResourceOverride(kTextureGpuType, name, dds->data(), dds->size(), label.c_str());
}

bool PicturesReady() { return nevr_friends_pc::g_picturesReady.load(); }

}  // namespace nevr_friends_platform

namespace nevr_menu_friends_avatars {

void Install(std::uintptr_t /*gameBase*/) {
  using namespace nevr_friends_pc;
  void* setTexture = evr::ResolveVA(kSetTextureVA);
  void* init = evr::ResolveVA(kInitVA);
  const bool setOk = evr::CheckBytes(setTexture, kSetTexturePrologue.data(), kSetTexturePrologue.size());
  const bool initOk = evr::CheckBytes(init, kInitPrologue.data(), kInitPrologue.size());
  if (!setOk || !initOk) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.MENU] friends panel avatars off reason=prologue_mismatch set_texture=%s init=%s",
        setOk ? "ok" : "mismatch", initOk ? "ok" : "mismatch");
    return;
  }
  g_setTexture = reinterpret_cast<SetTextureFn>(setTexture);
  g_originalInit = reinterpret_cast<InitFn>(init);
  if (evr::Hook(init, reinterpret_cast<void*>(&InitHook), reinterpret_cast<void**>(&g_originalInit),
                "CR15DynLoadTextureBehavior::Initialize")) {
    g_picturesReady.store(true);
    Log(EchoVR::LogLevel::Info, "[NEVR.MENU] friends panel avatars on: behaviour init hook installed va=0x%llx",
        static_cast<unsigned long long>(kInitVA));
  }
}

}  // namespace nevr_menu_friends_avatars
