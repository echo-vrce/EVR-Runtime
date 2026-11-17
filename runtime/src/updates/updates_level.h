// The main menu's UPDATES card: the stock UPDATE NEWS card (actor 0x27dfdb284b22a843), brought back above the FRIENDS
// panel. The friends panel's level edits (friends/friends_level.h) send that card 60 m below the view and give it a 0 x 0
// pointer mesh; these edits run after them on the same three tables and place it again:
//   CR15PointerMeshCR    the card's mesh: the stock 2.1 x 1.05 times kSize. The card is drawn at its mesh's size, so
//                        (drawn size = mesh x transform scale) this is what makes it smaller;
//   CTransformCR         its pose: the FRIENDS panel's rotation and x / z, just above the panel, scale 1 (a smaller
//                        transform scale draws a smaller card but its pointer no longer hits it);
//   CActorDataResource   the same pose in the actor's local transform.
// Whether the card shows is decided at run time (updates_pc.cpp enables or disables the actor).
//
// Pure byte logic, header only, no platform headers, no exceptions: PC and Quest (mnu_master_lowspec has the same
// tables and actors) both use it.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "evr/symbol.h"

namespace evr_updates_level {

using Bytes = std::vector<std::uint8_t>;

inline constexpr std::uint64_t kLevel = 0xAC360E41E4EDE056ULL;  // mnu_master (PC)
inline constexpr std::uint64_t kQuestLevel = evr::Symbol64("mnu_master_lowspec");
inline constexpr std::uint64_t kCard = 0x27DFDB284B22A843ULL;   // UPDATE NEWS card -> UPDATES

inline constexpr std::uint64_t kPointerMeshType = evr::Symbol64("CR15PointerMeshCR");
inline constexpr std::uint64_t kTransformType = evr::Symbol64("CTransformCR");
inline constexpr std::uint64_t kActorDataType = evr::Symbol64("CActorDataResource");

inline constexpr std::array<float, 2> kStockMesh{2.1f, 1.05f};
inline constexpr float kSize = 0.6f;
inline constexpr float kScale = 1.0f;
// The FRIENDS panel stands at (-3.6, -0.275, 4.4) and is 2.40 m tall (top at +0.925); the card (0.63 m tall) sits
// just above it.
inline constexpr std::array<float, 4> kRotation{0.0f, 0.9135f, 0.0f, 0.4067f};
inline constexpr std::array<float, 3> kPosition{-3.6f, 1.31f, 4.4f};

namespace detail {

template <typename T>
T Get(const Bytes& b, std::size_t at) {
  T v{};
  std::memcpy(&v, b.data() + at, sizeof(v));
  return v;
}
template <typename T>
void Put(Bytes* b, std::size_t at, const T& v) {
  std::memcpy(b->data() + at, &v, sizeof(v));
}
inline bool Fail(std::string* why, const char* what) {
  *why = what;
  return false;
}
// A flat component table: 56-byte header (+8 byte size, +40 capacity, +48 count), then the entries.
inline bool Flat(const Bytes& b, std::size_t* entry, std::size_t* count) {
  if (b.size() < 56) return false;
  const auto bytes = Get<std::uint64_t>(b, 8), capacity = Get<std::uint64_t>(b, 40), n = Get<std::uint64_t>(b, 48);
  if (capacity == 0 || bytes % capacity != 0 || n > capacity) return false;
  *entry = static_cast<std::size_t>(bytes / capacity);
  *count = static_cast<std::size_t>(n);
  return *entry >= 16 && 56 + *entry * *count <= b.size();
}
inline std::vector<std::size_t> EntriesOf(const Bytes& b, std::size_t entry, std::size_t count, std::uint64_t actor) {
  std::vector<std::size_t> out;
  for (std::size_t k = 0; k < count; ++k)
    if (Get<std::uint64_t>(b, 56 + entry * k + 8) == actor) out.push_back(56 + entry * k);
  return out;
}

}  // namespace detail

// ---- CR15PointerMeshCR (80-byte entries: +32 width, +36 height) ----
inline bool EditPointerMeshes(const Bytes& in, Bytes* out, std::string* why) {
  using namespace detail;
  std::size_t entry = 0, count = 0;
  if (!Flat(in, &entry, &count) || entry < 40) return Fail(why, "not a flat pointer-mesh table");
  const auto card = EntriesOf(in, entry, count, kCard);
  if (card.size() != 1) return Fail(why, "the card does not have one pointer mesh");
  Bytes b = in;
  Put(&b, card[0] + 32, std::array<float, 2>{kStockMesh[0] * kSize, kStockMesh[1] * kSize});
  *out = std::move(b);
  return true;
}

// ---- CTransformCR (176-byte entries: +0x20 rotation x y z w, +0x30 position x y z, +0x3C scale x y z) ----
inline bool EditTransforms(const Bytes& in, Bytes* out, std::string* why) {
  using namespace detail;
  std::size_t entry = 0, count = 0;
  if (!Flat(in, &entry, &count) || entry < 0x48) return Fail(why, "not a flat transform table");
  const auto card = EntriesOf(in, entry, count, kCard);
  if (card.empty()) return Fail(why, "the card has no transform");
  Bytes b = in;
  for (const std::size_t at : card) {
    Put(&b, at + 0x20, kRotation);
    Put(&b, at + 0x30, kPosition);
    Put(&b, at + 0x3C, std::array<float, 3>{kScale, kScale, kScale});
  }
  *out = std::move(b);
  return true;
}

// ---- CActorDataResource: the card's local transform (48 bytes: rotation, position, scale, padding) ----
inline bool EditActorData(const Bytes& in, Bytes* out, std::string* why) {
  using namespace detail;
  constexpr std::size_t kHeader = 784;
  if (in.size() < kHeader) return Fail(why, "shorter than the actor data header");
  const auto count = [&in](std::size_t d) { return Get<std::uint64_t>(in, d + 48); };
  const std::uint64_t headers = count(0), components = count(64), actors = count(128);
  if (count(184) != actors || count(240) != actors || count(296) != actors) return Fail(why, "the actor arrays disagree");
  std::size_t at = kHeader + 16 * headers;
  std::uint64_t indices = 0;
  for (std::uint64_t k = 0; k < components; ++k) {
    if (at + 64 * (k + 1) > in.size()) return Fail(why, "the components run past the end");
    indices += Get<std::uint64_t>(in, at + 64 * k + 8 + 48);
  }
  at += 64 * components + 2 * indices;
  at = (at + 7) / 8 * 8;
  const std::size_t ids = at, transforms = ids + 3 * 8 * actors;
  if (transforms + 48 * actors > in.size()) return Fail(why, "the transforms run past the end");
  for (std::uint64_t i = 0; i < actors; ++i) {
    if (Get<std::uint64_t>(in, ids + 8 * i) != kCard) continue;
    Bytes b = in;
    const std::size_t p = transforms + 48 * static_cast<std::size_t>(i);
    Put(&b, p, kRotation);
    Put(&b, p + 16, kPosition);
    Put(&b, p + 28, kScale);
    *out = std::move(b);
    return true;
  }
  return Fail(why, "the card is not in the actor data");
}

}  // namespace evr_updates_level
