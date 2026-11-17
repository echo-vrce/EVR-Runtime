#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "evr/symbol.h"
#include "party/party_rules.h"

using namespace evr_party;

namespace {

constexpr std::uint64_t kMe = 123456789012345678ULL;
constexpr std::uint64_t kAlice = 222222222222222222ULL;
constexpr std::uint64_t kBob = 333333333333333333ULL;
constexpr std::uint64_t kParty = 0x1122334455667788ULL;

std::string Hex(const std::uint8_t* p, std::size_t n) {
  std::string s;
  char b[3];
  for (std::size_t i = 0; i < n; ++i) {
    std::snprintf(b, sizeof(b), "%02x", p[i]);
    s += b;
  }
  return s;
}

std::vector<std::uint8_t> Two(std::uint64_t a, std::uint64_t b) {
  std::vector<std::uint8_t> v;
  Append64(v, a);
  Append64(v, b);
  return v;
}

std::vector<std::uint8_t> Data(std::uint64_t party, std::uint64_t member, const std::string& json) {
  std::vector<std::uint8_t> v = Two(party, member);
  Append32(v, 1);
  Append32(v, static_cast<std::uint32_t>(json.size()));
  v.insert(v.end(), json.begin(), json.end());
  return v;
}

bool Feed(Party& p, std::uint64_t s, const std::vector<std::uint8_t>& v) { return p.Feed(s, v.data(), v.size(), 1000); }

std::vector<EventKind> Kinds(Party& p) {
  std::vector<EventKind> out;
  for (const Event& e : p.DrainEvents()) out.push_back(e.kind);
  return out;
}

Party Leading() {
  Party p;
  p.SetSelf(kMe);
  p.Create();
  Feed(p, sym::kCreateSuccess, Two(kParty, kMe));
  p.DrainEvents();
  return p;
}

}  // namespace

TEST(PartyRules, Sha1KnownVector) {
  const std::string abc = "abc";
  const auto d = Sha1(reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size());
  EXPECT_EQ(Hex(d.data(), d.size()), "a9993e364706816aba3e25717850c26c9cd0d89d");
}

TEST(PartyRules, PlayerUuidMatchesAtlas) {
  // Atlas: uuid.NewV5(uuid.Nil, "OVR-ORG-123456789012345678").
  const Uuid u = PlayerUuid(kMe);
  EXPECT_EQ(Hex(u.data(), u.size()), "975a683bf83c544bb33adba80a1f00e0");
}

TEST(PartyRules, PayloadLayouts) {
  const Uuid me = PlayerUuid(kMe);
  const Message s = Standard(sym::kJoin, me, kParty);
  ASSERT_EQ(s.bytes.size(), 0x28u);
  EXPECT_EQ(Read64(s.bytes.data()), 0u);
  EXPECT_EQ(Hex(s.bytes.data() + 8, 16), Hex(me.data(), 16));
  EXPECT_EQ(Read64(s.bytes.data() + 0x18), 0u);
  EXPECT_EQ(Read64(s.bytes.data() + 0x20), kParty);

  const Message t = Targeted(sym::kKick, me, PlayerUuid(kAlice), 1);
  ASSERT_EQ(t.bytes.size(), 0x30u);
  EXPECT_EQ(Hex(t.bytes.data(), 16), Hex(me.data(), 16));
  EXPECT_EQ(Hex(t.bytes.data() + 16, 16), Hex(PlayerUuid(kAlice).data(), 16));
  EXPECT_EQ(Read32(t.bytes.data() + 0x28), 1u);

  const Message d = DataUpdate(me, kScopeMember, 7, "{\"a\":1}");
  ASSERT_EQ(d.bytes.size(), 0x28u + 8 + 7);
  EXPECT_EQ(Read64(d.bytes.data() + 0x20), kScopeMember);
  EXPECT_EQ(Read32(d.bytes.data() + 0x28), 7u);
  EXPECT_EQ(Read32(d.bytes.data() + 0x2C), 7u);
}

TEST(PartyRules, JoinFailureCodes) {
  EXPECT_EQ(GameJoinFailure(1), 1u);
  EXPECT_EQ(GameJoinFailure(2), 4u);
  EXPECT_EQ(GameJoinFailure(3), 3u);
  EXPECT_EQ(GameJoinFailure(5), 5u);
  EXPECT_EQ(GameJoinFailure(9), 0u);
}

