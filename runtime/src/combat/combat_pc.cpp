// Starts EchoCombat's parts (combat/combat.h). Each part's nEVR entry points were renamed by the build
// (CMakeLists.txt: NvrPluginInitEx -> evr_combat_<part>_init, DllMain -> evr_combat_<part>_dllmain).
#include "combat/combat.h"

#include <windows.h>

#include "evr/core.h"
#include "nevr/plugin_interface.h"

#define EVR_COMBAT_PART(part)                                             \
  extern "C" int evr_combat_##part##_init(const NvrGameContext*, const char*); \
  BOOL APIENTRY evr_combat_##part##_dllmain(HMODULE, DWORD, LPVOID);
EVR_COMBAT_PART(content)
EVR_COMBAT_PART(dronefix)
EVR_COMBAT_PART(powerboost)
EVR_COMBAT_PART(veilnet)
#undef EVR_COMBAT_PART

namespace evr_combat {
namespace {

HMODULE Self() {
  HMODULE self = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCSTR>(&Self), &self);
  return self;
}

struct Part {
  const char* name;
  int (*init)(const NvrGameContext*, const char*);
  BOOL(APIENTRY* dllmain)(HMODULE, DWORD, LPVOID);
};

const Part kParts[] = {
    {"content", &evr_combat_content_init, &evr_combat_content_dllmain},
    {"dronefix", &evr_combat_dronefix_init, &evr_combat_dronefix_dllmain},
    {"powerboost", &evr_combat_powerboost_init, &evr_combat_powerboost_dllmain},
    {"veilnet", &evr_combat_veilnet_init, &evr_combat_veilnet_dllmain},
};

}  // namespace

void Start(const Part* parts, int count, bool server, const char* what) {
  const HMODULE self = Self();
  NvrGameContext ctx{server ? NEVR_HOST_IS_SERVER : 0u};
  int on = 0;
  for (int i = 0; i < count; ++i) {
    const Part& p = parts[i];
    p.dllmain(self, DLL_PROCESS_ATTACH, nullptr);  // records the module (PowerBoost.ini beside it)
    const int rc = p.init(&ctx, "{}");
    on += rc == 0;
    evr::Log(rc == 0 ? evr::LogLevel::Info : evr::LogLevel::Warning, "[EVR.COMBAT] %s %s", p.name,
             rc == 0 ? "on" : "off (see its lines above)");
  }
  evr::Log(evr::LogLevel::Info, "[EVR.COMBAT] %s: %d of %d on (%s)", what, on, count, server ? "server" : "client");
}

void InstallContent(bool server) { Start(kParts, 1, server, "EchoCombat content"); }

void InstallGear(bool server) { Start(kParts + 1, 3, server, "EchoCombat gear"); }

}  // namespace evr_combat
