// evr/ui_text.h: write text into the game's menu canvases.
//
// A target is (actor, item): the actor whose canvas component holds the text (its name symbol in the level) and
// the canvas item (its symbol). Posting is allowed from any thread; the write happens on the game-loop thread in
// the next frame in which the actor exists. A later post to the same target replaces a pending one.
#pragma once

#include <cstdint>
#include <string>

namespace evr {

enum class UiTextResult { Queued, Refused };

/// Queues `text` for (actor, item). Refused when the text is longer than 127 bytes or holds a byte the game's font
/// cannot draw (outside printable ASCII).
UiTextResult PostUiText(std::uint64_t actor, std::uint64_t item, const std::string& text);

/// Called for every text the game itself sets (not ours), on the game-loop thread. Return a replacement (it must
/// stay alive until the next call) or nullptr to keep `text`. Register at runtime start (before the game loop runs);
/// never removed.
using UiTextFilter = const char* (*)(std::uint64_t item, const char* text);
void OnUiTextSet(UiTextFilter fn);

/// Hooks the game's text setter and registers the frame callback that drains the queue.
bool InstallUiText();

}  // namespace evr
