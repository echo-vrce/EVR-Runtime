// party/party_pc.cpp: our Social object for the stock Win10 echovr.exe (timestamp 0x6452dff6). Every address is
// checked against its bytes before it is used. Layouts and slot meanings were read from the game's own call sites;
// HANDOFF.md ("Parties on PC") has the evidence.
//
// How it fits together:
//   - Social accessor (0x1406169C0, called once from CR15NetGame::Initialize): the stock one asks the platform DLL
//     for its Social object and gets nothing on PC. We return ours instead; the game then calls Initialize (slot 9)
//     with its 15 party callbacks and drives the object every frame through Update (slot 13).
//   - The TcpBroadcaster's incoming dispatcher (0x140F85480): LoginSuccess tells us which broadcaster and peer is the
//     login service; the SNS party replies are fed to the party model (party_rules.h) and kept from the game, which
//     has no listener for them.
//   - Requests are queued and sent from the frame callback through the broadcaster's SendToPeer (vtable +0xA8).
//   - Party and member data are the game's CJson objects: loaded from Atlas's text, serialised to share ours.
//   - Names and invites to friends go through Atlas RPCs on a worker thread (party/names, party/invite).
#include "party/party.h"
#include "earlyquit/lockout.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "evr/core.h"
#include "evr/frame.h"
#include "party/party_rules.h"
#include "social/social.h"

namespace evr_party {
namespace {

using evr::Log;
using evr::LogLevel;

// ---- game addresses ----

// Social accessor (rcx = the platform module) -> the Social object or null.
constexpr std::uint64_t kAccessorVA = 0x1406169C0;
constexpr std::uint8_t kAccessorBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24,
                                           0x20, 0x57, 0x48, 0x81, 0xEC, 0x30, 0x02, 0x00, 0x00};
using AccessorFn = void* (*)(void*);
AccessorFn g_accessor = nullptr;

// TcpBroadcaster incoming dispatcher (this, symbol, peer, data, size); "no listeners are registered" when none.
constexpr std::uint64_t kDispatchVA = 0x140F85480;
constexpr std::uint8_t kDispatchBytes[] = {0x4C, 0x89, 0x4C, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10,
                                           0x53, 0x56, 0x41, 0x55, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x48};
using DispatchFn = std::uint64_t (*)(void*, std::uint64_t, std::uint64_t, const void*, std::uint64_t);
DispatchFn g_dispatch = nullptr;

// The concrete broadcaster's vtable (RTTI SClientData) and its SendToPeer, slot 21:
// (this, peer, symbol, item, itemSize, buffer, bufferSize).
constexpr std::uint64_t kBroadcasterVtableVA = 0x141CD1130;
constexpr std::size_t kSendToPeerSlot = 0xA8 / 8;
constexpr std::uint64_t kSendToPeerVA = 0x140F8A9C0;
constexpr std::uint8_t kSendToPeerBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x4C, 0x89, 0x4C, 0x24, 0x20,
                                             0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10};
using SendToPeerFn = void (*)(void*, std::uint64_t, std::uint64_t, const void*, std::uint64_t, const void*,
                              std::uint64_t);

// CJson: load (json, text, length) -> 0 ok; clear (json); serialise (json, block[0x40], 0, "") with the text at
// block+0 and its length at block+0x30, then the block's reset (when [+0x1c] & 6) and destroy.
constexpr std::uint64_t kJsonLoadVA = 0x1405F0BD0;
constexpr std::uint8_t kJsonLoadBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x56, 0x57};
constexpr std::uint64_t kJsonClearVA = 0x1405ECE60;
constexpr std::uint8_t kJsonClearBytes[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9};
constexpr std::uint64_t kJsonSerializeVA = 0x1405F1DC0;
constexpr std::uint8_t kJsonSerializeBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                                0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10};
constexpr std::uint64_t kBlockResetVA = 0x1400D4E50;
constexpr std::uint8_t kBlockResetBytes[] = {0x83, 0x61, 0x1C, 0xFE, 0x33, 0xC0, 0x48, 0x89, 0x01};
constexpr std::uint64_t kBlockDestroyVA = 0x1400D2760;
constexpr std::uint8_t kBlockDestroyBytes[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9,
                                               0x48, 0x83, 0x79, 0x08, 0x00};
struct JsonOps {
  std::uint32_t (*load)(void*, const char*, std::int64_t) = nullptr;
  void (*clear)(void*) = nullptr;
  void (*serialize)(void*, void*, std::uint64_t, const char*) = nullptr;
  void (*blockReset)(void*) = nullptr;
  void (*blockDestroy)(void*) = nullptr;
} g_json;
bool g_jsonReady = false;

// ---- the Social object ----

constexpr std::size_t kObjectBytes = 0xBA0;
constexpr std::size_t kCallbacksAt = 0x08;  // 15 delegates of 0x20: {instance, method[16], proxy}
constexpr std::size_t kCallbackCount = 15;
constexpr std::size_t kCallbackStride = 0x20;
constexpr std::size_t kPartyJson = 0x1F0;   // the party's CJson
constexpr std::size_t kLocalCount = 0x200;
constexpr std::size_t kMemberCount = 0x204;
constexpr std::size_t kMemberJsonPtr = 0x248;  // -> 10 member CJson objects of 16 bytes
constexpr std::size_t kMaxMembersAt = 0x250;
constexpr std::size_t kLobbyUuid = 0x260;
constexpr std::size_t kMatchType = 0x270;
constexpr std::size_t kTeam = 0x278;
constexpr std::size_t kLobbyType = 0x27A;
constexpr std::size_t kFlags = 0x27C;  // bit0 party data dirty, bit1 host wants joinable, bit2 creating,
                                       // bit3 joining, bit4 offline
