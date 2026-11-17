// evr/frame.h: run work once per game frame, on the game-loop thread.
#pragma once

namespace evr {

using FrameFn = void (*)();

/// `fn` runs once per main-loop iteration on the game-loop thread, in registration order. Register before
/// InstallFrameHook or from a frame callback; never removed.
void OnFrame(FrameFn fn);

/// Hooks the game's main loop. Called once at runtime start.
bool InstallFrameHook();

/// True on the game-loop thread while frame callbacks run.
bool InFrame();

}  // namespace evr
