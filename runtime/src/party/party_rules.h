// party/party_rules.h: the tablet party as the client keeps it, apart from the hooks (party_pc.cpp) so it is tested
// without the game.
//
// The stock PC game has no platform Social object (pnsovr is a Quest/Oculus library), so the tablet's PARTY tab
// reads "Parties are currently disabled.". party_pc.cpp hands the game our own object, and every party action the
// game asks of it goes to Atlas as an SNS party message on the login websocket. This file holds what those actions
// send, and what Atlas's replies do to the party.
//
// Wire formats (Atlas server/evr/sns_party.go), little-endian:
//   standard request, 0x28 bytes: routing u64 (0) | local user UUID [16] | session u64 (0) | param u64
//   targeted request, 0x30 bytes: local user UUID [16] | target UUID [16] | session u64 (0) | param u32 | 0 u32
//   data update: a standard request (param = scope: 0 the party's data, 1 our own member data) | seq u32 |
//                json length u32 | json
//   replies: party u64, then a member/owner u64, or a single error byte (see Feed).
// A player's UUID is UUIDv5 in the nil namespace of "OVR-ORG-<account id>"; Atlas maps it back to the account when
// that player joined an SNS party (snsPartyTrackAndJoin). Ids on the wire are account ids (Discord ids).
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace evr_party {

// ---- SHA-1 and UUIDv5 ----

inline std::array<std::uint8_t, 20> Sha1(const std::uint8_t* data, std::size_t n) {
  std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  std::vector<std::uint8_t> m(data, data + n);
  const std::uint64_t bits = static_cast<std::uint64_t>(n) * 8;
  m.push_back(0x80);
  while (m.size() % 64 != 56) m.push_back(0);
  for (int i = 7; i >= 0; --i) m.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
  auto rol = [](std::uint32_t v, int s) { return (v << s) | (v >> (32 - s)); };
  for (std::size_t off = 0; off < m.size(); off += 64) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i)
      w[i] = (std::uint32_t{m[off + i * 4]} << 24) | (std::uint32_t{m[off + i * 4 + 1]} << 16) |
             (std::uint32_t{m[off + i * 4 + 2]} << 8) | std::uint32_t{m[off + i * 4 + 3]};
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      std::uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
      const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  std::array<std::uint8_t, 20> out{};
  for (int i = 0; i < 5; ++i)
    for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<std::uint8_t>(h[i] >> (24 - j * 8));
  return out;
}

using Uuid = std::array<std::uint8_t, 16>;

/// UUIDv5 of `name` in the nil namespace, as raw bytes (the order the wire carries).
inline Uuid Uuid5(const std::string& name) {
  std::vector<std::uint8_t> in(16, 0);
  in.insert(in.end(), name.begin(), name.end());
  const auto d = Sha1(in.data(), in.size());
  Uuid u{};
  std::memcpy(u.data(), d.data(), 16);
  u[6] = static_cast<std::uint8_t>((u[6] & 0x0F) | 0x50);
  u[8] = static_cast<std::uint8_t>((u[8] & 0x3F) | 0x80);
  return u;
}

/// The UUID Atlas knows a player by.
inline Uuid PlayerUuid(std::uint64_t accountId) { return Uuid5("OVR-ORG-" + std::to_string(accountId)); }

// ---- message symbols (CSymbol64 of Atlas's message tokens) ----

namespace sym {
// requests
inline constexpr std::uint64_t kCreate = 0x0b7bd21332523994ULL;
inline constexpr std::uint64_t kJoin = 0xb57b22cc5352e00cULL;
inline constexpr std::uint64_t kLeave = 0xb77b0be7a94a9fb6ULL;
inline constexpr std::uint64_t kLock = 0xc2478aa479f3e16aULL;
inline constexpr std::uint64_t kUnlock = 0x5a4e99802fa3d704ULL;
inline constexpr std::uint64_t kSetJoinPolicy = 0xe1d46b6fb78fd9e6ULL;
inline constexpr std::uint64_t kDataUpdate = 0x3448ca6e8d9dd0ceULL;
inline constexpr std::uint64_t kKick = 0xfaf57beb59917d64ULL;
inline constexpr std::uint64_t kPass = 0x518543cd886a6946ULL;
inline constexpr std::uint64_t kInviteResponse = 0xe3654a09203555a3ULL;
// replies
inline constexpr std::uint64_t kCreateSuccess = 0x0b7ac20124523993ULL;
inline constexpr std::uint64_t kCreateFailure = 0x0b6fd60b2b423885ULL;
inline constexpr std::uint64_t kJoinSuccess = 0xb57a32de4552e00bULL;
inline constexpr std::uint64_t kJoinFailure = 0xb56f26d44a42e11dULL;
inline constexpr std::uint64_t kJoinNotify = 0xcc38103e64879e53ULL;
inline constexpr std::uint64_t kLeaveSuccess = 0xb77a1bf5bf4a9fb1ULL;
inline constexpr std::uint64_t kLeaveFailure = 0xb76f0fffb05a9ea7ULL;
inline constexpr std::uint64_t kLeaveNotify = 0x05315abefc8f804bULL;
inline constexpr std::uint64_t kKickNotify = 0x28cb04891f93dc81ULL;
inline constexpr std::uint64_t kKickSuccess = 0xfaf46bf94f917d63ULL;
inline constexpr std::uint64_t kKickFailure = 0xfae17ff340817c75ULL;
inline constexpr std::uint64_t kPassNotify = 0x9d946c88d5a8aca5ULL;
inline constexpr std::uint64_t kPassSuccess = 0x518453df9e6a6941ULL;
inline constexpr std::uint64_t kPassFailure = 0x519147d5917a6857ULL;
inline constexpr std::uint64_t kLockNotify = 0x93a6b1a6cd4ef8ddULL;
inline constexpr std::uint64_t kUnlockNotify = 0xd8cfd3795010481fULL;
inline constexpr std::uint64_t kLockSuccess = 0xc2469ab66ff3e16dULL;
inline constexpr std::uint64_t kLockFailure = 0xc2538ebc60e3e07bULL;
inline constexpr std::uint64_t kUnlockSuccess = 0x5a4f899239a3d703ULL;
inline constexpr std::uint64_t kUnlockFailure = 0x5a5a9d9836b3d615ULL;
inline constexpr std::uint64_t kInviteNotify = 0x218f721f09026dabULL;
inline constexpr std::uint64_t kInviteListResponse = 0x685a5fb8447b1155ULL;
inline constexpr std::uint64_t kDataNotify = 0x832143ccbf160955ULL;
inline constexpr std::uint64_t kUpdateSuccess = 0xdee671b237a5278dULL;
inline constexpr std::uint64_t kUpdateFailure = 0xdef365b838b5269bULL;
inline constexpr std::uint64_t kUpdateNotify = 0x23c834cb3bc6ecf5ULL;
inline constexpr std::uint64_t kUpdateMemberSuccess = 0x4edffb9fc8cc8731ULL;
inline constexpr std::uint64_t kUpdateMemberFailure = 0x4ecaef95c7dc8627ULL;
inline constexpr std::uint64_t kUpdateMemberNotify = 0x451eb6ca40dde289ULL;
}  // namespace sym

/// The game's login-success message: the peer it arrives from is the login service, where party requests go.
inline constexpr std::uint64_t kLoginSuccess = 0xa5acc1a90d0cce47ULL;

/// Whether `symbol` is a party reply this model reads (party_pc.cpp keeps these from the game, which has no
/// listener for them).
inline bool IsPartyReply(std::uint64_t s) {
  switch (s) {
    case sym::kCreateSuccess: case sym::kCreateFailure: case sym::kJoinSuccess: case sym::kJoinFailure:
    case sym::kJoinNotify: case sym::kLeaveSuccess: case sym::kLeaveFailure: case sym::kLeaveNotify:
    case sym::kKickNotify: case sym::kKickSuccess: case sym::kKickFailure: case sym::kPassNotify:
    case sym::kPassSuccess: case sym::kPassFailure: case sym::kLockNotify: case sym::kUnlockNotify:
    case sym::kLockSuccess: case sym::kLockFailure: case sym::kUnlockSuccess: case sym::kUnlockFailure:
    case sym::kInviteNotify: case sym::kInviteListResponse: case sym::kDataNotify: case sym::kUpdateSuccess:
    case sym::kUpdateFailure: case sym::kUpdateNotify: case sym::kUpdateMemberSuccess:
    case sym::kUpdateMemberFailure: case sym::kUpdateMemberNotify:
      return true;
    default:
      return false;
  }
}

// ---- payloads ----

struct Message {
  std::uint64_t symbol = 0;
  std::vector<std::uint8_t> bytes;
};

inline void Append64(std::vector<std::uint8_t>& b, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}
inline void Append32(std::vector<std::uint8_t>& b, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}
inline std::uint64_t Read64(const std::uint8_t* p) {
  std::uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}