constexpr std::size_t kRoomId = 0x2A8;
constexpr std::size_t kOwnerIndex = 0x2B0;
constexpr std::size_t kJoinPolicy = 0x2B4;

enum Cb : std::size_t {
  kCbCreated = 0, kCbJoined = 1, kCbJoinFailed = 2, kCbUpdated = 3, kCbHostChanged = 4, kCbLeft = 5,
  kCbKicked = 6, kCbInviteAccepted = 7, kCbMemberJoined = 9, kCbMemberUpdated = 10, kCbMemberLeft = 11,
  kCbInviteReceived = 14,
};

alignas(16) std::uint8_t g_object[kObjectBytes];
alignas(16) std::uint8_t g_memberJson[16 * kMaxMembers];
constexpr std::size_t kSlotCount = 75;
constexpr std::size_t kGuardSlots = 10;
void* g_vtable[kSlotCount + kGuardSlots];

std::uint8_t* Obj(void* self) { return static_cast<std::uint8_t*>(self); }
std::uint32_t Get32(void* self, std::size_t at) {
  std::uint32_t v;
  std::memcpy(&v, Obj(self) + at, 4);
  return v;
}
void Put32(void* self, std::size_t at, std::uint32_t v) { std::memcpy(Obj(self) + at, &v, 4); }
void Put64(void* self, std::size_t at, std::uint64_t v) { std::memcpy(Obj(self) + at, &v, 8); }
void* MemberJson(std::size_t i) { return g_memberJson + 16 * i; }

// ---- shared state (the dispatcher may run off the game-loop thread) ----

std::mutex g_mutex;
Party g_party;
void* g_broadcaster = nullptr;
std::uint64_t g_loginPeer = 0;
bool g_haveLogin = false;
std::vector<Message> g_outbox;
std::map<std::uint64_t, std::string> g_names;  // account id -> name
std::set<std::string> g_interned;              // names handed to the game: never freed
std::atomic<bool> g_installed{false};

// Worker (names and invites).
std::condition_variable g_wake;
std::vector<std::string> g_inviteQueue;  // Nakama user ids
bool g_wakeSoon = false;

const char* Intern(const std::string& s) { return g_interned.insert(s).first->c_str(); }

void Queue(const std::vector<Message>& msgs, const char* what) {
  if (msgs.empty()) return;
  for (const Message& m : msgs) g_outbox.push_back(m);
  Log(LogLevel::Info, "[EVR.PARTY] %s: %zu request(s) queued", what, msgs.size());
}

void EnsureSelf() {
  if (g_party.Self() == 0) g_party.SetSelf(evr_social::AccountId());
}

std::string NameOf(std::uint64_t id) {
  const auto it = g_names.find(id);
  if (it != g_names.end() && !it->second.empty()) return it->second;
  return "PLAYER";
}

// ---- callbacks into the game ----

std::uint8_t* Delegate(std::size_t i) { return g_object + kCallbacksAt + kCallbackStride * i; }
void* DelegateProxy(std::size_t i) {
  void* fn;
  std::memcpy(&fn, Delegate(i) + 0x18, 8);
  return fn;
}
void* DelegateInstance(std::size_t i) {
  void* p;
  std::memcpy(&p, Delegate(i), 8);
  return p;
}

void Call(std::size_t i) {
  if (void* fn = DelegateProxy(i)) reinterpret_cast<void (*)(void*, void*)>(fn)(DelegateInstance(i), Delegate(i) + 8);
}
void CallU32(std::size_t i, std::uint32_t v) {
  if (void* fn = DelegateProxy(i))
    reinterpret_cast<void (*)(void*, void*, std::uint32_t)>(fn)(DelegateInstance(i), Delegate(i) + 8, v);
}
void CallIdName(std::size_t i, std::uint64_t id, const char* name) {
  if (void* fn = DelegateProxy(i))
    reinterpret_cast<void (*)(void*, void*, std::uint64_t, const char*)>(fn)(DelegateInstance(i), Delegate(i) + 8, id,
                                                                             name);
}
bool Gate(std::size_t i) {
  void* fn = DelegateProxy(i);
  if (fn == nullptr) return true;
  const std::uint64_t r = reinterpret_cast<std::uint64_t (*)(void*, void*)>(fn)(DelegateInstance(i), Delegate(i) + 8);
  return (r & 0xFFFFFFFFULL) != 0;
}

// ---- party data ----

std::uint64_t g_slotMember[kMaxMembers] = {};
JsonText g_slotData[kMaxMembers];
JsonText g_partyLoaded;
std::uint64_t g_sharedParty = 0;
bool g_memberWritten = false;
std::string g_lastShared[2];

bool Load(void* json, const std::string& text, const char* what) {
  const std::uint32_t r = g_json.load(json, text.data(), static_cast<std::int64_t>(text.size()));
  if (r != 0) Log(LogLevel::Warning, "[EVR.PARTY] %s data not loaded (%zu bytes, result %u)", what, text.size(), r);
  return r == 0;
}

bool Serialize(void* json, std::string* out) {
  alignas(16) std::uint8_t block[0x40] = {};
  g_json.serialize(json, block, 0, "");
  const char* text;
  std::uint64_t len;
  std::uint32_t flags;
  std::memcpy(&text, block, 8);
  std::memcpy(&len, block + 0x30, 8);
  std::memcpy(&flags, block + 0x1C, 4);
  out->assign(text != nullptr ? text : "", text != nullptr ? static_cast<std::size_t>(len) : 0);
  if ((flags & 6U) != 0) g_json.blockReset(block);
  g_json.blockDestroy(block);
  while (!out->empty() && out->back() == '\0') out->pop_back();
  if (out->find_first_not_of(" \t\r\n") == std::string::npos) *out = "{}";
  const nlohmann::json j = nlohmann::json::parse(*out, nullptr, false);
  return !j.is_discarded() && j.is_object();
}

