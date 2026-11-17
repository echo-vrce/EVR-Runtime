// UPDATES card and panel (PC). See updates.h, updates_level.h (where the card stands) and updates_panel.h (what it says).
//
// The card is the stock UPDATE NEWS card (actor 0x27dfdb284b22a843); it is enabled only while there is an update.
// Its click reaches the home page as 0xfb75d76107e450d8 from a relay actor, and the home page opens the stock news
// pop-up (actor 0x777cf8ceec9bfd21; header, picture, news lines, LEARN MORE left and CONTINUE right). We send that
// same event once when the menu first shows, so a new update opens the panel by itself. On opening, the pop-up's
// script writes its stock texts, so we write ours again for a while after each opening.
// Left is SKIP / LATER: rewritten into the right click, so the stock LEARN MORE link never opens and the panel closes.
// Right is UPDATE / RESTART NOW: rewritten into an event no script handles, so the panel stays open to show progress.
//
// The update itself is the loader's (hid.dll): it checked the feed at launch and keeps the verified offer. We read its
// status as JSON and ask it to start, skip or relaunch through its exports. Download, signature and hash checks,
// staging and rollback all stay in evr-updater.
#include "updates/updates.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "evr/actors.h"
#include "evr/core.h"
#include "evr/events.h"
#include "evr/frame.h"
#include "evr/resources.h"
#include "evr/symbol.h"
#include "evr/ui_text.h"
#include "updates/updates_level.h"
#include "updates/updates_panel.h"

