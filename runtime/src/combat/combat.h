// combat/combat.h: EchoCombat, built into the runtime (EchoCombat's nevr-pack sources, ported to MinGW).
//
// Parts, started in this order by Install():
//   content    serves EchoCombat's pack (<version dir>\content\echocombat\: changed manifests, added packages,
//              script DLLs, content.json with every sha256) in place of the stock files; nothing in the game folder
//              changes. If the pack does not check out, nothing is served and the game runs stock.
//   dronefix   DRONE fixes (follow-update crash, muzzle, CubeSat shown with the gun)
//   powerboost OVERCHARGE, MEDIGUN (PowerBoost.ini beside evr-runtime.dll)
//   veilnet    relays UMBRAL VEIL / NEUTRON / TRANSLOCATOR / OVERCHARGE; private match RULES / PAUSE / map buttons
// The three gear parts stay off unless the content part serves the pack (combat/content_gate.h): their code only
// makes sense over EchoCombat's scripts and tables.
#pragma once

namespace evr_combat {

/// The content part: first thing at runtime start, before the game reads its manifests. Clients and servers.
void InstallContent(bool server);

/// The gear parts (dronefix, powerboost, veilnet): after every runtime hook is in. Three of their hooks are on
/// functions the runtime hooks too (SendComponentEvent x2, SetItemText): they chain in behind the runtime's
/// (core.h HookOriginalSlot), which needs the runtime's to be there first.
void InstallGear(bool server);

}  // namespace evr_combat
