#include <gtest/gtest.h>

#include "shim/detours.h"

namespace {

volatile int g_bias = 0;

__attribute__((noinline)) int Target(int x) {
  // Long enough for MinHook's 5-byte jump and not a leaf it would refuse.
  int r = x * 3 + g_bias;
  for (int i = 0; i < (x & 3); ++i) r += g_bias;
  return r;
}

using TargetFn = int (*)(int);
TargetFn Real_Target = &Target;

int Hooked_Target(int x) { return Real_Target(x) + 1000; }

int Call(int x) {
  TargetFn volatile f = &Target;  // through a pointer: the compiler can't inline the hooked function away
  return f(x);
}

}  // namespace

TEST(DetoursShim, AttachThenDetach) {
  ASSERT_EQ(Call(2), 6);
  ASSERT_EQ(DetourTransactionBegin(), NO_ERROR);
  DetourUpdateThread(GetCurrentThread());
  ASSERT_EQ(DetourAttach(&(PVOID&)Real_Target, Hooked_Target), NO_ERROR);
  ASSERT_EQ(DetourTransactionCommit(), NO_ERROR);
  EXPECT_NE(reinterpret_cast<void*>(Real_Target), reinterpret_cast<void*>(&Target));  // now the trampoline
  EXPECT_EQ(Call(2), 1006);
  EXPECT_EQ(Real_Target(2), 6);

  ASSERT_EQ(DetourTransactionBegin(), NO_ERROR);
  ASSERT_EQ(DetourDetach(&(PVOID&)Real_Target, Hooked_Target), NO_ERROR);
  ASSERT_EQ(DetourTransactionCommit(), NO_ERROR);
  EXPECT_EQ(reinterpret_cast<void*>(Real_Target), reinterpret_cast<void*>(&Target));
  EXPECT_EQ(Call(2), 6);
}

TEST(DetoursShim, AFailedCommitUndoesTheWholeTransaction) {
  PVOID bogus = reinterpret_cast<PVOID>(0x10);  // not code: MinHook refuses it
  ASSERT_EQ(DetourTransactionBegin(), NO_ERROR);
  ASSERT_EQ(DetourAttach(&(PVOID&)Real_Target, Hooked_Target), NO_ERROR);
  ASSERT_EQ(DetourAttach(&bogus, Hooked_Target), NO_ERROR);
  EXPECT_NE(DetourTransactionCommit(), NO_ERROR);
  EXPECT_EQ(reinterpret_cast<void*>(Real_Target), reinterpret_cast<void*>(&Target));  // the first one was undone
  EXPECT_EQ(Call(2), 6);
}

// A hook the runtime made first (evr::Hook): EchoCombat's on the same function chains in behind it.
#include <MinHook.h>

namespace {

__attribute__((noinline)) int Target2(int x) {
  int r = x * 5 + g_bias;
  for (int i = 0; i < (x & 3); ++i) r -= g_bias;
  return r;
}
using Target2Fn = int (*)(int);
Target2Fn g_runtimeOriginal = nullptr;  // the runtime's slot
Target2Fn Real_Target2 = &Target2;      // EchoCombat's
int Runtime_Target2(int x) { return g_runtimeOriginal(x) + 100; }
int Combat_Target2(int x) { return Real_Target2(x) + 1000; }
int Call2(int x) {
  Target2Fn volatile f = &Target2;
  return f(x);
}

}  // namespace

namespace evr {
void** HookOriginalSlot(void* target) {
  return target == reinterpret_cast<void*>(&Target2) ? reinterpret_cast<void**>(&g_runtimeOriginal) : nullptr;
}
}  // namespace evr

TEST(DetoursShim, ChainsBehindTheRuntimesHook) {
  MH_Initialize();
  ASSERT_EQ(MH_CreateHook(reinterpret_cast<void*>(&Target2), reinterpret_cast<void*>(&Runtime_Target2),
                          reinterpret_cast<void**>(&g_runtimeOriginal)), MH_OK);
  ASSERT_EQ(MH_EnableHook(reinterpret_cast<void*>(&Target2)), MH_OK);
  ASSERT_EQ(Call2(1), 105);

  ASSERT_EQ(DetourTransactionBegin(), NO_ERROR);
  ASSERT_EQ(DetourAttach(&(PVOID&)Real_Target2, Combat_Target2), NO_ERROR);
  ASSERT_EQ(DetourTransactionCommit(), NO_ERROR);
  EXPECT_EQ(Call2(1), 1105);  // runtime -> EchoCombat -> original

  ASSERT_EQ(DetourTransactionBegin(), NO_ERROR);
  ASSERT_EQ(DetourDetach(&(PVOID&)Real_Target2, Combat_Target2), NO_ERROR);
  ASSERT_EQ(DetourTransactionCommit(), NO_ERROR);
  EXPECT_EQ(Call2(1), 105);
  MH_DisableHook(reinterpret_cast<void*>(&Target2));
  MH_RemoveHook(reinterpret_cast<void*>(&Target2));
}