TEST(PartyRules, CreateNeedsSelfAndOnlyOnce) {
  Party p;
  EXPECT_TRUE(p.Create().empty());
  p.SetSelf(kMe);
  const auto out = p.Create();
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].symbol, sym::kCreate);
  EXPECT_TRUE(p.Creating());
  EXPECT_TRUE(p.Create().empty());
  ASSERT_TRUE(Feed(p, sym::kCreateSuccess, Two(kParty, kMe)));
  EXPECT_EQ(Kinds(p), std::vector<EventKind>{EventKind::kCreated});
  EXPECT_EQ(p.PartyId(), kParty);
  EXPECT_TRUE(p.IsHost());
  EXPECT_EQ(p.MemberCount(), 1u);
  EXPECT_EQ(p.MemberIdAt(0), kMe);
}

TEST(PartyRules, InviteOutsidePartyCreatesThenHandsOut) {
  Party p;
  p.SetSelf(kMe);
  const auto out = p.QueueInvite("alice-user");
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].symbol, sym::kCreate);
  EXPECT_TRUE(p.QueueInvite("bob-user").empty());  // already creating
  EXPECT_TRUE(p.ReadyInvites().empty());
  Feed(p, sym::kCreateSuccess, Two(kParty, kMe));
  EXPECT_EQ(p.ReadyInvites(), (std::vector<std::string>{"alice-user", "bob-user"}));
  EXPECT_TRUE(p.ReadyInvites().empty());
}

TEST(PartyRules, CreateFailureDropsQueuedInvites) {
  Party p;
  p.SetSelf(kMe);
  p.QueueInvite("alice-user");
  const std::vector<std::uint8_t> code{1};
  Feed(p, sym::kCreateFailure, code);
  EXPECT_FALSE(p.Creating());
  EXPECT_EQ(Kinds(p), std::vector<EventKind>{EventKind::kCreateFailed});
  p.Create();
  Feed(p, sym::kCreateSuccess, Two(kParty, kMe));
  EXPECT_TRUE(p.ReadyInvites().empty());
}

TEST(PartyRules, AcceptInviteJoinsWithMembersFromData) {
  Party p;
  p.SetSelf(kMe);
  ASSERT_TRUE(Feed(p, sym::kInviteNotify, Two(kParty, kAlice)));
  EXPECT_EQ(Kinds(p), std::vector<EventKind>{EventKind::kInviteReceived});
  ASSERT_EQ(p.InviteCount(), 1u);
  EXPECT_EQ(p.InviteAt(0)->senderId, kAlice);

  const auto out = p.AcceptInvite(0);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].symbol, sym::kInviteResponse);
  EXPECT_EQ(Hex(out[0].bytes.data() + 16, 16), Hex(PlayerUuid(kAlice).data(), 16));
  EXPECT_EQ(Read32(out[0].bytes.data() + 0x28), 1u);
  EXPECT_TRUE(p.Joining());
  EXPECT_EQ(p.RoomId(), kParty);
  EXPECT_EQ(p.InviteCount(), 0u);

  // Atlas: the party's data, then each other member's, then the join success.
  Feed(p, sym::kDataNotify, Data(kParty, 0, "{\"lobbyid\":\"x\"}"));
  Feed(p, sym::kDataNotify, Data(kParty, kAlice, "{\"headsettype\":1}"));
  Feed(p, sym::kDataNotify, Data(kParty, kBob, "{\"headsettype\":2}"));
  EXPECT_TRUE(p.DrainEvents().empty());
  Feed(p, sym::kJoinSuccess, Two(kParty, kAlice));
  EXPECT_EQ(Kinds(p), std::vector<EventKind>{EventKind::kJoined});
  EXPECT_FALSE(p.Joining());
  EXPECT_FALSE(p.IsHost());
  EXPECT_EQ(p.Owner(), kAlice);
  ASSERT_EQ(p.MemberCount(), 3u);
  EXPECT_EQ(p.MemberIdAt(0), kMe);
  EXPECT_EQ(p.MemberIdAt(1), kAlice);
  EXPECT_EQ(*p.Members()[1].data, "{\"headsettype\":1}");
  ASSERT_NE(p.PartyData(), nullptr);
  EXPECT_EQ(*p.PartyData(), "{\"lobbyid\":\"x\"}");
}

