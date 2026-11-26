// menu/wording.h: stock menu texts that name Oculus where the game is now Echo's own, and what we show instead.
// Kept apart from the hook (wording_pc.cpp) so it is tested without the game.
#pragma once

#include <cstddef>
#include <cstring>
#include <vector>

namespace evr_wording {

struct Rewrite {
  const char* stock;
  const char* ours;
  // Used in place of `ours` when a library's copy of `stock` has no room for it; never longer than `stock`.
  const char* tight = nullptr;
};

// The party tab's empty friends page (script library 30b4ca0fb0a1ee02, .rdata RVA 0x3eb18).
inline constexpr Rewrite kRewrites[] = {
    {"You have no Oculus Friends.", "You have no Echo Friends."},
    // The tablet's ECHO ARENA STATS row for ArenaTies: Atlas counts early quits there (arena has no ties). The row
    // label skips the text setter, so it is also rewritten where it sits in the script libraries.
    {"Arena Ties", "Early Quits", "Early Quit"},
    {"ARENA TIES", "EARLY QUITS", "EARLY QUIT"},
};

/// Our text for a stock one, or nullptr to keep it.
inline const char* Reword(const char* text) {
  if (text == nullptr) return nullptr;
  for (const Rewrite& r : kRewrites)
    if (std::strcmp(text, r.stock) == 0) return r.ours;
  return nullptr;
}

/// Offsets in `data` where `text` stands as a whole string: NUL (or the start) before it, NUL after it.
inline std::vector<std::size_t> FindWhole(const unsigned char* data, std::size_t size, const char* text) {
  std::vector<std::size_t> at;
  const std::size_t n = std::strlen(text);
  if (n == 0 || size < n + 1) return at;
  for (std::size_t i = 0; i + n < size; ++i)
    if ((i == 0 || data[i - 1] == 0) && data[i + n] == 0 && std::memcmp(data + i, text, n) == 0) at.push_back(i);
  return at;
}

/// What to write over the whole string `r.stock` at `data[at]`: `ours` when the zero bytes after the stock NUL
/// leave it room (its NUL lands on a byte that was already zero), else `tight`, else nullptr.
inline const char* InPlace(const unsigned char* data, std::size_t size, std::size_t at, const Rewrite& r) {
  const std::size_t stock = std::strlen(r.stock);
  for (const char* want : {r.ours, r.tight}) {
    if (want == nullptr) continue;
    const std::size_t n = std::strlen(want);
    if (n <= stock) return want;
    if (at + n >= size) continue;
    bool room = true;
    for (std::size_t i = at + stock + 1; i <= at + n; ++i) room = room && data[i] == 0;
    if (room) return want;
  }
  return nullptr;
}

}  // namespace evr_wording

namespace evr_wording {
/// Registers the text filter and the script-library string rewrites. Call before the game loop runs.
void Install();
}  // namespace evr_wording
