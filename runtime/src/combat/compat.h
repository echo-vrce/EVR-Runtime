// Forced first include of EchoCombat's sources (combat/*.cpp, written for MSVC) under MinGW GCC.
#pragma once

#include <windows.h>
#include <excpt.h>

#include "evr/seh.h"

#undef GetExceptionCode
#define GetExceptionCode() EVR_EXCEPTION_CODE()

#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif
