// Release-shipped cosmetics and the Story rarity on PC (cosmetics.h, cosmetics_items.h).
//
// Story rarity: a port of EchoBits&Bobs StoryRarity (echovr.exe 631547.rad15_live, found with revault):
//   0x140c46650 CR15ItemRarityFrameBehavior::SetItemRarity(this, rarity): a Story item's frame shows its Common
//               sprite (this+0x58 sprite table, +0x08; white) tinted #FC0087 (0x140727020 SetColor), else the game's.
//   0x140c46850 CR15SelectedFrameBehavior::SetRarity(this, a2): the same for the selected frame (Common at +0x28),
//               the rarity read from the item a2 resolves to (0x140ce0bd0; obj vtable +0x1c8 = valid(index)).
//   0x140f50d40 the rarity label (this, char out[64], item): "Story".
//   0x140f1b7e0 AssignmentStr (this, char out[64], item): the line under the name. A CDN item's spec "source" first
//               (SetSourceLines), else a Story item's "Lone Echo Story Character" / "... Item" (booster).
//   Helpers: 0x14071c820 element visible(canvas, id, on), 0x140727ce0 element sprite(canvas, id, sprite).
//   The item card's script (bin\win10\scripts\f762837db44bc4ea.dll, loaded by the game later): its rarity getter
//   (+0x1f940) notes the rarity per thread; its command send (+0x32a0) turns the default grey text (0.855 x3) pink.
// The plugin used SEH; this runtime is built with MinGW, so every pointer it follows is checked with IsReadable.
// If the StoryRarity plugin is also loaded its detours change the prologues: our checks fail and we stay off.
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
#include <string>

#include "cdn/cdn_catalog.h"
#include "content/content.h"
#include "cosmetics/cosmetics.h"
#include "cosmetics/cosmetics_items.h"
#include "evr/core.h"
#include "evr/resources.h"

namespace evr_cosmetics {
namespace {

using evr::Log;
using evr::LogLevel;

template <typename T>
bool ReadAt(const void* at, T* out) {
  if (at == nullptr || !evr::IsReadable(at, sizeof(T))) return false;
  std::memcpy(out, at, sizeof(T));
  return true;
}
const std::uint8_t* Ptr(const void* at) {
  const std::uint8_t* p = nullptr;
  return ReadAt(at, &p) ? p : nullptr;
}

// An item record's u64 field: item -> u16 index table -> u16 (stride 0x60) -> 664-byte record.
bool FieldAt(const std::uint8_t* index, const std::uint8_t* mid, const std::uint8_t* records, std::uint16_t item,
             std::size_t field, std::uint64_t* out) {
  std::uint16_t a = 0, b = 0;
  return index != nullptr && mid != nullptr && records != nullptr &&
         ReadAt(index + static_cast<std::uintptr_t>(item) * 4, &a) &&
         ReadAt(mid + static_cast<std::uintptr_t>(a) * 0x60, &b) &&
         ReadAt(records + static_cast<std::uintptr_t>(b) * evr_cdn::kEntryBytes + field, out);
}
// The same from a booth list object (label / AssignmentStr `self`).
bool BoothField(const std::uint8_t* self, std::uint16_t item, std::size_t field, std::uint64_t* out) {
  const std::uint8_t* recordsAt = Ptr(self + 0xc0);
  return FieldAt(Ptr(self + 0xd0), Ptr(self + 0x100), Ptr(recordsAt), item, field, out);
}

// ---- the chassis records ----

std::mutex g_listMutex;
std::shared_ptr<const std::string> g_list;  // the last list served (kept alive while the game may read it)

bool AppendChassis(const void* original, std::uint64_t size, const void** out, std::uint64_t* outSize, void*) {
  std::string patched;
  std::vector<std::string> skipped;
  if (!evr_cdn::AppendCosmeticItems(std::string(static_cast<const char*>(original), size), ChassisItems(), &patched,
                                    &skipped)) {
    Log(LogLevel::Warning, "[EVR.COSMETICS] item list not patched: unexpected layout (%llu bytes)",
        (unsigned long long)size);
    return false;
  }
  for (const std::string& n : skipped)
    Log(LogLevel::Warning, "[EVR.COSMETICS] %s skipped (name exists or template missing)", n.c_str());
  std::lock_guard<std::mutex> lock(g_listMutex);
  static std::vector<std::shared_ptr<const std::string>> retired;
  if (g_list) retired.push_back(g_list);
  g_list = std::make_shared<const std::string>(std::move(patched));
  *out = g_list->data();
  *outSize = g_list->size();
  Log(LogLevel::Info, "[EVR.COSMETICS] item list: %zu release chassis added", ChassisItems().size() - skipped.size());
  return true;
}

// ---- Story rarity ----

constexpr std::uint64_t kSetItemRarityVA = 0x140C46650, kSetSelectedRarityVA = 0x140C46850,
                        kRarityLabelVA = 0x140F50D40, kElementVisibleVA = 0x14071C820,
                        kElementSpriteVA = 0x140727CE0, kResolveItemVA = 0x140CE0BD0, kAssignmentStrVA = 0x140F1B7E0,
                        kElementColorVA = 0x140727020;  // CUICanvasInstance::SetColor(canvas, id, const float rgb[3])
constexpr std::uint8_t kElementColorPrologue[16] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x89, 0x54, 0x24,
                                                    0x10, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b};
constexpr std::uint8_t kSetItemRarityPrologue[16] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74,
                                                     0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48};
