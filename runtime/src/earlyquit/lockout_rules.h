// earlyquit/lockout_rules.h: what the early-quit lockout patch decides, kept apart from the hooks
// (lockout_pc.cpp) so it is tested without the game.
//
// What Atlas sends (server/evr/login_earlyquitupdatenotification.go), 0x38 bytes:
//   +0x18 i64 stamp    every update to a player carries a larger one than the last
//   +0x20 i64 expiry   the lockout's end, Unix seconds; 0 (or -1) for none
//   +0x28 i32 early quits, +0x2c i32 steady matches, +0x30 i32 steady early quits, +0x34 u8 penalty level,
//   +0x35 u8 steady level
//
// What the stock game does with it. CR15NetGame::SetEarlyQuitPenaltyLevel (0x1401ae2e0) takes the update only when
// the stamp it holds at netGame+0x64820 is -1 or smaller than the new one, then stores the counters and always writes
// -1 as the countdown's end (netGame+0x64828). The scripts read the countdown only through
// CR15NetEarlyQuitPenaltyExpression (0x140d96190), whose "countdown active" output is hard-coded to false. So the
// stock client never shows a lockout. We put the expiry back after the setter, set "countdown running" (flags bit
// 45, which CR15NetGame::Update clears at the end), and make the expression's active output follow the seconds left.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace evr_earlyquit {

inline constexpr std::size_t kUpdateBytes = 0x38;

struct Update {
  std::int64_t stamp = 0;
  std::int64_t expiry = 0;
  std::int32_t earlyQuits = 0;
  std::int32_t steadyMatches = 0;
  std::int32_t steadyEarlyQuits = 0;
  std::uint8_t penaltyLevel = 0;
  std::uint8_t steadyLevel = 0;
};

/// The update's fields from its wire bytes; false when there are fewer than kUpdateBytes.
inline bool ReadUpdate(const std::uint8_t* p, std::size_t n, Update& u) {
  if (p == nullptr || n < kUpdateBytes) return false;
  std::memcpy(&u.stamp, p + 0x18, 8);
  std::memcpy(&u.expiry, p + 0x20, 8);
  std::memcpy(&u.earlyQuits, p + 0x28, 4);
  std::memcpy(&u.steadyMatches, p + 0x2c, 4);
  std::memcpy(&u.steadyEarlyQuits, p + 0x30, 4);
  u.penaltyLevel = p[0x34];
  u.steadyLevel = p[0x35];
  return true;
}

/// Whether the game's setter takes an update with `stamp` while it holds `held` (its own gate, 0x1401ae2f0).
inline constexpr bool GameTakes(std::int64_t held, std::int64_t stamp) { return held == -1 || held < stamp; }

/// What to write after the setter ran. `take` false: leave the game's state alone.
struct Lockout {
  bool take = false;
  std::int64_t expiry = -1;  // netGame+0x64828; -1 for none
  bool running = false;      // flags bit 45
};

/// `held` is the stamp the game held before the call; `expiry` what the update carried; `now` Unix seconds.
inline constexpr Lockout AfterUpdate(std::int64_t held, std::int64_t stamp, std::int64_t expiry, std::int64_t now) {
  if (!GameTakes(held, stamp)) return {};
  if (expiry > now) return {true, expiry, true};
  return {true, -1, false};  // none, or already over: a clear
}

/// The expression's "lockoutcountdownactive" from its "lockoutcountdownsec".
inline constexpr bool CountdownActive(std::int64_t secondsLeft) { return secondsLeft > 0; }

/// The feature-flag byte the game keeps. The stock callback stores `sent & 0xdd`, which drops bit 1 (the lockout
/// timer and FIND MATCH lockout) and bit 5 (the EARLY QUIT INFO pages); we patch the mask to 0xff.
inline constexpr std::uint8_t kStockFlagMask = 0xdd;
inline constexpr std::uint8_t kOurFlagMask = 0xff;
inline constexpr std::uint8_t KeptFlags(std::uint8_t sent, std::uint8_t mask) { return sent & mask; }

// ---- The tablet notice (EARLY QUIT NOTICE / LOCKOUT NOTICE) ----
//
// The tablet's ReturnToHome (script library 18000621255c6ea5, node dfb343f6) asks feature bit 5 first: off goes
// home; on goes to a notice page (lockoutcountdownactive picks LOCKOUT NOTICE, else EARLY QUIT NOTICE). The notice's
// OK runs ReturnToHome, so with bit 5 on it can never leave the notice. Bit 5 also turns on the scoreboard's loyalty
// values and the early-quit labels, so it stays on; only that one node is made to read "a notice is pending".

