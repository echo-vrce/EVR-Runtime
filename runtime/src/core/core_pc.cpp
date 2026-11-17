#include <windows.h>
#include <winhttp.h>

#include <MinHook.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "evr/core.h"

namespace evr {
namespace {

std::mutex g_logMutex;

std::wstring Wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

bool Region(const void* at, std::size_t length, DWORD wantMask) {
  if (at == nullptr) return false;
  auto p = reinterpret_cast<std::uintptr_t>(at);
  const std::uintptr_t end = p + length;
  while (p < end) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(p), &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(mbi.Protect & wantMask)) return false;
    p = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
  }
  return true;
}

}  // namespace

void Log(LogLevel level, const char* format, ...) {
  char line[2048];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  SYSTEMTIME t;
  GetLocalTime(&t);
  const char* tag = level == LogLevel::Info ? "I" : level == LogLevel::Warning ? "W" : "E";
  std::lock_guard<std::mutex> lock(g_logMutex);
  if (FILE* f = std::fopen((PluginsDir() + "\\evr-runtime.log").c_str(), "ab")) {
    std::fprintf(f, "%02d:%02d:%02d.%03d %s %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, tag, line);
    std::fclose(f);
  }
}

bool IsReadable(const void* at, std::size_t length) {
  return Region(at, length, PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
}

bool IsWritable(void* at, std::size_t length) {
  return Region(at, length, PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
}

std::uintptr_t GameBase() { return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)); }

void* ResolveVA(std::uint64_t va) {
  const std::uintptr_t base = GameBase();
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (va < kImageBase || va - kImageBase >= nt->OptionalHeader.SizeOfImage) return nullptr;
  return reinterpret_cast<void*>(base + static_cast<std::uintptr_t>(va - kImageBase));
}

bool CheckBytes(const void* at, const std::uint8_t* bytes, std::size_t length) {
  return IsReadable(at, length) && std::memcmp(at, bytes, length) == 0;
}

bool Hook(void* target, void* detour, void** original, const char* name) {
  static const MH_STATUS init = MH_Initialize();
  (void)init;
  MH_STATUS s = MH_CreateHook(target, detour, original);
  if (s == MH_OK) s = MH_EnableHook(target);
  Log(s == MH_OK ? LogLevel::Info : LogLevel::Error, "[EVR.HOOK] %s %s (%s)", name, s == MH_OK ? "installed" : "FAILED",
      MH_StatusToString(s));
  return s == MH_OK;
}

HttpReply Http(const char* method, const std::string& url, const std::string& bearer, const std::string& payload,
               std::size_t maxBytes) {
  HttpReply reply;
  const std::wstring wurl = Wide(url);
  URL_COMPONENTS uc{sizeof(uc)};
  wchar_t host[256]{}, path[2048]{};
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2048;
  wchar_t extra[2048]{};
  uc.lpszExtraInfo = extra;
  uc.dwExtraInfoLength = 2048;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return reply;
  HINTERNET s = WinHttpOpen(L"evr-runtime", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
  if (!s) return reply;
  WinHttpSetTimeouts(s, 10000, 10000, 20000, 20000);
  HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
  const std::wstring target = std::wstring(path) + extra;
  HINTERNET r = c ? WinHttpOpenRequest(c, Wide(method).c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                  : nullptr;
  if (r) {
    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!bearer.empty()) headers += L"Authorization: Bearer " + Wide(bearer) + L"\r\n";
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(r, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    const bool sent = WinHttpSendRequest(r, headers.c_str(), static_cast<DWORD>(-1),
                                         payload.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(payload.data()),
                                         static_cast<DWORD>(payload.size()), static_cast<DWORD>(payload.size()), 0) &&
                      WinHttpReceiveResponse(r, nullptr);
    if (sent) {
      DWORD status = 0, len = sizeof(status);
      WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                          &len, WINHTTP_NO_HEADER_INDEX);
      reply.status = static_cast<long>(status);
      reply.ok = true;
      for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0) break;
        if (reply.body.size() + avail > maxBytes) {
          reply.ok = false;
          break;
        }
        const std::size_t at = reply.body.size();
        reply.body.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(r, reply.body.data() + at, avail, &got)) {
          reply.ok = false;
          break;
        }
        reply.body.resize(at + got);
      }
    }
    WinHttpCloseHandle(r);
  }
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  return reply;
}

std::string GameDir() {
  char p[MAX_PATH]{};
  GetModuleFileNameA(nullptr, p, MAX_PATH);
  std::string s(p);
  return s.substr(0, s.find_last_of("\\/"));
}
std::string PluginsDir() { return GameDir() + "\\plugins"; }
std::string CdnDir() { return GameDir() + "\\CDN"; }
std::string ContentDir() { return PluginsDir() + "\\content"; }

}  // namespace evr
