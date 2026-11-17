// CREATE GAME / JOIN VIA CODE wizard -> Atlas invite codes (PC).
//
// The wizard does not call any service itself: its only output is a click metric, a JSON line the game hands to its
// remote-log submit (0x140113960, (const char* line, u64 length, ...)), with "panel_type":"deep_linking". Two of those
// lines are the wizard's requests (field names as nevr-runtime's ADR 0017 reports them):
//   panel_name create_game_review_settings, event_name create_invite: Server_region, Expires_after ("3 HOURS")
//   panel_name join_via_code, event_name join: join_code
// We copy such a line, and a worker sends Atlas invite/create or invite/resolve with the player's token.
//
// Joining (create summary's JOIN, or a resolved code) uses the game's own join-by-lobby-id path, our reading of
// CR15Game::BeginMultiplayer (0x1400fda70): the game object (the static at 0x1420a0478) holds a staged lobby id
// (16-byte GUID) at +0x8540 and a team word at +0x8560, which BeginMultiplayerLobby (0x1400fddd0) consumes and clears.
// It takes another route first when (+0x8520 & 0xF) && [+0x8528], or when the id at +0x8530 is not all zero, so we
// stage nothing then. All of it runs on the game-loop thread.
#include "menu/invite.h"

#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "evr/actors.h"
#include "evr/core.h"
#include "evr/events.h"
#include "evr/symbol.h"
#include "evr/frame.h"
#include "evr/ui_text.h"
#include "social/social.h"

