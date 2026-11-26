// evr-runtime.dll entry. Loaded by the loader (hid.dll) from the active loose folder before echovr.exe's own entry point
// runs, so engine hooks are in place before CEngine::Init.
#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

#include "evr/actors.h"
#include "evr/core.h"
#include "evr/events.h"
#include "evr/frame.h"
#include "evr/resources.h"
#include "evr/symbol.h"
#include "evr/ui_text.h"
#include "cdn/cdn.h"
#include "content/content.h"
#include "cosmetics/cosmetics.h"
#include "earlyquit/lockout.h"
#include "menu/wording.h"
#include "friends/friends_install.h"
#include "login/login.h"
#include "login/unlocks_probe.h"
#include "menu/create_join.h"
#include "menu/invite.h"
#include "menu/queue.h"
#include "evr/crashlog.h"
#include "evr/seh.h"
#include "combat/combat.h"
#if EVR_PARTY
#include "party/party.h"
#endif
#include "updates/updates.h"

namespace {

// The build every pinned address in this runtime was read from (PE header facts of the stock Win10 echovr.exe).
struct GameBuild {
  std::uint32_t timestamp;
  std::uint32_t sizeOfImage;
};

GameBuild ReadBuild() {
  const auto base = evr::GameBase();
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  return {nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage};
}

nlohmann::json ReadConfig() {
  std::string text;
  if (FILE* f = std::fopen((evr::PluginsDir() + "\\config.json").c_str(), "rb")) {
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
  }
  try {
    nlohmann::json j = nlohmann::json::parse(text);
    return j.is_object() ? j : nlohmann::json::object();
  } catch (const nlohmann::json::exception&) {
    return nlohmann::json::object();
  }
}

// plugins\config.json "log_events": N logs the first N distinct component events (dev aid; 0 or absent: off).
int LogEventsSetting() {
  try {
    return ReadConfig().value("log_events", 0);
  } catch (const nlohmann::json::exception&) {
    return 0;
  }
}

// plugins\config.json "queue_gate": true opens the matchmaker queue's platform gate (menu/queue_pc.cpp). Off by
// default: with it on the game crashed on the first social lobby find (2026-11-17 22:23 and 22:28).
// plugins\config.json "combat": false turns EchoCombat (combat/) off; on by default.
bool CombatSetting() {
  try {
    return ReadConfig().value("combat", true);
  } catch (const nlohmann::json::exception&) {
    return true;
  }
}

bool QueueGateSetting() {
  try {
    return ReadConfig().value("queue_gate", false);
  } catch (const nlohmann::json::exception&) {
    return false;
  }
}

// plugins\config.json "queue_estimate_scale": test aid for the FIND MATCH bar's unit (default 1).
int QueueEstimateScaleSetting() {
  try {
    return ReadConfig().value("queue_estimate_scale", 1);
  } catch (const nlohmann::json::exception&) {
    return 1;
  }
}

// plugins\config.json "content_mount": "before_stock" mounts the release's sets before the stock ones (dev
// experiment; default after), "content_override": false only logs which copy the game read (default: serve ours).
evr_content::Options ContentSettings() {
  evr_content::Options o;
  try {
    const nlohmann::json c = ReadConfig();
    o.before_stock = c.value("content_mount", std::string("after_stock")) == "before_stock";
    o.override_stock = c.value("content_override", true);
    o.probe = c.value("content_probe", false);
  } catch (const nlohmann::json::exception&) {
  }
  return o;
}

// Whether the game was started as a dedicated server: `-server` among its command-line arguments (any case).
bool IsServerLaunch() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return false;
  bool server = false;
  for (int i = 1; i < argc && !server; ++i) server = _wcsicmp(argv[i], L"-server") == 0;
  LocalFree(argv);
  return server;
}

}  // namespace

extern "C" __declspec(dllexport) int EvrRuntimeStart(const char* version) {
  const GameBuild b = ReadBuild();
  evr::Log(evr::LogLevel::Info, "[EVR] runtime %s start: game timestamp=0x%08x image=0x%x", version ? version : "?",
           b.timestamp, b.sizeOfImage);
  evr::InstallCrashLog();  // first: names any later crash in evr-runtime.log
  evr::InstallFaultGuard();  // after the crash log: a fault a guard catches is not logged as a crash
  const bool server = IsServerLaunch();
  // EchoCombat, on clients and servers alike (a combat server runs its scripts): its content before the game reads
  // its manifests, its gear once the runtime's own hooks are in (combat/combat.h).
  const bool combat = CombatSetting();
  if (combat) evr_combat::InstallContent(server);
  else evr::Log(evr::LogLevel::Info, "[EVR.COMBAT] off (config.json \"combat\": false)");
  if (server) {
    // A dedicated server (`-server`): login only (the runtime declaration). Every client
    // change (menus, item list, content sets, CDN, friends, queue, updates) is skipped: with them the server failed
    // to load the lobby level ("[NETGAME] Failed to load level", mpl_lobby_b2, 2026-11-18).
    evr::Log(evr::LogLevel::Info, "[EVR] server launch (-server): client changes skipped");
    evr_login::Install(version);
    if (combat) evr_combat::InstallGear(true);
    return 0;
  }
  evr::InstallResourceHook();
  evr_earlyquit::Install();  // registers frame and event callbacks
  if (QueueGateSetting()) evr_queue::Install(QueueEstimateScaleSetting());  // before login: the queue mode is read when login settings arrive
  evr_wording::Install();  // a text filter: before InstallUiText
  evr::InstallUiText();
#if EVR_PARTY
  evr_party::Install();  // the Social object and party replies; registers a frame callback (cmake -DEVR_PARTY=ON)
#endif
  evr::InstallFrameHook();
  evr_friends::Install();  // registers its frame and event callbacks
  evr_content::Install(ContentSettings());  // the release's own package sets (after friends: same AddPackage hook)
  evr_cosmetics::Install();  // Story rarity, booth source lines, release chassis (needs content; before the CDN)
  evr_create_join::Install();
  const int logEvents = LogEventsSetting();
  evr_updates::Install();  // after the friends panel: its level edits run first
  evr::LogDistinctEvents(logEvents);
  evr::InstallEventHook();
  evr::InstallActorHooks(logEvents);
  evr_invite::Install(logEvents > 0);
  // No provider redirect (pnsovr -> pnsdemo): nEVR signs the player in (echovrce.com/device) and the runtime runs on
  // top of it, using nEVR's token for friends and parties (social/social_pc.cpp).
  evr_login::Install(version);
  if (logEvents > 0) evr_unlocks_probe::Install();
  evr_cdn::Start();
  if (combat) evr_combat::InstallGear(false);  // last: after every runtime hook (combat/combat.h)
  // Features install here, each guarded by its own prologue checks:
  //   friends panel (resource transforms, AddPackage, avatars), update panel, CDN cosmetics/posters,
  //   early-quit lockout countdown.
  return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
  return TRUE;
}
