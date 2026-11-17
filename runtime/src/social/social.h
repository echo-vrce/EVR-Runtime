// social/social.h: the player's friends and party, from Atlas (Nakama REST with the player's own Bearer token).
//
// The token comes from Atlas's POST /evr/runtime/token with the discordid and password the game's own login already
// sends (the loginservice_host of the game's _local\config.json). The REST base is that file's apiservice_host. The
// password is read, sent to that endpoint and nothing else; it is never logged.
//
// The roster is fetched by a worker (GET /v2/friend, mutual friends only) and adopted on the game-loop thread, so the
// pointers a Roster hands out stay valid until the next Adopt on that thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace evr_social {

struct Friend {
  std::uint64_t id = 0;   // Symbol64 of the Nakama user id: the panel's key for a friend
  std::string userId;     // Nakama user id
  std::string name;       // display name, else username
  std::string avatarUrl;
  bool online = false;
};

class Roster {
 public:
  std::uint32_t Count() const { return static_cast<std::uint32_t>(friends_.size()); }
  std::uint32_t Online() const;
  const char* NameAt(std::uint32_t row) const { return row < Count() ? friends_[row].name.c_str() : ""; }
  const char* StatusTextAt(std::uint32_t) const { return ""; }  // "ONLINE" / "OFFLINE" until presence carries more
  bool OnlineAt(std::uint32_t row) const { return row < Count() && friends_[row].online; }
  bool IdAt(std::uint32_t row, std::uint64_t* id) const;
  std::uint64_t PartyIdAt(std::uint32_t) const { return 0; }
  const Friend* Find(std::uint64_t id) const;

  std::vector<Friend> friends_;
};

/// Starts the roster worker (fetches now, then every 20 s and when RefreshSoon is called).
void Start();

/// Game-loop thread: takes the newest fetched roster. Call once per frame before reading Friends().
void Adopt();

/// The roster the game-loop thread reads.
const Roster& Friends();

/// Any thread: drops a friend at the next Adopt (after an unfriend) and fetches again.
void Remove(std::uint64_t id);
void RefreshSoon();

/// Worker threads only (they block): the REST base ("" when the game's config has none) and a valid access token
/// ("" when there is none).
std::string ServiceBase();
std::string Token();

/// Any thread: the player's account id (the discordid of the game's login config), 0 when there is none. Never
/// logged.
std::uint64_t AccountId();

/// Worker threads only: POST /v2/rpc/<id>?unwrap with `json` and the player's token. Not ok when there is no base or
/// token. On a refusal Atlas answers {"error","message","code"}; the message is the refusal's token.
struct RpcReply {
  bool ok = false;     // 200
  long status = 0;
  std::string body;    // the RPC's answer on 200, else Atlas's error object
  std::string error;   // the error's message, when there is one
};
RpcReply CallRpc(const std::string& id, const std::string& json);

/// Worker threads only: invites the friend to the player's party through Atlas party/invite. Refusals (logged):
/// not_in_game, not_in_party, target_offline.
RpcReply InviteToParty(const std::string& userId);

/// The profile pop-up's button text for an invite's outcome.
const char* PartyInviteLabel(const RpcReply& r);

}  // namespace evr_social