/// Whether an update adds an early quit. The first update after launch (held stamp -1) carries the player's lifetime
/// count while the game still holds 0, so it is never new: a player with 10 old early quits got "0 -> 10" at every
/// login, and the pending notice left the lobby's tablets (Public Watch) on an empty notice page (2026-11-26).
inline constexpr bool NewEarlyQuit(std::int64_t held, std::int32_t quitsBefore, std::int32_t quitsAfter) {
  return held != -1 && quitsAfter > quitsBefore;
}

/// A notice is due after an update the game took that starts a lockout or adds an early quit (NewEarlyQuit).
inline constexpr bool NoticeDue(bool taken, bool lockoutRunning, bool newEarlyQuit) {
  return taken && (lockoutRunning || newEarlyQuit);
}

/// The notice's buttons: right_button_press (OK) and left_button_press (INFO), sent with the notice page's key.
inline constexpr std::uint64_t kNoticeOk = 0x9E171AFD326DD5B9ULL;
inline constexpr std::uint64_t kNoticeInfo = 0x052C36430F4094CDULL;
inline constexpr std::uint64_t kNoticeButtonKey = 0x3375A9C5CCDCC06AULL;
inline constexpr bool IsNoticeButton(std::uint64_t symbol, std::uint64_t key) {
  return key == kNoticeButtonKey && (symbol == kNoticeOk || symbol == kNoticeInfo);
}

/// Node dfb343f6's last instruction picks its result: CMOVE RAX,RCX (bit 5 off -> "false"). With MOV RAX,RCX; NOP
/// in its place the node always answers "false" and ReturnToHome goes home.
inline constexpr std::uint8_t kNodeAsks[4] = {0x48, 0x0F, 0x44, 0xC1};  // cmove rax,rcx
inline constexpr std::uint8_t kNodeSaysNo[4] = {0x48, 0x89, 0xC8, 0x90};  // mov rax,rcx; nop
inline constexpr const std::uint8_t* NodeBytes(bool noticePending) { return noticePending ? kNodeAsks : kNodeSaysNo; }

// ---- The main menu's EARLY QUIT page ----
//
// The main menu (script library c18bafab7b6faa62) shows earlyquit_quit_notice_page when feature bit 5 AND
// showearlyquitwarning (flags bit 46) are set. Something in the client sets bit 46 by itself (seen 2026-11-17 after a
// failed lobby join with no early quit charged), so the runtime keeps bit 46 only while Atlas has reported a new
// early quit; the page's Continue (SetEarlyQuitWarningShown) clears it, which ends that report.
inline constexpr std::uint64_t kWarningBit = 1ULL << 46;

enum class WarningStep {
  kNone,          // bit and report agree
  kClear,         // bit set without a report from Atlas: clear it
  kAcknowledged,  // report pending but the bit was cleared (the player pressed Continue): drop the report
};

inline constexpr WarningStep NextWarningStep(bool bitSet, bool atlasReported) {
  if (bitSet && !atlasReported) return WarningStep::kClear;
  if (!bitSet && atlasReported) return WarningStep::kAcknowledged;
  return WarningStep::kNone;
}

// ---- The FIND MATCH bar ----
//
// The matchmaking bar's library (17d77f27d465760b) has two branch nodes (RVA 0x11eb0 and 0x12680) that call a bool
// node, `(input || ctx[+0x68]->+0xe2) && feature bit 1`, and leave by the other pin when it is true. The stock client
// drops bit 1, so they always took the first pin; with bit 1 kept, pressing Find Match took the other one and the
// bar lost its FINDING MATCH top bar, timer and cancel (seen 2026-11-17, no lockout running). We keep bit 1 for the
// other menus and turn only the two calls into "false": XOR EAX,EAX and three NOPs in place of the 5-byte CALL.
struct BarNode {
  std::size_t rva;
  std::uint8_t call[5];  // E8 rel32 to the bool node
};
inline constexpr BarNode kBarNodes[] = {
    {0x11EB0, {0xE8, 0x37, 0x33, 0x01, 0x00}},  // -> 0x251f0
    {0x12680, {0xE8, 0xC7, 0x30, 0x01, 0x00}},  // -> 0x25750
};
inline constexpr std::size_t kBarCallAt = 4;  // after SUB RSP,0x28
inline constexpr std::uint8_t kBarNodeHead[kBarCallAt] = {0x48, 0x83, 0xEC, 0x28};
// What follows the call: MOV RCX,<pin true>, the stock first pin.
inline constexpr std::uint8_t kBarNodeTail[10] = {0x48, 0xB9, 0x39, 0xAA, 0x68, 0xC9, 0x83, 0x99, 0xDC, 0x54};
inline constexpr std::uint8_t kBarSaysNo[5] = {0x31, 0xC0, 0x90, 0x90, 0x90};  // xor eax,eax; nop x3
/// The call's target RVA, from the node's RVA and its rel32.
inline constexpr std::size_t BarCallTarget(const BarNode& n) {
  const std::int32_t rel = static_cast<std::int32_t>(n.call[1] | (n.call[2] << 8) | (n.call[3] << 16) |
                                                     (static_cast<std::uint32_t>(n.call[4]) << 24));
  return n.rva + kBarCallAt + 5 + rel;
}

