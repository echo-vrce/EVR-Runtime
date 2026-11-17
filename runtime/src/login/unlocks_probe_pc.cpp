// Dev probe (log_events > 0 only): why the stock client rebuilds clientprofile.newunlocks.
//
// Our reading: 0x1401552D0 (game, json) walks json "unlocks|<cat>|<item>" and adds to newunlocks every flagged item
// that the cached server profile (login service = game+0x28D0, cache at +0x38) does not hold yet; the new set is at
// game+0x51C20 (count at +0x30). It is reached only through two transaction-service delegates, on the "update" object
// of SNSReconcileIAPResult (call returns to 0x14019A48F) and SNSPurchaseItemsResult (0x140197B08). We log which one,
// whether the cache had any unlocks, and how many items were added.
//
// The other writer, 0x140152C30 (game, {u64* symbols, count}), adds a list of unlock symbols as they are. It is reached
// from the SR15NetNewUnlocks handler (call returns to 0x140182794; the game server sends it after its own
// "grant missing unlocks" check) and from the SNSNewUnlocksNotification handler (returns to 0x140182827).
#include "login/unlocks_probe.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "evr/core.h"

namespace evr_unlocks_probe {
namespace {

using CompareFn = void (*)(void* game, void* json);
using FirstKeyFn = void* (*)(void* json, const char* path, void* after);

constexpr std::uint64_t kCompareVA = 0x1401552D0;
constexpr std::uint8_t kComparePrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x54,
                                             0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41};
constexpr std::uint64_t kFirstKeyVA = 0x1405F73C0;
constexpr std::uint8_t kFirstKeyPrologue[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41,
                                              0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xC0};
constexpr std::uint64_t kFromReconcile = 0x14019A48F, kFromPurchase = 0x140197B08;
constexpr std::size_t kService = 0x28D0, kCache = 0x38, kNewSetCount = 0x51C20 + 0x30;

using AddListFn = void (*)(void* game, void* list);
constexpr std::uint64_t kAddListVA = 0x140152C30;
constexpr std::uint8_t kAddListPrologue[] = {0x40, 0x55, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x89,
                                             0x5C, 0x24, 0x40, 0x48, 0x8B, 0xE9, 0x48, 0x89};
constexpr std::uint64_t kFromNetNewUnlocks = 0x140182794, kFromNotification = 0x140182827;

CompareFn g_compare = nullptr;
AddListFn g_addList = nullptr;

std::uint64_t NewCount(void* game) {
  std::uint64_t n = 0;
  auto* at = static_cast<std::uint8_t*>(game) + kNewSetCount;
  if (evr::IsReadable(at, 8)) std::memcpy(&n, at, 8);
  return n;
}

int ServiceHasUnlocks(std::uint8_t* service) {
  void* first = evr::ResolveVA(kFirstKeyVA);
  if (service == nullptr || first == nullptr || std::memcmp(first, kFirstKeyPrologue, 16) != 0) return -1;
  return reinterpret_cast<FirstKeyFn>(first)(service + kCache, "unlocks", nullptr) != nullptr ? 1 : 0;
}

int CacheHasUnlocks(void* game) {
  auto* slot = static_cast<std::uint8_t*>(game) + kService;
  if (!evr::IsReadable(slot, 8)) return -1;
  return ServiceHasUnlocks(*reinterpret_cast<std::uint8_t**>(slot));
}

const char* CacheText(int cached) { return cached < 0 ? "?" : cached ? "has unlocks" : "EMPTY (no unlocks)"; }

// RefreshProfileFromServerCB (game, message): the handler of SNSRefreshProfileFromServer (its static symbol is at
// 0x1420A0EE0); message+0x18/+0x20 is the profile JSON text and its length. It merges that into the cache.
using RefreshFn = void (*)(void* game, void* message);
constexpr std::uint64_t kRefreshVA = 0x14019B6B0;
constexpr std::uint8_t kRefreshPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                             0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
RefreshFn g_refresh = nullptr;

void RefreshHook(void* game, void* message) {
  const int cached = CacheHasUnlocks(game);
  std::uint64_t length = 0;
  bool mentionsUnlocks = false;
  auto* m = static_cast<std::uint8_t*>(message);
  if (m != nullptr && evr::IsReadable(m + 0x18, 16)) {
    const char* text = *reinterpret_cast<const char**>(m + 0x18);
    std::memcpy(&length, m + 0x20, 8);
    if (text != nullptr && length > 0 && length < (1u << 24) && evr::IsReadable(text, length))
      mentionsUnlocks = std::string(text, length).find("\"unlocks\"") != std::string::npos;
  }
  g_refresh(game, message);
  evr::Log(evr::LogLevel::Info,
           "[EVR.UNLOCKS] RefreshProfileFromServerCB (message SNSRefreshProfileFromServer): json %llu bytes, has "
           "\"unlocks\"=%s; cache at entry %s, after %s; newunlocks now %llu",
           static_cast<unsigned long long>(length), mentionsUnlocks ? "yes" : "no", CacheText(cached),
           CacheText(CacheHasUnlocks(game)), static_cast<unsigned long long>(NewCount(game)));
}

// 0x140617240 (service, json): merges json into the cached server profile (+0x38) and saves serverprofile.json.
using MergeFn = void (*)(void* service, void* json);
constexpr std::uint64_t kMergeVA = 0x140617240;
constexpr std::uint8_t kMergePrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
                                           0xEC, 0x50, 0x4C, 0x8B, 0xC2, 0x48, 0x89, 0x4C};
