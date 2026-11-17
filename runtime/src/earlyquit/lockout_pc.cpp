// earlyquit/lockout_pc.cpp: the PC hooks for the early-quit lockout. Addresses are the stock Win10 echovr.exe
// (timestamp 0x6452dff6); every one is checked against its bytes before it is touched.
#include "earlyquit/lockout.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>

#include "earlyquit/lockout_rules.h"
#include "evr/actors.h"
#include "evr/core.h"
#include "evr/events.h"
#include "evr/frame.h"

namespace evr_earlyquit {
namespace {

// CR15NetGame fields.
constexpr std::size_t kHeldStamp = 0x64820;  // the stamp the setter gates on
constexpr std::size_t kExpiry = 0x64828;     // the countdown's end; the expression and CR15NetGame::Update read it
constexpr std::size_t kFlagsPtr = 0x2DA0;    // -> u64 flags
constexpr std::uint64_t kRunningBit = 1ULL << 45;

// CR15NetGame::SetEarlyQuitPenaltyLevel(netGame, stamp, expiry, earlyQuits, steadyMatches, steadyEarlyQuits,
// penaltyLevel, steadyLevel). Prologue through MOV RAX,[RCX+0x64820].
constexpr std::uint64_t kSetterVA = 0x1401AE2E0;
constexpr std::uint8_t kSetterBytes[] = {0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x81, 0x20, 0x48, 0x06, 0x00};
using SetterFn = void (*)(void*, std::int64_t, std::int64_t, std::int32_t, std::int32_t, std::int32_t, std::uint8_t,
                          std::uint8_t);
SetterFn g_setter = nullptr;

// CR15NetEarlyQuitPenaltyExpression::operator()(this, out, thread, context). out+0x00 bool active,
// out+0x10 i64 seconds left.
constexpr std::uint64_t kExpressionVA = 0x140D96190;
constexpr std::uint8_t kExpressionBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                             0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57};
using ExpressionFn = void (*)(void*, void*, void*, void*);
ExpressionFn g_expression = nullptr;

// CR15NetGame::EarlyQuitFeatureFlagsCB: MOV RAX,[RDX+8]; MOVZX EDX,byte [RAX]; AND DL,0xDD; MOV [RCX+0x64847],DL.
// The mask is the byte at +9.
constexpr std::uint64_t kFlagsCallbackVA = 0x1401618B0;
constexpr std::uint8_t kFlagsCallbackBytes[] = {0x48, 0x8B, 0x42, 0x08, 0x0F, 0xB6, 0x10, 0x80, 0xE2, kStockFlagMask,
                                                0x88, 0x91, 0x47, 0x48, 0x06, 0x00};
constexpr std::size_t kMaskByte = 9;

// CR15NetGame::DispatchEventToSession(netGame, event), and the word the game's SetEarlyQuitWarningShown
// (0x1401ae430) loads the penalty event from: delegate_onearlyquitpenaltyupdate.
constexpr std::uint64_t kDispatchVA = 0x1401A9FE0;
constexpr std::uint8_t kDispatchBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                           0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48};
constexpr std::uint64_t kPenaltyEventWordVA = 0x1416DCDB0;
constexpr std::uint64_t kPenaltyEvent = 0xA2E48B35C7CD078FULL;
using DispatchFn = void (*)(void*, std::uint64_t);
DispatchFn g_dispatch = nullptr;  // set only when its bytes and the event word check out

std::atomic<int> g_lastActive{-1};

// netGame+0x64830: the early-quit count the setter stores.
constexpr std::size_t kEarlyQuits = 0x64830;

// The tablet's ReturnToHome bit-5 node (lockout_rules.h). Script library loaded by the game at run time, image base
// 0x180000000 on disk; the node is at RVA 0x8f60 and its select at +0x5a. Bytes up to the select are checked before
// anything is written; the library is looked up every frame, so a reload is patched again.
constexpr const char* kTabletLibrary = "18000621255c6ea5.dll";
constexpr std::size_t kNodeRva = 0x8F60;
constexpr std::size_t kSelectAt = 0x5A;
constexpr std::uint8_t kNodeBytes[kSelectAt] = {
    0x48, 0x81, 0xEC, 0x38, 0x03, 0x00, 0x00, 0x4C, 0x8D, 0x0D, 0x22, 0x9E, 0xFF, 0xFF, 0x4C, 0x8D, 0x44, 0x24,
    0x20, 0x48, 0x8D, 0x54, 0x24, 0x30, 0x48, 0x89, 0x4C, 0x24, 0x20, 0xC7, 0x44, 0x24, 0x28, 0x05, 0x00, 0x00,
    0x00, 0xFF, 0x15, 0xDD, 0x43, 0x01, 0x00, 0x8B, 0x15, 0x57, 0x43, 0x01, 0x00, 0x4C, 0x8D, 0x44, 0x24, 0x2C,
    0x48, 0x8D, 0x4C, 0x24, 0x30, 0xFF, 0x15, 0xCF, 0x42, 0x01, 0x00, 0x80, 0x7C, 0x24, 0x2C, 0x00, 0x48, 0xB8,
    0x39, 0xAA, 0x68, 0xC9, 0x83, 0x99, 0xDC, 0x54, 0x48, 0xB9, 0x3B, 0x9D, 0xA5, 0x99, 0xBD, 0xD1, 0xCC, 0x56};

std::atomic<bool> g_noticePending{false};

// The main menu's EARLY QUIT page (lockout_rules.h): bit 46 is kept only for an early quit Atlas reported.
std::atomic<void*> g_netGame{nullptr};      // from the setter (it runs at login)
std::atomic<bool> g_atlasReportedQuit{false};

std::uint64_t* FlagsOf(void* netGame) {
  std::uint64_t* flags = nullptr;
  std::memcpy(&flags, static_cast<std::uint8_t*>(netGame) + kFlagsPtr, sizeof(flags));
  return (flags != nullptr && evr::IsWritable(flags, sizeof(*flags))) ? flags : nullptr;
}

void ApplyWarning() {
  void* netGame = g_netGame.load();
  if (netGame == nullptr) return;
  std::uint64_t* flags = FlagsOf(netGame);
  if (flags == nullptr) return;
  switch (NextWarningStep((*flags & kWarningBit) != 0, g_atlasReportedQuit.load())) {
    case WarningStep::kClear:
      *flags &= ~kWarningBit;
      evr::Log(evr::LogLevel::Info,
               "[EVR.EARLYQUIT] early-quit warning set by the client without an early quit from Atlas: cleared");
      break;
    case WarningStep::kAcknowledged:
      g_atlasReportedQuit.store(false);
      evr::Log(evr::LogLevel::Info, "[EVR.EARLYQUIT] early-quit warning acknowledged");
      break;
    case WarningStep::kNone:
      break;
  }
}
HMODULE g_checkedLibrary = nullptr;  // the library load whose node bytes were checked
bool g_libraryOk = false;

/// Makes the node match g_noticePending. Game-loop thread (frame callback, event listener).
void ApplyNotice() {
  HMODULE lib = GetModuleHandleA(kTabletLibrary);
  if (lib == nullptr) {
    g_checkedLibrary = nullptr;
    return;
  }
  auto* node = reinterpret_cast<std::uint8_t*>(lib) + kNodeRva;
  if (lib != g_checkedLibrary) {
    g_checkedLibrary = lib;
    g_libraryOk = evr::IsReadable(node, kSelectAt + 4) && evr::CheckBytes(node, kNodeBytes, kSelectAt) &&
                  (evr::CheckBytes(node + kSelectAt, kNodeAsks, 4) || evr::CheckBytes(node + kSelectAt, kNodeSaysNo, 4));
    evr::Log(g_libraryOk ? evr::LogLevel::Info : evr::LogLevel::Warning,
             "[EVR.EARLYQUIT] tablet library at %p: notice node %s", static_cast<void*>(lib),
             g_libraryOk ? "found" : "bytes differ, OK on the notice stays stock");
  }
  if (!g_libraryOk) return;
  const std::uint8_t* want = NodeBytes(g_noticePending.load());
  std::uint8_t* at = node + kSelectAt;
  if (std::memcmp(at, want, 4) == 0) return;
  DWORD old = 0;
  if (!VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &old)) return;
  std::uint32_t word;
  std::memcpy(&word, want, 4);
  reinterpret_cast<std::atomic<std::uint32_t>*>(at)->store(word);  // one store: the script thread sees old or new
  VirtualProtect(at, 4, old, &old);
  FlushInstructionCache(GetCurrentProcess(), at, 4);
}

// The FIND MATCH bar's two bit-1 branch nodes (lockout_rules.h). Patched once per load of the library.
constexpr const char* kBarLibrary = "17d77f27d465760b.dll";
HMODULE g_barLibrary = nullptr;

void ApplyBar() {
  HMODULE lib = GetModuleHandleA(kBarLibrary);
  if (lib == nullptr || lib == g_barLibrary) {
    if (lib == nullptr) g_barLibrary = nullptr;
    return;
  }
  g_barLibrary = lib;
  int patched = 0;
  for (const BarNode& n : kBarNodes) {
    auto* node = reinterpret_cast<std::uint8_t*>(lib) + n.rva;
    std::uint8_t* call = node + kBarCallAt;
    if (!evr::IsReadable(node, kBarCallAt + 5 + sizeof(kBarNodeTail)) ||
        !evr::CheckBytes(node, kBarNodeHead, kBarCallAt) ||
        !evr::CheckBytes(call + 5, kBarNodeTail, sizeof(kBarNodeTail)))
      continue;
    if (evr::CheckBytes(call, kBarSaysNo, 5)) {
      ++patched;
      continue;
    }
    if (!evr::CheckBytes(call, n.call, 5)) continue;
    DWORD old = 0;
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old)) continue;
    std::memcpy(call, kBarSaysNo, 5);
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    ++patched;
  }
  evr::Log(patched == 2 ? evr::LogLevel::Info : evr::LogLevel::Warning,
           "[EVR.EARLYQUIT] find-match bar library at %p: %d of 2 bit-1 nodes set to stock", static_cast<void*>(lib),
           patched);
}

/// Writes `n` code bytes (VirtualProtect, copy, flush).
bool WriteCode(std::uint8_t* at, const std::uint8_t* bytes, std::size_t n) {
  DWORD old = 0;
  if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
  std::memcpy(at, bytes, n);
  VirtualProtect(at, n, old, &old);
  FlushInstructionCache(GetCurrentProcess(), at, n);
  return true;
}

// The main menu's EARLY QUIT card (lockout_rules.h): never shown. Patched once per load of the library.
HMODULE g_cardLibrary = nullptr;

void ApplyCard() {
  HMODULE lib = GetModuleHandleA(kCardLibrary);
  if (lib == nullptr || lib == g_cardLibrary) {
    if (lib == nullptr) g_cardLibrary = nullptr;
    return;
  }
  g_cardLibrary = lib;
  auto* base = reinterpret_cast<std::uint8_t*>(lib);
  int done = 0;
  for (std::size_t rva : kCardJumps) {
    std::uint8_t* je = base + rva;
    if (!evr::IsReadable(je - sizeof(kCardTest), sizeof(kCardTest) + 1) ||
        !evr::CheckBytes(je - sizeof(kCardTest), kCardTest, sizeof(kCardTest)))
      continue;
    if (*je == kCardJmp || (*je == kCardJe && WriteCode(je, &kCardJmp, 1))) ++done;
  }
  for (std::size_t rva : kCardNodes) {
    std::uint8_t* at = base + rva;
    if (!evr::IsReadable(at, 5)) continue;
    if (evr::CheckBytes(at, kCardNodeSaysNo, 5) ||
        (evr::CheckBytes(at, kCardNodeReads, 5) && WriteCode(at, kCardNodeSaysNo, 5)))
      ++done;
  }
  evr::Log(done == 7 ? evr::LogLevel::Info : evr::LogLevel::Warning,
           "[EVR.EARLYQUIT] EARLY QUIT card library at %p: %d of 7 bit-5 tests set to off", static_cast<void*>(lib), done);
}

// The main menu's EARLY QUIT page (lockout_rules.h): shown only for an early quit Atlas reported.
HMODULE g_menuChecked = nullptr;
bool g_menuOk = false;

void ApplyMenuPage() {
  HMODULE lib = GetModuleHandleA(kMenuLibrary);
  if (lib == nullptr) {
    g_menuChecked = nullptr;
    return;
  }
  auto* at = reinterpret_cast<std::uint8_t*>(lib) + kMenuPageAt;
  if (lib != g_menuChecked) {
    g_menuChecked = lib;
    g_menuOk = (reinterpret_cast<std::uintptr_t>(at) % 8) == 0 &&
               evr::IsReadable(at - sizeof(kMenuPageBefore), sizeof(kMenuPageBefore) + 8 + sizeof(kMenuPageAfter)) &&
               evr::CheckBytes(at - sizeof(kMenuPageBefore), kMenuPageBefore, sizeof(kMenuPageBefore)) &&
               evr::CheckBytes(at + 8, kMenuPageAfter, sizeof(kMenuPageAfter)) &&
               (evr::CheckBytes(at, kMenuPageShows, 8) || evr::CheckBytes(at, kMenuPageHides, 8));
    evr::Log(g_menuOk ? evr::LogLevel::Info : evr::LogLevel::Warning,
             "[EVR.EARLYQUIT] main menu library at %p: EARLY QUIT page node %s", static_cast<void*>(lib),
             g_menuOk ? "found" : "bytes differ, the page stays stock");
  }
  if (!g_menuOk) return;
  const std::uint8_t* want = MenuPageBytes(g_atlasReportedQuit.load());
  if (std::memcmp(at, want, 8) == 0) return;
  DWORD old = 0;
  if (!VirtualProtect(at, 8, PAGE_EXECUTE_READWRITE, &old)) return;
  std::uint64_t word;
  std::memcpy(&word, want, 8);
  reinterpret_cast<std::atomic<std::uint64_t>*>(at)->store(word);  // aligned: the script thread sees old or new
  VirtualProtect(at, 8, old, &old);
  FlushInstructionCache(GetCurrentProcess(), at, 8);
}

// FIND MATCH after a lockout (lockout_rules.h).
std::atomic<bool> g_lockoutSeen{false};
std::atomic<std::uint64_t> g_barOnAt{0};
std::atomic<std::uint64_t> g_barSpace{0};  // the space whose bar bounced; 0 = nothing to redo
std::atomic<bool> g_barRestoring{false};    // our own enables, so they are not taken as a new find

void OnBarActor(std::uint64_t space, std::uint64_t actor, bool on) {
  if (actor != kBarActor || g_barRestoring.load()) return;
  const std::uint64_t now = GetTickCount64();
  if (on) {
    g_barOnAt.store(now);
    return;
  }
  if (BarBounced(g_lockoutSeen.load(), g_barOnAt.exchange(0), now)) g_barSpace.store(space);
}

void RedrawBar() {
  const std::uint64_t space = g_barSpace.exchange(0);
  if (space == 0) return;
  g_barRestoring.store(true);
  int on = 0;
  for (std::uint64_t a : kBarActors) on += evr::SetActorEnabled(space, a, true) ? 1 : 0;
  g_barRestoring.store(false);
  evr::Log(evr::LogLevel::Info,
           "[EVR.EARLYQUIT] find-match bar switched off right after it came on: %d of 3 bar actors switched back on", on);
}

