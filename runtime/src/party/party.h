// party/party.h: tablet parties on PC. The game gets our own Social object (the stock PC build has none, so the
// PARTY tab says "Parties are currently disabled."), and its party actions reach Atlas as SNS party messages on the
// login websocket. What is sent and what replies do: party_rules.h. Hooks and the object: party_pc.cpp.
#pragma once

namespace evr_party {

/// Hooks the Social accessor and the websocket dispatcher (each checked against its bytes first, and the feature
/// skipped, logged, on a mismatch) and registers a frame callback. Call before InstallFrameHook, before the game
/// initialises its net game.
void Install();

/// Whether the Social object is in place: the login then declares social level 1, so Atlas sends party data.
bool Installed();

}  // namespace evr_party