MergeFn g_merge = nullptr;

const char* MergeCaller(std::uintptr_t ret) {
  struct Site {
    std::uint64_t va;
    const char* name;
  };
  static const Site kSites[] = {
      {0x14015557C, "newunlocks compare (SNSReconcileIAPResult / SNSPurchaseItemsResult update)"},
      {0x14018BB2C, "iap_success (purchasedcombat)"},
      {0x14019B74E, "RefreshProfileFromServerCB (SNSRefreshProfileFromServer)"},
      {0x1401A7966, "caller 0x1401A7961 (not identified)"},
      {0x1401A7CFF, "caller 0x1401A7CFA (not identified)"},
  };
  for (const Site& s : kSites)
    if (ret == reinterpret_cast<std::uintptr_t>(evr::ResolveVA(s.va))) return s.name;
  return "other";
}

void MergeHook(void* service, void* json) {
  const auto ret = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
  const int cached = service ? ServiceHasUnlocks(static_cast<std::uint8_t*>(service)) : -1;
  g_merge(service, json);
  evr::Log(evr::LogLevel::Info, "[EVR.UNLOCKS] cache merge from %s (ret=%p): cache at entry %s, after %s",
           MergeCaller(ret), reinterpret_cast<void*>(ret), CacheText(cached),
           CacheText(service ? ServiceHasUnlocks(static_cast<std::uint8_t*>(service)) : -1));
}

void CompareHook(void* game, void* json) {
  const auto ret = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
  const char* from = ret == reinterpret_cast<std::uintptr_t>(evr::ResolveVA(kFromReconcile))  ? "SNSReconcileIAPResult"
                     : ret == reinterpret_cast<std::uintptr_t>(evr::ResolveVA(kFromPurchase)) ? "SNSPurchaseItemsResult"
                                                                                              : "other";
  const int cached = CacheHasUnlocks(game);
  const std::uint64_t before = NewCount(game);
  g_compare(game, json);
  const std::uint64_t after = NewCount(game);
  evr::Log(evr::LogLevel::Info,
           "[EVR.UNLOCKS] compare from %s (ret=%p): cache at entry %s, newunlocks %llu -> %llu (added %lld)",
           from, reinterpret_cast<void*>(ret), CacheText(cached),
           static_cast<unsigned long long>(before), static_cast<unsigned long long>(after),
           static_cast<long long>(after) - static_cast<long long>(before));
}

void AddListHook(void* game, void* list) {
  const auto ret = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
  const char* from = ret == reinterpret_cast<std::uintptr_t>(evr::ResolveVA(kFromNetNewUnlocks)) ? "SR15NetNewUnlocks (game server)"
                     : ret == reinterpret_cast<std::uintptr_t>(evr::ResolveVA(kFromNotification))
                         ? "SNSNewUnlocksNotification"
                         : "other";
  std::uint64_t count = 0;
  if (list != nullptr && evr::IsReadable(static_cast<std::uint8_t*>(list) + 8, 8))
    std::memcpy(&count, static_cast<std::uint8_t*>(list) + 8, 8);
  const std::uint64_t before = NewCount(game);
  g_addList(game, list);
  evr::Log(evr::LogLevel::Info, "[EVR.UNLOCKS] add list from %s (ret=%p): %llu symbols, newunlocks %llu -> %llu", from,
           reinterpret_cast<void*>(ret), static_cast<unsigned long long>(count),
           static_cast<unsigned long long>(before), static_cast<unsigned long long>(NewCount(game)));
}

}  // namespace

bool Install() {
  evr::HookVA(kRefreshVA, kRefreshPrologue, reinterpret_cast<void*>(&RefreshHook), reinterpret_cast<void**>(&g_refresh),
              "RefreshProfileFromServerCB (dev probe)");
  evr::HookVA(kMergeVA, kMergePrologue, reinterpret_cast<void*>(&MergeHook), reinterpret_cast<void**>(&g_merge),
              "server profile cache merge (dev probe)");
  evr::HookVA(kAddListVA, kAddListPrologue, reinterpret_cast<void*>(&AddListHook), reinterpret_cast<void**>(&g_addList),
              "newunlocks add list (dev probe)");
  return evr::HookVA(kCompareVA, kComparePrologue, reinterpret_cast<void*>(&CompareHook),
                     reinterpret_cast<void**>(&g_compare), "newunlocks compare (dev probe)");
}

}  // namespace evr_unlocks_probe
