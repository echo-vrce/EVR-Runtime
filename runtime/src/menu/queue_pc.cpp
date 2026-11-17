// menu/queue_pc.cpp: the matchmaker queue's platform gate (PC). Stock Win10 echovr.exe (timestamp 0x6452dff6).
//
// The FIND MATCH bar's estimated time comes from the game's matchmaker queue (ready_at_dawn/join_queue and
// poll_queue_position, answered by Atlas). Atlas turns the queue on through login settings (matchmaker_queue_mode,
// match_type_specific_matchmakers), but the game reads the mode only when the player's account id
// ([netGame+0x910]->+0x28d0, formatted "<platform>-<n>") has platform type 4 or 5:
//   AND EAX,0xF; SUB RAX,4; CMP RAX,1; JA <queue off>
// Our players log in with DMO- ids, so the queue stayed off and the bar read 00:00. Each JA becomes two NOPs. The
// three sites: the mode read when settings arrive (0x1401805be), the join on a find (0x140168669), and the join after
// a settings change (0x140180720).
#include "menu/queue.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <nlohmann/json.hpp>

#include "earlyquit/lockout.h"
#include "evr/core.h"
#include "evr/frame.h"

namespace evr_queue {
namespace {

constexpr std::uint8_t kGateTest[] = {0x83, 0xE0, 0x0F, 0x48, 0x83, 0xE8, 0x04, 0x48, 0x83, 0xF8, 0x01};
constexpr std::uint64_t kJumps[] = {0x1401805BE, 0x140168669, 0x140180720};  // the JA rel8 of each gate
constexpr std::uint8_t kJa = 0x77;
constexpr std::uint8_t kNops[2] = {0x90, 0x90};

// The queue's token source (0x140176370): a global platform object whose +0x30 is the access token string. Only the
// queue's Join (0x1401788f0), Heartbeat (0x140170990) and Leave (0x1401792b0) call it, and each reads only +0x30
// (0 -> empty token). Without the Oculus platform it returns null and Join read [0+0x30] (crash 2026-11-17 22:36,
// echovr.exe+0x17897b). When it returns null we return a zeroed stand-in: the queue sends an empty access_token.
constexpr std::uint64_t kTokenSourceVA = 0x140176370;
constexpr std::uint8_t kTokenSourceBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20};
using TokenSourceFn = void* (*)();
TokenSourceFn g_tokenSource = nullptr;
alignas(16) std::uint8_t g_noToken[0x100] = {};

void* TokenSourceHook() {
  void* real = g_tokenSource();
  return real != nullptr ? real : g_noToken;
}

// The queue's HTTP client gets its base URL once, from the game's own "https://graph.oculus.com" (0x1416e3858),
// through the shared setter 0x1401f60c0(client, url) (call at 0x140173eed). Left alone, the queue would talk to
// Oculus's Graph API ("Invalid OAuth 2.0 Access Token", seen 2026-11-17 22:41). When the setter is given exactly that
// string, we pass Atlas's API host instead (apiservice_host of the game's _local\config.json, no trailing slash).
constexpr std::uint64_t kQueueHostVA = 0x1416E3858;
constexpr char kQueueHost[] = "https://graph.oculus.com";
constexpr std::uint64_t kSetBaseUrlVA = 0x1401F60C0;
constexpr std::uint8_t kSetBaseUrlBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                             0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48};
using SetBaseUrlFn = std::uint64_t (*)(void*, const char*);
SetBaseUrlFn g_setBaseUrl = nullptr;
const char* g_queueHost = nullptr;  // the game's string, resolved
std::string g_atlasHost;

std::uint64_t SetBaseUrlHook(void* client, const char* url) {
  if (url != nullptr && url == g_queueHost) {
    evr::Log(evr::LogLevel::Info, "[EVR.QUEUE] matchmaker queue host: %s (not %s)", g_atlasHost.c_str(), kQueueHost);
    return g_setBaseUrl(client, g_atlasHost.c_str());
  }
  return g_setBaseUrl(client, url);
}

