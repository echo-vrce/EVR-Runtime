// evr/symbol.h: the engine's 64-bit symbol hash (CSymbol64).
//
// A CRC-like mix with seed ~0: h = sx(byte) ^ T[h >> 56] ^ (h << 8), where T[i] is the carry-less product of
// 0x95AC9329AC4BC9B5 and i, shifted left one bit and truncated to 64 bits (no reduction, unlike a textbook CRC).
// Each byte is lower-cased for A-Z and sign-extended (the engine hashes signed `char`).
// Written for EVR-Runtime; checked against hashes the game itself uses (tests/test_symbol.cpp).
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace evr {

inline constexpr std::uint64_t kSymbolPoly = 0x95AC9329AC4BC9B5ULL;
inline constexpr std::uint64_t kSymbolSeed = ~0ULL;

namespace detail {
constexpr std::array<std::uint64_t, 256> MakeSymbolTable() {
  std::array<std::uint64_t, 256> t{};
  for (std::uint64_t i = 0; i < 256; ++i) {
    std::uint64_t c = 0;
    for (int b = 0; b < 8; ++b)
      if (i & (1ULL << b)) c ^= kSymbolPoly << b;
    t[i] = c << 1;
  }
  return t;
}
inline constexpr auto kSymbolTable = MakeSymbolTable();
}  // namespace detail

constexpr std::uint64_t Symbol64(std::string_view s, std::uint64_t seed = kSymbolSeed) {
  std::uint64_t h = seed;
  for (char ch : s) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    const auto sx = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<signed char>(ch)));
    h = sx ^ detail::kSymbolTable[h >> 56] ^ (h << 8);
  }
  return h;
}

constexpr std::uint64_t Symbol64(const char* s, std::uint64_t seed = kSymbolSeed) {
  return s ? Symbol64(std::string_view(s), seed) : seed;
}

}  // namespace evr
