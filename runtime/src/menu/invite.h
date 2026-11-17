// CREATE GAME / JOIN VIA CODE wizard -> Atlas invite codes (PC).
#pragma once

#include <string>

namespace evr_invite {

enum class Kind { None, Create, Join };

/// The newest answer from Atlas.
struct Result {
  Kind kind = Kind::None;
  bool ok = false;
  std::string error;      // Atlas's refusal token (invalid, expired, closed, denied, limit, rate, unavailable, no_history)
  std::string code;       // create: the new code; join: the code typed
  std::string matchId;
  std::string expiresAt;  // create
  std::string mode, level;  // join
};

/// Hooks the game's remote-log submit to catch the wizard's clicks. `dev` logs the whole click line.
bool Install(bool dev);

/// Any thread.
Result Last();

}  // namespace evr_invite