void ClearJson(void* self) {
  if (g_jsonReady) {
    g_json.clear(Obj(self) + kPartyJson);
    for (std::size_t i = 0; i < kMaxMembers; ++i) g_json.clear(MemberJson(i));
  }
  std::memset(Obj(self) + kPartyJson, 0, 16);
  std::memset(g_memberJson, 0, sizeof(g_memberJson));
  for (std::size_t i = 0; i < kMaxMembers; ++i) {
    g_slotMember[i] = 0;
    g_slotData[i].reset();
  }
  g_partyLoaded.reset();
  g_sharedParty = 0;
  g_lastShared[0].clear();
  g_lastShared[1].clear();
}

// Loads what Atlas sent for the other members (slots 1..9) and, for a member, the party's data.
void LoadReceived(void* self, const std::vector<Member>& members) {
  if (!g_jsonReady) return;
  for (std::size_t i = 1; i < kMaxMembers; ++i) {
    const std::uint64_t id = i < members.size() ? members[i].id : 0;
    const JsonText data = i < members.size() ? members[i].data : nullptr;
    if (g_slotMember[i] == id && g_slotData[i] == data) continue;
    if (data != nullptr)
      Load(MemberJson(i), *data, "member");
    else if (g_slotData[i] != nullptr)
      g_json.clear(MemberJson(i));
    g_slotMember[i] = id;
    g_slotData[i] = data;
  }
  const JsonText party = g_party.IsHost() ? nullptr : g_party.PartyData();
  if (!g_party.IsHost() && party != g_partyLoaded) {
    if (party != nullptr)
      Load(Obj(self) + kPartyJson, *party, "party");
    else
      g_json.clear(Obj(self) + kPartyJson);
    g_partyLoaded = party;
  }
}

// Shares the host's party data when the game marked it dirty, and ours when the game wrote it.
void ShareLocal(void* self) {
  if (!g_jsonReady || !g_party.InParty() || g_party.Busy()) return;
  const bool fresh = g_party.PartyId() != g_sharedParty;
  if (fresh) {
    g_sharedParty = g_party.PartyId();
    g_memberWritten = true;
    g_lastShared[0].clear();
    g_lastShared[1].clear();
  }
  std::string text;
  const std::uint32_t flags = Get32(self, kFlags);
  if (g_party.IsHost() && ((flags & 1U) != 0 || fresh)) {
    Put32(self, kFlags, flags & ~1U);
    if (Serialize(Obj(self) + kPartyJson, &text) && text != g_lastShared[0]) {
      g_lastShared[0] = text;
      Queue(g_party.Share(kScopeParty, text), "party data (party)");
    }
  }
  if (g_memberWritten) {
    g_memberWritten = false;
    if (Serialize(MemberJson(0), &text) && text != g_lastShared[1]) {
      g_lastShared[1] = text;
      Queue(g_party.Share(kScopeMember, text), "party data (member)");
    }
  }
}

void SyncObject(void* self, const std::vector<Member>& members) {
  Put64(self, kRoomId, g_party.RoomId());
  std::uint32_t owner = 0;
  for (std::size_t i = 0; i < members.size(); ++i)
    if (members[i].id == g_party.Owner()) owner = static_cast<std::uint32_t>(i);
  Put32(self, kOwnerIndex, owner);
  if (Get32(self, kLocalCount) != 0) Put32(self, kMemberCount, static_cast<std::uint32_t>(members.size()));
  std::uint32_t flags = Get32(self, kFlags);
  flags = g_party.Creating() ? (flags | 4U) : (flags & ~4U);
  flags = g_party.Joining() ? (flags | 8U) : (flags & ~8U);
  Put32(self, kFlags, flags);
  Put32(self, kMaxMembersAt, static_cast<std::uint32_t>(kMaxMembers));
  const std::uintptr_t json = reinterpret_cast<std::uintptr_t>(g_memberJson);
  std::memcpy(Obj(self) + kMemberJsonPtr, &json, 8);
}

bool HostWantsJoinable(void* self) { return (Get32(self, kFlags) & 2U) != 0; }

void ResetFields(void* self) {
  ClearJson(self);
  Put32(self, kFlags, (Get32(self, kFlags) & ~1U) | 2U);
  Put32(self, kLocalCount, 0);
  Put32(self, kMemberCount, 0);
  Put64(self, kMatchType, ~0ULL);
  const std::uint16_t team = 0xFFFF;
  std::memcpy(Obj(self) + kTeam, &team, 2);
  Obj(self)[kLobbyType] = 2;
}

const char* EventName(EventKind k) {
  switch (k) {
    case EventKind::kCreated: return "created";
    case EventKind::kCreateFailed: return "create failed";
    case EventKind::kJoined: return "joined";
    case EventKind::kJoinFailed: return "join failed";
    case EventKind::kUpdated: return "party data";
    case EventKind::kHostChanged: return "leader changed";
    case EventKind::kLeft: return "left";
    case EventKind::kKicked: return "kicked";
    case EventKind::kMemberJoined: return "member joined";
    case EventKind::kMemberUpdated: return "member data";
    case EventKind::kMemberLeft: return "member left";
    case EventKind::kInviteReceived: return "invite received";
  }
  return "?";
}

// ---- slots ----

void S_Nop(void*) {}
void S_NopU32(void*, std::uint32_t) {}
void S_NopU32U32(void*, std::uint32_t, std::uint32_t) {}
std::uint64_t S_Zero(void*) { return 0; }
std::uint64_t S_ZeroU32(void*, std::uint32_t) { return 0; }
const char* S_EmptyU32(void*, std::uint32_t) { return ""; }
std::uint64_t* S_OutZero(void*, std::uint64_t* out, std::uint32_t) {
  if (out != nullptr) *out = 0;
  return out;
}