constexpr std::uint8_t kSetSelectedRarityPrologue[16] = {0x40, 0x56, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8b,
                                                         0xf1, 0x48, 0x8d, 0x4c, 0x24, 0x30, 0xe8, 0x6d};
constexpr std::uint8_t kRarityLabelPrologue[16] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
                                                   0x81, 0xd0, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xda};
constexpr std::uint8_t kElementVisiblePrologue[16] = {0x89, 0x54, 0x24, 0x10, 0x57, 0x41, 0x57, 0x48,
                                                      0x83, 0xec, 0x38, 0x48, 0x8b, 0x81, 0xc0, 0x00};
constexpr std::uint8_t kElementSpritePrologue[16] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x89, 0x54, 0x24,
                                                     0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b};
constexpr std::uint8_t kResolveItemPrologue[16] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x53, 0x48, 0x83,
                                                   0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x83, 0xfa};
constexpr std::uint8_t kAssignmentStrPrologue[16] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c,
                                                     0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};

using SetItemRarityFn = void (*)(std::uint8_t* self, std::uint64_t rarity);
using SetSelectedRarityFn = void (*)(std::uint8_t* self, void* a2);
using TextFn = char* (*)(std::uint8_t* self, char* out, std::uint16_t item);  // label and AssignmentStr
using ElementVisibleFn = void (*)(void* canvas, std::uint32_t id, int on);
using ElementSpriteFn = void (*)(void* canvas, std::uint32_t id, std::uint64_t sprite);
using ElementColorFn = int (*)(void* canvas, std::uint32_t id, const float* rgb);
struct ItemRef {
  std::uint8_t* obj;
  std::uint32_t index, pad;
};
using ResolveItemFn = void (*)(ItemRef* out, void* a2);
using ValidFn = int (*)(std::uint8_t* obj, std::uint32_t index);

SetItemRarityFn g_setItemRarity = nullptr;
SetSelectedRarityFn g_setSelectedRarity = nullptr;
TextFn g_rarityLabel = nullptr, g_assignmentStr = nullptr;
ElementVisibleFn g_elementVisible = nullptr;
ElementSpriteFn g_elementSprite = nullptr;
ElementColorFn g_elementColor = nullptr;
ResolveItemFn g_resolveItem = nullptr;

std::mutex g_sourceMutex;
std::map<std::uint64_t, std::string> g_sources;  // CDN items: internal name symbol -> line

void LogOnce(volatile LONG* flag, const char* what) {
  if (InterlockedExchange(flag, 1) == 0) Log(LogLevel::Info, "[EVR.COSMETICS] %s", what);
}

