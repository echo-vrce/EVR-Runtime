// The game's login (our reading of 0x14017EF10, "[NETGAME] Logging in...") builds the login profile in a CJson on its
// stack: lobbyversion, publisher_lock, hmdserialnumber, hmdproductname, then the game's own fields (0x140129B40), and
// hands it to the login service's send (0x14060C370, (service, CJson*)). The call returns to 0x14017F048. We hook that
// send and, only for that call site, add our object with the game's own path setters (0x1405FDB30 string,
// 0x1405FD4F0 integer; "a|b" is a nested path, as the game's own "incident_categories[%llu]|id").
#include "login/login.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <windows.h>

#include <nlohmann/json.hpp>

#include "evr/core.h"

#ifndef EVR_BUILD
#define EVR_BUILD "dev"
#endif
#ifndef EVR_COMMIT
#define EVR_COMMIT ""
#endif

namespace evr_login {
namespace {

using SendFn = std::uint64_t (*)(void* service, void* json);
using SetStringFn = void* (*)(void* json, const char* path, const char* value);
using SetIntFn = void* (*)(void* json, const char* path, std::int64_t value);

constexpr std::uint64_t kSendVA = 0x14060C370;
constexpr std::uint8_t kSendPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
                                          0xEC, 0x20, 0x8B, 0x81, 0x9C, 0x00, 0x00, 0x00};
constexpr std::uint64_t kLoginReturnVA = 0x14017F048;
constexpr std::uint64_t kSetStringVA = 0x1405FDB30, kSetIntVA = 0x1405FD4F0;
constexpr std::uint8_t kSetterPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
                                            0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x81};

SendFn g_send = nullptr;
std::string g_version = "0.0.0", g_channel = "stable";

bool Matches(std::uint64_t va, const std::uint8_t (&prologue)[16]) {
  void* at = evr::ResolveVA(va);
  return at != nullptr && evr::IsReadable(at, 16) && std::memcmp(at, prologue, 16) == 0;
}

// plugins\config.json "channel" (the updater's), "stable" when absent.
std::string ChannelSetting() {
  std::string text;
  if (FILE* f = std::fopen((evr::PluginsDir() + "\\config.json").c_str(), "rb")) {
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
  }
  const nlohmann::json c = nlohmann::json::parse(text, nullptr, false);
  if (c.is_object() && c.contains("channel") && c["channel"].is_string() && !c["channel"].get<std::string>().empty())
    return c["channel"].get<std::string>();
  return "stable";
}

// The loader's channel for this boot (it asks Atlas: "beta" for /beta testers), else plugins\config.json's.
std::string LoaderChannel() {
  using ChannelFn = std::size_t (*)(char*, std::size_t);
  HMODULE h = GetModuleHandleA((evr::GameDir() + "\hid.dll").c_str());
  auto fn = h != nullptr ? reinterpret_cast<ChannelFn>(GetProcAddress(h, "EvrUpdateChannel")) : nullptr;
  if (fn == nullptr) return g_channel;
  char buf[32] = {};
  const std::size_t n = fn(buf, sizeof(buf));
  return n > 0 && n < sizeof(buf) ? std::string(buf, n) : g_channel;
}

void Declare(void* json) {
  if (!Matches(kSetStringVA, kSetterPrologue) || !Matches(kSetIntVA, kSetterPrologue)) {
    evr::Log(evr::LogLevel::Warning, "[EVR.LOGIN] evr_runtime not declared: game build differs");
    return;
  }
  auto setString = reinterpret_cast<SetStringFn>(evr::ResolveVA(kSetStringVA));
  auto setInt = reinterpret_cast<SetIntFn>(evr::ResolveVA(kSetIntVA));
  setString(json, "evr_runtime|version", g_version.c_str());
  setString(json, "evr_runtime|build", EVR_BUILD);
  setString(json, "evr_runtime|commit", EVR_COMMIT);
  setString(json, "evr_runtime|platform", "pc");
  const std::string channel = LoaderChannel();
  setString(json, "evr_runtime|channel", channel.c_str());
  // Social level 1 would also bring Atlas's SNS friend-presence and recently-met pushes, which nothing here handles.
  // Parties (party/) need no level: Atlas sends party data to a client once it has sent an SNS party request
  // (SessionParameters.ReadsPartyData).
  setInt(json, "evr_runtime|social", 0);
  evr::Log(evr::LogLevel::Info, "[EVR.LOGIN] declared evr_runtime version=%s build=%s channel=%s platform=pc social=0",
           g_version.c_str(), EVR_BUILD, channel.c_str());
}

std::uint64_t SendHook(void* service, void* json) {
  const auto ret = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
  if (json != nullptr && ret == reinterpret_cast<std::uintptr_t>(evr::ResolveVA(kLoginReturnVA))) Declare(json);
  return g_send(service, json);
}

}  // namespace

bool Install(const char* version) {
  if (version != nullptr && *version) g_version = version;
  if (!g_version.empty() && g_version[0] == 'v') g_version.erase(0, 1);
  g_channel = ChannelSetting();
  return evr::HookVA(kSendVA, kSendPrologue, reinterpret_cast<void*>(&SendHook), reinterpret_cast<void**>(&g_send),
                     "login service send (evr_runtime declaration)");
}

}  // namespace evr_login