void OnNoticeFrame() {
  RedrawBar();
  ApplyNotice();
  ApplyWarning();
  ApplyBar();
  ApplyMenuPage();
  ApplyCard();
}

void OnNoticeButton(std::uint64_t /*space*/, std::uint64_t event) {
  std::uint64_t symbol = 0, key = 0;
  const auto* e = reinterpret_cast<const std::uint8_t*>(event);
  std::memcpy(&symbol, e + evr::event_layout::kSymbolOffset, 8);
  std::memcpy(&key, e + evr::event_layout::kComponentOffset, 8);
  if (!IsNoticeButton(symbol, key) || !g_noticePending.exchange(false)) return;
  ApplyNotice();  // before the game runs the button's ReturnToHome
  evr::Log(evr::LogLevel::Info, "[EVR.EARLYQUIT] notice dismissed (%s)", symbol == kNoticeOk ? "OK" : "INFO");
}

std::int64_t Read64(const void* base, std::size_t at) {
  std::int64_t v;
  std::memcpy(&v, static_cast<const std::uint8_t*>(base) + at, 8);
  return v;
}

void Write64(void* base, std::size_t at, std::int64_t v) { std::memcpy(static_cast<std::uint8_t*>(base) + at, &v, 8); }

void SetterHook(void* netGame, std::int64_t stamp, std::int64_t expiry, std::int32_t earlyQuits,
                std::int32_t steadyMatches, std::int32_t steadyEarlyQuits, std::uint8_t penaltyLevel,
                std::uint8_t steadyLevel) {
  if (netGame == nullptr) {
    g_setter(netGame, stamp, expiry, earlyQuits, steadyMatches, steadyEarlyQuits, penaltyLevel, steadyLevel);
    return;
  }
  const std::int64_t held = Read64(netGame, kHeldStamp);
  std::int32_t quitsBefore = 0;
  std::memcpy(&quitsBefore, static_cast<std::uint8_t*>(netGame) + kEarlyQuits, 4);
  g_setter(netGame, stamp, expiry, earlyQuits, steadyMatches, steadyEarlyQuits, penaltyLevel, steadyLevel);
  const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));  // the game's clock: _time64
  const Lockout l = AfterUpdate(held, stamp, expiry, now);
  evr::Log(evr::LogLevel::Info,
           "[EVR.EARLYQUIT] penalty update stamp=%lld held=%lld expiry=%lld now=%lld level=%u early_quits=%d taken=%d "
           "lockout_s=%lld",
           static_cast<long long>(stamp), static_cast<long long>(held), static_cast<long long>(expiry),
           static_cast<long long>(now), static_cast<unsigned>(penaltyLevel), earlyQuits, l.take ? 1 : 0,
           static_cast<long long>(l.running ? l.expiry - now : 0));
  g_netGame.store(netGame);
  if (!l.take) return;
  if (NoticeDue(l.take, l.running, quitsBefore, earlyQuits) && !g_noticePending.exchange(true))
    evr::Log(evr::LogLevel::Info, "[EVR.EARLYQUIT] notice pending (%s)", l.running ? "lockout" : "early quit");
  Write64(netGame, kExpiry, l.expiry);
  if (std::uint64_t* flags = FlagsOf(netGame)) {
    *flags = l.running ? (*flags | kRunningBit) : (*flags & ~kRunningBit);
    if (earlyQuits > quitsBefore) {  // Atlas counted a new early quit: the menu's EARLY QUIT page may show it
      g_atlasReportedQuit.store(true);
      *flags |= kWarningBit;
      evr::Log(evr::LogLevel::Info, "[EVR.EARLYQUIT] early-quit warning from Atlas (%d -> %d)", quitsBefore, earlyQuits);
    }
  }
  // The setter raised the event (when the counters changed) before the expiry was in place: raise it again so the
  // menus read the countdown.
  if (g_dispatch != nullptr) g_dispatch(netGame, kPenaltyEvent);
}

