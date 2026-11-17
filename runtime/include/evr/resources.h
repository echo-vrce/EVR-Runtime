// evr/resources.h: change resources as the game reads them (PC: AsyncResourceIOCallback; Quest: its own hook).
//
// A resource is keyed by (type, name). `type` is the class symbol (e.g. Symbol64("CTransformCR")) or the class
// seeded with the buffer's platform tag: Symbol64("Win10", class) for buffer 0, Symbol64("Win10GPU", class) for a
// texture payload (buffer 1). `name` is the resource's symbol.
#pragma once

#include <cstddef>
#include <cstdint>

namespace evr {

/// Return true and set *out / *outSize to serve other bytes (they must stay alive); false serves the stock bytes.
using ResourceTransform = bool (*)(const void* original, std::uint64_t size, const void** out, std::uint64_t* outSize,
                                   void* user);

/// Buffer 0 of (type, name) is offered to `fn` each time the game reads it; for a "Win10GPU"-seeded texture type,
/// buffer 1 (the payload). Several transforms of one resource
/// run in registration order, each on the previous one's output.
bool RegisterResourceTransform(std::uint64_t type, std::uint64_t name, ResourceTransform fn, void* user,
                               const char* label);

/// Serve `data` (kept alive by the caller) in place of the buffer of (type, name) the next time it is read.
bool RegisterResourceOverride(std::uint64_t type, std::uint64_t name, const void* data, std::size_t size,
                              const char* label);

/// Hooks the game's read-finished callback. Called once at runtime start.
bool InstallResourceHook();

}  // namespace evr
