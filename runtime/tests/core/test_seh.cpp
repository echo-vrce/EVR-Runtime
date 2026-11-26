#include <gtest/gtest.h>

#include <cstdarg>
#include <cstdint>

#include "evr/core.h"
#include "evr/seh.h"

namespace evr {
void Log(LogLevel, const char*, ...) {}  // the guard's one log line; the test needs no log file
}  // namespace evr

namespace {

volatile std::uintptr_t g_bad = 0x10;  // never mapped

int ReadBad() { return *reinterpret_cast<volatile int*>(g_bad); }

int Guarded(bool fault) {
  EVR_TRY {
    if (fault) return ReadBad();
    return 7;
  }
  EVR_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { return -static_cast<int>(EVR_EXCEPTION_CODE() == EXCEPTION_ACCESS_VIOLATION); }
  return 0;
}

}  // namespace

TEST(FaultGuard, NoFaultRunsTheBlock) { EXPECT_EQ(Guarded(false), 7); }

TEST(FaultGuard, AFaultRunsTheHandlerWithItsCode) { EXPECT_EQ(Guarded(true), -1); }

TEST(FaultGuard, ManyFaultsInARow) {
  for (int i = 0; i < 1000; ++i) ASSERT_EQ(Guarded(i % 2 == 0), i % 2 == 0 ? -1 : 7);
}

TEST(FaultGuard, NestedGuardsCatchTheirOwn) {
  int inner = 0, outer = 0;
  EVR_TRY {
    EVR_TRY { ReadBad(); }
    EVR_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { inner = 1; }
    ReadBad();  // the inner guard is gone: the outer one catches this
  }
  EVR_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { outer = 1; }
  EXPECT_EQ(inner, 1);
  EXPECT_EQ(outer, 1);
}

TEST(FaultGuard, LocalsWrittenBeforeTheFaultSurvive) {
  volatile int steps = 0;
  EVR_TRY {
    steps = 1;
    ReadBad();
    steps = 2;
  }
  EVR_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { steps = steps + 10; }
  EXPECT_EQ(steps, 11);
}

TEST(FaultGuard, AnEarlyReturnLeavesNoGuardArmed) {
  EXPECT_EQ(Guarded(false), 7);  // returned from inside the block
  int caught = 0;
  EVR_TRY { ReadBad(); }
  EVR_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { caught = 1; }
  EXPECT_EQ(caught, 1);
}