void ExpressionHook(void* self, void* out, void* thread, void* context) {
  g_expression(self, out, thread, context);
  if (out == nullptr) return;
  const bool active = CountdownActive(Read64(out, 0x10));
  static_cast<std::uint8_t*>(out)[0] = active ? 1 : 0;
  if (active) g_lockoutSeen.store(true);
  if (g_lastActive.exchange(active ? 1 : 0) != (active ? 1 : 0))
    evr::Log(evr::LogLevel::Info, "[EVR.EARLYQUIT] countdown %s (%lld s left)", active ? "on" : "off",
             static_cast<long long>(Read64(out, 0x10)));
}

/// Writes one code byte after checking the bytes around it.
template <std::size_t N>
bool PatchCodeByte(std::uint64_t va, const std::uint8_t (&expected)[N], std::size_t at, std::uint8_t value,
                   const char* name) {
  auto* p = static_cast<std::uint8_t*>(evr::ResolveVA(va));
  if (p == nullptr || !evr::CheckBytes(p, expected, N)) {
    evr::Log(evr::LogLevel::Warning, "[EVR.EARLYQUIT] %s skipped: bytes differ at 0x%llx", name,
             static_cast<unsigned long long>(va));
    return false;
  }
  DWORD old = 0;
  if (!VirtualProtect(p + at, 1, PAGE_EXECUTE_READWRITE, &old)) {
    evr::Log(evr::LogLevel::Warning, "[EVR.EARLYQUIT] %s skipped: VirtualProtect failed (%lu)", name, GetLastError());
    return false;
  }
  p[at] = value;
  VirtualProtect(p + at, 1, old, &old);
  FlushInstructionCache(GetCurrentProcess(), p + at, 1);
  evr::Log(evr::LogLevel::Info, "[EVR.EARLYQUIT] %s patched at 0x%llx", name, static_cast<unsigned long long>(va + at));
  return true;
}

}  // namespace

void Install() {
  // The re-raised event: only when the dispatcher and the game's own event word are what we read.
  void* dispatch = evr::ResolveVA(kDispatchVA);
  const void* word = evr::ResolveVA(kPenaltyEventWordVA);
  std::uint64_t event = 0;
  if (word != nullptr && evr::IsReadable(word, 8)) std::memcpy(&event, word, 8);
  if (dispatch != nullptr && evr::CheckBytes(dispatch, kDispatchBytes, sizeof(kDispatchBytes)) && event == kPenaltyEvent)
    g_dispatch = reinterpret_cast<DispatchFn>(dispatch);
  else
    evr::Log(evr::LogLevel::Warning, "[EVR.EARLYQUIT] penalty event will not be re-raised (dispatcher or event differ)");

  const bool setter = evr::HookVA(kSetterVA, kSetterBytes, reinterpret_cast<void*>(&SetterHook),
                                  reinterpret_cast<void**>(&g_setter), "SetEarlyQuitPenaltyLevel");
  const bool expression = evr::HookVA(kExpressionVA, kExpressionBytes, reinterpret_cast<void*>(&ExpressionHook),
                                      reinterpret_cast<void**>(&g_expression), "EarlyQuitPenaltyExpression");
  const bool flags = PatchCodeByte(kFlagsCallbackVA, kFlagsCallbackBytes, kMaskByte, kOurFlagMask, "feature-flag mask");
  evr::OnFrame(&OnNoticeFrame);
  evr::OnComponentEvent(&OnNoticeButton);
  evr::OnActorEnabled(&OnBarActor);
  evr::Log(evr::LogLevel::Info, "[EVR.EARLYQUIT] installed setter=%d expression=%d flags=%d redispatch=%d", setter ? 1 : 0,
           expression ? 1 : 0, flags ? 1 : 0, g_dispatch != nullptr ? 1 : 0);
}

void* NetGame() { return g_netGame.load(); }

}  // namespace evr_earlyquit