void JoinParty(void* self, std::uint64_t partyId);

void S_JoinInternal(void* self, std::uint64_t partyId) { JoinParty(self, partyId); }

void S_Leave(void*) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Queue(g_party.Leave(), "leave");
}

std::uint32_t S_JoinableInternal(void*) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_party.Locked() ? 0U : 1U;
}

void S_SetJoinableInternal(void*, std::uint32_t joinable) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Queue(g_party.SetLocked(joinable == 0), joinable != 0 ? "unlock" : "lock");
}

// SendInvite and the invite UI's target: a friend's id from the FriendId slot (Symbol64 of their Nakama user id).
void InviteFriend(std::uint64_t target) {
  const evr_social::Friend* f = evr_social::Friends().Find(target);
  if (f == nullptr || f->userId.empty()) {
    Log(LogLevel::Info, "[EVR.PARTY] invite: id 0x%016llx is not a friend in the roster", (unsigned long long)target);
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  EnsureSelf();
  if (g_party.Self() == 0) {
    Log(LogLevel::Warning, "[EVR.PARTY] invite: no account id in the game's login config");
    return;
  }
  Queue(g_party.QueueInvite(f->userId), "invite (creates the party)");
  Log(LogLevel::Info, "[EVR.PARTY] invite to %s queued", f->name.c_str());
}

void S_SendInvite(void*, std::uint64_t target) { InviteFriend(target); }

std::uint64_t S_Initialize(void* self, std::uint32_t maxUsers, const void* callbacks) {
  if (callbacks != nullptr)
    std::memcpy(Obj(self) + kCallbacksAt, callbacks, kCallbackCount * kCallbackStride);
  else
    std::memset(Obj(self) + kCallbacksAt, 0, kCallbackCount * kCallbackStride);
  Log(LogLevel::Info, "[EVR.PARTY] social object initialised (max users %u, callbacks %s)", maxUsers,
      callbacks != nullptr ? "set" : "none");
  return 0;
}

void S_Shutdown(void* self) {
  std::memset(Obj(self) + kCallbacksAt, 0, kCallbackCount * kCallbackStride);
  Log(LogLevel::Info, "[EVR.PARTY] social object shut down");
}

// Reset: the game calls it whenever its social-disabled bits clear (around matchmaking). It clears the object's
// fields only; the party itself stays (Atlas keeps it across matches, and the next Update fills the fields again).
void S_Reset(void* self) {
  ResetFields(self);
  Log(LogLevel::Info, "[EVR.PARTY] social object reset (the party is kept)");
}

void S_SetLocalUser(void* self, std::uint32_t index) {
  if (index != 0) return;
  Put32(self, kLocalCount, 1);
  if (Get32(self, kMemberCount) == 0) Put32(self, kMemberCount, 1);
}

void S_SetJoinPolicy(void* self, std::uint32_t policy) {
  Put32(self, kJoinPolicy, policy);
  std::lock_guard<std::mutex> lock(g_mutex);
  Queue(g_party.SetJoinPolicy(policy), "join policy");
}

void S_Pass(void*, std::uint32_t index) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Queue(g_party.Pass(index), "pass leadership");
}

void S_Kick(void*, std::uint32_t index) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Queue(g_party.Kick(index), "kick");
}

std::uint32_t S_Ready(void*) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_party.InParty() && !g_party.Joining() ? 1U : 0U;
}

std::uint32_t S_JoinPolicy(void* self) { return Get32(self, kJoinPolicy); }

std::uint32_t S_Joinable(void* self) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_party.Joinable(HostWantsJoinable(self)) ? 1U : 0U;
}

std::uint64_t* S_Host(void*, std::uint64_t* out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  EnsureSelf();
  if (out != nullptr) *out = g_party.Owner();
  return out;
}

std::uint32_t S_IsHost(void*) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_party.IsHost() ? 1U : 0U;
}

std::uint64_t S_Id(void*) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_party.RoomId();
}

std::uint32_t S_MemberCount(void*) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return static_cast<std::uint32_t>(g_party.MemberCount());
}

// The game's own login account id: the lobby's [[+0x910]]+0x28D0 user object, read by 0x140618690 (the reader the
// lobby uses for its own id at 0x140180582; it returns {platform, number}).
std::uint64_t GameAccountNumber() {
  auto read = [](const void* base, std::size_t off) -> std::uint64_t {
    if (base == nullptr) return 0;
    const auto* at = static_cast<const std::uint8_t*>(base) + off;
    if (!evr::IsReadable(at, 8)) return 0;
    std::uint64_t v;
    std::memcpy(&v, at, 8);
    return v;
  };
  using ReadId = std::uint64_t* (*)(void* user, std::uint64_t* out);
  constexpr std::uintptr_t kReadIdVA = 0x140618690;
  static const std::uint8_t kProlog[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20};
  static void* const fn = evr::ResolveVA(kReadIdVA);
  static const bool ok = fn != nullptr && evr::IsReadable(fn, sizeof(kProlog)) &&
                         std::memcmp(fn, kProlog, sizeof(kProlog)) == 0;
  if (!ok) {
    static bool said = false;
    if (!said) Log(LogLevel::Warning, "[EVR.PARTY] own account reader 0x140618690 not found");
    said = true;
    return 0;
  }
  const auto lobby = read(evr_earlyquit::NetGame(), 0x40);
  const auto session = read(reinterpret_cast<const void*>(lobby), 0x910);
  const auto owner = read(reinterpret_cast<const void*>(session), 0);
  const auto user = read(reinterpret_cast<const void*>(owner), 0x28D0);
  std::uint64_t out[2] = {0, 0};
  if (user != 0 && read(reinterpret_cast<const void*>(user), 0) != 0)
    reinterpret_cast<ReadId>(fn)(reinterpret_cast<void*>(user), out);
  static std::uint64_t lastLogged = ~0ull;
  if (out[1] != lastLogged) {
    lastLogged = out[1];
    Log(LogLevel::Info, "[EVR.PARTY] own account: netgame=%p lobby=%llx session=%llx owner=%llx user=%llx -> %llu-%llu",
        evr_earlyquit::NetGame(), (unsigned long long)lobby, (unsigned long long)session, (unsigned long long)owner,
        (unsigned long long)user, (unsigned long long)out[0], (unsigned long long)out[1]);
  }
  return out[1];
}

