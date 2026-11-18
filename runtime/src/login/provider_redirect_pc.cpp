// Without -noovr the game loads the Oculus social provider (pnsovr), which needs an Oculus platform session this
// install does not have: pnsovr's login prerequisites (org-scoped id, Oculus username, user proof) never resolve, so
// the login request is never sent and "[LOGIN] Log in request failed: One or more prerequisites are missing" is
// shown. Passing -noovr makes the game load the demo provider (pnsdemo) instead, which logs in from the credentials
// in the login URL (as a DMO- account) and Atlas accepts. We make that happen without the flag: the game builds its
// net-service providers through CNSProvider::LoadByName(slot, name, cfg, out) (echovr.exe 0x140606690), which
// CreateNetServiceProviders calls with "pnsovr" for the platform slot, and we swap that one name for "pnsdemo"
// before the game loads it. Any other name, "pnsdemo" and a null name included, passes through unchanged, so a run
// that already uses the demo provider is untouched. (pnsrad, Ready At Dawn's own provider, is rejected by this Atlas
// as "Unsupported provider", so the redirect targets pnsdemo, the provider this server already accepts.)
#include "login/provider_redirect.h"

#include <cstdint>
#include <cstring>

#include "evr/core.h"

namespace evr_provider {
namespace {

using LoadByNameFn = std::uint64_t (*)(void* slot, const char* name, void* cfg, void* out);

// CNSProvider::LoadByName: mov [rsp+10h],rbx; mov [rsp+18h],rbp; mov [rsp+20h],rsi; push r14; sub rsp,430h.
constexpr std::uint64_t kLoadByNameVA = 0x140606690;
constexpr std::uint8_t kLoadByNamePrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
                                                0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x41,
                                                0x56, 0x48, 0x81, 0xEC, 0x30, 0x04, 0x00, 0x00};
constexpr char kDemo[] = "pnsdemo";

LoadByNameFn g_original = nullptr;

bool IsOculus(const char* name) { return name != nullptr && std::strcmp(name, "pnsovr") == 0; }

std::uint64_t LoadByNameHook(void* slot, const char* name, void* cfg, void* out) {
  if (!IsOculus(name)) return g_original(slot, name, cfg, out);
  const std::uint64_t result = g_original(slot, kDemo, cfg, out);
  evr::Log(evr::LogLevel::Info, "[EVR.LOGIN] provider pnsovr -> pnsdemo (no Oculus prerequisites) result=%llu",
           static_cast<unsigned long long>(result));
  return result;
}

}  // namespace

bool Install() {
  return evr::HookVA(kLoadByNameVA, kLoadByNamePrologue, reinterpret_cast<void*>(&LoadByNameHook),
                     reinterpret_cast<void**>(&g_original), "CNSProvider::LoadByName (pnsovr -> pnsdemo)");
}

}  // namespace evr_provider
