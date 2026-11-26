// wineclock.h: keeps KUSER_SHARED_DATA.InterruptTime moving where Wine leaves it at 0.
//
// The wall, drone, translocator and buff scripts (tools/veil_fx.py, fix_drone_fx.py,
// fix_loc_fx.py, oc_switch.py) and VeilNet's token expiries all time themselves with
// the qword at 0x7FFE0008 (InterruptTime, 100 ns), read straight from the shared user
// page: the scripts have no imports, so there is no API they could call. Wine 5.0.3
// commits that page zero-filled and read-write in each process and never writes
// InterruptTime, so it reads 0 for the life of the process. A wall's wait gate then
// never times out and the server's main thread spins in the wall script until the
// engine's watchdog logs "Deadlock detected!" (tek's report, 9 Nov 2026).
//
// StartWineClock() checks the field once: if it does not move over ~0.2 s (Windows
// moves it every timer tick, 15.6 ms at most) and the page is writable (it is read-only
// on Windows), a thread writes QueryUnbiasedInterruptTime there every millisecond, the
// same 100 ns units, so every reader's constants stay right. On Windows, and on Wine
// builds that keep the field up to date, it does nothing.
#pragma once
#include <windows.h>
#include <intrin.h>

namespace wineclock {

struct KSystemTime {                      // KSYSTEM_TIME at 0x7FFE0008
    volatile ULONG low;
    volatile LONG high1;
    volatile LONG high2;
};
inline KSystemTime *Field() { return (KSystemTime *)0x7FFE0008; }
inline ULONGLONG Read() { return *(volatile ULONGLONG *)0x7FFE0008; }

inline void Write(ULONGLONG t) {
    // Readers that follow the KSYSTEM_TIME protocol re-read until high1 == high2; the
    // scripts read low and high1 as one qword, which one 64-bit store keeps whole.
    Field()->high2 = (LONG)(t >> 32);
    _WriteBarrier();
    *(volatile ULONGLONG *)0x7FFE0008 = t;
}

inline bool Frozen() {
    ULONGLONG a = Read();
    for (int i = 0; i < 4; ++i) {
        Sleep(50);
        if (Read() != a) return false;
    }
    return true;
}

inline bool Writable() {
    MEMORY_BASIC_INFORMATION mi = {};
    if (!VirtualQuery((void *)0x7FFE0008, &mi, sizeof(mi)) || mi.State != MEM_COMMIT) return false;
    DWORD p = mi.Protect & 0xFF;
    return p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE || p == PAGE_WRITECOPY ||
           p == PAGE_EXECUTE_WRITECOPY;
}

//: 0 not checked yet, 1 the clock moves by itself, 2 pumped, 3 frozen but not writable
static volatile LONG g_state = 0;
static volatile LONG g_started = 0;
static volatile bool g_stop = false;
static HANDLE g_check = nullptr, g_pump = nullptr;

inline DWORD WINAPI Pump(LPVOID) {
    while (!g_stop) {
        ULONGLONG t;
        if (QueryUnbiasedInterruptTime(&t)) Write(t);
        Sleep(1);
    }
    return 0;
}

inline DWORD WINAPI Check(LPVOID) {
    if (!Frozen() || g_stop) { g_state = 1; return 0; }
    if (!Writable()) { g_state = 3; return 0; }
    ULONGLONG t;
    if (QueryUnbiasedInterruptTime(&t)) Write(t);   // moving before the pump's first sleep
    g_state = 2;
    if ((g_pump = CreateThread(nullptr, 0, Pump, nullptr, 0, nullptr)) != nullptr)
        SetThreadPriority(g_pump, THREAD_PRIORITY_ABOVE_NORMAL);
    return 0;
}

//: Safe from DllMain: it only starts a thread. Once per process.
inline void StartWineClock() {
    if (InterlockedExchange(&g_started, 1)) return;
    g_check = CreateThread(nullptr, 0, Check, nullptr, 0, nullptr);
}

//: Before the DLL is unloaded (not from DllMain): ends both threads. The field keeps its
//: last value afterwards, frozen again, as Wine left it.
inline void StopWineClock() {
    g_stop = true;
    if (g_check) { WaitForSingleObject(g_check, 2000); CloseHandle(g_check); g_check = nullptr; }
    if (g_pump) { WaitForSingleObject(g_pump, 2000); CloseHandle(g_pump); g_pump = nullptr; }
}

}  // namespace wineclock
