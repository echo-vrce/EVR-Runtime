// Adding cards to a menu page resource (CR15UIPage2CR), in memory. Pure byte logic, unit-tested without a game.
//
// The format (our reading of the serializer, the same layout nevr-runtime's ADR 0017 notes describe) is a run of
// arrays. Each array is a 56-byte header: +0x00 pointer (0 on disk), +0x08 byte size, +0x10 0, +0x18 u32 0,
// +0x1C u32 flags 1, +0x20 0x20, +0x28 count, +0x30 count again. Its rows follow later, in the order the serializer
// reads them:
//   pages header, then the 328-byte page records;
//   for each page, five arrays at +32 (472-byte elements), +88, +144, +200, +272 (16-byte rows). The element rows
//     come first, then each element's eight child arrays' rows, element by element, then the +88..+272 rows.
//   an element: u64 owner actor, u64 0, u64 state (`default`), then eight arrays at +24 + 56k. +80 holds the sprite
//     rows (u64 key, u64 value), +416 the text rows (u64 key, 64-byte NUL-padded string); +304 and +360 are always
//     empty in the stock page (row size unknown).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace evr_menu_page {

constexpr std::size_t kHeader = 56;
constexpr std::size_t kPageBytes = 328;
constexpr std::size_t kElementBytes = 472;
constexpr std::size_t kPairBytes = 16;
constexpr std::size_t kTextBytes = 72;
constexpr std::size_t kPageArrays[5] = {32, 88, 144, 200, 272};
constexpr std::size_t kSpriteArray = 80;
constexpr std::size_t kTextArray = 416;

struct NewCard {
  std::uint64_t owner;
  std::uint64_t tile;  // the sprite's value (e.g. Symbol64("tile_create"))
  std::string text, subtext, tip;
};

struct Keys {
  std::uint64_t state, sprite, text, subtext, tip;  // Symbol64 of default, sprite, text, subtext, tip
};

namespace detail {

inline std::uint64_t U64(const std::uint8_t* p) {
  std::uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}
inline std::uint32_t U32(const std::uint8_t* p) {
  std::uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}
inline void Put64(std::vector<std::uint8_t>& b, std::uint64_t v) {
  const std::size_t at = b.size();
  b.resize(at + 8);
  std::memcpy(b.data() + at, &v, 8);
}

inline std::size_t ChildRow(std::size_t k) {  // row size of an element's k-th child array; 0 = must be empty
  const std::size_t at = 24 + 56 * k;
  if (at == kTextArray) return kTextBytes;
  if (at == 304 || at == 360) return 0;
  return kPairBytes;
}

// Reads the array header at `at`. False when it is not one the format writes.
inline bool Array(const std::vector<std::uint8_t>& b, std::size_t at, std::size_t row, std::uint64_t* count) {
  if (at > b.size() || b.size() - at < kHeader) return false;
  const std::uint8_t* h = b.data() + at;
  const std::uint64_t bytes = U64(h + 8), n = U64(h + 0x28);
  if (U64(h) != 0 || U64(h + 0x10) != 0 || U32(h + 0x18) != 0 || U32(h + 0x1C) != 1 || n != U64(h + 0x30)) return false;
  if (row == 0 ? (n != 0 || bytes != 0) : (n > b.size() / row || bytes != n * row)) return false;
  *count = n;
  return true;
}

inline void PutArray(std::vector<std::uint8_t>& b, std::uint64_t count, std::size_t row) {
  Put64(b, 0);
  Put64(b, count * row);
  Put64(b, 0);
  Put64(b, std::uint64_t{1} << 32);  // u32 0, u32 flags 1
  Put64(b, 0x20);
  Put64(b, count);
  Put64(b, count);
}

inline void PutText(std::vector<std::uint8_t>& b, std::uint64_t key, const std::string& s) {
  Put64(b, key);
  const std::size_t at = b.size();
  b.resize(at + 64, 0);
  std::memcpy(b.data() + at, s.data(), s.size() < 63 ? s.size() : 63);
}

}  // namespace detail

/// Where one page's pieces are.
struct Page {
  std::size_t elementsHeader = 0;  // its +32 array header
  std::size_t elementsAt = 0;      // its first element
  std::uint64_t elements = 0;
  std::size_t childRowsEnd = 0;    // after the last element's child rows: where a new element's child rows go
  std::vector<std::uint64_t> owners;
};