// The canvas element a frame behaviour draws, and its sprite table's Common sprite at `common`.
bool Frame(std::uint8_t* self, std::size_t common, void** canvas, std::uint32_t* id, std::uint64_t* sprite) {
  const std::uint8_t* c = Ptr(self + 0x48);
  const std::uint8_t* element = Ptr(self + 0x50);
  const std::uint8_t* sprites = Ptr(self + 0x58);
  if (c == nullptr || element == nullptr || sprites == nullptr || !ReadAt(element + 0x60, id) ||
      !ReadAt(sprites + common, sprite))
    return false;
  *canvas = const_cast<std::uint8_t*>(c);
  return true;
}

// ---- tinting: a Story frame is the white Common sprite coloured pink. The colour stays on the element, so a tinted
// element's own colour is saved once and put back when it next shows another rarity.
// CUICanvasInstance: elements at +0x90 (0xe0 each), count at +0xc0; element types 1-3 keep rgb at +0x78.
bool ElementRgb(void* canvas, std::uint32_t id, float rgb[3]) {
  const auto* c = static_cast<const std::uint8_t*>(canvas);
  std::uint32_t count = 0;
  const std::uint8_t* elements = Ptr(c + 0x90);
  int type = 0;
  if (!ReadAt(c + 0xc0, &count) || id >= count || elements == nullptr) return false;
  const std::uint8_t* e = elements + static_cast<std::uintptr_t>(id) * 0xe0;
  if (!ReadAt(e + 8, &type) || type < 1 || type > 3 || !evr::IsReadable(e + 0x78, 12)) return false;
  std::memcpy(rgb, e + 0x78, 12);
  return true;
}

struct Tinted {
  void* canvas;
  std::uint32_t id;
  float rgb[3];
};
std::mutex g_tintMutex;
std::vector<Tinted> g_tinted;

void Tint(void* canvas, std::uint32_t id) {
  {
    std::lock_guard<std::mutex> lock(g_tintMutex);
    bool known = false;
    for (const Tinted& t : g_tinted) known = known || (t.canvas == canvas && t.id == id);
    Tinted t{canvas, id, {}};
    if (!known && g_tinted.size() < 256 && ElementRgb(canvas, id, t.rgb)) g_tinted.push_back(t);
  }
  g_elementColor(canvas, id, kStoryFrameColour);
}

void Untint(void* canvas, std::uint32_t id) {
  float rgb[3];
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(g_tintMutex);
    for (std::size_t i = 0; i < g_tinted.size(); ++i) {
      if (g_tinted[i].canvas != canvas || g_tinted[i].id != id) continue;
      std::memcpy(rgb, g_tinted[i].rgb, sizeof(rgb));
      g_tinted[i] = g_tinted.back();
      g_tinted.pop_back();
      found = true;
      break;
    }
  }
  if (found) g_elementColor(canvas, id, rgb);
}

void SetItemRarityHook(std::uint8_t* self, std::uint64_t rarity) {
  void* canvas = nullptr;
  std::uint32_t id = 0;
  std::uint64_t common = 0;
  if (rarity != kStoryRarity || !evr::IsWritable(self + 0x60, 8) || !Frame(self, 0x08, &canvas, &id, &common)) {
    g_setItemRarity(self, rarity);
    if (Frame(self, 0x08, &canvas, &id, &common)) Untint(canvas, id);
    return;
  }
  std::memcpy(self + 0x60, &rarity, 8);
  g_elementVisible(canvas, id, 1);
  g_elementSprite(canvas, id, common);
  Tint(canvas, id);
  static volatile LONG once = 0;
  LogOnce(&once, "item frame: Story");
}

void SetSelectedRarityHook(std::uint8_t* self, void* a2) {
  ItemRef ref{};
  g_resolveItem(&ref, a2);
  std::uint64_t rarity = 0;
  const std::uint8_t* vtable = ref.obj != nullptr ? Ptr(ref.obj) : nullptr;
  ValidFn valid = nullptr;
  if (vtable != nullptr && ReadAt(vtable + 0x1c8, &valid) && valid != nullptr && valid(ref.obj, ref.index) != 0) {
    const auto* o = reinterpret_cast<const std::uint8_t*>(ref.obj);
    FieldAt(Ptr(o + 0x1a * 8), Ptr(o + 0x20 * 8), Ptr(Ptr(o + 0x18 * 8)), static_cast<std::uint16_t>(ref.index), 0x70,
            &rarity);
  }
  void* canvas = nullptr;
  std::uint32_t id = 0;
  std::uint64_t common = 0;
  if (rarity == kStoryRarity && Frame(self, 0x28, &canvas, &id, &common)) {
    g_elementSprite(canvas, id, common);
    Tint(canvas, id);
    static volatile LONG once = 0;
    LogOnce(&once, "selected frame: Story");
    return;
  }
  g_setSelectedRarity(self, a2);
  if (Frame(self, 0x28, &canvas, &id, &common)) Untint(canvas, id);
}