// The local member's id is the one the game puts in its own requests (the lobby find's evr id): it must be the game's
// login account number, not the Discord id the party protocol uses (Atlas closed the connection on a find with
// "mismatched evr id" DMO-<discord id> vs DMO-<login number>, 2026-11-17 23:51). Other members keep their party ids.
std::uint64_t* S_MemberId(void*, std::uint64_t* out, std::uint32_t index) {
  std::lock_guard<std::mutex> lock(g_mutex);
  EnsureSelf();
  if (out == nullptr) return out;
  std::uint64_t id = g_party.MemberIdAt(index);
  if (id != 0 && id == g_party.Self()) {
    const std::uint64_t own = GameAccountNumber();
    if (own != 0) id = own;
  }
  *out = id;
  return out;
}

const char* S_MemberName(void*, std::uint32_t index) {
  std::lock_guard<std::mutex> lock(g_mutex);
  EnsureSelf();
  const std::uint64_t id = g_party.MemberIdAt(index);
  return id == 0 ? "" : Intern(NameOf(id));
}

std::uint32_t S_MemberVisible(void*, std::uint32_t) { return 1; }

// Only our own member data (index 0) is writable; the next Update shares it.
void* S_MemberDataWritable(void* self, std::int32_t index) {
  if (index != 0 || Get32(self, kLocalCount) == 0) return nullptr;
  g_memberWritten = true;
  return MemberJson(0);
}

void S_EnterLobby(void* self, const void* uuid, std::uint64_t matchType, std::uint16_t team, std::uint8_t lobbyType,
                  int offline) {
  if (uuid != nullptr) std::memcpy(Obj(self) + kLobbyUuid, uuid, 16);
  Put64(self, kMatchType, matchType);
  std::memcpy(Obj(self) + kTeam, &team, 2);
  Obj(self)[kLobbyType] = lobbyType;
  const std::uint32_t flags = Get32(self, kFlags);
  Put32(self, kFlags, offline != 0 ? (flags | 0x10U) : (flags & ~0x10U));
}
void S_EnterLobbySession(void* self, const void* uuid, std::uint64_t matchType, std::uint16_t team,
                         std::uint8_t lobbyType) {
  S_EnterLobby(self, uuid, matchType, team, lobbyType, 0);
}
void S_EnterLobbyRegistration(void* self, const void* uuid, std::uint64_t matchType, std::uint16_t team) {
  if (uuid != nullptr) std::memcpy(Obj(self) + kLobbyUuid, uuid, 16);
  Put64(self, kMatchType, matchType);
  std::memcpy(Obj(self) + kTeam, &team, 2);
}
void S_ExitLobby(void* self) {
  Put64(self, kMatchType, ~0ULL);
  const std::uint16_t team = 0xFFFF;
  std::memcpy(Obj(self) + kTeam, &team, 2);
  Obj(self)[kLobbyType] = 2;
  Put32(self, kFlags, Get32(self, kFlags) & ~0x10U);
}

void S_OpenFriendRequestUI(void*, std::uint64_t, std::uint64_t) {}
void S_OpenInviteUITarget(void*, std::uint32_t, std::uint64_t target) { InviteFriend(target); }
void S_OpenPartyUITarget(void*, std::uint32_t, std::uint64_t) {}

void S_RefreshFriends(void*) { evr_social::RefreshSoon(); }
std::uint32_t S_FriendCount(void*) { return evr_social::Friends().Count(); }
std::uint32_t S_FriendOnline(void*) { return evr_social::Friends().Online(); }
std::uint32_t S_FriendOffline(void*) { return evr_social::Friends().Count() - evr_social::Friends().Online(); }
std::uint64_t* S_FriendId(void*, std::uint64_t* out, std::uint32_t index) {
  std::uint64_t id = 0;
  evr_social::Friends().IdAt(index, &id);
  if (out != nullptr) *out = id;
  return out;
}
const char* S_FriendName(void*, std::uint32_t index) { return evr_social::Friends().NameAt(index); }
std::uint32_t S_FriendStatus(void*, std::uint32_t index) { return evr_social::Friends().OnlineAt(index) ? 2U : 0U; }

// An online friend who is not in the party, while an invite may go out (none in flight, room left).
std::uint32_t S_FriendIsInvitable(void* self, std::uint32_t index) {
  const evr_social::Roster& r = evr_social::Friends();
  if (!r.OnlineAt(index) || index >= r.Count()) return 0;
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_party.CanInvite(HostWantsJoinable(self)) ? 1U : 0U;
}

std::uint32_t S_InviteCount(void*) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return static_cast<std::uint32_t>(g_party.InviteCount());
}
const char* S_InviteSender(void*, std::int32_t index) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const Invite* inv = index < 0 ? nullptr : g_party.InviteAt(static_cast<std::size_t>(index));
  return inv == nullptr ? "" : Intern(NameOf(inv->senderId));
}
std::uint64_t S_InviteSentTime(void*, std::int32_t index) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const Invite* inv = index < 0 ? nullptr : g_party.InviteAt(static_cast<std::size_t>(index));
  return inv == nullptr ? 0 : inv->sentTime;
}

