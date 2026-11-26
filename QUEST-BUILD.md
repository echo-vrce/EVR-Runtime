# Quest (Android) build — state and plan

The PCVR build (hid.dll + evr-runtime.dll) is the mature one. This note tracks the Quest (aarch64-linux-android)
build, which runs inside the r15 app `com.readyatdawn.r15`. Approach is based on nevr-runtime's design; no code is
copied or referenced from it.

## Toolchain

- Android SDK/NDK: `J:\AndroidSDK`, NDK `J:\AndroidSDK\ndk\30.0.16248370`. Set `ANDROID_NDK_HOME` to that NDK.
- `rustup target add aarch64-linux-android` (installed) and `cargo install cargo-ndk` (installed).
- Build a crate for the headset: `cargo ndk -t arm64-v8a build --release -p <crate>`.
- Quest game binary to reverse/pin against: `J:\EchoVR-Tools-Launcher\quest_combat_port\libr15.so`.
- Existing Quest framework to lean on for injection/patching: `J:\EchoVR-Tools-Launcher\EchoQuestXR` (runtime/patcher/loader).

## Loose folder on the headset

`/sdcard/Android/media/com.readyatdawn.r15/files/EVR` (the r15 app's external files dir). The updater's `root`.
Layout mirrors PC: `state.json`, `versions/<ver>/` (each holds `evr-runtime.so`), `staging/<ver>/`.

## Done

- **Shared Rust logic is Quest-ready.** `evr-manifest` and `evr-updater` cross-compile clean for aarch64-linux-android
  (`cargo ndk -t arm64-v8a build --release -p evr-manifest -p evr-updater`). `Platform::current()` already maps
  Android -> `Platform::Quest`, and `ureq` networking is cross-platform.
- **Quest loader crate scaffolded:** `crates/evr-loader-quest` -> `libevr_loader.so`. It is the counterpart of the PC
  `hid.dll`: an ELF `.init_array` constructor runs `early()` (promote staged update + boot bookkeeping, then
  `dlopen` the active `evr-runtime.so` and call `EvrRuntimeStart`), a worker thread checks the feed, and it exports
  the same `EvrUpdate*` C ABI the runtime's UPDATES card drives. The Android body is behind
  `cfg(target_os = "android")`; on a PC host it builds as a stub so `cargo build/test --workspace` still passes.
- **Release pipeline carries Quest content.** `.github/workflows/release.yml` lays out `rel/quest/data/rad15/android`
  from a `content-quest` GitHub release (alongside `content-pc` -> `rel/pc/data/rad15/win10`); `evr-sign` emits a
  separate per-platform build and each loader applies only `Platform::current()`'s files.

## Not done (the hard parts, in rough order)

1. **Injection model.** How `libevr_loader.so` gets into the r15 process (APK repack / preload / EchoQuestXR patcher).
   The `.init_array` constructor assumes the OS loads the .so; the mechanism that makes it load is still to wire up.
2. **ARM64 hooking.** MinHook (the PC runtime's hook engine) is x86/x64 only. The Quest runtime needs an aarch64
   inline-hook path (trampolines, instruction relocation, I-cache flush) before any game function can be hooked.
3. **Re-pin game addresses against `libr15.so`.** None of the PC runtime's pinned x64 addresses transfer; every hooked
   function, RTTI/vtable and resource-callback address must be re-found in the Quest binary.
4. **Quest C++ runtime build target.** CMake has no Android target yet. The pure logic (canvas/level/package builders,
   cdn/cosmetics/content headers) and `runtime/src/quest/**` are Android-ready, but the `*_pc.cpp` files are Windows;
   a Quest runtime needs its own platform layer (resource hook, packages/AddPackage slot, HTTP, logging to the EVR dir)
   built with the NDK into `evr-runtime.so`.
5. **build-quest CI job** (release.yml has a `# build-quest` placeholder): `cargo ndk` the loader .so + the NDK CMake
   build of the runtime .so, upload as the `quest` build artifact.
6. **On-device testing** (updates, content mount, poster, friends panel) on a real headset.

## Build commands

```sh
export ANDROID_NDK_HOME=J:/AndroidSDK/ndk/30.0.16248370
cargo ndk -t arm64-v8a build --release -p evr-loader-quest   # -> target/aarch64-linux-android/release/libevr_loader.so
cargo build --workspace                                       # host build still passes (quest loader stubs out)
```