/// apiservice_host from <game>\_local\config.json, without a trailing slash; empty when absent.
std::string AtlasApiHost() {
  std::string dir = evr::PluginsDir();  // <game>\bin\win10\plugins
  for (int up = 0; up < 3; ++up) {
    const std::size_t cut = dir.find_last_of("\\/");
    if (cut == std::string::npos) return {};
    dir.resize(cut);
  }
  std::string text;
  if (FILE* f = std::fopen((dir + "\\_local\\config.json").c_str(), "rb")) {
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
  }
  std::string host;
  try {
    host = nlohmann::json::parse(text).value("apiservice_host", std::string());
  } catch (const nlohmann::json::exception&) {
    return {};
  }
  while (!host.empty() && host.back() == '/') host.pop_back();
  return host;
}

// Diagnostic: the FIND MATCH estimate getter (0x140182100, called by NetMatchmakerQueueExpression at 0x140d9c035)
// returns -1 unless [this+0x2da0] bit 2 is clear, [netGame+0x448] == 3 and the queue map at [netGame+0x910]
// (+0x50 keys, +0x58 count, +0x68 queues) has the key [netGame+0x6a0]; then it returns queue+0x60. Logs its inputs
// whenever they change, to name the condition that keeps the bar at 00:00.
constexpr std::uint64_t kEstimateGetterVA = 0x140182100;
constexpr std::uint8_t kEstimateGetterBytes[] = {0x48, 0x8B, 0x51, 0x40, 0x48, 0x85, 0xD2};
// CMP dword [rdx+0x448],3 ; then JNE rel8: in the estimate getter, and in the position getter 0x140182170 (the
// expression's other output; with -1 there the bar also shows 00:00, seen 2026-11-17 22:59).
constexpr std::uint64_t kStateTestVAs[] = {0x14018211A, 0x14018218A};
constexpr std::uint8_t kEstimateStateTest[] = {0x83, 0xBA, 0x48, 0x04, 0x00, 0x00, 0x03};
using EstimateGetterFn = int (*)(void*);
EstimateGetterFn g_estimateGetter = nullptr;

template <typename T>
T ReadOr(const void* base, std::size_t off, T fallback) {
  if (base == nullptr) return fallback;
  const auto* at = static_cast<const std::uint8_t*>(base) + off;
  if (!evr::IsReadable(at, sizeof(T))) return fallback;
  T v;
  std::memcpy(&v, at, sizeof(T));
  return v;
}

// Test aid: plugins\config.json "queue_estimate_scale" multiplies a valid estimate before the bar sees it (default 1).
int g_estimateScale = 1;

std::atomic<void*> g_seenNetGame{nullptr};  // the getter's `this`: CR15NetGame, a fallback for NetGame()

int EstimateGetterHook(void* self) {
  g_seenNetGame.store(self);
  const int raw = g_estimateGetter(self);
  const int result = raw > 0 ? raw * g_estimateScale : raw;
  static std::uint64_t last = ~0ULL;
  void* net = ReadOr<void*>(self, 0x40, nullptr);
  void* flags = ReadOr<void*>(self, 0x2da0, nullptr);
  const std::uint32_t flag = ReadOr<std::uint32_t>(flags, 0, 0xFFFFFFFF);
  const std::int32_t state = ReadOr<std::int32_t>(net, 0x448, -99);
  const std::uint64_t key = ReadOr<std::uint64_t>(net, 0x6a0, 0);
  void* mgr = ReadOr<void*>(net, 0x910, nullptr);
  const std::uint64_t count = ReadOr<std::uint64_t>(mgr, 0x58, 0);
  const std::uint64_t sig = static_cast<std::uint64_t>(static_cast<std::uint32_t>(result)) ^
                            (static_cast<std::uint64_t>(state) << 32) ^ key ^ (count << 40) ^ (flag << 20);
  if (sig != last) {
    last = sig;
    char keys[256] = {};
    int at = 0;
    void* list = ReadOr<void*>(mgr, 0x50, nullptr);
    for (std::uint64_t i = 0; i < count && i < 8 && at < 220; ++i)
      at += std::snprintf(keys + at, sizeof(keys) - at, " %016llx",
                          static_cast<unsigned long long>(ReadOr<std::uint64_t>(list, i * 8, 0)));
    evr::Log(evr::LogLevel::Info,
             "[EVR.QUEUE] estimate getter -> %d: flag=0x%x state=%d key=%016llx queues=%llu [%s ]", result, flag, state,
             static_cast<unsigned long long>(key), static_cast<unsigned long long>(count), keys);
  }
  return result;
}