inline std::uint32_t Read32(const std::uint8_t* p) {
  std::uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

inline Message Standard(std::uint64_t symbol, const Uuid& self, std::uint64_t param) {
  Message m{symbol, {}};
  m.bytes.reserve(0x28);
  Append64(m.bytes, 0);
  m.bytes.insert(m.bytes.end(), self.begin(), self.end());
  Append64(m.bytes, 0);
  Append64(m.bytes, param);
  return m;
}

inline Message Targeted(std::uint64_t symbol, const Uuid& self, const Uuid& target, std::uint32_t param) {
  Message m{symbol, {}};
  m.bytes.reserve(0x30);
  m.bytes.insert(m.bytes.end(), self.begin(), self.end());
  m.bytes.insert(m.bytes.end(), target.begin(), target.end());
  Append64(m.bytes, 0);
  Append32(m.bytes, param);
  Append32(m.bytes, 0);
  return m;
}

inline constexpr std::uint64_t kScopeParty = 0;
inline constexpr std::uint64_t kScopeMember = 1;

inline Message DataUpdate(const Uuid& self, std::uint64_t scope, std::uint32_t seq, const std::string& json) {
  Message m = Standard(sym::kDataUpdate, self, scope);
  Append32(m.bytes, seq);
  Append32(m.bytes, static_cast<std::uint32_t>(json.size()));
  m.bytes.insert(m.bytes.end(), json.begin(), json.end());
  return m;
}

/// The game's PartyJoinFailed code for Atlas's (partyJoinFailureCode and the join handlers): 1 not found or
/// closed, 2 any other error, 3 refused by the join policy, 5 full. The game knows 1, 3, 4, 5 and 6.
inline constexpr std::uint32_t GameJoinFailure(std::uint8_t atlas) {
  switch (atlas) {
    case 1: return 1;
    case 2: return 4;
    case 3: return 3;
    case 5: return 5;
    default: return 0;
  }
}

// ---- the party ----

/// The game's member JSON array has 10 slots ([obj+0x248]); Atlas's party limit is 10 too.
inline constexpr std::size_t kMaxMembers = 10;
inline constexpr std::size_t kMaxInvites = 16;

using JsonText = std::shared_ptr<const std::string>;

struct Member {
  std::uint64_t id = 0;  // account id
  JsonText data;         // the member's shared JSON (nullptr: none yet); ours is never kept here
};

struct Invite {
  std::uint64_t partyId = 0;
  std::uint64_t senderId = 0;
  std::uint64_t sentTime = 0;  // Unix seconds
};

enum class EventKind {
  kCreated, kCreateFailed, kJoined, kJoinFailed, kUpdated, kHostChanged, kLeft, kKicked,
  kMemberJoined, kMemberUpdated, kMemberLeft, kInviteReceived,
};

struct Event {
  EventKind kind = EventKind::kUpdated;
  std::uint32_t value = 0;  // kJoinFailed: the game's code; kMember*: the member's index
  std::uint64_t id = 0;     // kMemberLeft: who
};

/// One player's party. Every method runs under the caller's lock; actions return the requests to send.
class Party {
 public:
  void SetSelf(std::uint64_t accountId) {
    if (accountId == 0 || accountId == self_) return;
    self_ = accountId;
    selfUuid_ = PlayerUuid(accountId);
  }
  std::uint64_t Self() const { return self_; }

  // -- what the game reads --
  std::uint64_t PartyId() const { return partyId_; }
  std::uint64_t RoomId() const { return partyId_ != 0 ? partyId_ : joiningParty_; }
  std::uint64_t Owner() const { return partyId_ != 0 ? owner_ : self_; }
  bool InParty() const { return partyId_ != 0; }
  bool Creating() const { return creating_; }
  bool Joining() const { return joiningParty_ != 0; }
  bool Busy() const { return creating_ || joiningParty_ != 0 || leaving_; }
  bool IsHost() const { return partyId_ == 0 || owner_ == self_; }
  bool Locked() const { return locked_; }
  /// Members, the local player first (the game writes its own data to member 0). Outside a party, just us.
  std::vector<Member> Members() const {
    std::vector<Member> out;
    out.push_back(Member{self_, nullptr});
    for (const Member& m : members_) out.push_back(m);
    return out;
  }
  std::size_t MemberCount() const { return 1 + members_.size(); }
  std::uint64_t MemberIdAt(std::size_t i) const {
    if (i == 0) return self_;
    return i - 1 < members_.size() ? members_[i - 1].id : 0;
  }
  bool HasMember(std::uint64_t id) const { return id == self_ || FindMember(id) >= 0; }
  JsonText PartyData() const { return partyData_; }
  /// Room for one more: the host's own wish (`hostJoinable`) or, for a member, the server's lock.
  bool Joinable(bool hostJoinable) const {
    if (partyId_ == 0 || Busy() || MemberCount() >= kMaxMembers) return false;
    return IsHost() ? hostJoinable : !locked_;
  }
  /// Whether an invite to someone may go out now: outside a party (sending one creates it), or a joinable party.
  bool CanInvite(bool hostJoinable) const { return partyId_ == 0 ? !Busy() : Joinable(hostJoinable); }

  /// Invites, newest first, as the game indexes them.
  std::size_t InviteCount() const { return invites_.size(); }
  const Invite* InviteAt(std::size_t index) const {
    return index < invites_.size() ? &invites_[invites_.size() - 1 - index] : nullptr;
  }

  // -- actions --

  /// Creates a party (none, nothing in flight).
  std::vector<Message> Create() {
    if (self_ == 0 || partyId_ != 0 || Busy()) return {};
    creating_ = true;
    return {Standard(sym::kCreate, selfUuid_, 0)};
  }

  /// Queues an invite to a Nakama user. Outside a party this creates one first; ReadyInvites hands the queue out once
  /// the party exists (invites go through Atlas party/invite, which needs the party).
  std::vector<Message> QueueInvite(const std::string& userId) {
    if (userId.empty()) return {};
    if (std::find(pendingInvites_.begin(), pendingInvites_.end(), userId) == pendingInvites_.end())
      pendingInvites_.push_back(userId);
    return partyId_ == 0 ? Create() : std::vector<Message>{};
  }
  std::vector<std::string> ReadyInvites() {
    if (partyId_ == 0 || Busy()) return {};
    std::vector<std::string> out;
    out.swap(pendingInvites_);
    return out;
  }

  std::vector<Message> Join(std::uint64_t partyId) {
    if (self_ == 0 || partyId == 0 || partyId == partyId_ || Busy()) return {};
    StartJoin(partyId, 0);
    return {Standard(sym::kJoin, selfUuid_, partyId)};
  }

  /// Accepts the invite the game lists at `index`: Atlas joins its party (the reply is a join's).
  std::vector<Message> AcceptInvite(std::size_t index) {
    const Invite* inv = InviteAt(index);
    if (inv == nullptr || self_ == 0 || Busy()) return {};
    const Invite taken = *inv;
    RemoveInvite(index);
    StartJoin(taken.partyId, taken.senderId);
    return {Targeted(sym::kInviteResponse, selfUuid_, PlayerUuid(taken.senderId), 1)};
  }

  std::vector<Message> DismissInvite(std::size_t index) {
    const Invite* inv = InviteAt(index);
    if (inv == nullptr || self_ == 0) return {};
    const std::uint64_t sender = inv->senderId;
    RemoveInvite(index);
    return {Targeted(sym::kInviteResponse, selfUuid_, PlayerUuid(sender), 0)};
  }

  std::vector<Message> Leave() {
    if (partyId_ == 0 || leaving_) return {};
    leaving_ = true;
    return {Standard(sym::kLeave, selfUuid_, 0)};
  }

  /// Host only, never ourselves (index 0).
  std::vector<Message> Kick(std::size_t index) {
    const std::uint64_t id = MemberIdAt(index);
    if (partyId_ == 0 || !IsHost() || index == 0 || id == 0) return {};
    return {Targeted(sym::kKick, selfUuid_, PlayerUuid(id), 0)};
  }
  std::vector<Message> Pass(std::size_t index) {
    const std::uint64_t id = MemberIdAt(index);
    if (partyId_ == 0 || !IsHost() || index == 0 || id == 0) return {};
    return {Targeted(sym::kPass, selfUuid_, PlayerUuid(id), 0)};
  }

  /// Host only: the server-side lock, when it differs from what we hold.
  std::vector<Message> SetLocked(bool locked) {
    if (partyId_ == 0 || !IsHost() || Busy() || locked == locked_ || lockSent_ == static_cast<int>(locked)) return {};
    lockSent_ = locked ? 1 : 0;  // once until the server answers
    return {Standard(locked ? sym::kLock : sym::kUnlock, selfUuid_, 0)};
  }

  std::vector<Message> SetJoinPolicy(std::uint32_t policy) {
    if (partyId_ == 0 || !IsHost()) return {};
    return {Standard(sym::kSetJoinPolicy, selfUuid_, policy)};
  }

  /// Our party data (host only) or our member data, in a party.
  std::vector<Message> Share(std::uint64_t scope, const std::string& json) {
    if (partyId_ == 0 || Busy() || (scope == kScopeParty && !IsHost())) return {};
    return {DataUpdate(selfUuid_, scope, ++seq_, json)};
  }

  /// Forgets the party with no request: the login connection was replaced, and Atlas leaves a party for a session
  /// that closes. A party (or a join) the game knew of ends with a Left event.
  void Lost() {
    const bool had = partyId_ != 0 || joiningParty_ != 0;
    ClearParty();
    creating_ = false;
    pendingInvites_.clear();
    if (had) Push(EventKind::kLeft);
  }

  // -- replies --

  /// Applies a reply. False when `symbol` is not a party reply or its payload is short.
  bool Feed(std::uint64_t symbol, const std::uint8_t* p, std::size_t n, std::uint64_t now = 0) {
    if (!IsPartyReply(symbol)) return false;
    const bool two = p != nullptr && n >= 16;
    const std::uint64_t a = p != nullptr && n >= 8 ? Read64(p) : 0;
    const std::uint64_t b = two ? Read64(p + 8) : 0;
    switch (symbol) {
      case sym::kCreateSuccess:
        if (!two) return false;
        if (!creating_) return true;
        creating_ = false;
        ClearParty();
        if (self_ == 0) SetSelf(b);
        partyId_ = a;
        owner_ = b != 0 ? b : self_;
        Push(EventKind::kCreated);
        return true;
      case sym::kCreateFailure:
        if (!creating_) return true;
        creating_ = false;
        pendingInvites_.clear();
        Push(EventKind::kCreateFailed, n >= 1 ? p[0] : 0);
        return true;
      case sym::kJoinSuccess: {
        if (!two) return false;
        if (joiningParty_ == 0 || a != joiningParty_) return true;
        std::vector<Member> learned;
        learned.swap(joinMembers_);
        JsonText partyData = joinPartyData_;
        ClearParty();
        partyId_ = a;
        owner_ = b;
        partyData_ = partyData;
        for (const Member& m : learned) AddMember(m.id, m.data);
        if (owner_ != self_) AddMember(owner_, nullptr);
        Push(EventKind::kJoined);
        return true;
      }
      case sym::kJoinFailure: {
        if (n < 9) return false;
        if (joiningParty_ == 0 || a != joiningParty_) return true;
        EndJoin();
        Push(EventKind::kJoinFailed, GameJoinFailure(p[8]));
        return true;
      }
      case sym::kJoinNotify:
        if (!two) return false;
        if (a == partyId_ && partyId_ != 0 && !HasMember(b) && AddMember(b, nullptr))
          Push(EventKind::kMemberJoined, static_cast<std::uint32_t>(MemberCount() - 1));
        return true;
      case sym::kLeaveSuccess:
      case sym::kLeaveFailure:
        if (!leaving_) return true;
        ClearParty();
        Push(EventKind::kLeft);
        return true;
      case sym::kLeaveNotify:
      case sym::kKickNotify:
        if (!two) return false;
        if (a != partyId_ || partyId_ == 0) return true;
        if (b == self_) {
          ClearParty();
          Push(symbol == sym::kKickNotify ? EventKind::kKicked : EventKind::kLeft);
        } else if (RemoveMember(b)) {
          Push(EventKind::kMemberLeft, 0, b);
        }
        return true;
      case sym::kPassNotify:
        if (!two) return false;
        if (a != partyId_ || partyId_ == 0 || b == owner_) return true;
        owner_ = b;
        if (!HasMember(b)) AddMember(b, nullptr);
        Push(EventKind::kHostChanged);
        return true;
      case sym::kLockNotify:
      case sym::kUnlockNotify:
        if (n < 8) return false;
        if (a == partyId_ && partyId_ != 0) locked_ = symbol == sym::kLockNotify;
        lockSent_ = -1;
        return true;
      case sym::kLockFailure:
      case sym::kUnlockFailure:
        lockSent_ = -1;
        return true;
      case sym::kInviteNotify: {
        if (!two) return false;
        if (a == 0 || b == 0 || b == self_ || a == partyId_) return true;
        for (std::size_t i = 0; i < invites_.size(); ++i)
          if (invites_[i].partyId == a) {
            invites_.erase(invites_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
          }
        if (invites_.size() >= kMaxInvites) invites_.erase(invites_.begin());
        invites_.push_back(Invite{a, b, now});
        Push(EventKind::kInviteReceived);
        return true;
      }
      case sym::kDataNotify: {
        if (n < 24) return false;
        const std::uint32_t len = Read32(p + 20);
        if (n < 24 + static_cast<std::size_t>(len)) return false;
        auto json = std::make_shared<const std::string>(reinterpret_cast<const char*>(p + 24), len);
        if (joiningParty_ != 0 && a == joiningParty_) {  // Atlas sends a joiner the party's data first
          if (b == 0) {
            joinPartyData_ = json;
          } else if (b != self_) {
            bool found = false;
            for (Member& m : joinMembers_)
              if (m.id == b) {
                m.data = json;
                found = true;
              }
            if (!found && joinMembers_.size() + 1 < kMaxMembers) joinMembers_.push_back(Member{b, json});
          }
          return true;
        }
        if (a != partyId_ || partyId_ == 0) return true;
        if (b == 0) {
          if (!IsHost()) {
            partyData_ = json;
            Push(EventKind::kUpdated);
          }
          return true;
        }
        if (b == self_) return true;
        const int at = FindMember(b);
        if (at >= 0) {
          members_[static_cast<std::size_t>(at)].data = json;
          Push(EventKind::kMemberUpdated, static_cast<std::uint32_t>(at + 1));
        } else if (AddMember(b, json)) {  // data ahead of the join notice
          Push(EventKind::kMemberJoined, static_cast<std::uint32_t>(MemberCount() - 1));
        }
        return true;
      }
      default:
        return true;  // acknowledgements: nothing changes
    }
  }

  std::vector<Event> DrainEvents() {
    std::vector<Event> out;
    out.swap(events_);
    return out;
  }

  /// Account ids the game may show a name for: members and invite senders.
  std::vector<std::uint64_t> NamedIds() const {
    std::vector<std::uint64_t> out;
    for (const Member& m : members_) out.push_back(m.id);
    for (const Invite& i : invites_) out.push_back(i.senderId);
    return out;
  }

 private:
  void Push(EventKind k, std::uint32_t v = 0, std::uint64_t id = 0) { events_.push_back(Event{k, v, id}); }
  int FindMember(std::uint64_t id) const {
    for (std::size_t i = 0; i < members_.size(); ++i)
      if (members_[i].id == id) return static_cast<int>(i);
    return -1;
  }
  bool AddMember(std::uint64_t id, JsonText data) {
    if (id == 0 || id == self_ || FindMember(id) >= 0 || MemberCount() >= kMaxMembers) return false;
    members_.push_back(Member{id, std::move(data)});
    return true;
  }
  bool RemoveMember(std::uint64_t id) {
    const int at = FindMember(id);
    if (at < 0) return false;
    members_.erase(members_.begin() + at);
    return true;
  }
  void RemoveInvite(std::size_t index) {
    invites_.erase(invites_.begin() + static_cast<std::ptrdiff_t>(invites_.size() - 1 - index));
  }
  void StartJoin(std::uint64_t partyId, std::uint64_t inviter) {
    joiningParty_ = partyId;
    joinInviter_ = inviter;
    joinMembers_.clear();
    joinPartyData_.reset();
  }
  void EndJoin() { StartJoin(0, 0); }
  void ClearParty() {
    partyId_ = 0;
    owner_ = 0;
    locked_ = false;
    lockSent_ = -1;
    leaving_ = false;
    members_.clear();
    partyData_.reset();
    EndJoin();
  }

  std::uint64_t self_ = 0;
  Uuid selfUuid_{};
  std::uint64_t partyId_ = 0;
  std::uint64_t owner_ = 0;
  bool creating_ = false;
  bool leaving_ = false;
  bool locked_ = false;
  int lockSent_ = -1;  // the lock request in flight (-1: none)
  std::uint64_t joiningParty_ = 0;
  std::uint64_t joinInviter_ = 0;
  std::vector<Member> joinMembers_;
  JsonText joinPartyData_;
  std::vector<Member> members_;  // everyone but us
  JsonText partyData_;           // the leader's party data, as a member holds it
  std::vector<Invite> invites_;  // arrival order
  std::vector<std::string> pendingInvites_;
  std::uint32_t seq_ = 0;
  std::vector<Event> events_;
};

}  // namespace evr_party
