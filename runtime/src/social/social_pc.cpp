// social_pc.cpp: Atlas access for the runtime on PC (social.h).
#include "social/social.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "evr/core.h"
#include "evr/symbol.h"

namespace evr_social {
namespace {

using evr::Log;
using evr::LogLevel;

constexpr std::uint64_t kRosterEveryMs = 20000;
constexpr std::uint64_t kTokenRetryMs = 60000;
constexpr std::uint64_t kTokenMarginS = 120;  // refresh this long before the access token expires

// ---- the game's own config ----

struct Config {
  bool read = false;
  std::string base;       // apiservice_host without the trailing '/'
  std::string discordId;  // from loginservice_host
  std::string password;   // from loginservice_host; never logged
};

std::string UrlDecode(const std::string& s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
      i += 2;
    } else {
      out.push_back(s[i] == '+' ? ' ' : s[i]);
    }
  }
  return out;
}

std::string QueryValue(const std::string& url, const std::string& key) {
  const std::size_t q = url.find('?');
  if (q == std::string::npos) return "";
  std::size_t at = q + 1;
  while (at < url.size()) {
    std::size_t end = url.find('&', at);
    if (end == std::string::npos) end = url.size();
    const std::string pair = url.substr(at, end - at);
    const std::size_t eq = pair.find('=');
    if (eq != std::string::npos && pair.substr(0, eq) == key) return UrlDecode(pair.substr(eq + 1));
    at = end + 1;
  }
  return "";
}

Config ReadConfig() {
  Config c;
  c.read = true;
  const std::string path = evr::GameDir() + "\\..\\..\\_local\\config.json";
  std::string text;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
  }
  try {
    const nlohmann::json j = nlohmann::json::parse(text);
    c.base = j.value("apiservice_host", "");
    while (!c.base.empty() && c.base.back() == '/') c.base.pop_back();
    const std::string login = j.value("loginservice_host", "");
    c.discordId = QueryValue(login, "discordid");
    if (c.discordId.empty()) c.discordId = QueryValue(login, "discord_id");
    c.password = QueryValue(login, "password");
  } catch (const nlohmann::json::exception&) {
  }
  Log(LogLevel::Info, "[EVR.SOCIAL] game config %s api=%s login_credentials=%s", text.empty() ? "missing" : "read",
      c.base.empty() ? "none" : c.base.c_str(), !c.discordId.empty() && !c.password.empty() ? "yes" : "no");
  return c;
}

// ---- token ----

struct TokenState {
  std::mutex mutex;
  Config config;
  std::string access;
  std::uint64_t expiresAtMs = 0;
  std::uint64_t retryAtMs = 0;
};

TokenState& T() {
  static TokenState t;
  return t;
}

const Config& Cfg() {
  TokenState& t = T();
  if (!t.config.read) t.config = ReadConfig();
  return t.config;
}

// ---- roster ----

struct RosterState {
  std::mutex mutex;
  std::condition_variable wake;
  bool wakeSoon = false;
  std::shared_ptr<Roster> fetched;      // newest from the worker, not adopted yet
  std::vector<std::uint64_t> removals;  // applied at Adopt
  std::atomic<bool> started{false};
};

RosterState& R() {
  static RosterState r;
  return r;
}