TEST(PartyRules, JoinFailureReportsGameCode) {
  Party p;
  p.SetSelf(kMe);
  p.Join(kParty);
  std::vector<std::uint8_t> v;
  Append64(v, kParty);
  v.push_back(5);
  ASSERT_TRUE(Feed(p, sym::kJoinFailure, v));
  const auto ev = p.DrainEvents();
  ASSERT_EQ(ev.size(), 1u);
  EXPECT_EQ(ev[0].kind, EventKind::kJoinFailed);
  EXPECT_EQ(ev[0].value, 5u);
  EXPECT_FALSE(p.Joining());
  EXPECT_FALSE(p.InParty());
}

TEST(PartyRules, RepliesForAnotherPartyAreIgnored) {
  Party p = Leading();
  Feed(p, sym::kJoinNotify, Two(kParty + 1, kAlice));
  Feed(p, sym::kJoinSuccess, Two(kParty + 1, kAlice));
  EXPECT_TRUE(p.DrainEvents().empty());
  EXPECT_EQ(p.MemberCount(), 1u);
  EXPECT_EQ(p.PartyId(), kParty);
}

TEST(PartyRules, MembersJoinLeaveAndCapAtTen) {
  Party p = Leading();
  for (std::uint64_t i = 1; i <= 12; ++i) Feed(p, sym::kJoinNotify, Two(kParty, 1000 + i));
  EXPECT_EQ(p.MemberCount(), kMaxMembers);
  const auto ev = p.DrainEvents();
  ASSERT_EQ(ev.size(), kMaxMembers - 1);
  EXPECT_EQ(ev[0].kind, EventKind::kMemberJoined);
  EXPECT_EQ(ev[0].value, 1u);
  EXPECT_EQ(ev.back().value, 9u);
  EXPECT_FALSE(p.Joinable(true));  // full

  Feed(p, sym::kLeaveNotify, Two(kParty, 1003));
  const auto left = p.DrainEvents();
  ASSERT_EQ(left.size(), 1u);
  EXPECT_EQ(left[0].kind, EventKind::kMemberLeft);
  EXPECT_EQ(left[0].id, 1003u);
  EXPECT_EQ(p.MemberCount(), kMaxMembers - 1);
  EXPECT_FALSE(p.HasMember(1003));
  EXPECT_TRUE(p.Joinable(true));
  EXPECT_FALSE(p.Joinable(false));  // the host closed it
}

TEST(PartyRules, KickAndPassTargetMembers) {
  Party p = Leading();
  Feed(p, sym::kJoinNotify, Two(kParty, kAlice));
  EXPECT_TRUE(p.Kick(0).empty());
  const auto kick = p.Kick(1);
  ASSERT_EQ(kick.size(), 1u);
  EXPECT_EQ(kick[0].symbol, sym::kKick);
  EXPECT_EQ(Hex(kick[0].bytes.data() + 16, 16), Hex(PlayerUuid(kAlice).data(), 16));
  const auto pass = p.Pass(1);
  ASSERT_EQ(pass.size(), 1u);
  EXPECT_EQ(pass[0].symbol, sym::kPass);

  p.DrainEvents();
  Feed(p, sym::kPassNotify, Two(kParty, kAlice));
  EXPECT_EQ(Kinds(p), std::vector<EventKind>{EventKind::kHostChanged});
  EXPECT_FALSE(p.IsHost());
  EXPECT_TRUE(p.Kick(1).empty());  // not the host any more
  EXPECT_TRUE(p.Share(kScopeParty, "{}").empty());
  EXPECT_EQ(p.Share(kScopeMember, "{}").size(), 1u);
}

TEST(PartyRules, KickedOrLeftClearsParty) {
  Party p = Leading();
  Feed(p, sym::kJoinNotify, Two(kParty, kAlice));
  Feed(p, sym::kKickNotify, Two(kParty, kMe));
  EXPECT_EQ(Kinds(p).back(), EventKind::kKicked);
  EXPECT_FALSE(p.InParty());
  EXPECT_EQ(p.MemberCount(), 1u);

  Party q = Leading();
  ASSERT_EQ(q.Leave().size(), 1u);
  EXPECT_TRUE(q.Leave().empty());
  const std::vector<std::uint8_t> none{0};
  Feed(q, sym::kLeaveSuccess, none);
  EXPECT_EQ(Kinds(q), std::vector<EventKind>{EventKind::kLeft});
  EXPECT_FALSE(q.InParty());
}

