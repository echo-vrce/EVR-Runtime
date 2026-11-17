// Swapping where two actors stand, in a level's transform table (CTransformCR). Pure byte logic.
//
// The table is a 56-byte array header (+0x08 byte size = count * 176, +0x28 count, +0x30 count again) and then one
// 176-byte entry per transform: +0x08 the owning actor, +0x20 the pose (rotation x y z w, position x y z, scale x y z:
// ten floats, 40 bytes). The main menu lays its cards out by these poses, so swapping two cards' poses swaps their places
// and each keeps facing the player.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace evr_menu_slots {

constexpr std::size_t kHeader = 56;
constexpr std::size_t kEntry = 176;
constexpr std::size_t kOwner = 0x08;
constexpr std::size_t kPose = 0x20;
constexpr std::size_t kPoseBytes = 40;

/// The entry offset of `owner`'s only transform, or 0 when it has none or several.
inline std::size_t EntryOf(const std::vector<std::uint8_t>& b, std::uint64_t count, std::uint64_t owner) {
  std::size_t found = 0;
  for (std::uint64_t i = 0; i < count; ++i) {
    const std::size_t at = kHeader + i * kEntry;
    std::uint64_t o;
    std::memcpy(&o, b.data() + at + kOwner, 8);
    if (o != owner) continue;
    if (found != 0) return 0;
    found = at;
  }
  return found;
}

/// The table with `a` and `b` in each other's place. Empty `why` on success.
inline std::vector<std::uint8_t> SwapPoses(const std::vector<std::uint8_t>& in, std::uint64_t a, std::uint64_t b,
                                           std::string* why) {
  std::uint64_t bytes = 0, count = 0, count2 = 0;
  if (in.size() < kHeader) return *why = "too short", std::vector<std::uint8_t>{};
  std::memcpy(&bytes, in.data() + 0x08, 8);
  std::memcpy(&count, in.data() + 0x28, 8);
  std::memcpy(&count2, in.data() + 0x30, 8);
  if (count != count2 || count > in.size() / kEntry || bytes != count * kEntry || kHeader + bytes > in.size())
    return *why = "not a transform table", std::vector<std::uint8_t>{};
  const std::size_t ea = EntryOf(in, count, a), eb = EntryOf(in, count, b);
  if (ea == 0 || eb == 0) return *why = "a card has no single transform", std::vector<std::uint8_t>{};
  std::vector<std::uint8_t> out(in);
  std::memcpy(out.data() + ea + kPose, in.data() + eb + kPose, kPoseBytes);
  std::memcpy(out.data() + eb + kPose, in.data() + ea + kPose, kPoseBytes);
  why->clear();
  return out;
}

}  // namespace evr_menu_slots