Roster& Current() {  // game-loop thread only
  static Roster roster;
  return roster;
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool FetchRoster(Roster* out) {
  const std::string base = ServiceBase();
  const std::string token = Token();
  if (base.empty() || token.empty()) return false;
  // state 0: mutual friends.
  const evr::HttpReply reply = evr::Http("GET", base + "/v2/friend?limit=100&state=0", token, "");
  if (!reply.ok || reply.status != 200) {
    Log(LogLevel::Warning, "[EVR.SOCIAL] friend list request failed status=%ld", reply.status);
    return false;
  }
  try {
    const nlohmann::json doc = nlohmann::json::parse(reply.body);
    for (const auto& f : doc.value("friends", nlohmann::json::array())) {
      if (f.value("state", 0) != 0) continue;
      const auto& u = f.value("user", nlohmann::json::object());
      Friend fr;
      fr.userId = u.value("id", "");
      if (fr.userId.empty()) continue;
      fr.id = evr::Symbol64(fr.userId);
      fr.name = u.value("display_name", "");
      if (fr.name.empty()) fr.name = u.value("username", "");
      fr.avatarUrl = u.value("avatar_url", "");
      fr.online = u.value("online", false);
      out->friends_.push_back(std::move(fr));
    }
  } catch (const nlohmann::json::exception&) {
    Log(LogLevel::Warning, "[EVR.SOCIAL] friend list unreadable bytes=%zu", reply.body.size());
    return false;
  }
  std::stable_sort(out->friends_.begin(), out->friends_.end(), [](const Friend& a, const Friend& b) {
    if (a.online != b.online) return a.online;
    return Lower(a.name) < Lower(b.name);
  });
  return true;
}

void Worker() {
  RosterState& r = R();
  std::size_t lastCount = static_cast<std::size_t>(-1);
  for (;;) {
    auto roster = std::make_shared<Roster>();
    if (FetchRoster(roster.get())) {
      if (roster->friends_.size() != lastCount) {
        Log(LogLevel::Info, "[EVR.SOCIAL] friends=%zu online=%u", roster->friends_.size(), roster->Online());
        lastCount = roster->friends_.size();
      }
      std::lock_guard<std::mutex> lock(r.mutex);
      r.fetched = std::move(roster);
    }
    std::unique_lock<std::mutex> lock(r.mutex);
    r.wake.wait_for(lock, std::chrono::milliseconds(kRosterEveryMs), [&] { return r.wakeSoon; });
    r.wakeSoon = false;
  }
}

// nEVR (nevr-runtime.dll) signs the player in through echovrce.com/device and keeps the session token; friends and
// parties use that token when nEVR is loaded, so a player's nEVR friends are the friends shown here.
HMODULE Nevr() { return GetModuleHandleW(L"nevr-runtime.dll"); }

std::string NevrToken() {
  using GetToken = const char* (*)();
  HMODULE h = Nevr();
  auto fn = h ? reinterpret_cast<GetToken>(GetProcAddress(h, "TokenAuth_GetToken")) : nullptr;
  const char* token = fn ? fn() : nullptr;
  return token != nullptr ? std::string(token) : std::string();
}

std::uint64_t NevrDiscordId() {
  using GetId = std::uint64_t (*)();
  HMODULE h = Nevr();
  auto fn = h ? reinterpret_cast<GetId>(GetProcAddress(h, "TokenAuth_GetDiscordId")) : nullptr;
  return fn ? fn() : 0;
}

}  // namespace

std::uint32_t Roster::Online() const {
  return static_cast<std::uint32_t>(std::count_if(friends_.begin(), friends_.end(), [](const Friend& f) { return f.online; }));
}

bool Roster::IdAt(std::uint32_t row, std::uint64_t* id) const {
  if (row >= Count()) return false;
  *id = friends_[row].id;
  return true;
}

const Friend* Roster::Find(std::uint64_t id) const {
  for (const Friend& f : friends_)
    if (f.id == id) return &f;
  return nullptr;
}

void Start() {
  if (R().started.exchange(true)) return;
  std::thread(Worker).detach();
}

void Adopt() {
  RosterState& r = R();
  std::shared_ptr<Roster> next;
  std::vector<std::uint64_t> removals;
  {
    std::lock_guard<std::mutex> lock(r.mutex);
    next = std::move(r.fetched);
    removals.swap(r.removals);
  }
  Roster& cur = Current();
  if (next) cur.friends_ = std::move(next->friends_);
  for (const std::uint64_t id : removals) {
    cur.friends_.erase(std::remove_if(cur.friends_.begin(), cur.friends_.end(), [id](const Friend& f) { return f.id == id; }),
                       cur.friends_.end());
  }
}

const Roster& Friends() { return Current(); }

void Remove(std::uint64_t id) {
  {
    std::lock_guard<std::mutex> lock(R().mutex);
    R().removals.push_back(id);
  }
  RefreshSoon();
}

void RefreshSoon() {
  {
    std::lock_guard<std::mutex> lock(R().mutex);
    R().wakeSoon = true;
  }
  R().wake.notify_all();
}

std::string ServiceBase() {
  std::lock_guard<std::mutex> lock(T().mutex);
  return Cfg().base;
}

std::uint64_t AccountId() {
  if (const std::uint64_t id = NevrDiscordId()) return id;
  std::lock_guard<std::mutex> lock(T().mutex);
  const std::string& id = Cfg().discordId;
  if (id.empty() || id.size() > 20 || id.find_first_not_of("0123456789") != std::string::npos) return 0;
  return std::strtoull(id.c_str(), nullptr, 10);
}

