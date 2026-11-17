// core/crashlog_pc.cpp: logs fatal exceptions to evr-runtime.log (PC). Written for EVR-Runtime.
//
// A vectored handler sees the exception before any frame handler, so it also sees first-chance faults the game
// recovers from; those are logged too, which is why the number of logged addresses is capped. The handler writes
// with its own CreateFile/WriteFile (no CRT, no runtime log lock: the fault may be inside the logger itself) and
// walks the faulting thread's stack with RtlLookupFunctionEntry / RtlVirtualUnwind from the exception context.
#include "evr/crashlog.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include "evr/core.h"

namespace evr {
namespace {

constexpr int kMaxReports = 6;    // distinct fault addresses logged per run
constexpr int kMaxFrames = 24;
std::atomic<int> g_reports{0};
std::uintptr_t g_seen[kMaxReports] = {};
char g_logPath[MAX_PATH] = {};

bool IsFatal(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case 0xC0000409:  // STATUS_STACK_BUFFER_OVERRUN (fast fail)
      return true;
    default:
      return false;
  }
}

/// "module+0xoffset" (or the raw address) into out.
void Where(std::uintptr_t addr, char* out, std::size_t n) {
  HMODULE m = nullptr;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(addr), &m) &&
      m != nullptr) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* base = path;
    for (const char* c = path; *c; ++c)
      if (*c == 92 || *c == 47) base = c + 1;
    const std::uintptr_t off = addr - reinterpret_cast<std::uintptr_t>(m);
    // echovr.exe also as its static VA, which is what the handoff and the disassembly use.
    if (m == GetModuleHandleW(nullptr))
      std::snprintf(out, n, "%s+0x%llx (VA 0x%llx)", base, (unsigned long long)off,
                    (unsigned long long)(kImageBase + off));
    else
      std::snprintf(out, n, "%s+0x%llx", base, (unsigned long long)off);
  } else {
    std::snprintf(out, n, "0x%llx", (unsigned long long)addr);
  }
}

void Write(HANDLE f, const char* format, ...) {
  char line[1024];
  va_list args;
  va_start(args, format);
  int len = std::vsnprintf(line, sizeof(line) - 2, format, args);
  va_end(args);
  if (len < 0) return;
  if (len > static_cast<int>(sizeof(line)) - 3) len = static_cast<int>(sizeof(line)) - 3;
  line[len++] = '\r';
  line[len++] = '\n';
  DWORD wrote = 0;
  WriteFile(f, line, static_cast<DWORD>(len), &wrote, nullptr);
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
  if (info == nullptr || info->ExceptionRecord == nullptr || info->ContextRecord == nullptr)
    return EXCEPTION_CONTINUE_SEARCH;
  const EXCEPTION_RECORD& r = *info->ExceptionRecord;
  if (!IsFatal(r.ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
  const auto at = reinterpret_cast<std::uintptr_t>(r.ExceptionAddress);
  const int slot = g_reports.load();
  if (slot >= kMaxReports) return EXCEPTION_CONTINUE_SEARCH;
  for (int i = 0; i < slot; ++i)
    if (g_seen[i] == at) return EXCEPTION_CONTINUE_SEARCH;
  const int mine = g_reports.fetch_add(1);
  if (mine >= kMaxReports) return EXCEPTION_CONTINUE_SEARCH;
  g_seen[mine] = at;

  HANDLE f = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;
  SYSTEMTIME t;
  GetLocalTime(&t);
  char where[160];
  Where(at, where, sizeof(where));
  Write(f, "%02d:%02d:%02d.%03d E [EVR.CRASH] exception 0x%08lx at %s thread %lu", t.wHour, t.wMinute, t.wSecond,
        t.wMilliseconds, r.ExceptionCode, where, GetCurrentThreadId());
  if (r.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r.NumberParameters >= 2) {
    const ULONG_PTR kind = r.ExceptionInformation[0];
    Write(f, "  %s of 0x%llx", kind == 0 ? "read" : kind == 1 ? "write" : kind == 8 ? "execute" : "access",
          (unsigned long long)r.ExceptionInformation[1]);
  }
  const CONTEXT& c = *info->ContextRecord;
  Write(f, "  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx", c.Rax, c.Rbx, c.Rcx, c.Rdx);
  Write(f, "  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx", c.Rsi, c.Rdi, c.Rbp, c.Rsp);
  Write(f, "  r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx", c.R8, c.R9, c.R10, c.R11);
  Write(f, "  r12=%016llx r13=%016llx r14=%016llx r15=%016llx", c.R12, c.R13, c.R14, c.R15);

  // Unwind from the fault, frame by frame.
  CONTEXT ctx = c;
  for (int i = 0; i < kMaxFrames && ctx.Rip != 0; ++i) {
    char frame[160];
    Where(ctx.Rip, frame, sizeof(frame));
    Write(f, "  #%02d %s", i, frame);
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
    if (fn == nullptr) {  // leaf function: the return address is at [rsp]
      if (!IsReadable(reinterpret_cast<const void*>(ctx.Rsp), 8)) break;
      ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
      ctx.Rsp += 8;
      continue;
    }
    void* handlerData = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
  }
  CloseHandle(f);
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void InstallCrashLog() {
  std::snprintf(g_logPath, sizeof(g_logPath), "%s\\evr-runtime.log", PluginsDir().c_str());
  const bool ok = AddVectoredExceptionHandler(1, &OnException) != nullptr;
  Log(ok ? LogLevel::Info : LogLevel::Warning, "[EVR.CRASH] crash logger %s", ok ? "installed" : "not installed");
}

}  // namespace evr
