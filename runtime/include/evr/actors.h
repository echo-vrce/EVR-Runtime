// evr/actors.h: turn the game's actors on and off, and watch the game do it (PC: our own reading of echovr.exe).
//
// The game enables and disables an actor with (CncaGameSpace* space, SActor ident, u8 reasons), a mask of why it is off (scripts use 1); EnableActorNode
// (0x1410aca70) queues these for the script it runs in. An ident's low 16 bits are the actor's index in the space's
// actor table (evr/events.h event_layout).
#pragma once

#include <cstdint>

namespace evr {

/// Called after the game enabled (`on`) or disabled an actor. `actor` is its name (CSymbol64), 0 when unknown.
/// Game-loop thread.
using ActorListener = void (*)(std::uint64_t space, std::uint64_t actor, bool on);
void OnActorEnabled(ActorListener fn);

/// Hooks the game's enable and disable. `logLimit` > 0 also logs the first `logLimit` calls ([EVR.ACTOR]).
bool InstallActorHooks(int logLimit);

/// Enables or disables the actor named `actor` in `space` (reason bit 1, the one scripts use). False when it is not in that space or
/// the hooks are not installed. Game-loop thread.
bool SetActorEnabled(std::uint64_t space, std::uint64_t actor, bool on);

/// The index of `actor` in `space`'s actor table, or -1.
int ActorIndex(std::uint64_t space, std::uint64_t actor);

}  // namespace evr