// The FIND MATCH bar's timer (bar library 17d77f27d465760b.dll, a script loaded per level). It counts from
// match_search_start_time, which the script (0x12050) reads from the PARTY's data key "mm|starttime" through
// R15NetPartyIntExpression (its reader 0x25470). The game copies its own start time ([lobby+0x8e8], _time64 seconds,
// lobby = [netGame+0x40]) into the party's data only when a party object exists ([netGame+0x647c8], the Oculus
// Social object): our players have none, so the read gave 0 and the bar read 00:00 for good. Per library load, the
// script's call to the reader goes to a 14-byte stub in the library's own int3 padding, which jumps to
// StartTimeHook: the party's value when there is one, else the game's own start time.
constexpr char kBarLibrary[] = "17d77f27d465760b.dll";
constexpr std::size_t kStartCallRva = 0x12059;  // CALL rel32 -> 0x25470, in the function that sets the start time
constexpr std::uint8_t kStartCallBytes[] = {0xE8, 0x12, 0x34, 0x01, 0x00};
constexpr std::size_t kStartReadRva = 0x25470;
constexpr std::uint8_t kStartReadBytes[] = {0x48, 0x81, 0xEC, 0x88, 0x03, 0x00, 0x00, 0x48, 0x89, 0x4C, 0x24, 0x20};
constexpr std::size_t kCaveRva = 0x11EDB;  // 21 bytes of int3 between functions
constexpr std::size_t kStubSize = 14;      // JMP [RIP+0] ; dq target
constexpr std::size_t kLobby = 0x40;
constexpr std::size_t kLobbyState = 0x448;   // 4..6: in a game (where our finds start)
constexpr std::size_t kLobbyStartTime = 0x8E8;

using StartReadFn = int (*)(void*);
std::atomic<StartReadFn> g_startRead{nullptr};
HMODULE g_barLibrary = nullptr;

int StartTimeHook(void* node) {
  const StartReadFn read = g_startRead.load();
  const int party = read != nullptr ? read(node) : 0;
  if (party != 0) return party;
  void* netGame = evr_earlyquit::NetGame();
  if (netGame == nullptr) netGame = g_seenNetGame.load();
  void* lobby = ReadOr<void*>(netGame, kLobby, nullptr);
  const std::int64_t start = ReadOr<std::int64_t>(lobby, kLobbyStartTime, 0);
  return start > 0 && start < 0x7FFFFFFF ? static_cast<int>(start) : party;
}

// The FIND MATCH bar's "Estimated" line (status script 18d18df21fc61b61.dll, ui_ar_mp_mm_status) shows
// R15NetMatchmakingStatsExpression's matchtimeaverageseconds (evaluator 0x140d9c080), which is the float at
// [lobby+0x5fc] (reader 0x1401820e0, lobby = [netGame+0x40]). Only event_update_findmatch_estimated_time
// (handler 0x1401821e0) sets that field, and nothing raises it for our players: Estimated 00:00 (2026-11-17 23:18,
// 23:22). While the field is 0, the hook answers with the game's own queue estimate (the getter 0x140182100,
// seconds from Atlas's estimated_wait_time; -1 when the current find has no queue, then 0 stays).
constexpr std::uint64_t kMatchTimeVA = 0x1401820E0;
constexpr std::uint8_t kMatchTimeBytes[] = {0x48, 0x8B, 0x41, 0x40, 0x48, 0x85, 0xC0};
using MatchTimeFn = float (*)(void*);
MatchTimeFn g_matchTime = nullptr;

