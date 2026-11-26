// The few nEVR plugin-API names EchoCombat's code was written against. Inside EVR-Runtime nothing is a plugin:
// combat_pc.cpp calls each part's init and shutdown itself and fills the context.
#pragma once

#include <stdint.h>

#define NEVR_PLUGIN_API_VERSION 1u
#define NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY 0x1u
#define NEVR_PLUGIN_CAP_HOOKS_ENGINE 0x2u
#define NEVR_PLUGIN_CAP_NETWORK 0x4u
#define NEVR_HOST_IS_SERVER 0x1u

struct NvrGameContext {
  uint32_t flags;
};

struct NvrPluginInfo {
  const char* name;
  const char* description;
  uint32_t version_major;
  uint32_t version_minor;
  uint32_t version_patch;
};
