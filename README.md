# EVR-Runtime

In-game features and updates for Echo VR (PCVR first, Quest later). The player installs one small loader once.
After that, the runtime and its content arrive as signed updates, offered in the game's own main menu.

Server side, it talks to Atlas (the Nakama-based backend) over the game's own login socket and
Atlas's REST API.

## What it adds to the game

| Feature | Where | What it does |
| --- | --- | --- |
| Updates | `runtime/src/updates` | An **UPDATES** card and panel on the main menu: version, changelog, **Update** / **Skip** / **Restart now**. |
| Friends panel | `runtime/src/friends` | A **FRIENDS** panel on the main menu (replaces SEASON INFO), with avatars and a friend profile pop-up. |
| Social | `runtime/src/social` | Friends list and party state from Atlas, with the player's own token (`POST /evr/runtime/token`). |
| Party tablets | `runtime/src/party` | The tablet's **PARTY** tab: create, invite, accept, kick, leave, as SNS party messages to Atlas. Optional build (see below). |
| Matchmaker queue | `runtime/src/menu/queue_pc.cpp` | The FIND MATCH bar shows queue position, estimated and elapsed time from Atlas's queue. |
| Create / join via code | `runtime/src/menu/create_join*` | **CREATE GAME** and **JOIN VIA CODE** cards, backed by Atlas invite codes. |
| Early-quit info | `runtime/src/earlyquit` | Shows the early-quit lockout countdown and info pages that the stock client never shows. |
| Login declaration | `runtime/src/login` | Adds an `evr_runtime` object (version, build, platform, channel) to the game's login request. |
| CDN | `runtime/src/cdn` | Fetches Atlas's signed CDN index (new cosmetics, lobby posters) and caches the files. |
| Wording | `runtime/src/menu/wording*` | Replaces stock menu texts that still name Oculus. |

Every patch checks the game's bytes first. On a mismatch it is skipped and logged, never applied blind.

## Repository layout

```text
crates/
  evr-loader/     hid.dll: the proxy DLL the player installs (cdylib)
  evr-updater/    update client: feed, offer, skip, download, verify, stage, promote at boot
  evr-manifest/   the signed release manifest the loader trusts (shared by PC and Quest)
  evr-sign/       key generation and manifest signing (CI and local testing)
runtime/          evr-runtime.dll (C++, CMake): the in-game features above
  src/            one folder per feature; *_pc.cpp holds the hooks, *.h / *_rules.h the testable logic
  tests/          unit tests (evr_tests), run without the game
keys/             pinned public keys (release manifests, CDN)
docs/             PLAN.md (design and release pipeline), TESTING.md
tools/dev-run.ps1 dev cycle: install the fresh build, launch, print the runtime log
HANDOFF.md        running work log: findings, addresses, decisions
```

## How it loads (PC)

Next to `echovr.exe` (`bin\win10`):

```text
hid.dll                    the loader (evr-loader)
plugins\
  config.json              runtime settings (below)
  versions\<ver>\          installed releases; evr-runtime.dll lives here
  evr-runtime.log          the runtime's log
  local\                   dev mode only: extra plugin DLLs
CDN\                       cosmetics and poster cache
```

Before the game's entry point runs, the loader:

1. promotes a staged update and counts the boot (rolls back after repeated failed boots),
2. loads `evr-runtime.dll` from the active release,
3. in dev mode only (`"dev": true` in `plugins\config.json`), loads every `*.dll` in `plugins\local\` in name order,
   calling an optional `EvrPluginStart` export.

A worker thread then checks the update feed; the runtime's UPDATES panel drives the update through the loader's
`EvrUpdate*` exports.

## Configuration

`plugins\config.json`:

| Key | Default | Meaning |
| --- | --- | --- |
| `dev` | `false` | Load local plugins from `plugins\local\` (loader). |
| `queue_gate` | `false` | Let any account use Atlas's matchmaker queue (the FIND MATCH bar). |
| `queue_estimate_scale` | `1` | Test aid: multiplies the queue estimate before the bar reads it. |
| `log_events` | `0` | Dev aid: log the first N distinct component events. |

Social features read the Atlas address (`apiservice_host`) and login credentials from the game's own
`_local\config.json`.

## Building

Requirements: Rust (stable), CMake, and MinGW-w64 (the runtime is built with MinGW Makefiles).

```bash
cargo build --release
```

builds the loader as `target/release/hid.dll` and the other crates.

```bash
cmake -S runtime -B runtime/build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build runtime/build -j8
```

builds `runtime/build/evr-runtime.dll`. For the party tablets, configure a separate build directory with
`-DEVR_PARTY=ON` (for example `runtime/build-party`).

## Testing

```bash
cargo test --release
```

```bash
ctest --test-dir runtime/build --output-on-failure
```

For a quick in-game check, `tools/dev-run.ps1` closes Echo, installs the fresh `hid.dll` and `evr-runtime.dll`,
launches the game and prints `plugins\evr-runtime.log`. Manual test plans are in `docs/TESTING.md`.

## Releases

Releases are signed manifests: `evr-sign` builds them and the loader only accepts one signed by a key pinned in
`keys/pinned.txt`. The branch, review and signing rules are in `docs/PLAN.md`.