float MatchTimeHook(void* netGame) {
  const float stored = g_matchTime(netGame);
  if (stored > 0.0f || g_estimateGetter == nullptr || netGame == nullptr) return stored;
  const int seconds = g_estimateGetter(netGame);
  return seconds > 0 ? static_cast<float>(seconds) : stored;
}

bool WriteCode(std::uint8_t* at, const void* bytes, std::size_t n) {
  DWORD old = 0;
  if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
  std::memcpy(at, bytes, n);
  VirtualProtect(at, n, old, &old);
  FlushInstructionCache(GetCurrentProcess(), at, n);
  return true;
}

void ApplyBarTimer() {
  HMODULE lib = GetModuleHandleA(kBarLibrary);
  if (lib == nullptr || lib == g_barLibrary) {
    if (lib == nullptr) g_barLibrary = nullptr;
    return;
  }
  g_barLibrary = lib;
  auto* base = reinterpret_cast<std::uint8_t*>(lib);
  std::uint8_t* call = base + kStartCallRva;
  std::uint8_t* read = base + kStartReadRva;
  std::uint8_t* cave = base + kCaveRva;
  std::uint8_t stub[kStubSize] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
  const auto target = reinterpret_cast<std::uint64_t>(&StartTimeHook);
  std::memcpy(stub + 6, &target, 8);
  std::uint8_t toCave[5] = {0xE8};
  const auto rel = static_cast<std::int32_t>(cave - (call + 5));
  std::memcpy(toCave + 1, &rel, 4);
  const char* result = "on";
  if (!evr::IsReadable(call, 5) || !evr::IsReadable(read, sizeof(kStartReadBytes)) ||
      !evr::IsReadable(cave, kStubSize) || !evr::CheckBytes(read, kStartReadBytes, sizeof(kStartReadBytes))) {
    result = "off (bytes differ)";
  } else if (evr::CheckBytes(call, toCave, 5) && evr::CheckBytes(cave, stub, kStubSize)) {
    g_startRead.store(reinterpret_cast<StartReadFn>(read));  // already ours (same load)
  } else {
    bool caveFree = true;
    for (std::size_t i = 0; i < kStubSize; ++i) caveFree = caveFree && cave[i] == 0xCC;
    if (!caveFree || !evr::CheckBytes(call, kStartCallBytes, 5)) {
      result = "off (bytes differ)";
    } else {
      g_startRead.store(reinterpret_cast<StartReadFn>(read));
      if (!WriteCode(cave, stub, kStubSize) || !WriteCode(call, toCave, 5)) result = "off (write failed)";
    }
  }
  evr::Log(evr::LogLevel::Info, "[EVR.QUEUE] FIND MATCH bar library at %p: start time without a party %s",
           static_cast<void*>(lib), result);
}

}  // namespace