char* Write(char* out, const char* text, std::size_t n) {
  std::memset(out, 0, 64);
  std::memcpy(out, text, n < 63 ? n : 63);
  return out;
}

char* RarityLabelHook(std::uint8_t* self, char* out, std::uint16_t item) {
  std::uint64_t rarity = 0;
  if (out != nullptr && BoothField(self, item, 0x70, &rarity) && rarity == kStoryRarity)
    return Write(out, kStoryLabel, std::strlen(kStoryLabel));
  return g_rarityLabel(self, out, item);
}

char* AssignmentStrHook(std::uint8_t* self, char* out, std::uint16_t item) {
  char* r = g_assignmentStr(self, out, item);
  std::uint64_t sym = 0, rarity = 0, type = 0;
  if (out == nullptr || !BoothField(self, item, 0, &sym)) return r;
  {
    std::lock_guard<std::mutex> lock(g_sourceMutex);
    const auto it = g_sources.find(sym);
    if (it != g_sources.end()) return Write(out, it->second.data(), it->second.size());
  }
  if (BoothField(self, item, 0x70, &rarity) && rarity == kStoryRarity) {
    const char* line = BoothField(self, item, 0x68, &type) && type == kBoosterType ? kStorySourceBooster : kStorySource;
    return Write(out, line, std::strlen(line));
  }
  return r;
}

template <typename Fn, std::size_t N>
bool Helper(std::uint64_t va, const std::uint8_t (&prologue)[N], Fn* out) {
  void* fn = evr::ResolveVA(va);
  if (!evr::CheckBytes(fn, prologue, N)) return false;
  *out = reinterpret_cast<Fn>(fn);
  return true;
}

void HookStory() {
  if (!Helper(kElementVisibleVA, kElementVisiblePrologue, &g_elementVisible) ||
      !Helper(kElementSpriteVA, kElementSpritePrologue, &g_elementSprite) ||
      !Helper(kResolveItemVA, kResolveItemPrologue, &g_resolveItem) ||
      !Helper(kElementColorVA, kElementColorPrologue, &g_elementColor)) {
    Log(LogLevel::Warning, "[EVR.COSMETICS] Story rarity off: game build differs (helpers)");
    return;
  }
  evr::HookVA(kSetItemRarityVA, kSetItemRarityPrologue, reinterpret_cast<void*>(&SetItemRarityHook),
              reinterpret_cast<void**>(&g_setItemRarity), "item rarity frame (Story)");
  evr::HookVA(kSetSelectedRarityVA, kSetSelectedRarityPrologue, reinterpret_cast<void*>(&SetSelectedRarityHook),
              reinterpret_cast<void**>(&g_setSelectedRarity), "selected rarity frame (Story)");
  evr::HookVA(kRarityLabelVA, kRarityLabelPrologue, reinterpret_cast<void*>(&RarityLabelHook),
              reinterpret_cast<void**>(&g_rarityLabel), "rarity label (Story)");
}

void HookSourceLine() {
  evr::HookVA(kAssignmentStrVA, kAssignmentStrPrologue, reinterpret_cast<void*>(&AssignmentStrHook),
              reinterpret_cast<void**>(&g_assignmentStr), "booth item AssignmentStr");
}

// ---- the item card's text colour (its script DLL) ----

constexpr wchar_t kCardScript[] = L"f762837db44bc4ea.dll";
constexpr std::uintptr_t kCardGetRarity = 0x1f940, kCardSend = 0x32a0, kCardDefaultNode = 0x10ec0;
constexpr std::uint8_t kCardGetPrologue[16] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x55, 0x48, 0x8d,
                                               0xac, 0x24, 0xc0, 0xf8, 0xff, 0xff, 0x48, 0x81};