TEST(PartyRules, DataForMembersAndParty) {
  Party p = Leading();
  Feed(p, sym::kJoinNotify, Two(kParty, kAlice));
  p.DrainEvents();
  Feed(p, sym::kDataNotify, Data(kParty, kAlice, "{\"x\":1}"));
  auto ev = p.DrainEvents();
  ASSERT_EQ(ev.size(), 1u);
  EXPECT_EQ(ev[0].kind, EventKind::kMemberUpdated);
  EXPECT_EQ(ev[0].value, 1u);
  Feed(p, sym::kDataNotify, Data(kParty, 0, "{\"p\":1}"));  // our own party's data: the host keeps its own
  EXPECT_TRUE(p.DrainEvents().empty());
  EXPECT_EQ(p.PartyData(), nullptr);
  Feed(p, sym::kDataNotify, Data(kParty, kBob, "{}"));  // data ahead of its join notice
  ev = p.DrainEvents();
  ASSERT_EQ(ev.size(), 1u);
  EXPECT_EQ(ev[0].kind, EventKind::kMemberJoined);
  EXPECT_EQ(ev[0].value, 2u);
  auto shortData = Data(kParty, kBob, "{}");
  shortData.pop_back();
  EXPECT_FALSE(Feed(p, sym::kDataNotify, shortData));
}

TEST(PartyRules, InvitesNewestFirstOnePerParty) {
  Party p;
  p.SetSelf(kMe);
  Feed(p, sym::kInviteNotify, Two(1, kAlice));
  Feed(p, sym::kInviteNotify, Two(2, kBob));
  Feed(p, sym::kInviteNotify, Two(1, kAlice));  // again: moves to the front
  ASSERT_EQ(p.InviteCount(), 2u);
  EXPECT_EQ(p.InviteAt(0)->partyId, 1u);
  EXPECT_EQ(p.InviteAt(1)->partyId, 2u);
  const auto out = p.DismissInvite(1);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(Read32(out[0].bytes.data() + 0x28), 0u);
  ASSERT_EQ(p.InviteCount(), 1u);
  EXPECT_EQ(p.InviteAt(0)->partyId, 1u);
  EXPECT_EQ(p.InviteAt(5), nullptr);
}

TEST(PartyRules, LockFollowsServer) {
  Party p = Leading();
  EXPECT_EQ(p.SetLocked(true).size(), 1u);
  std::vector<std::uint8_t> v;
  Append64(v, kParty);
  Feed(p, sym::kLockNotify, v);
  EXPECT_TRUE(p.Locked());
  EXPECT_TRUE(p.SetLocked(true).empty());
  EXPECT_EQ(p.SetLocked(false)[0].symbol, sym::kUnlock);
}

TEST(PartyRules, UnknownSymbolsAreNotConsumed) {
  Party p;
  EXPECT_FALSE(p.Feed(kLoginSuccess, nullptr, 0));
  EXPECT_TRUE(IsPartyReply(sym::kKickSuccess));
  EXPECT_FALSE(IsPartyReply(sym::kCreate));
}

TEST(PartyRules, LockRequestGoesOutOnceUntilAnswered) {
  Party p = Leading();
  EXPECT_EQ(p.SetLocked(true).size(), 1u);
  EXPECT_TRUE(p.SetLocked(true).empty());
  const std::vector<std::uint8_t> code{1};
  Feed(p, sym::kLockFailure, code);
  EXPECT_EQ(p.SetLocked(true).size(), 1u);
}

TEST(PartyRules, LostConnectionEndsParty) {
  Party p = Leading();
  Feed(p, sym::kJoinNotify, Two(kParty, kAlice));
  p.DrainEvents();
  p.Lost();
  EXPECT_EQ(Kinds(p), std::vector<EventKind>{EventKind::kLeft});
  EXPECT_FALSE(p.InParty());
  p.Lost();
  EXPECT_TRUE(p.DrainEvents().empty());
}

TEST(PartyRules, SymbolsAreAtlasTokens) {
  EXPECT_EQ(evr::Symbol64("SNSPartyCreateRequest"), sym::kCreate);
  EXPECT_EQ(evr::Symbol64("SNSPartyJoinSuccess"), sym::kJoinSuccess);
  EXPECT_EQ(evr::Symbol64("SNSPartyInviteResponse"), sym::kInviteResponse);
  EXPECT_EQ(evr::Symbol64("SNSPartyDataNotify"), sym::kDataNotify);
  EXPECT_EQ(evr::Symbol64("SNSPartyKickNotify"), sym::kKickNotify);
}