namespace evr_invite {
Result Last();
namespace {

using SubmitFn = std::uint64_t (*)(const char* line, std::uint64_t length, std::uint64_t, std::uint64_t);

constexpr std::uint64_t kSubmitVA = 0x140113960;
constexpr std::uint8_t kSubmitPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
                                            0xEC, 0x30, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF9};
constexpr std::uint64_t kMaxLine = 4096;

SubmitFn g_submit = nullptr;
bool g_dev = false;
std::atomic<int> g_shapeLogs{0};
Result g_last;  // written by workers under g_mutex, read by Last()
SRWLOCK g_lock = SRWLOCK_INIT;
std::string g_pendingJoin;  // match id the frame callback joins; under g_lock
std::atomic<int> g_repostFrames{0};  // frames left to write the code again (the page resets it when shown)
void ShowCode();

constexpr std::uint64_t kGameVA = 0x1420A0478;  // CR15Game*
constexpr std::uint64_t kBeginLobbyVA = 0x1400FDDD0;
constexpr std::uint8_t kBeginLobbyPrologue[] = {0x48, 0x83, 0xEC, 0x78, 0x33, 0xC0, 0xC7, 0x44};
constexpr std::size_t kRouteFlags = 0x8520, kRouteWord = 0x8528, kOtherLobby = 0x8530, kStagedLobby = 0x8540,
                      kStagedTeam = 0x8560;

// The menu's PLAY: the home page (actor 05ce113632359ce3) gets evt_card_clicked with PLAY's page element id as key; it
// shows the connecting page (2fe7102931d1c4e0) and asks the game to begin multiplayer, which takes a staged lobby id
// before it searches. So a join stages the lobby and sends that event: the player sees the stock connecting screen.
constexpr std::uint64_t kHomePage = 0x05ce113632359ce3;
constexpr std::uint64_t kCardClicked = evr::Symbol64("evt_card_clicked");
constexpr std::uint64_t kPlayElement = 0x35ec861316abbb34;
constexpr std::uint64_t kSendLocalVA = 0x14050FB60;
std::uint64_t g_menuSpace = 0;              // game-loop thread
std::uint8_t g_stagedGuid[16] = {};         // game-loop thread
int g_stageCheckFrames = 0;                 // game-loop thread: frames until the fallback begin

bool SendPlay() {
  const std::uint64_t space = g_menuSpace;
  const int index = space ? evr::ActorIndex(space, kHomePage) : -1;
  void* send = evr::ResolveVA(kSendLocalVA);
  if (index < 0 || send == nullptr || !evr::IsReadable(reinterpret_cast<void*>(space + 0x58), 8)) return false;
  std::uint64_t ev[5];
  ev[0] = kCardClicked;
  std::memcpy(&ev[1], reinterpret_cast<void*>(space + 0x58), 8);
  ev[2] = 0x0000FFFFFFFF0000ULL | static_cast<std::uint64_t>(index);
  ev[3] = kPlayElement;
  ev[4] = ~0ULL;
  reinterpret_cast<void (*)(void*, const void*, std::uint32_t)>(send)(reinterpret_cast<void*>(space), ev, 1);
  return true;
}

void QueueJoin(const std::string& matchId) {
  AcquireSRWLockExclusive(&g_lock);
  g_pendingJoin = matchId;
  ReleaseSRWLockExclusive(&g_lock);
}

int Hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// "e4fc4864-c557-4de4-a471-ffe0eddabb55[.node]" -> the Windows GUID layout (Data1..3 little-endian, Data4 in order).
bool ParseGuid(const std::string& id, std::uint8_t out[16]) {
  std::string hex;
  for (char c : id.substr(0, id.find('.'))) {
    if (c == '-') continue;
    if (Hex(c) < 0) return false;
    hex += c;
  }
  if (hex.size() != 32) return false;
  std::uint8_t raw[16];
  for (int i = 0; i < 16; ++i) raw[i] = static_cast<std::uint8_t>(Hex(hex[2 * i]) * 16 + Hex(hex[2 * i + 1]));
  const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  for (int i = 0; i < 16; ++i) out[i] = raw[order[i]];
  return true;
}

void JoinNow(const std::string& matchId) {
  std::uint8_t guid[16];
  if (!ParseGuid(matchId, guid)) {
    evr::Log(evr::LogLevel::Warning, "[EVR.INVITE] join not started: match id %s is not a uuid", matchId.c_str());
    return;
  }
  auto** slot = static_cast<std::uint8_t**>(evr::ResolveVA(kGameVA));
  void* begin = evr::ResolveVA(kBeginLobbyVA);
  if (slot == nullptr || begin == nullptr || !evr::IsReadable(slot, 8) ||
      !evr::IsReadable(begin, sizeof(kBeginLobbyPrologue)) ||
      std::memcmp(begin, kBeginLobbyPrologue, sizeof(kBeginLobbyPrologue)) != 0) {
    evr::Log(evr::LogLevel::Warning, "[EVR.INVITE] join not started: game build differs");
    return;
  }
  std::uint8_t* game = *slot;
  if (game == nullptr || !evr::IsReadable(game + kRouteFlags, kStagedTeam + 2 - kRouteFlags)) {
    evr::Log(evr::LogLevel::Warning, "[EVR.INVITE] join not started: no game object");
    return;
  }
  std::uint64_t routeWord = 0;
  std::memcpy(&routeWord, game + kRouteWord, 8);
  static const std::uint8_t kZero[16] = {};
  if (((game[kRouteFlags] & 0xF) != 0 && routeWord != 0) || std::memcmp(game + kOtherLobby, kZero, 16) != 0) {
    evr::Log(evr::LogLevel::Warning, "[EVR.INVITE] join not started: the game has another join pending");
    return;
  }
  std::memcpy(game + kStagedLobby, guid, 16);
  const std::uint16_t noTeam = 0xFFFF;
  std::memcpy(game + kStagedTeam, &noTeam, 2);
  if (SendPlay()) {
    std::memcpy(g_stagedGuid, guid, 16);
    g_stageCheckFrames = 90;
    evr::Log(evr::LogLevel::Info, "[EVR.INVITE] joining match %s through the menu's PLAY", matchId.c_str());
    return;
  }
  evr::Log(evr::LogLevel::Info, "[EVR.INVITE] joining match %s (no menu space, direct)", matchId.c_str());
  reinterpret_cast<void (*)(void*)>(begin)(game);
}

// After PLAY: the game consumed the stage (joined), or it did not and we begin directly.
void CheckStage() {
  if (g_stageCheckFrames <= 0 || --g_stageCheckFrames > 0) return;
  auto** slot = static_cast<std::uint8_t**>(evr::ResolveVA(kGameVA));
  std::uint8_t* game = slot ? *slot : nullptr;
  if (game == nullptr || !evr::IsReadable(game + kStagedLobby, 16)) return;
  if (std::memcmp(game + kStagedLobby, g_stagedGuid, 16) != 0) {
    evr::Log(evr::LogLevel::Info, "[EVR.INVITE] the menu's PLAY took the staged lobby");
    return;
  }
  evr::Log(evr::LogLevel::Warning, "[EVR.INVITE] the staged lobby is still there after PLAY: beginning directly");
  reinterpret_cast<void (*)(void*)>(evr::ResolveVA(kBeginLobbyVA))(game);
}

void Frame() {
  CheckStage();
  if (g_repostFrames.load() > 0) {
    g_repostFrames.fetch_sub(1);
    ShowCode();
  }
  AcquireSRWLockShared(&g_lock);
  const bool any = !g_pendingJoin.empty();
  ReleaseSRWLockShared(&g_lock);
  if (!any) return;
  AcquireSRWLockExclusive(&g_lock);
  const std::string id = g_pendingJoin;
  g_pendingJoin.clear();
  ReleaseSRWLockExclusive(&g_lock);
  JoinNow(id);
}

std::string Text(const nlohmann::json& o, const char* key) {
  if (o.contains(key) && o[key].is_string()) return o[key].get<std::string>();
  if (o.contains("event_detail") && o["event_detail"].is_object()) {
    const nlohmann::json& d = o["event_detail"];
    if (d.contains(key) && d[key].is_string()) return d[key].get<std::string>();
  }
  return "";
}

// "3 HOURS", "30 MINUTES", "1 DAY", "7 DAYS" -> seconds; 0 (Atlas's default, 24 h) when it does not read.
long long ExpirySeconds(const std::string& s) {
  long long n = 0;
  std::size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) n = n * 10 + (s[i++] - '0');
  std::string unit;
  for (; i < s.size(); ++i)
    if (std::isalpha(static_cast<unsigned char>(s[i]))) unit += static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
  if (n <= 0) return 0;
  if (unit.rfind("min", 0) == 0) return n * 60;
  if (unit.rfind("hour", 0) == 0) return n * 3600;
  if (unit.rfind("day", 0) == 0) return n * 86400;
  return 0;
}