std::string Token() {
  TokenState& t = T();
  std::lock_guard<std::mutex> lock(t.mutex);
  const Config& c = Cfg();
  const std::uint64_t now = GetTickCount64();
  if (Nevr() != nullptr) {
    std::string token = NevrToken();
    static bool logged = false;
    if (!logged || token.empty() != t.access.empty()) {
      Log(LogLevel::Info, "[EVR.SOCIAL] %s", token.empty() ? "nEVR loaded, not signed in yet" : "using nEVR's sign-in token");
      logged = true;
    }
    t.access = token;
    return token;
  }
  if (!t.access.empty() && now < t.expiresAtMs) return t.access;
  if (now < t.retryAtMs) return "";
  if (c.base.empty() || c.discordId.empty() || c.password.empty()) {
    t.retryAtMs = now + kTokenRetryMs;
    Log(LogLevel::Info, "[EVR.SOCIAL] no token: the game config has no %s", c.base.empty() ? "apiservice_host" : "login credentials");
    return "";
  }
  const nlohmann::json body{{"discord_id", c.discordId}, {"password", c.password}};
  const evr::HttpReply reply = evr::Http("POST", c.base + "/evr/runtime/token", "", body.dump(), 64 * 1024);
  if (!reply.ok || reply.status != 200) {
    t.retryAtMs = now + kTokenRetryMs;
    Log(LogLevel::Warning, "[EVR.SOCIAL] token request failed status=%ld (Atlas needs /evr/runtime/token)", reply.status);
    return "";
  }
  try {
    const nlohmann::json j = nlohmann::json::parse(reply.body);
    t.access = j.value("access_token", "");
    const long long expires = j.value("expires_in", 3600LL);
    t.expiresAtMs = now + static_cast<std::uint64_t>(std::max<long long>(expires - kTokenMarginS, 60)) * 1000;
  } catch (const nlohmann::json::exception&) {
    t.access.clear();
  }
  if (t.access.empty()) {
    t.retryAtMs = now + kTokenRetryMs;
    Log(LogLevel::Warning, "[EVR.SOCIAL] token reply unreadable");
    return "";
  }
  Log(LogLevel::Info, "[EVR.SOCIAL] token issued, valid for %llus",
      static_cast<unsigned long long>((t.expiresAtMs - now) / 1000));
  return t.access;
}

RpcReply InviteToParty(const std::string& userId) {
  if (userId.empty()) {
    RpcReply r;
    r.error = "invalid";
    return r;
  }
  const nlohmann::json body{{"user_id", userId}};
  RpcReply r = CallRpc("party/invite", body.dump());
  if (!r.ok)
    Log(LogLevel::Info, "[EVR.SOCIAL] party invite not sent status=%ld error=%s%s", r.status, r.error.c_str(),
        r.status == 404 ? " (Atlas has no party/invite RPC yet)" : "");
  return r;
}

const char* PartyInviteLabel(const RpcReply& r) {
  if (r.ok) return "INVITED";
  if (r.error == "not_in_game") return "JOIN A GAME FIRST";
  if (r.error == "not_in_party") return "MAKE A PARTY FIRST";
  if (r.error == "target_offline") return "FRIEND IS OFFLINE";
  if (r.status == 404) return "INVITES SOON";
  return "TRY AGAIN";
}

RpcReply CallRpc(const std::string& id, const std::string& json) {
  RpcReply r;
  const std::string base = ServiceBase();
  const std::string token = Token();
  if (base.empty() || token.empty()) {
    r.error = "no_token";
    return r;
  }
  const evr::HttpReply reply = evr::Http("POST", base + "/v2/rpc/" + id + "?unwrap", token, json);
  r.status = reply.status;
  r.body = reply.body;
  r.ok = reply.ok && reply.status == 200;
  if (!r.ok) {
    const nlohmann::json e = nlohmann::json::parse(reply.body, nullptr, false);
    if (e.is_object() && e.contains("message") && e["message"].is_string()) r.error = e["message"].get<std::string>();
    if (r.error.empty()) r.error = reply.ok ? "status" : "unreachable";
  }
  return r;
}

}  // namespace evr_social
