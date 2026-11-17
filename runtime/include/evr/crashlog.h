// evr/crashlog.h: names a crash in evr-runtime.log (the game writes no minidump on this PC).
#pragma once

namespace evr {

/// Installs a first vectored exception handler that logs access violations and other fatal exceptions: code,
/// address as module+offset, the faulting access, the registers and the unwound call chain. Logs at most a few
/// distinct addresses, then stays quiet; never handles the exception (the game's own handlers still run).
void InstallCrashLog();

}  // namespace evr