namespace evr_updates {
namespace {

namespace ul = evr_updates_level;
namespace up = evr_updates_panel;

// ---- the card's level edits ----

using Edit = bool (*)(const ul::Bytes&, ul::Bytes*, std::string*);

struct Table {
  const char* label;
  std::uint64_t type;
  Edit edit;
  ul::Bytes served;
};
Table g_tables[] = {
    {"updates card pointer mesh", ul::kPointerMeshType, &ul::EditPointerMeshes, {}},
    {"updates card transform", ul::kTransformType, &ul::EditTransforms, {}},
    {"updates card actor data", ul::kActorDataType, &ul::EditActorData, {}},
};

bool Transform(const void* original, std::uint64_t size, const void** out, std::uint64_t* outSize, void* user) {
  auto* t = static_cast<Table*>(user);
  const auto* p = static_cast<const std::uint8_t*>(original);
  std::string why;
  ul::Bytes result;
  if (!t->edit(ul::Bytes(p, p + size), &result, &why)) {
    evr::Log(evr::LogLevel::Warning, "[EVR.UPDATES] %s not edited: %s", t->label, why.c_str());
    return false;
  }
  t->served = std::move(result);
  *out = t->served.data();
  *outSize = t->served.size();
  return true;
}

// ---- the loader's exports (hid.dll next to echovr.exe) ----

using StatusFn = std::size_t (*)(char* buffer, std::size_t capacity);
using CallFn = int (*)();

struct Loader {
  StatusFn status = nullptr;
  CallFn start = nullptr, skip = nullptr, relaunch = nullptr, bootOk = nullptr;
};
Loader g_loader;

bool FindLoader() {
  const std::string path = evr::GameDir() + "\\hid.dll";
  HMODULE h = GetModuleHandleA(path.c_str());
  if (h == nullptr) return false;
  g_loader.status = reinterpret_cast<StatusFn>(GetProcAddress(h, "EvrUpdateStatus"));
  g_loader.start = reinterpret_cast<CallFn>(GetProcAddress(h, "EvrUpdateStart"));
  g_loader.skip = reinterpret_cast<CallFn>(GetProcAddress(h, "EvrUpdateSkip"));
  g_loader.relaunch = reinterpret_cast<CallFn>(GetProcAddress(h, "EvrUpdateRelaunch"));
  g_loader.bootOk = reinterpret_cast<CallFn>(GetProcAddress(h, "EvrUpdateBootOk"));
  return g_loader.status != nullptr && g_loader.start != nullptr && g_loader.skip != nullptr &&
         g_loader.relaunch != nullptr;
}

bool ReadStatus(up::Status* out) {
  std::string buf(16 * 1024, '\0');
  std::size_t n = g_loader.status(buf.data(), buf.size());
  if (n > buf.size()) {
    buf.assign(n, '\0');
    n = g_loader.status(buf.data(), buf.size());
  }
  if (n == 0 || n > buf.size()) return false;
  buf.resize(n);
  try {
    const auto j = nlohmann::json::parse(buf);
    up::Status s;
    s.state = up::StateFromName(j.value("state", ""));
    s.version = j.value("version", "");
    s.title = j.value("title", "");
    s.error = j.value("error", "");
    s.notes = j.value("notes", std::vector<std::string>{});
    s.files = j.value("files", 0ULL);
    s.bytes = j.value("bytes", 0ULL);
    s.done = j.value("done", 0ULL);
    s.total = j.value("total", 0ULL);
    s.relaunch = j.value("relaunch", false);
    s.skipped = j.value("skipped", false);
    *out = std::move(s);
    return true;
  } catch (const nlohmann::json::exception& e) {
    evr::Log(evr::LogLevel::Warning, "[EVR.UPDATES] loader status unreadable: %s", e.what());
    return false;
  }
}

// ---- the menu ----

constexpr std::uint64_t kCard = ul::kCard;
constexpr std::uint64_t kPopup = 0x777CF8CEEC9BFD21ULL;  // the stock news pop-up
// What the card's click becomes: the home page receives fb75d761 from this relay actor, key 6cfc99d2, and opens the
// pop-up. We send the same event to open the panel by itself.
constexpr std::uint64_t kOpenPanel = 0xFB75D76107E450D8ULL;
constexpr std::uint64_t kOpenRelay = 0x08324E714B4A21AAULL;
constexpr std::uint64_t kOpenKey = 0x6CFC99D2A9BDA0DFULL;
constexpr std::uint64_t kLeftClicked = evr::Symbol64("evt_left_btn_clicked");
constexpr std::uint64_t kRightClicked = evr::Symbol64("evt_right_btn_clicked");  // the pop-up closes on it
constexpr std::uint64_t kSwallowed = evr::Symbol64("evt_evr_updates_handled");  // no script handles it
constexpr int kRewriteFrames = 90;   // after the pop-up opens, its script writes the stock texts; ours win after it
constexpr int kPollFrames = 10;
constexpr int kAutoOpenFrames = 180;  // after the menu first shows, before the panel opens by itself

// Each text we own, with the stock text the game writes there (the pop-up writes its own on every opening; the
// text filter swaps it for ours as it is written, so the stock one never shows).
struct Item {
  std::uint64_t actor;
  std::uint64_t id;
  std::string up::Texts::*field;
  const char* stock;     // prefix of the game's own text for it
  bool whileOpen;        // swapped only while our panel is open (other pop-ups use the same button texts)
};
const Item kItems[] = {
    {kCard, evr::Symbol64("text"), &up::Texts::card, "SELF-GOALING PATCH", false},
    {kPopup, evr::Symbol64("headertext"), &up::Texts::header, "SELF-GOALING PATCH", false},
    {kPopup, evr::Symbol64("bodytext1"), &up::Texts::news, "Check out the Echo VR Mission Logs", false},
    {kPopup, evr::Symbol64("lefttext"), &up::Texts::left, "LEARN MORE", true},
    {kPopup, evr::Symbol64("righttext"), &up::Texts::right, "CONTINUE", true},
};
constexpr std::size_t kItemCount = sizeof(kItems) / sizeof(kItems[0]);

// Game-loop thread only.
up::Status g_status;
bool g_haveStatus = false;
std::string g_shown[kItemCount];
up::Texts g_texts;          // what we show now (the text filter hands out its strings)
std::uint64_t g_space = 0;  // the menu's space, from the card's own events
int g_cardShown = -1;       // what we last told the game: -1 nothing yet
int g_rewrite = 0;
int g_poll = 0;
int g_menuFrames = -1;      // frames since the menu first showed; -1 not yet
bool g_autoOpened = false;
bool g_bootOkSent = false;
bool g_panelOpen = false;   // between the open event and a click that closes it

void Act(up::Action a) {
  if (a == up::Action::kNone) return;
  const char* what = a == up::Action::kStart ? "start" : a == up::Action::kSkip ? "skip" : "relaunch";
  evr::Log(evr::LogLevel::Info, "[EVR.UPDATES] %s %s", what, g_status.version.c_str());
  // The loader's calls do file and network work; keep them off the game loop.
  std::thread([a] {
    const int r = a == up::Action::kStart ? g_loader.start() : a == up::Action::kSkip ? g_loader.skip()
                                                                                         : g_loader.relaunch();
    evr::Log(r == 0 ? evr::LogLevel::Info : evr::LogLevel::Warning, "[EVR.UPDATES] loader answered %d", r);
  }).detach();
  g_poll = 0;
}

// The relay's index in the menu space, for the open event's sender word.
bool OpenPanel() {
  const int relay = evr::ActorIndex(g_space, kOpenRelay);
  if (relay < 0 || !evr::IsReadable(reinterpret_cast<const void*>(g_space + 0x58), 8)) return false;
  std::uint64_t ev[5];
  ev[0] = kOpenPanel;
  ev[1] = *reinterpret_cast<const std::uint64_t*>(g_space + 0x58);
  ev[2] = evr::SenderWord(static_cast<std::uint16_t>(relay));
  ev[3] = kOpenKey;
  ev[4] = ~0ULL;
  return evr::SendComponentEvent(g_space, ev);
}

void Frame() {
  if (g_loader.status == nullptr) return;
  if (--g_poll <= 0) {
    g_poll = kPollFrames;
    up::Status s;
    if (ReadStatus(&s)) {
      if (!g_haveStatus || s.state != g_status.state)
        evr::Log(evr::LogLevel::Info, "[EVR.UPDATES] status %d version=%s skipped=%d relaunch=%d error=%s",
                 static_cast<int>(s.state), s.version.c_str(), s.skipped ? 1 : 0, s.relaunch ? 1 : 0, s.error.c_str());
      g_status = std::move(s);
      g_haveStatus = true;
    }
  }
  if (!g_haveStatus) return;
  g_texts = up::Compose(g_status);
  const up::Texts& t = g_texts;
  const bool force = g_rewrite > 0;
  if (force) --g_rewrite;
  for (std::size_t i = 0; i < kItemCount; ++i) {
    const std::string& text = t.*(kItems[i].field);
    if (!force && g_shown[i] == text) continue;
    if (evr::PostUiText(kItems[i].actor, kItems[i].id, text) == evr::UiTextResult::Queued) g_shown[i] = text;
  }
  if (g_space == 0) return;
  const int visible = up::CardVisible(g_status) ? 1 : 0;
  if (visible != g_cardShown && evr::SetActorEnabled(g_space, kCard, visible != 0)) {
    g_cardShown = visible;
    evr::Log(evr::LogLevel::Info, "[EVR.UPDATES] card %s", visible ? "shown" : "hidden");
  }
  if (g_menuFrames >= 0 && g_menuFrames < kAutoOpenFrames) ++g_menuFrames;
  if (!g_autoOpened && g_menuFrames >= kAutoOpenFrames && g_status.state != up::State::kChecking) {
    g_autoOpened = true;
    if (up::OpensByItself(g_status))
      evr::Log(OpenPanel() ? evr::LogLevel::Info : evr::LogLevel::Warning, "[EVR.UPDATES] panel opened by itself");
  }
}

// The game's own texts as it writes them: the pop-up's stock header, news and button labels become ours, and its
// LEARN MORE tooltip ("Open the hyperlink") is blanked while our panel is open.
const char* Filter(std::uint64_t item, const char* text) {
  if (text == nullptr || !g_haveStatus || !up::CardVisible(g_status)) return nullptr;
  if (g_panelOpen && std::strcmp(text, "Open the hyperlink") == 0) return "";
  for (const Item& it : kItems) {
    if (it.id != item || (it.whileOpen && !g_panelOpen)) continue;
    if (std::strncmp(text, it.stock, std::strlen(it.stock)) == 0) return (g_texts.*(it.field)).c_str();
  }
  return nullptr;
}

void Event(std::uint64_t space, std::uint64_t event) {
  if (!evr::IsReadable(reinterpret_cast<const void*>(event), evr::event_layout::kEventBytes)) return;
  auto* symbol = reinterpret_cast<std::uint64_t*>(event + evr::event_layout::kSymbolOffset);
  if (*symbol == kOpenPanel) {
    const auto* w = reinterpret_cast<const std::uint64_t*>(event);
    evr::Log(evr::LogLevel::Info, "[EVR.UPDATES] panel opening words=%016llx %016llx %016llx %016llx %016llx",
             (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3],
             (unsigned long long)w[4]);
    g_rewrite = kRewriteFrames;
    g_poll = 0;
    g_panelOpen = true;
    return;
  }
  const std::uint64_t sender = evr::EventSender(space, event);
  if (sender == kCard) {
    if (g_space != space) {
      g_space = space;
      g_cardShown = -1;
      if (g_menuFrames < 0) g_menuFrames = 0;
    }
    if (!g_bootOkSent && g_loader.bootOk != nullptr) {
      g_bootOkSent = true;  // the main menu is up: this runtime version boots
      evr::Log(evr::LogLevel::Info, "[EVR.UPDATES] main menu reached: boot ok -> %d", g_loader.bootOk());
    }
    return;
  }
  if (sender != kPopup || !g_haveStatus) return;
  const std::uint64_t s = *symbol;
  if (s == kLeftClicked) {
    *symbol = kRightClicked;  // the stock LEARN MORE link never opens; the panel closes as on CONTINUE
    g_panelOpen = false;
    Act(up::OnLeft(g_status));
  } else if (s == kRightClicked) {
    if (!up::RightKeepsOpen(g_status)) {  // nothing to do: the game closes the panel
      g_panelOpen = false;
      return;
    }
    *symbol = kSwallowed;
    Act(up::OnRight(g_status));
  }
}

}  // namespace

void Install() {
  for (Table& t : g_tables)
    if (!evr::RegisterResourceTransform(t.type, ul::kLevel, &Transform, &t, t.label))
      evr::Log(evr::LogLevel::Warning, "[EVR.UPDATES] %s: transform not registered", t.label);
  if (!FindLoader()) {
    evr::Log(evr::LogLevel::Warning, "[EVR.UPDATES] the loader has no update exports: the card stays the stock one");
    return;
  }
  evr::OnFrame(&Frame);
  evr::OnComponentEvent(&Event);
  evr::OnUiTextSet(&Filter);
  evr::Log(evr::LogLevel::Info, "[EVR.UPDATES] ready");
}

}  // namespace evr_updates
