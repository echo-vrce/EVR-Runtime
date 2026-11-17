// earlyquit/lockout.h: the early-quit lockout countdown and EARLY QUIT INFO pages, which the stock client never shows
// whatever Atlas sends. Decisions and evidence: lockout_rules.h.
#pragma once

namespace evr_earlyquit {

/// Four pieces, each checked against the game's bytes first and skipped (logged) on a mismatch:
///  - the penalty setter: the update's expiry becomes the countdown, and the penalty event is raised again;
///  - the penalty expression: "countdown active" follows the seconds left;
///  - the feature-flag callback's mask byte: every flag Atlas sends is kept;
///  - the tablet's ReturnToHome: leaves the notice once OK or INFO is pressed (frame callback + event listener).
/// Registers frame and event callbacks: call before InstallFrameHook and InstallEventHook.
void Install();

/// The game's CR15NetGame, once the penalty setter has run at login; null before.
void* NetGame();

}  // namespace evr_earlyquit
