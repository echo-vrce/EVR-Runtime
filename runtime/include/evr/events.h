// evr/events.h: see every component event the game sends (button clicks, page changes), before the game handles it.
//
// An event is the game's SComponentEvent, 0x28 bytes (layout below, from our reading of echovr.exe's senders at
// 0x14050fc90 / 0x14050fe90 / 0x140510060, which build it on their stack). A listener may rewrite the event's symbol
// or key in place: the game then handles the rewritten event.
#pragma once

#include <cstddef>
#include <cstdint>

namespace evr {

namespace event_layout {
inline constexpr std::size_t kEventBytes = 0x28;
inline constexpr std::size_t kSymbolOffset = 0x00;     // CSymbol64: the event's name (e.g. evt_play_btn_clicked)
inline constexpr std::size_t kSpaceIdOffset = 0x08;    // *(space + 0x58) of the sending space
inline constexpr std::size_t kSenderOffset = 0x10;     // low 16 bits: the sender's index in the space's actor table
inline constexpr std::size_t kComponentOffset = 0x18;  // CSymbol64: the key (often the element or component clicked)
inline constexpr std::uint16_t kNoSender = 0xFFFF;
// The space's actor table: [space + 0x370] -> table, [table + 0x80] -> u64 names, [table + 0xa8] -> count.
inline constexpr std::size_t kSpaceActorTableOffset = 0x370;
inline constexpr std::size_t kTableNamesOffset = 0x80;
inline constexpr std::size_t kTableCountOffset = 0xA8;
}  // namespace event_layout

/// `space` is the game space (CncaGameSpace*), `event` the SComponentEvent's address. Game-loop thread.
using EventListener = void (*)(std::uint64_t space, std::uint64_t event);

/// Adds a listener; register before InstallEventHook. Never removed.
void OnComponentEvent(EventListener fn);

/// The name (CSymbol64) of the actor that sent `event` in `space`, 0 when it has none or cannot be read.
std::uint64_t EventSender(std::uint64_t space, std::uint64_t event);

/// Dev aid: log each distinct (symbol, sender, key) once, up to `limit` lines ([EVR.EVENT] seen ...).
void LogDistinctEvents(int limit);

/// Sends `event` (an SComponentEvent, kEventBytes) to `space` and the spaces it holds, as the game's own
/// 0x14050fb60 does (flags 1, as real call sites pass). Listeners see it first. Game-loop thread. False when the
/// sender is not hooked.
bool SendComponentEvent(std::uint64_t space, const void* event);

/// The ident word of the actor at `index` (event_layout::kSenderOffset).
inline constexpr std::uint64_t SenderWord(std::uint16_t index) { return 0x0000FFFFFFFF0000ULL | index; }

/// Hooks the game's three event senders. Each event reaches the listeners once, however many spaces it is sent to.
bool InstallEventHook();

}  // namespace evr