// The wizard's code fields: the "-----" text item of the INVITE CODE row (canvas 0a8654ced24a070b, on two actors)
// and of the CURRENT INVITE CODE panel (canvas 49e9b1982a63fcd1); the EXPIRES line (canvas 7f603e5c8d5b4798).
constexpr std::uint64_t kCodeItem = 0x35e290123dfa9c72;
constexpr std::uint64_t kCodeActors[] = {0xa9bd0cb4e6f2004c, 0xe28a9b96434eeff4, 0xfab58d7fa959be3b};
constexpr std::uint64_t kExpiresActor = 0x0ec8e16986ef88c8, kExpiresItem = 0xd38c12d22d3552eb;
// The INVITE LINK button (98be8277516da28c, ui_mnu_button) and the page it opens (actor c3f9f167b2a06043, canvas
// 8df8f57b922442b1). The stock page points at a dead graph.echo.games link; we make it the SHARE CODE page.
constexpr std::uint64_t kLinkButton = 0x98be8277516da28c, kButtonText = 0xc8c33e482e7606a4;  // "text"
constexpr std::uint64_t kLinkPage = 0xc3f9f167b2a06043;
constexpr std::uint64_t kLinkTitle = 0xa8452de53657bd6e, kLinkBody = 0xf9a947a0638c030d, kLinkText = 0x3657cd7405999810,
                        kLinkNote = 0xf9a947a0638c030e;
constexpr std::uint64_t kCloseLink = evr::Symbol64("evt_close_link_clicked");


// "2026-11-17T21:31:36Z" -> "11-17 22:31" in the player's local time; "" when it does not read.
std::string LocalExpiry(const std::string& iso) {
  SYSTEMTIME utc{}, local{};
  int y, mo, d, h, mi, sec;
  if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) return "";
  utc.wYear = static_cast<WORD>(y);
  utc.wMonth = static_cast<WORD>(mo);
  utc.wDay = static_cast<WORD>(d);
  utc.wHour = static_cast<WORD>(h);
  utc.wMinute = static_cast<WORD>(mi);
  utc.wSecond = static_cast<WORD>(sec);
  if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return "";
  char out[32];
  std::snprintf(out, sizeof(out), "%02u-%02u %02u:%02u", local.wMonth, local.wDay, local.wHour, local.wMinute);
  return out;
}

void CopyToClipboard(const std::string& text) {
  if (!OpenClipboard(nullptr)) return;
  EmptyClipboard();
  if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1)) {
    std::memcpy(GlobalLock(mem), text.c_str(), text.size() + 1);
    GlobalUnlock(mem);
    if (SetClipboardData(CF_TEXT, mem) == nullptr) GlobalFree(mem);
  }
  CloseClipboard();
}

void ShowCode() {
  const Result r = Last();
  if (r.kind != Kind::Create) return;
  const std::string code = r.ok ? r.code : "ERROR: " + r.error;
  for (std::uint64_t actor : kCodeActors) evr::PostUiText(actor, kCodeItem, code);
  if (!r.ok) return;
  const std::string expires = LocalExpiry(r.expiresAt);
  if (!expires.empty()) evr::PostUiText(kExpiresActor, kExpiresItem, "EXPIRES: " + expires);
  evr::PostUiText(kLinkButton, kButtonText, "SHARE CODE");
  evr::PostUiText(kLinkPage, kLinkTitle, "SHARE CODE");
  evr::PostUiText(kLinkPage, kLinkBody, "Players pick JOIN VIA CODE on the main menu and type this code.");
  evr::PostUiText(kLinkPage, kLinkText, r.code);
  evr::PostUiText(kLinkPage, kLinkNote,
                  expires.empty() ? "Copied to your clipboard." : "Copied to your clipboard. Expires " + expires + ".");
}

