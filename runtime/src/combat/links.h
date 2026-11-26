// combat/links.h: how EchoCombat's parts call each other inside evr-runtime.dll.
//
// As nEVR plugins they found each other by DLL name (GetModuleHandleW(L"VeilNet.dll") + GetProcAddress). Built into
// the runtime there is no VeilNet.dll, DroneFix.dll or PowerBoost.dll, so every such lookup came back empty: a drone
// the server killed never went away on the clients, a MEDIGUN beam's on / off never left the holder's machine, and
// MEDIGUN was never allowed. Each part defines its link at file scope (outside its own anonymous namespace); each is
// safe to call while that part is off (it then does nothing, or answers "no").
#pragma once

#include <stdint.h>

void EvrCombat_VeilNetDroneKilled(uint32_t index);      // server: tell every client a drone died
void EvrCombat_VeilNetMedigun(uint32_t id);             // client: this player's MEDIGUN on / off, to the server
int EvrCombat_VeilNetMedigunAllowed();                  // may MEDIGUN work on this server?
void EvrCombat_DroneFixKilled(uint32_t index);          // client: the server says this drone died
void EvrCombat_PowerBoostMedigunRemote(uint32_t id);    // another player's MEDIGUN on / off
uint64_t EvrCombat_PowerBoostLocalWeapon();             // this player's weapon slot
