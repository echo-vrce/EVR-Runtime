# EVR-Runtime: plan

In-game updates for Echo VR on PCVR and Quest. The player installs once. After that, every update is offered in the
game's own menu, and the player picks **Update** or **Skip**.

Status: draft, 2026-11-17. PCVR comes first, then Quest. Every crate builds for both targets from the start.

## 1. What the player sees

1. The player launches Echo VR. The loader checks the latest GitHub release on the player's channel (`stable` or `beta`).
2. If that release is newer than the installed one and the player has not skipped it, a panel opens over the main menu.
   It shows the version, the images and the changelog, plus **Update** and **Skip**.
3. **Skip** saves `skipped = <version>` and the panel stays closed until a newer release comes out. The **Updates** button
   on the main menu (a new card) opens the panel again at any time.
4. **Update** downloads the files, checks them, and stages them in the loose-file folder. Then the player gets one of two
   results:
   - **CDN content** (cosmetics, lobby posters) shows up straight away. No restart.
   - **Engine-level content** (new code, package or manifest changes, new menus) shows **Restart now**, and the game
     relaunches itself in a few seconds. Section 4 explains why this can't be avoided.

## 2. Parts

| Part | PCVR | Quest | Installed by |
| --- | --- | --- | --- |
| **Loader** (trust anchor, small, almost never changes) | proxy DLL in the game folder | `.so` added to the APK, `DT_NEEDED` of `libr15.so` | the player, **once** (new APK version code / new PC zip) |
| **Runtime** (menus, party tablets, update panel, CDN client) | `evr-runtime.dll` in the loose folder | `libevr_runtime.so` in the loose folder | the updater |
| **Loose folder** (assets and packages, loaded ahead of stock) | `%LOCALAPPDATA%\evr\` | `/data/data/<pkg>/files/evr/` | the updater |
| **CDN** (cosmetics and posters only) | HTTPS + signed index | same | the server team; the game reads it live |

Only the loader ever needs a new APK or a new PC download. Keep it tiny, keep its interface stable, and the player
doesn't have to reinstall.

## 3. Release pipeline and trust (no single person can ship)

```
feature branch ──PR──▶ dev ──(beta builds, testers)──PR──▶ main ──Actions──▶ tag vX.Y.Z + signed release
```

- **Branch protection** on `dev` and `main`: PRs only, **2 approving reviews**, CODEOWNERS review on `loader/`,
  `crates/evr-manifest/` and `.github/`, required CI, no force push, and the rules apply to admins too.
- **Only CI builds.** The release workflow builds every binary from the tagged commit. Nobody uploads files by hand. The
  manifest lists the sha256 and size of each file and the commit it was built from.
- **Signing key** (ed25519): stored only as a secret in the `release` GitHub **Environment**. That environment
  requires approval from a second maintainer before the job runs, so it takes two people to publish.
- **The client pins public keys** inside the loader and accepts a manifest only if all of these hold:
  - the signature is valid,
  - `version > installed` (no downgrade except an explicit signed rollback),
  - `expires` is still in the future,
  - `channel` matches,
  - every file's hash matches.

  Key rotation needs 2 of 3 pinned keys (K1/K2 online-ish, KR recovery offline).
- **Open source only.** Release code comes from this public repo. The workflow refuses a tag whose commit isn't on
  `main`, and the build is reproducible (pinned toolchain, `--locked`).
- **Assets** go through the same manifest. A PR that adds an asset puts its source in the repo, not just a blob.
- **Rollback.** The loader keeps the last-known-good runtime. Two crashes in a row before the menu loads trigger an
  automatic revert, and the bad version is never offered again.

## 4. "No restart": what's possible

- **Possible live:** CDN cosmetics and posters. The game streams these resources, and our hook serves the bytes when
  the game asks for them. We write our own format and loader.
- **Not possible live:** new code and package/manifest files. The engine opens its package sets once at boot, and
  nothing makes it read them again (engine behaviour; we re-verify it ourselves). Hot-swapping a hooked DLL
  in a running game process is a crash risk.
- **So:** staging is instant, and applying takes one automatic relaunch:
  - **PC:** `CreateProcess` self, then exit.
  - **Quest:** an `AlarmManager` / relaunch intent, then `finishAndRemoveTask`.

  The player never leaves the game flow and never redownloads the APK.

## 5. Menu, own party tablets, CDN

- **Menu.** Written from scratch. No code from nevr-runtime, and no direct copies. We do our own reverse engineering
  of how the game loads the main-menu page. Then we serve a page we build, with the extra cards (Updates, Party),
  gated on a sha256 pin of the stock page so an unknown build is left untouched. The same logic is used on PC and
  Quest, with per-platform hook backends.
- **Party tablets.** Written from scratch, not copied from nevr's tablet. Party state comes from Atlas (Nakama) over our
  own RPCs (`evr_party_*`), and it's drawn on a new tablet page we build. The design is tracked in
  `docs/party-tablet.md` (TODO).
- **CDN.** Cosmetics and lobby posters only. There's a signed `index.json` with entries
  `{id, kind: cosmetic|poster, sha256, url}`. Coordinate with the Atlas agent so the Nakama side serves or links it.
  Anything bigger goes through the update system.

## 6. Versions

- The release tag is `vX.Y.Z`, and the manifest `version` is the same value.
- The **loader ABI** is an integer. A runtime declares the loader ABI it needs, and a mismatch means "reinstall
  needed". That should almost never happen.
- **PC:** the release zip includes the loader. The runtime and assets come from the manifest.
- **Quest:** the APK `versionCode` moves to our own scheme (`<base>*1000 + loaderAbi`). It changes **only** when the
  loader changes. The APK `versionName` gets `-evr<loaderAbi>`.

## 7. Phases

1. **Core (now):** `evr-manifest` (types, ed25519 verify, rules), `evr-updater` (GitHub release query, download,
   verify, stage, skip state, atomic swap, last-known-good), and the release workflow. Unit-tested on both targets.
2. **PC loader:** proxy DLL, loads the staged runtime, crash counter and rollback, relaunch.
3. **PC runtime:** menu card and update panel (image + changelog page), CDN client, test against the local Atlas.
4. **PC party tablet.**
5. **Quest:** build the same crates for `aarch64-linux-android`, add the loader `.so` to the APK, set the
   versionCode, relaunch intent, headset run.
6. **Beta on `dev`**, then `main`, then the first public tag.

## 8. Open items

- Which proxy DLL name to use on PC. nevr uses `BugSplat64.dll`, so we need a different one to avoid a clash, or to
  coexist with it.
- Quest: confirm that `dlopen` from app-private storage works with Echo's targetSdk (expected yes, since targetSdk < 29).
- Hosting for release assets and images: GitHub release assets (rate limits) or a mirror on R2 named by the signed
  manifest.