// After CLOSE on the share page the summary page comes back and writes <notset> again: write the code for a while.
void OnEvent(std::uint64_t space, std::uint64_t event) {
  if (evr::EventSender(space, event) == kHomePage) g_menuSpace = space;
  std::uint64_t symbol = 0;
  std::memcpy(&symbol, reinterpret_cast<const void*>(event + evr::event_layout::kSymbolOffset), 8);
  if (symbol == kCloseLink) g_repostFrames.store(90);
}

void Store(const Result& r) {
  AcquireSRWLockExclusive(&g_lock);
  g_last = r;
  ReleaseSRWLockExclusive(&g_lock);
}

std::string Squash(const std::string& s) {
  std::string out;
  for (char c : s)
    if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// The wizard's SERVER REGION is display text ("US WEST", "DEFAULT"); Atlas's invite/regions lists the caller's
// servers' own region codes with a location. A label that names a code or a location (ignoring case, spaces and
// punctuation; a whole match first, then one inside the other) picks that code, anything else "default" (best ping).
std::string RegionCode(const std::string& label) {
  const std::string want = Squash(label);
  if (want.empty() || want == "default" || want == "auto" || want == "notset") return "default";
  const evr_social::RpcReply r = evr_social::CallRpc("invite/regions", R"({"experience":"social"})");
  const nlohmann::json rows = r.ok ? nlohmann::json::parse(r.body, nullptr, false) : nlohmann::json();
  if (!rows.is_array()) {
    evr::Log(evr::LogLevel::Warning, "[EVR.INVITE] regions unavailable (%s): region \"%s\" -> default",
             r.error.c_str(), label.c_str());
    return "default";
  }
  std::string partial;
  for (const auto& row : rows) {
    if (!row.is_object()) continue;
    const std::string code = row.value("code", ""), where = row.value("location", "");
    if (code.empty() || code == "default") continue;
    const std::string c = Squash(code), w = Squash(where);
    if (want == c || want == w) return code;
    if (partial.empty() && ((!w.empty() && (w.find(want) != std::string::npos || want.find(w) != std::string::npos)) ||
                            c.find(want) != std::string::npos))
      partial = code;
  }
  if (g_dev) evr::Log(evr::LogLevel::Info, "[EVR.INVITE] regions %s", r.body.c_str());
  return partial.empty() ? "default" : partial;
}

std::string g_lastRegion, g_lastExpires;  // the last create's wizard settings, for REGENERATE; under g_lock

void Create(std::string region, std::string expires) {
  AcquireSRWLockExclusive(&g_lock);
  g_lastRegion = region;
  g_lastExpires = expires;
  ReleaseSRWLockExclusive(&g_lock);
  const long long seconds = ExpirySeconds(expires);
  const std::string code = RegionCode(region);
  const nlohmann::json req{{"experience", "social"}, {"region", code}, {"expires_s", seconds}};
  const evr_social::RpcReply r = evr_social::CallRpc("invite/create", req.dump());
  Result out;
  out.kind = Kind::Create;
  if (r.ok) {
    const nlohmann::json a = nlohmann::json::parse(r.body, nullptr, false);
    if (a.is_object()) {
      out.code = a.value("code", "");
      out.matchId = a.value("match_id", "");
      out.expiresAt = a.value("expires_at", "");
    }
    out.ok = !out.code.empty();
  }
  out.error = out.ok ? "" : (r.error.empty() ? "unreadable" : r.error);
  Store(out);
  if (out.ok) CopyToClipboard(out.code);
  g_repostFrames.store(30);
  evr::Log(out.ok ? evr::LogLevel::Info : evr::LogLevel::Warning,
           "[EVR.INVITE] create region=\"%s\" (%s) expires=\"%s\" (%llds) -> %s%s match=%s expires_at=%s status=%ld",
           region.c_str(), code.c_str(), expires.c_str(), seconds, out.ok ? "code " : "refused ",
           out.ok ? out.code.c_str() : out.error.c_str(), out.matchId.c_str(), out.expiresAt.c_str(), r.status);
}

// REGENERATE on the current-code page: end the code we hold (invite/revoke), then create a new one with the same
// settings. A code that is already gone still gets a new one.
void Regenerate() {
  const Result last = Last();
  AcquireSRWLockShared(&g_lock);
  const std::string region = g_lastRegion, expires = g_lastExpires;
  ReleaseSRWLockShared(&g_lock);
  if (last.kind == Kind::Create && last.ok) {
    const evr_social::RpcReply r = evr_social::CallRpc("invite/revoke", nlohmann::json{{"code", last.code}}.dump());
    evr::Log(r.ok ? evr::LogLevel::Info : evr::LogLevel::Warning, "[EVR.INVITE] revoke code_len=%zu -> %s status=%ld",
             last.code.size(), r.ok ? "ok" : r.error.c_str(), r.status);
  }
  Create(region, expires);
}

void Resolve(std::string code) {
  const nlohmann::json req{{"code", code}};
  const evr_social::RpcReply r = evr_social::CallRpc("invite/resolve", req.dump());
  Result out;
  out.kind = Kind::Join;
  out.code = code;
  if (r.ok) {
    const nlohmann::json a = nlohmann::json::parse(r.body, nullptr, false);
    if (a.is_object()) {
      out.matchId = a.value("match_id", "");
      out.mode = a.value("mode", "");
      out.level = a.value("level", "");
    }
    out.ok = !out.matchId.empty();
  }
  out.error = out.ok ? "" : (r.error.empty() ? "unreadable" : r.error);
  Store(out);
  if (out.ok) QueueJoin(out.matchId);
  evr::Log(out.ok ? evr::LogLevel::Info : evr::LogLevel::Warning,
           "[EVR.INVITE] join code_len=%zu -> %s%s mode=%s level=%s status=%ld", code.size(),
           out.ok ? "match " : "refused ", out.ok ? out.matchId.c_str() : out.error.c_str(), out.mode.c_str(),
           out.level.c_str(), r.status);
}

void OnMetric(const std::string& line) {
  const nlohmann::json doc = nlohmann::json::parse(line, nullptr, false);
  if (!doc.is_object() || Text(doc, "panel_type") != "deep_linking") return;
  const std::string panel = Text(doc, "panel_name"), event = Text(doc, "event_name");
  if (g_dev) evr::Log(evr::LogLevel::Info, "[EVR.INVITE] wizard click %s", line.c_str());
  else evr::Log(evr::LogLevel::Info, "[EVR.INVITE] wizard click panel=%s event=%s", panel.c_str(), event.c_str());
  if (panel == "create_game_review_settings" && event == "create_invite") {
    std::thread(Create, Text(doc, "Server_region"), Text(doc, "Expires_after")).detach();
  } else if (panel == "create_game_summary" && event == "join") {
    const Result last = Last();
    if (last.kind == Kind::Create && last.ok) QueueJoin(last.matchId);
    else evr::Log(evr::LogLevel::Warning, "[EVR.INVITE] join click but no invite was created (%s)", last.error.c_str());
  } else if (event.find("regenerate") != std::string::npos || panel.find("regenerate") != std::string::npos) {
    std::thread(Regenerate).detach();
  } else if (panel == "join_via_code" && event == "join") {
    std::string code = Text(doc, "join_code");
    if (code.empty() || code == "<invalid>") {
      evr::Log(evr::LogLevel::Warning, "[EVR.INVITE] join click without a code");
      return;
    }
    std::thread(Resolve, code).detach();
  }
}

std::uint64_t SubmitHook(const char* line, std::uint64_t length, std::uint64_t a, std::uint64_t b) {
  if (line != nullptr && length > 0 && length <= kMaxLine && evr::IsReadable(line, length)) {
    const std::string text(line, static_cast<std::size_t>(length));
    if (text.find("deep_linking") != std::string::npos) OnMetric(text);
  } else if (g_dev && g_shapeLogs.fetch_add(1) < 5) {
    evr::Log(evr::LogLevel::Info, "[EVR.INVITE] submit args not (line, length): rcx=%p rdx=%llu", line,
             (unsigned long long)length);
  }
  return g_submit(line, length, a, b);
}

}  // namespace

Result Last() {
  AcquireSRWLockShared(&g_lock);
  Result r = g_last;
  ReleaseSRWLockShared(&g_lock);
  return r;
}

bool Install(bool dev) {
  g_dev = dev;
  evr::OnFrame(&Frame);
  evr::OnComponentEvent(&OnEvent);
  return evr::HookVA(kSubmitVA, kSubmitPrologue, reinterpret_cast<void*>(&SubmitHook),
                     reinterpret_cast<void**>(&g_submit), "remote log submit (invite wizard)");
}

}  // namespace evr_invite
