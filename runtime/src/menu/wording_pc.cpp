// menu/wording_pc.cpp: applies menu/wording.h through the game's text setter, and over the stock strings in the
// script libraries (`<16 hex>.dll`, loaded and unloaded by the game at run time) for texts that skip the setter.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "evr/core.h"
#include "evr/frame.h"
#include "evr/ui_text.h"
#include "menu/wording.h"

namespace evr_wording {
namespace {

const char* Filter(std::uint64_t /*item*/, const char* text) { return Reword(text); }

constexpr DWORD kMaxModules = 1024;

struct Site {
  char* at;
  const Rewrite* rewrite;
  const char* written;
};
std::map<HMODULE, std::vector<Site>> g_libraries;  // game-loop thread only

// Script libraries the loader reported (LdrRegisterDllNotification), handed to the game-loop thread. Asking Windows
// for every module's name on a timer instead (K32EnumProcessModules + K32GetModuleBaseNameA every 250 ms) walked the
// whole module list once per module through system calls: with Echo's hundreds of script libraries loaded that took
// the game down to ~2 fps (player report, 2026-11-26).
struct Loaded {
  HMODULE lib;
  bool loaded;  // false: unloaded
  char name[24];
};
std::mutex g_queueMutex;
std::vector<Loaded> g_queue;
std::atomic<bool> g_queued{false};

bool IsScriptLibrary(const char* name) {
  if (std::strlen(name) != 20 || _stricmp(name + 16, ".dll") != 0) return false;
  for (int i = 0; i < 16; ++i)
    if (!std::isxdigit(static_cast<unsigned char>(name[i]))) return false;
  return true;
}

bool Write(char* at, const char* text, std::size_t clear) {
  DWORD old = 0;
  if (!VirtualProtect(at, clear, PAGE_READWRITE, &old)) return false;
  std::memset(at, 0, clear);
  std::memcpy(at, text, std::strlen(text));
  VirtualProtect(at, clear, old, &old);
  return true;
}

std::size_t Span(const Site& s) {
  return std::max(std::strlen(s.rewrite->stock), std::strlen(s.written)) + 1;
}

/// Finds and rewrites every whole stock string in the library's read-only data sections.
std::vector<Site> Scan(HMODULE lib, const char* name) {
  std::vector<Site> sites;
  auto* base = reinterpret_cast<std::uint8_t*>(lib);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (!evr::IsReadable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE) return sites;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (!evr::IsReadable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE) return sites;
  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
    const DWORD c = sec->Characteristics;
    if (!(c & IMAGE_SCN_CNT_INITIALIZED_DATA) || (c & IMAGE_SCN_MEM_EXECUTE)) continue;
    const std::size_t size = std::min<std::size_t>(sec->Misc.VirtualSize, sec->SizeOfRawData);
    auto* data = base + sec->VirtualAddress;
    if (size == 0 || !evr::IsReadable(data, size)) continue;
    for (const Rewrite& r : kRewrites) {
      for (std::size_t off : FindWhole(data, size, r.stock)) {
        const char* want = InPlace(data, size, off, r);
        const unsigned long rva = static_cast<unsigned long>(sec->VirtualAddress + off);
        if (want == nullptr) {
          evr::Log(evr::LogLevel::Warning, "[EVR.WORDING] %s+0x%lx \"%s\": no room, stays stock", name, rva, r.stock);
          continue;
        }
        Site s{reinterpret_cast<char*>(data + off), &r, want};
        const bool ok = Write(s.at, want, Span(s));
        evr::Log(ok ? evr::LogLevel::Info : evr::LogLevel::Warning, "[EVR.WORDING] %s+0x%lx \"%s\" -> \"%s\"%s", name,
                 rva, r.stock, want, ok ? "" : " (write failed)");
        if (ok) sites.push_back(s);
      }
    }
  }
  return sites;
}

// The loader's notification (LDR_DLL_NOTIFICATION_DATA: Flags, then FullDllName, BaseDllName, DllBase, SizeOfImage),
// under the loader lock: only the name is checked and the library queued.
struct UnicodeString {
  USHORT length, maximum;
  PWSTR buffer;
};
struct DllNotificationData {
  ULONG flags;
  const UnicodeString* fullName;
  const UnicodeString* baseName;
  PVOID base;
  ULONG size;
};
constexpr ULONG kDllLoaded = 1, kDllUnloaded = 2;

void Queue(const Loaded& l) {
  std::lock_guard<std::mutex> lock(g_queueMutex);
  g_queue.push_back(l);
  g_queued.store(true, std::memory_order_release);
}

void CALLBACK OnDll(ULONG reason, const DllNotificationData* data, PVOID) {
  if ((reason != kDllLoaded && reason != kDllUnloaded) || data == nullptr || data->baseName == nullptr) return;
  const UnicodeString& n = *data->baseName;
  const std::size_t chars = n.length / sizeof(wchar_t);
  if (chars != 20 || n.buffer == nullptr) return;
  Loaded l{static_cast<HMODULE>(data->base), reason == kDllLoaded, {}};
  for (std::size_t i = 0; i < chars; ++i) {
    if (n.buffer[i] > 0x7f) return;
    l.name[i] = static_cast<char>(n.buffer[i]);
  }
  if (IsScriptLibrary(l.name)) Queue(l);
}

/// Frame callback: rewrites each script library the loader reported since the last frame (a reload brings the stock
/// text back and is reported again); unloaded ones drop out. One atomic load per frame when nothing loaded.
void ScanLoaded() {
  if (!g_queued.load(std::memory_order_acquire)) return;
  std::vector<Loaded> queue;
  {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    queue.swap(g_queue);
    g_queued.store(false, std::memory_order_relaxed);
  }
  for (const Loaded& l : queue) {
    if (l.loaded) g_libraries[l.lib] = Scan(l.lib, l.name);
    else g_libraries.erase(l.lib);
  }
}

/// The script libraries already loaded when the runtime starts (once, at Install).
void QueueExisting() {
  HMODULE mods[kMaxModules];
  DWORD bytes = 0;
  if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &bytes)) return;
  const DWORD count = std::min<DWORD>(bytes / sizeof(HMODULE), kMaxModules);
  for (DWORD i = 0; i < count; ++i) {
    char name[MAX_PATH] = {};
    if (!K32GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, sizeof(name)) || !IsScriptLibrary(name)) continue;
    Loaded l{mods[i], true, {}};
    std::memcpy(l.name, name, 20);
    Queue(l);
  }
}

bool WatchLoads() {
  using Callback = void(CALLBACK*)(ULONG, const DllNotificationData*, PVOID);
  using Register = LONG(NTAPI*)(ULONG, Callback, PVOID, PVOID*);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  auto reg = ntdll ? reinterpret_cast<Register>(GetProcAddress(ntdll, "LdrRegisterDllNotification")) : nullptr;
  static PVOID cookie = nullptr;
  return reg != nullptr && reg(0, &OnDll, nullptr, &cookie) >= 0;
}

}  // namespace

void Install() {
  evr::OnUiTextSet(&Filter);
  const bool watching = WatchLoads();  // before the existing ones: a library loading meanwhile is queued twice, harmless
  QueueExisting();
  evr::OnFrame(&ScanLoaded);
  evr::Log(watching ? evr::LogLevel::Info : evr::LogLevel::Warning, "[EVR.WORDING] ready%s",
           watching ? "" : " (no load notifications: only libraries loaded now are reworded)");
}

}  // namespace evr_wording