void S_AcceptInvite(void*, std::int32_t index) {
  if (index < 0) return;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_party.InviteAt(static_cast<std::size_t>(index)) == nullptr || g_party.Busy()) return;
  }
  if (!Gate(kCbInviteAccepted)) {
    Log(LogLevel::Info, "[EVR.PARTY] accept invite: the game declined");
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  EnsureSelf();
  Queue(g_party.AcceptInvite(static_cast<std::size_t>(index)), "accept invite");
}

void S_DismissInvite(void*, std::int32_t index) {
  if (index < 0) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  EnsureSelf();
  Queue(g_party.DismissInvite(static_cast<std::size_t>(index)), "dismiss invite");
}

void JoinParty(void*, std::uint64_t partyId) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (partyId == 0 || g_party.Busy() || partyId == g_party.PartyId()) return;
  }
  if (!Gate(kCbInviteAccepted)) {
    Log(LogLevel::Info, "[EVR.PARTY] join: the game declined");
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  EnsureSelf();
  Queue(g_party.Join(partyId), "join");
}

// Update (slot 13), every frame: the object's fields from the party, received data loaded, ours shared, then the
// game's callbacks for what happened (data first, so MemberJoined finds the member's headset type).
void S_Update(void* self, const void*) {
  std::vector<Event> events;
  std::vector<std::pair<std::uint64_t, std::string>> leftNames;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    EnsureSelf();
    const std::vector<Member> members = g_party.Members();
    SyncObject(self, members);
    LoadReceived(self, members);
    ShareLocal(self);
    if (g_party.InParty() && g_party.IsHost() && !g_party.Busy())
      Queue(g_party.SetLocked(!HostWantsJoinable(self)), "host joinable");
    const std::vector<std::string> invites = g_party.ReadyInvites();
    if (!invites.empty()) {
      g_inviteQueue.insert(g_inviteQueue.end(), invites.begin(), invites.end());
      g_wakeSoon = true;
      g_wake.notify_one();
    }
    events = g_party.DrainEvents();
    for (const Event& e : events)
      if (e.kind == EventKind::kMemberLeft) leftNames.emplace_back(e.id, NameOf(e.id));
    if (!events.empty()) {
      g_wakeSoon = true;  // new members or senders to name
      g_wake.notify_one();
    }
  }
  std::size_t left = 0;
  for (const Event& e : events) {
    Log(LogLevel::Info, "[EVR.PARTY] %s (value %u)", EventName(e.kind), e.value);
    switch (e.kind) {
      case EventKind::kCreated: Call(kCbCreated); break;
      case EventKind::kCreateFailed: break;  // the game has no callback for it
      case EventKind::kJoined: Call(kCbJoined); break;
      case EventKind::kJoinFailed: CallU32(kCbJoinFailed, e.value); break;
      case EventKind::kUpdated: Call(kCbUpdated); break;
      case EventKind::kHostChanged: Call(kCbHostChanged); break;
      case EventKind::kLeft: Call(kCbLeft); break;
      case EventKind::kKicked: Call(kCbKicked); break;
      case EventKind::kMemberJoined: CallU32(kCbMemberJoined, e.value); break;
      case EventKind::kMemberUpdated: CallU32(kCbMemberUpdated, e.value); break;
      case EventKind::kMemberLeft: {
        const char* name;
        {
          std::lock_guard<std::mutex> lock(g_mutex);
          name = Intern(left < leftNames.size() ? leftNames[left].second : "PLAYER");
        }
        ++left;
        CallIdName(kCbMemberLeft, e.id, name);
        break;
      }
      case EventKind::kInviteReceived: CallU32(kCbInviteReceived, 0); break;
    }
  }
}

template <std::size_t N>
std::uint64_t S_Guard(void*) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) Log(LogLevel::Warning, "[EVR.PARTY] unexpected social slot %zu called", N);
  return 0;
}

template <std::size_t... I>
void FillGuards(std::index_sequence<I...>) {
  ((g_vtable[kSlotCount + I] = reinterpret_cast<void*>(&S_Guard<kSlotCount + I>)), ...);
}

template <typename F>
void* V(F f) {
  return reinterpret_cast<void*>(f);
}

