// nEVR's plugin log calls, written to evr-runtime.log with the part's tag ("[PowerBoost] ...").
#pragma once

#include <stdarg.h>
#include <stdio.h>

#include "evr/core.h"
#include "nevr/plugin_interface.h"

#define NEVR_LOG_DEBUG 0u
#define NEVR_LOG_INFO 1u
#define NEVR_LOG_WARNING 2u
#define NEVR_LOG_ERROR 3u

struct NvrLogFields {};

namespace evr_combat_log {
inline void Write(const char* tag, unsigned level, const char* fmt, va_list args) {
  char line[1024];
  vsnprintf(line, sizeof(line), fmt, args);
  const evr::LogLevel l = level >= NEVR_LOG_ERROR     ? evr::LogLevel::Error
                          : level == NEVR_LOG_WARNING ? evr::LogLevel::Warning
                                                      : evr::LogLevel::Info;
  evr::Log(l, "[EVR.COMBAT] %s %s", tag, line);
}
}  // namespace evr_combat_log

#define EVR_COMBAT_LOG_FN(name, level)                                     \
  [[maybe_unused]] __attribute__((format(printf, 1, 2))) inline void name(const char* fmt, ...) { \
    va_list a;                                                             \
    va_start(a, fmt);                                                      \
    evr_combat_log::Write(kPluginLogTag, level, fmt, a);                   \
    va_end(a);                                                             \
  }

#define NEVR_DEFINE_PLUGIN_LOG(tag)                                                                \
  [[maybe_unused]] constexpr const char* kPluginLogTag = tag;                                      \
  [[maybe_unused]] inline void PluginLogBind(const NvrGameContext*) {}                             \
  EVR_COMBAT_LOG_FN(PluginLog, NEVR_LOG_INFO)                                                      \
  EVR_COMBAT_LOG_FN(PluginLogDebug, NEVR_LOG_DEBUG)                                                \
  EVR_COMBAT_LOG_FN(PluginLogWarning, NEVR_LOG_WARNING)                                            \
  EVR_COMBAT_LOG_FN(PluginLogError, NEVR_LOG_ERROR)                                                \
  [[maybe_unused]] inline void PluginLogFields(unsigned level, const char* msg, const NvrLogFields&) { \
    evr::Log(level >= NEVR_LOG_ERROR ? evr::LogLevel::Error                                        \
             : level == NEVR_LOG_WARNING ? evr::LogLevel::Warning                                  \
                                         : evr::LogLevel::Info,                                     \
             "[EVR.COMBAT] %s %s", kPluginLogTag, msg);                                            \
  }