constexpr std::uint8_t kCardSendPrologue[16] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83,
                                                0xec, 0x20, 0x48, 0x83, 0x7a, 0x18, 0x00, 0x48};
constexpr std::uint8_t kCardDefaultPrologue[16] = {0x48, 0x8b, 0xc4, 0x48, 0x83, 0xec, 0x68, 0x66,
                                                   0x0f, 0x6f, 0x05, 0xc1, 0x72, 0x01, 0x00, 0xc7};
constexpr std::uint32_t kCardGrey = 0x3f5adadc;  // 0.855: the default node's text colour

using CardGetFn = std::uint64_t (*)(void*, void*, void*, void*);
using CardSendFn = void* (*)(std::uint8_t*, void*, void*, void*);
CardGetFn g_cardGet = nullptr;
CardSendFn g_cardSend = nullptr;
thread_local std::uint64_t t_cardRarity = 0;

std::uint64_t CardGetHook(void* a, void* b, void* c, void* d) {
  const std::uint64_t r = g_cardGet(a, b, c, d);
  t_cardRarity = r;
  return r;
}

void* CardSendHook(std::uint8_t* cmd, void* ctx, void* c, void* d) {
  if (t_cardRarity == kStoryRarity && cmd != nullptr && evr::IsWritable(cmd + 0x30, 12)) {
    std::uint32_t rgb[3];
    std::memcpy(rgb, cmd + 0x30, sizeof(rgb));
    if (rgb[0] == kCardGrey && rgb[1] == kCardGrey && rgb[2] == kCardGrey) {
      std::memcpy(cmd + 0x30, kStoryTextColour, sizeof(kStoryTextColour));
      t_cardRarity = 0;
      static volatile LONG once = 0;
      LogOnce(&once, "item card: Story text coloured");
    }
  }
  return g_cardSend(cmd, ctx, c, d);
}

// The game loads the card script later: wait for it, pin it (so our hooks never outlive it), hook it once.
DWORD WINAPI CardWorker(LPVOID) {
  for (int i = 0; i < 2 * 60 * 30; ++i, Sleep(500)) {  // 30 minutes at most
    HMODULE m = GetModuleHandleW(kCardScript);
    if (m == nullptr) continue;
    const auto* base = reinterpret_cast<std::uint8_t*>(m);
    if (!evr::CheckBytes(base + kCardGetRarity, kCardGetPrologue, 16) ||
        !evr::CheckBytes(base + kCardSend, kCardSendPrologue, 16) ||
        !evr::CheckBytes(base + kCardDefaultNode, kCardDefaultPrologue, 16)) {
      Log(LogLevel::Warning, "[EVR.COSMETICS] item card script differs: Story text not coloured");
      return 0;
    }
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, kCardScript, &pinned);
    evr::Hook(const_cast<std::uint8_t*>(base) + kCardGetRarity, reinterpret_cast<void*>(&CardGetHook),
              reinterpret_cast<void**>(&g_cardGet), "item card rarity getter (Story)");
    evr::Hook(const_cast<std::uint8_t*>(base) + kCardSend, reinterpret_cast<void*>(&CardSendHook),
              reinterpret_cast<void**>(&g_cardSend), "item card colour send (Story)");
    return 0;
  }
  return 0;
}

}  // namespace

void SetSourceLines(std::map<std::uint64_t, std::string> lines) {
  std::lock_guard<std::mutex> lock(g_sourceMutex);
  g_sources = std::move(lines);
}

void Install() {
  if (evr_content::SetMounted(kChassisSet)) {
    evr::RegisterResourceTransform(evr_cdn::kRewardItemType, evr_cdn::kRewardItemName, &AppendChassis, nullptr,
                                   "release chassis");
    Log(LogLevel::Info, "[EVR.COSMETICS] chassis set %s mounted: %zu chassis will join the item list", kChassisSetName,
        ChassisItems().size());
  } else {
    Log(LogLevel::Info, "[EVR.COSMETICS] chassis set %s not in this version: no release chassis", kChassisSetName);
  }
  HookStory();
  HookSourceLine();
  if (HANDLE t = CreateThread(nullptr, 0, &CardWorker, nullptr, 0, nullptr)) CloseHandle(t);
}

}  // namespace evr_cosmetics
