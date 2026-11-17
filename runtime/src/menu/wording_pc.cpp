// menu/wording_pc.cpp: applies menu/wording.h through the game's text setter.
#include "menu/wording.h"

#include <cstdint>

#include "evr/core.h"
#include "evr/ui_text.h"

namespace evr_wording {
namespace {

const char* Filter(std::uint64_t /*item*/, const char* text) { return Reword(text); }

}  // namespace

void Install() {
  evr::OnUiTextSet(&Filter);
  evr::Log(evr::LogLevel::Info, "[EVR.WORDING] ready");
}

}  // namespace evr_wording
