// evr/core.h: the runtime's own platform layer (PC). Logging, memory, hooks, HTTP, paths.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace evr {

// ---- log: <game>\plugins\evr-runtime.log ----
enum class LogLevel { Info, Warning, Error };
void Log(LogLevel level, const char* format, ...);

// ---- memory ----
bool IsReadable(const void* at, std::size_t length);
bool IsWritable(void* at, std::size_t length);

// ---- the game image ----
inline constexpr std::uint64_t kImageBase = 0x140000000ULL;
std::uintptr_t GameBase();
/// A static VA of echovr.exe rebased to this run; nullptr when outside the image.
void* ResolveVA(std::uint64_t va);
/// True when `at` starts with `bytes` (guards every hook against another game build).
bool CheckBytes(const void* at, const std::uint8_t* bytes, std::size_t length);

/// Detour `target` to `detour`; `*original` receives the trampoline. Logged by `name`.
bool Hook(void* target, void* detour, void** original, const char* name);

/// One checked hook by VA + expected prologue. Returns false (and logs) when the bytes differ.
template <std::size_t N>
bool HookVA(std::uint64_t va, const std::uint8_t (&prologue)[N], void* detour, void** original, const char* name) {
  void* t = ResolveVA(va);
  if (t == nullptr || !CheckBytes(t, prologue, N)) {
    Log(LogLevel::Warning, "[EVR.HOOK] %s skipped: prologue mismatch at 0x%llx", name, static_cast<unsigned long long>(va));
    return false;
  }
  return Hook(t, detour, original, name);
}

// ---- HTTP (WinHTTP; worker threads only) ----
struct HttpReply {
  bool ok = false;
  long status = 0;
  std::string body;
};
HttpReply Http(const char* method, const std::string& url, const std::string& bearer, const std::string& payload,
               std::size_t maxBytes = 4u << 20);

// ---- paths ----
std::string GameDir();     // ...\bin\win10
std::string PluginsDir();  // ...\bin\win10\plugins
std::string CdnDir();      // ...\bin\win10\CDN
std::string ContentDir();  // ...\bin\win10\plugins\content (runtime-written package sets)

}  // namespace evr