// ---- The main menu's EARLY QUIT page during a lockout ----
//
// The page's condition node (library c18bafab7b6faa62, RVA 0x68d0) answers `bit 5 && pin`, and the stock menu shows it
// whenever a lockout countdown runs. For a moderator /lockout nobody quit early, so the page is shown only while Atlas
// has reported a new early quit this session (the same report that keeps bit 46). The node's second test and the
// "show" byte share one 8-byte-aligned word at RVA 0x6958: CMP byte [rsp+0x39],0; JE +0x0b; MOV AL,(1). In its place,
// JMP +0x10 (to XOR AL,AL) and NOPs, swapped with one aligned store.
inline constexpr const char* kMenuLibrary = "c18bafab7b6faa62.dll";
inline constexpr std::size_t kMenuPageAt = 0x6958;
inline constexpr std::uint8_t kMenuPageBefore[7] = {0x80, 0x7C, 0x24, 0x2C, 0x00, 0x74, 0x12};  // the bit-5 test
inline constexpr std::uint8_t kMenuPageAfter[12] = {0x01, 0x48, 0x81, 0xC4, 0x60, 0x06,
                                                    0x00, 0x00, 0x5B, 0xC3, 0x32, 0xC0};
inline constexpr std::uint8_t kMenuPageShows[8] = {0x80, 0x7C, 0x24, 0x39, 0x00, 0x74, 0x0B, 0xB0};
inline constexpr std::uint8_t kMenuPageHides[8] = {0xEB, 0x10, 0x90, 0x90, 0x90, 0x90, 0x90, 0xB0};
inline constexpr const std::uint8_t* MenuPageBytes(bool atlasReportedQuit) {
  return atlasReportedQuit ? kMenuPageShows : kMenuPageHides;
}

// ---- The main menu's EARLY QUIT card ----
//
// The card ("View Early Quit and Loyalty Info", library 346ee5436c2e559e) does everything behind feature bit 5:
// five inline tests (CMP byte [rsp+0x2c],0; JE) and two bool nodes (MOVZX EAX,byte [rsp+0x2c]; ret). The user wants
// the card never shown, lockout or not, so every test there reads "bit 5 off": JE becomes JMP, MOVZX becomes
// XOR EAX,EAX + NOPs. Bit 5 stays on for the tablet's notice and the FIND MATCH bar, which live in other libraries.
inline constexpr const char* kCardLibrary = "346ee5436c2e559e.dll";
inline constexpr std::uint8_t kCardTest[5] = {0x80, 0x7C, 0x24, 0x2C, 0x00};  // cmp byte [rsp+0x2c],0; then JE rel8
inline constexpr std::size_t kCardJumps[] = {0x519F, 0x5247, 0x52F4, 0x53C8, 0x5494};  // the JE bytes
inline constexpr std::uint8_t kCardJe = 0x74, kCardJmp = 0xEB;
inline constexpr std::size_t kCardNodes[] = {0x6211, 0x62D1};
inline constexpr std::uint8_t kCardNodeReads[5] = {0x0F, 0xB6, 0x44, 0x24, 0x2C};  // movzx eax,byte [rsp+0x2c]
inline constexpr std::uint8_t kCardNodeSaysNo[5] = {0x31, 0xC0, 0x90, 0x90, 0x90};  // xor eax,eax; nop x3

// ---- FIND MATCH after a lockout ----
//
// Once the bar has shown a lockout in a lobby visit, Find Match enables the bar's actors and the tablet's enable then
// switches them off again ~35 ms later (seen 2026-11-17). Both run from the engine's deferred-command queue
// (drained at 0x1401d3742; the script thunks 0x1404eda40 / 0x1404ecd30 are pushed by 0x1403faee0), so the stack
// does not name the script that queued the disable. Re-raising the penalty event did not bring the bar back.
// Workaround: when the bar's main actor goes off within kBarBounceMs of going on, after a lockout was shown, switch
// the bar's actors back on (reasons = 1, what the scripts' thunk set) on the next frame.
inline constexpr std::uint64_t kBarActor = 0x20207787FBCCF88DULL;
inline constexpr std::uint64_t kBarActors[] = {0x20207787FBCCF88DULL, 0x9ADA32D27410ED7BULL, 0xF4411BA1EFC9B76DULL};
inline constexpr std::uint64_t kBarBounceMs = 250;
inline constexpr bool BarBounced(bool lockoutSeen, std::uint64_t enabledAtMs, std::uint64_t disabledAtMs) {
  return lockoutSeen && enabledAtMs != 0 && disabledAtMs >= enabledAtMs && disabledAtMs - enabledAtMs <= kBarBounceMs;
}

}  // namespace evr_earlyquit
