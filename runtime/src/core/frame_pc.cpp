// Per-frame callbacks (PC), from our own reading of echovr.exe (sha256 b6d08277...).
//
// CncaGame::RunMainLoop calls the clock (0x1400d00c0, microseconds) at the loop head 0x14050bd47; the loop's back
// edge (0x14050bf62) jumps there, so a clock call returning to 0x14050bd4c is exactly one frame. The clock is called
// from many threads, so the detour only compares its return address before passing through.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "evr/core.h"
#include "evr/frame.h"

namespace evr {
namespace {

constexpr std::uint64_t kClockVA = 0x1400D00C0;
constexpr std::uint8_t kClockPrologue[] = {0x48, 0x83, 0xEC, 0x28, 0x83, 0x3D};
constexpr std::uint64_t kLoopHeadReturnVA = 0x14050BD4C;
// The call at the loop head: E8 + rel32 naming the clock, so a match proves the return address belongs to it.
constexpr std::uint64_t kLoopHeadCallVA = 0x14050BD47;
constexpr std::uint8_t kLoopHeadCall[] = {0xE8, 0x74, 0x43, 0xBC, 0xFF};

using ClockFn = std::uint64_t (*)();
ClockFn g_clock = nullptr;
std::uintptr_t g_loopReturn = 0;

std::mutex g_mutex;
std::vector<FrameFn> g_fns;
std::atomic<bool> g_logged{false};
thread_local bool t_inFrame = false;

void RunFrame() {
  if (!g_logged.exchange(true)) Log(LogLevel::Info, "[EVR.FRAME] first frame on thread %lu", GetCurrentThreadId());
  std::vector<FrameFn> fns;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    fns = g_fns;
  }
  t_inFrame = true;
  for (FrameFn fn : fns) fn();
  t_inFrame = false;
}

std::uint64_t ClockHook() {
  if (reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)) == g_loopReturn) RunFrame();
  return g_clock();
}

}  // namespace

void OnFrame(FrameFn fn) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_fns.push_back(fn);
}

bool InFrame() { return t_inFrame; }

bool InstallFrameHook() {
  void* call = ResolveVA(kLoopHeadCallVA);
  if (call == nullptr || !CheckBytes(call, kLoopHeadCall, sizeof(kLoopHeadCall))) {
    Log(LogLevel::Warning, "[EVR.FRAME] loop head call mismatch: no frame callbacks");
    return false;
  }
  g_loopReturn = reinterpret_cast<std::uintptr_t>(ResolveVA(kLoopHeadReturnVA));
  return HookVA(kClockVA, kClockPrologue, reinterpret_cast<void*>(&ClockHook), reinterpret_cast<void**>(&g_clock),
                "Clock (frame)");
}

}  // namespace evr