/// Walks the whole buffer as the serializer does. False when any byte is not accounted for.
inline bool Decode(const std::vector<std::uint8_t>& b, std::vector<Page>* pages) {
  using namespace detail;
  pages->clear();
  std::uint64_t count = 0;
  if (!Array(b, 0, kPageBytes, &count)) return false;
  std::size_t cur = kHeader + count * kPageBytes;
  if (cur > b.size()) return false;
  for (std::uint64_t p = 0; p < count; ++p) {
    const std::size_t rec = kHeader + p * kPageBytes;
    Page page;
    for (std::size_t slot : kPageArrays) {
      std::uint64_t n = 0;
      const std::size_t row = slot == 32 ? kElementBytes : kPairBytes;
      if (!Array(b, rec + slot, row, &n) || b.size() - cur < n * row) return false;
      const std::size_t start = cur;
      cur += n * row;
      if (slot != 32) continue;
      page.elementsHeader = rec + slot;
      page.elementsAt = start;
      page.elements = n;
      for (std::uint64_t e = 0; e < n; ++e) {
        const std::size_t el = start + e * kElementBytes;
        page.owners.push_back(U64(b.data() + el));
        for (std::size_t k = 0; k < 8; ++k) {
          std::uint64_t m = 0;
          const std::size_t row2 = ChildRow(k);
          if (!Array(b, el + 24 + 56 * k, row2, &m) || b.size() - cur < m * row2) return false;
          cur += m * row2;
        }
      }
      page.childRowsEnd = cur;
    }
    pages->push_back(std::move(page));
  }
  return cur == b.size();
}

/// Adds `cards` to page `index`. Empty `why` and the new buffer on success.
inline std::vector<std::uint8_t> AddCardsToPage(const std::vector<std::uint8_t>& in, std::size_t index,
                                                const std::vector<NewCard>& cards, const Keys& keys, std::string* why) {
  using namespace detail;
  std::vector<Page> pages;
  if (!Decode(in, &pages)) return *why = "not a page resource this build writes", std::vector<std::uint8_t>{};
  if (index >= pages.size()) return *why = "no such page", std::vector<std::uint8_t>{};
  const Page& page = pages[index];
  for (std::uint64_t o : page.owners)
    for (const NewCard& c : cards)
      if (o == c.owner) return *why = "a card is already there", std::vector<std::uint8_t>{};

  std::vector<std::uint8_t> elements, rows;
  for (const NewCard& c : cards) {
    Put64(elements, c.owner);
    Put64(elements, 0);
    Put64(elements, keys.state);
    for (std::size_t k = 0; k < 8; ++k) {
      const std::size_t at = 24 + 56 * k;
      if (at == kSpriteArray) PutArray(elements, 1, kPairBytes);
      else if (at == kTextArray) PutArray(elements, 3, kTextBytes);
      else PutArray(elements, 0, 0);
    }
    Put64(rows, keys.sprite);
    Put64(rows, c.tile);
    PutText(rows, keys.text, c.text);
    PutText(rows, keys.subtext, c.subtext);
    PutText(rows, keys.tip, c.tip);
  }

  const std::size_t elementsEnd = page.elementsAt + page.elements * kElementBytes;
  std::vector<std::uint8_t> out(in);
  out.insert(out.begin() + page.childRowsEnd, rows.begin(), rows.end());  // the later one first
  out.insert(out.begin() + elementsEnd, elements.begin(), elements.end());
  const std::uint64_t n = page.elements + cards.size();
  const std::uint64_t bytes = n * kElementBytes;
  std::memcpy(out.data() + page.elementsHeader + 0x08, &bytes, 8);
  std::memcpy(out.data() + page.elementsHeader + 0x28, &n, 8);
  std::memcpy(out.data() + page.elementsHeader + 0x30, &n, 8);

  std::vector<Page> check;
  if (!Decode(out, &check) || check.size() != pages.size() || check[index].elements != n)
    return *why = "the result does not decode", std::vector<std::uint8_t>{};
  why->clear();
  return out;
}

/// Adds `cards` to every page that has an element owned by `anchor`; `*added` is how many pages took them.
inline std::vector<std::uint8_t> AddCards(const std::vector<std::uint8_t>& in, std::uint64_t anchor,
                                          const std::vector<NewCard>& cards, const Keys& keys, int* added,
                                          std::string* why) {
  std::vector<Page> pages;
  *added = 0;
  if (!Decode(in, &pages)) return *why = "not a page resource this build writes", std::vector<std::uint8_t>{};
  std::vector<std::uint8_t> cur = in;
  for (std::size_t i = 0; i < pages.size(); ++i) {
    bool has = false;
    for (std::uint64_t o : pages[i].owners) has = has || o == anchor;
    if (!has) continue;
    std::vector<std::uint8_t> next = AddCardsToPage(cur, i, cards, keys, why);
    if (!why->empty()) return {};
    cur = std::move(next);
    ++*added;
  }
  if (*added == 0) return *why = "no page holds the anchor card", std::vector<std::uint8_t>{};
  why->clear();
  return cur;
}

}  // namespace evr_menu_page
