// evr_compat.h: the names the friends panel sources call, mapped onto EVR-Runtime's own core.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

#include "evr/core.h"
#include "evr/events.h"
#include "evr/resources.h"
#include "evr/ui_text.h"

namespace EchoVR {
enum class LogLevel { Info, Warning };
}

inline void Log(EchoVR::LogLevel level, const char* format, ...) {
  char line[2048];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  evr::Log(level == EchoVR::LogLevel::Warning ? evr::LogLevel::Warning : evr::LogLevel::Info, "%s", line);
}

using evr::RegisterResourceOverride;
using evr::RegisterResourceTransform;

// The SComponentEvent layout and actor table, under the names the panel sources use (evr/events.h).
namespace nevr_invite_choices {
inline constexpr std::size_t kEventBytes = evr::event_layout::kEventBytes;
inline constexpr std::size_t kEventSymbolOffset = evr::event_layout::kSymbolOffset;
inline constexpr std::size_t kEventSenderOffset = evr::event_layout::kSenderOffset;
inline constexpr std::size_t kEventComponentOffset = evr::event_layout::kComponentOffset;
inline constexpr std::uint16_t kNoSender = evr::event_layout::kNoSender;
inline constexpr std::size_t kSpaceActorTableOffset = evr::event_layout::kSpaceActorTableOffset;
inline constexpr std::size_t kTableNamesOffset = evr::event_layout::kTableNamesOffset;
inline constexpr std::size_t kTableCapacityOffset = evr::event_layout::kTableCountOffset;
}  // namespace nevr_invite_choices

// The UI text writer under the names the panel sources use (evr/ui_text.h). A queued post counts as written: the writer
// keeps it until the actor exists.
namespace nevr_ui_text {
enum class Check { Ok, Refused };
struct Target {
  std::uint64_t actor = 0;
  std::uint64_t item = 0;
};
struct Request {
  Target target;
  std::string text;
};
inline bool Ready() { return true; }
inline Check Post(Request request) {
  return evr::PostUiText(request.target.actor, request.target.item, request.text) == evr::UiTextResult::Queued
             ? Check::Ok
             : Check::Refused;
}
}  // namespace nevr_ui_text