void BuildObject() {
  void* slots[kSlotCount] = {
      V(&S_NopU32U32),            // 0 SwapMembers
      V(&S_NopU32),               // 1 RemoveMember
      V(&S_JoinInternal),         // 2 JoinInternal(party id)
      V(&S_Leave),                // 3 LeaveInternal
      V(&S_JoinableInternal),     // 4 JoinableInternal
      V(&S_SetJoinableInternal),  // 5 SetJoinableInternal(joinable)
      V(&S_Nop),                  // 6 PushMemberData (shared from Update)
      V(&S_Nop),                  // 7 ShareData (shared from Update)
      V(&S_SendInvite),           // 8 SendInvite(friend id)
      V(&S_Initialize),           // 9 Initialize(max users, callbacks)
      V(&S_Shutdown),             // 10 Shutdown
      V(&S_Nop),                  // 11 release: the object lives as long as the process
      V(&S_Reset),                // 12 Reset
      V(&S_Update),               // 13 Update(flags)
      V(&S_SetLocalUser),         // 14 SetLocalUser(index)
      V(&S_NopU32),               // 15 RemoveLocalMember
      V(&S_SetJoinPolicy),        // 16 SetJoinPolicy(policy)
      V(&S_Leave),                // 17 Leave
      V(&S_Pass),                 // 18 PassOwnership(member index)
      V(&S_Kick),                 // 19 Kick(member index)
      V(&S_Ready),                // 20 Ready
      V(&S_JoinPolicy),           // 21 JoinPolicy
      V(&S_Joinable),             // 22 Joinable
      V(&S_Host),                 // 23 Host(out)
      V(&S_IsHost),               // 24 IsHost
      V(&S_Id),                   // 25 Id
      V(&S_MemberCount),          // 26 MemberCount
      V(&S_MemberId),             // 27 MemberId(out, index)
      V(&S_MemberName),           // 28 MemberName(index)
      V(&S_MemberVisible),        // 29 MemberVisible(index)
      V(&S_MemberDataWritable),   // 30 MemberDataWritable(index)
      V(&S_EnterLobby),           // 31 EnterLobby
      V(&S_EnterLobbySession),    // 32 EnterLobby forwarder
      V(&S_EnterLobbyRegistration),  // 33 EnterLobby forwarder
      V(&S_ExitLobby),            // 34 ExitLobby
      V(&S_Nop),                  // 35 EnterGame
      V(&S_Nop),                  // 36 ExitGame
      V(&S_OpenFriendRequestUI),  // 37 OpenFriendRequestUI
      V(&S_NopU32),               // 38 OpenSendInviteUI
      V(&S_OpenInviteUITarget),   // 39 OpenNewSendInviteUI(target)
      V(&S_NopU32),               // 40 OpenNewSendInviteUI
      V(&S_NopU32),               // 41 OpenRecvInviteUI
      V(&S_OpenPartyUITarget),    // 42 OpenPartyUI(target)
      V(&S_NopU32),               // 43 OpenPartyUI
      V(&S_Zero),                 // 44 RefreshingFriends
      V(&S_RefreshFriends),       // 45 RefreshFriends
      V(&S_FriendCount),          // 46 FriendCount
      V(&S_FriendOnline),         // 47 OnlineFriendCount
      V(&S_FriendOffline),        // 48 OfflineFriendCount
      V(&S_FriendId),             // 49 FriendId(out, index)
      V(&S_FriendName),           // 50 FriendName(index)
      V(&S_FriendStatus),         // 51 FriendStatus(index): 2 online, 0 offline
      V(&S_EmptyU32),             // 52 FriendStatusString
      V(&S_FriendIsInvitable),    // 53 FriendIsInvitable
      V(&S_ZeroU32),              // 54 FriendIsJoinable
      V(&S_ZeroU32),              // 55 FriendPartyId
      V(&S_Zero),                 // 56 RecentlyMetRefreshing
      V(&S_Nop),                  // 57 RefreshRecentlyMet
      V(&S_Zero),                 // 58 RecentlyMetCount
      V(&S_Zero),                 // 59 RecentlyMetOnline
      V(&S_Zero),                 // 60 RecentlyMetOffline
      V(&S_OutZero),              // 61 RecentlyMetUserId(out, index)
      V(&S_EmptyU32),             // 62 RecentlyMetUserName
      V(&S_ZeroU32),              // 63 RecentlyMetUserStatus
      V(&S_EmptyU32),             // 64 RecentlyMetUserStatusString
      V(&S_ZeroU32),              // 65 RecentlyMetUserIsInvitable
      V(&S_ZeroU32),              // 66 RecentlyMetUserIsJoinable
      V(&S_ZeroU32),              // 67 RecentlyMetUserPartyId
      V(&S_Zero),                 // 68 RefreshingInvites
      V(&S_Nop),                  // 69 RefreshInvites
      V(&S_InviteCount),          // 70 InviteCount
      V(&S_InviteSender),         // 71 InviteSender(index)
      V(&S_InviteSentTime),       // 72 InviteSentTime(index)
      V(&S_AcceptInvite),         // 73 AcceptInvite(index)
      V(&S_DismissInvite),        // 74 DismissInvite(index)
  };
  std::memcpy(g_vtable, slots, sizeof(slots));
  FillGuards(std::make_index_sequence<kGuardSlots>{});
  std::memset(g_object, 0, sizeof(g_object));
  void** vt = g_vtable;
  std::memcpy(g_object, &vt, 8);
  ResetFields(g_object);
  Put32(g_object, kJoinPolicy, 3);
  SyncObject(g_object, g_party.Members());
}

// ---- hooks ----

void* AccessorHook(void* module) {
  void* stock = g_accessor(module);
  if (stock != nullptr) {
    Log(LogLevel::Info, "[EVR.PARTY] the platform has a Social object; ours is not used");
    return stock;
  }
  Log(LogLevel::Info, "[EVR.PARTY] social object handed to the game (parties through Atlas)");
  return g_object;
}

std::uint64_t DispatchHook(void* self, std::uint64_t symbol, std::uint64_t peer, const void* data,
                           std::uint64_t size) {
  if (symbol == kLoginSuccess) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_haveLogin && (g_broadcaster != self || g_loginPeer != peer)) {
      g_party.Lost();
      g_outbox.clear();
      Log(LogLevel::Info, "[EVR.PARTY] new login connection: the old party is gone");
    }
    g_broadcaster = self;
    g_loginPeer = peer;
    g_haveLogin = true;
    EnsureSelf();
  } else if (IsPartyReply(symbol)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_party.Feed(symbol, static_cast<const std::uint8_t*>(data), static_cast<std::size_t>(size),
                      static_cast<std::uint64_t>(std::time(nullptr))))
      Log(LogLevel::Warning, "[EVR.PARTY] reply 0x%016llx unreadable (%llu bytes)", (unsigned long long)symbol,
          (unsigned long long)size);
    return 0;
  }
  return g_dispatch(self, symbol, peer, data, size);
}