void Install(int estimateScale) {
  evr::OnFrame(&ApplyBarTimer);
  g_estimateScale = estimateScale > 0 ? estimateScale : 1;
  // Without our own queue host the queue would call Oculus: then the gate stays closed.
  g_atlasHost = AtlasApiHost();
  g_queueHost = static_cast<const char*>(evr::ResolveVA(kQueueHostVA));
  if (g_atlasHost.empty() || g_queueHost == nullptr || !evr::IsReadable(g_queueHost, sizeof(kQueueHost)) ||
      std::memcmp(g_queueHost, kQueueHost, sizeof(kQueueHost)) != 0 ||
      !evr::HookVA(kSetBaseUrlVA, kSetBaseUrlBytes, reinterpret_cast<void*>(&SetBaseUrlHook),
                   reinterpret_cast<void**>(&g_setBaseUrl), "Queue base URL")) {
    evr::Log(evr::LogLevel::Warning, "[EVR.QUEUE] no Atlas queue host (apiservice_host '%s'): the gate stays closed",
             g_atlasHost.c_str());
    return;
  }
  // The stand-in next: with the gate open and no token source, Join crashes.
  if (!evr::HookVA(kTokenSourceVA, kTokenSourceBytes, reinterpret_cast<void*>(&TokenSourceHook),
                   reinterpret_cast<void**>(&g_tokenSource), "Queue token source")) {
    evr::Log(evr::LogLevel::Warning, "[EVR.QUEUE] token source not hooked: the gate stays closed");
    return;
  }
  int open = 0;
  for (std::uint64_t va : kJumps) {
    auto* ja = static_cast<std::uint8_t*>(evr::ResolveVA(va));
    if (ja == nullptr || !evr::IsReadable(ja - sizeof(kGateTest), sizeof(kGateTest) + 2) ||
        !evr::CheckBytes(ja - sizeof(kGateTest), kGateTest, sizeof(kGateTest)))
      continue;
    if (evr::CheckBytes(ja, kNops, 2)) {
      ++open;
      continue;
    }
    if (ja[0] != kJa) continue;
    DWORD old = 0;
    if (!VirtualProtect(ja, 2, PAGE_EXECUTE_READWRITE, &old)) continue;
    ja[0] = kNops[0];
    ja[1] = kNops[1];
    VirtualProtect(ja, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), ja, 2);
    ++open;
  }
  evr::Log(open == 3 ? evr::LogLevel::Info : evr::LogLevel::Warning,
           "[EVR.QUEUE] matchmaker queue platform gate: %d of 3 sites open", open);
  // Both getters answer only in NetGame state 3 (lobby, the main menu). Our players find from inside the social
  // lobby, NetGame state 5 (in game), so the bar read -1 -> 00:00 (logged 2026-11-17 22:55: state=5, its queue key
  // present). Each state JNE becomes two NOPs; the queue-key check still applies.
  int stateOpen = 0;
  for (std::uint64_t va : kStateTestVAs) {
    auto* test = static_cast<std::uint8_t*>(evr::ResolveVA(va));
    if (test == nullptr || !evr::IsReadable(test, sizeof(kEstimateStateTest) + 2) ||
        !evr::CheckBytes(test, kEstimateStateTest, sizeof(kEstimateStateTest)))
      continue;
    std::uint8_t* jne = test + sizeof(kEstimateStateTest);
    if (evr::CheckBytes(jne, kNops, 2)) {
      ++stateOpen;
      continue;
    }
    if (jne[0] != 0x75) continue;
    DWORD old = 0;
    if (!VirtualProtect(jne, 2, PAGE_EXECUTE_READWRITE, &old)) continue;
    jne[0] = kNops[0];
    jne[1] = kNops[1];
    VirtualProtect(jne, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), jne, 2);
    ++stateOpen;
  }
  evr::Log(stateOpen == 2 ? evr::LogLevel::Info : evr::LogLevel::Warning,
           "[EVR.QUEUE] estimate and position outside the menu lobby: %d of 2 on", stateOpen);
  evr::HookVA(kEstimateGetterVA, kEstimateGetterBytes, reinterpret_cast<void*>(&EstimateGetterHook),
              reinterpret_cast<void**>(&g_estimateGetter), "Queue estimate getter");
  evr::HookVA(kMatchTimeVA, kMatchTimeBytes, reinterpret_cast<void*>(&MatchTimeHook),
              reinterpret_cast<void**>(&g_matchTime), "Matchmaking estimated time");
}

}  // namespace evr_queue