// Frame: sends what the slots queued, through the login connection.
void Frame() {
  std::vector<Message> out;
  void* broadcaster;
  std::uint64_t peer;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_outbox.empty() || !g_haveLogin) return;
    out.swap(g_outbox);
    broadcaster = g_broadcaster;
    peer = g_loginPeer;
  }
  void** vt = *static_cast<void***>(broadcaster);
  if (vt != evr::ResolveVA(kBroadcasterVtableVA)) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
      Log(LogLevel::Warning, "[EVR.PARTY] the login connection is not the known broadcaster; party requests dropped");
    return;
  }
  auto send = reinterpret_cast<SendToPeerFn>(vt[kSendToPeerSlot]);
  for (const Message& m : out) {
    send(broadcaster, peer, m.symbol, m.bytes.data(), m.bytes.size(), nullptr, 0);
    Log(LogLevel::Info, "[EVR.PARTY] sent 0x%016llx (%zu bytes)", (unsigned long long)m.symbol, m.bytes.size());
  }
}

// ---- worker: names and invites ----

void ApplyNames(const std::string& body) {
  const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
  if (!j.is_object() || !j.contains("names") || !j["names"].is_object()) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  for (auto it = j["names"].begin(); it != j["names"].end(); ++it) {
    if (!it.value().is_string()) continue;
    const std::uint64_t id = std::strtoull(it.key().c_str(), nullptr, 10);
    if (id != 0) g_names[id] = it.value().get<std::string>();
  }
}

void Worker() {
  std::uint64_t namesRetryAt = 0;
  for (;;) {
    std::vector<std::string> invites;
    std::vector<std::uint64_t> unnamed;
    {
      std::unique_lock<std::mutex> lock(g_mutex);
      g_wake.wait_for(lock, std::chrono::seconds(5), [] { return g_wakeSoon; });
      g_wakeSoon = false;
      invites.swap(g_inviteQueue);
      std::vector<std::uint64_t> ids = g_party.NamedIds();
      if (g_party.Self() != 0) ids.push_back(g_party.Self());
      for (const std::uint64_t id : ids)
        if (g_names.find(id) == g_names.end()) unnamed.push_back(id);
    }
    for (const std::string& userId : invites) {
      const evr_social::RpcReply r = evr_social::InviteToParty(userId);
      Log(r.ok ? LogLevel::Info : LogLevel::Warning, "[EVR.PARTY] invite %s%s%s", r.ok ? "sent" : "not sent",
          r.ok ? "" : ": ", r.error.c_str());
    }
    const std::uint64_t now = GetTickCount64();
    if (!unnamed.empty() && now >= namesRetryAt) {
      nlohmann::json ids = nlohmann::json::array();
      for (const std::uint64_t id : unnamed) ids.push_back(std::to_string(id));
      const evr_social::RpcReply r = evr_social::CallRpc("party/names", nlohmann::json{{"ids", ids}}.dump());
      if (r.ok) {
        ApplyNames(r.body);
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const std::uint64_t id : unnamed) g_names.emplace(id, "");  // asked once; unknown stays PLAYER
      } else {
        namesRetryAt = now + 15000;
        Log(LogLevel::Info, "[EVR.PARTY] names not fetched status=%ld error=%s", r.status, r.error.c_str());
      }
    }
  }
}

bool ResolveJson() {
  auto at = [](std::uint64_t va, const std::uint8_t* bytes, std::size_t n) -> void* {
    void* p = evr::ResolveVA(va);
    return p != nullptr && evr::CheckBytes(p, bytes, n) ? p : nullptr;
  };
  void* load = at(kJsonLoadVA, kJsonLoadBytes, sizeof(kJsonLoadBytes));
  void* clear = at(kJsonClearVA, kJsonClearBytes, sizeof(kJsonClearBytes));
  void* ser = at(kJsonSerializeVA, kJsonSerializeBytes, sizeof(kJsonSerializeBytes));
  void* reset = at(kBlockResetVA, kBlockResetBytes, sizeof(kBlockResetBytes));
  void* destroy = at(kBlockDestroyVA, kBlockDestroyBytes, sizeof(kBlockDestroyBytes));
  if (!load || !clear || !ser || !reset || !destroy) return false;
  g_json.load = reinterpret_cast<std::uint32_t (*)(void*, const char*, std::int64_t)>(load);
  g_json.clear = reinterpret_cast<void (*)(void*)>(clear);
  g_json.serialize = reinterpret_cast<void (*)(void*, void*, std::uint64_t, const char*)>(ser);
  g_json.blockReset = reinterpret_cast<void (*)(void*)>(reset);
  g_json.blockDestroy = reinterpret_cast<void (*)(void*)>(destroy);
  return true;
}

}  // namespace

bool Installed() { return g_installed.load(); }

void Install() {
  void* send = evr::ResolveVA(kSendToPeerVA);
  void** vt = static_cast<void**>(evr::ResolveVA(kBroadcasterVtableVA));
  if (send == nullptr || !evr::CheckBytes(send, kSendToPeerBytes, sizeof(kSendToPeerBytes)) || vt == nullptr ||
      vt[kSendToPeerSlot] != send) {
    Log(LogLevel::Warning, "[EVR.PARTY] skipped: the broadcaster's SendToPeer is not where expected");
    return;
  }
  g_jsonReady = ResolveJson();
  if (!g_jsonReady) {
    Log(LogLevel::Warning, "[EVR.PARTY] skipped: the game's JSON functions are not where expected");
    return;
  }
  BuildObject();
  if (!evr::HookVA(kDispatchVA, kDispatchBytes, reinterpret_cast<void*>(&DispatchHook),
                   reinterpret_cast<void**>(&g_dispatch), "party dispatcher"))
    return;
  if (!evr::HookVA(kAccessorVA, kAccessorBytes, reinterpret_cast<void*>(&AccessorHook),
                   reinterpret_cast<void**>(&g_accessor), "social accessor"))
    return;  // the dispatcher hook only passes through without the object
  evr::OnFrame(&Frame);
  std::thread(Worker).detach();
  g_installed = true;
  Log(LogLevel::Info, "[EVR.PARTY] installed: social object, party replies, up to %zu members", kMaxMembers);
}

}  // namespace evr_party
