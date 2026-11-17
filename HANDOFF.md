# EVR-Runtime handoff

Updated: 2026-11-17 21:45. This is the one handoff for the whole stack: **EVR-Runtime** (this repo), **atlas**
(`J:\EchoVR-Tools-Launcher\atlas`, the Nakama fork) and **EchoRustServer** (`J:\EchoVR-Tools-Launcher\EchoRustServer`,
the Rust game server and launcher). Nothing is committed in any of them; all work is in the working trees.

- Section 0: what is running, what works, the open bugs and the parties work, across all three.
- Then the runtime sections (what EVR-Runtime is, what is proven, early-quit lockout, parties on PC).
- Appendix A: the full atlas + EchoRustServer handoff (local test stack, atlas changes, Rust server, restarts,
  next steps). The same text also lives in `J:\EchoVR-Tools-Launcher\EchoRustServer\HANDOFF.md`.

## 0. Whole stack: current state (2026-11-17 21:45) — read this first

**Running**
- Atlas: `HOWTOBUILD/local/atlas.exe` = `atlas.2130.exe` (PID 16176 since 21:22). Older builds kept as `atlas.2059.exe`,
  `atlas.2037.exe`, `atlas.2011.exe`, `atlas.1944.exe`; their logs as `logs/atlas.<time>.stdout.log`.
- Rust game servers: `dist/windows/echo-rust-server.exe` (20:07 build, `server_mute`), 8 of 8 registered.
- Game: Echo VR PC with EVR-Runtime `plugins\versions\0.8.0\evr-runtime.dll` = a **party-free** build from 21:26
  (made from a copy of the runtime tree in this session's scratchpad with `src/party` left out). Previous DLLs kept as
  `evr-runtime.2120.dll`, `.2115`, `.2055`, `.2034`, `.pre-earlyquit`. **Do not install `runtime/build/evr-runtime.dll`**:
  it contains the party code, which has never run in the game.
- The user is a Global Operator in the local Atlas DB (system group "Global Operators").

**Working (seen in game)**
- `/lockout` (Discord, member autocomplete or username/EVR ID/Discord ID/account ID): LOCKOUT NOTICE with working OK,
  countdown bar on tablet and main menu, and the countdown now also arrives at login after a rejoin.
- Atlas refuses public Arena/Combat finds during a moderator lockout ("You are locked out of matchmaking for …"; the
  client shows KICKED FROM LOBBY GROUP).
- EARLY QUIT INFO loyalty values, server `/mute-player`, "Echo Friends" wording (built in; party tab not re-checked).

**Open bugs (EVR-Runtime, early-quit area)**
1. **FIND MATCH bar after a lockout.** Once a lockout has been shown in a lobby visit (cleared or run out), Find Match
   enables bar actors #32/#31/#30 (20207787fbccf88d / 9ada32d27410ed7b / f4411ba1efc9b76d) and ~35 ms later, right
   after `evt_ar_tablet_enable_started` (bbe776aa91c346e0, sender #16, key b9aee8d7a6daf176), they are disabled. A good
   find gets the bar's own `delegate_onactorenabled` (0d70b6bd637bb91c) first. Not caused by feature bit 1 (the two
   bit-1 nodes in 17d77f27d465760b are already stock in memory). The actor log's caller field (`by <lib>+0x…`) came out
   empty in the 21:26 run: the stack walk finds no frame inside a script library, so a different way to name the
   disabler is needed (e.g. log the return address into echovr.exe and map it, or hook the script's actor-disable
   node). Repro: `/lockout` yourself, clear it, press Find Match in the same lobby.
2. **Main menu EARLY QUIT card during a /lockout.** The user wants it hidden for moderator lockouts. The patch at
   c18bafab7b6faa62 RVA 0x6958 (condition of `earlyquit_quit_notice_page`) is applied in memory (read back from the
   running game at 21:28) but the card still shows, so that node is not what draws this card. Next: find the card's
   own actor/condition (likely another node reading lockoutcountdownactive), or hide it from Atlas's side.

**Status at 21:45, per the user: BOTH early-quit bugs are still open; the next agent should fix them.**
- **EARLY QUIT card still shows on the main menu during a /lockout** (screenshot 21:27). The installed build's gate on
  `earlyquit_quit_notice_page` (c18bafab7b6faa62 RVA 0x6958) is applied ("EARLY QUIT page node found" at 21:33:40, bytes
  read back from memory), so this card is drawn by something else. Find the card's actor and what enables it while
  lockoutcountdownactive is true; hide it unless Atlas reported an early quit (`g_atlasReportedQuit` in lockout_pc.cpp).
- **Lockout/FIND MATCH bar still doesn't show when queueing after a lockout.** The 21:40 workaround (re-raise the
  penalty event when bar actor #32 bounces) fired at 21:35:53.349 but the bar did not come back, so it does not work;
  remove or replace it. New evidence from the caller log: the bar actors are enabled AND disabled by engine code,
  not by a script library: `enable` by echovr.exe+0x4edece and `disable` by echovr.exe+0x4ed17e (the Enable/Disable
  bodies), both called from **echovr.exe+0x1d3742** (VA 0x1401d3742), which is called from +0x51931c < +0x517984 <
  +0x50be59 (the game loop). Next: disassemble around 0x1401d3742 (likely the AR tablet / tablet-enable component's
  per-child enable pass that reapplies a stored visibility state), find the state it reads for #30–#32 and what
  the lockout path left in it, and reset that state when the lockout ends.

**21:47 build (installed, NOT tried in game yet)**
- `plugins\versions\0.8.0\evr-runtime.dll` = the 21:47 build of the real tree (previous kept as `evr-runtime.2126.dll`).
  83 tests pass. Party code is now a CMake option, `-DEVR_PARTY=ON`, off by default, so a normal build is party-free;
  `runtime/build` is configured with it OFF.
- FIND MATCH bar: 0x1401d3742 is not a tablet pass. It is the engine's deferred-command queue drain (lock-free list,
  `call [r8]`). The scripts' enable/disable thunks (0x1404eda40 / 0x1404ecd30, reasons = 1) are pushed by
  0x1403faee0 (queue, context, thunk, actor ident) from ~50 sites, so the stack can't name who queued the disable.
  The 21:40 event re-raise is removed. New workaround: after a lockout was shown, if bar actor #32 goes off within
  250 ms of going on, the next frame switches #32/#31/#30 back on with `evr::SetActorEnabled` (log line
  "find-match bar switched off right after it came on: N of 3 bar actors switched back on"). If that still fails,
  hook 0x1403faee0 and log the return address when the thunk is 0x1404ecd30 and the actor is a bar actor.
- EARLY QUIT card during /lockout: not changed. Next: repro with the actor log on and read the `[EVR.ACTOR] enable`
  lines when the card appears, to find its actor.

**22:05: user test of the 21:47 build, and the next build (installed, NOT tried in game yet)**
- User confirmed (21:55): the FIND MATCH bar now shows after a lockout (log: "3 of 3 bar actors switched back on").
- Still wrong then: the EARLY QUIT card on the main menu (the user wants it NEVER shown, lockout or not), and the
  bar's estimated time reads 00:00.
- EARLY QUIT card: it is library `346ee5436c2e559e` ("View Early Quit and Loyalty Info"), not the main menu library
  (whose only bit-5 test is the page node). All 7 of its feature-bit-5 tests (JE at 0x519f/0x5247/0x52f4/0x53c8/
  0x5494 -> JMP; MOVZX at 0x6211/0x62d1 -> XOR EAX,EAX) now read "off" (`ApplyCard`, lockout_pc.cpp; log line
  "EARLY QUIT card library at ...: 7 of 7 bit-5 tests set to off"). Bit 5 stays on for the notice and the bar.
- 00:00 estimate: the game never called Atlas's `ready_at_dawn/join_queue` (no "matchmaker queue request" in the
  log) because login settings had no queue. echovr.exe reads `matchmaker_queue_mode` (0x140180590; "disabled"
  default, "testing", "enabled_for_requests", "enabled_for_ccu", "enabled") and `match_type_specific_matchmakers`
  {name: {queue_id, match_types[], queue_heartbeat_interval, queue_accepted_timeout}} (0x1401807c0). Queueable
  types (0x1401b9300): social_2.0, social_2.0_npe, social_2.0_private, echo_arena, echo_arena_public_ai,
  echo_combat_public_ai, echo_demo_public (no public combat). Atlas `server/evr/login_settings.go` now sends mode
  "testing" and one matchmaker `public_arena` (queue_id echo_arena, match_types [echo_arena], 5 s / 30 s).
  "testing" because modes 2/3 make matchmaking wait for the queue to accept (0x140180781), which ours never does.
  If the bar still shows 00:00, check the atlas log for "matchmaker queue request"; next try mode "enabled".
- Installed: `evr-runtime.dll` = 22:00 build (previous kept as `evr-runtime.2147.dll`); `atlas.exe` = `atlas.2210.exe`
  (previous `atlas.2130.exe`, its log `logs/atlas.2150.stdout.log`). 8 of 8 servers registered.
- Restart note: Atlas, WSL (Postgres/Redis) and the keep-alive were started from Claude sessions and die with them.
  At 21:49 they were down (the user saw "application didn't respond"); restarted from this session, and the
  launcher had to be restarted for the game servers to reconnect.

**22:16: slow lobby loads fixed; estimate still 00:00**
- User confirmed the EARLY QUIT card is gone.
- Lobby (level 0x3F9915D3001DC28E) went from ~0.9 s to ~9 s from the 21:26 run on. Cause: the actor log's caller
  stack walk (`ScriptCaller` in actors_pc.cpp, RtlCaptureStackBackTrace + module lookups per logged actor change,
  on while `log_events` > 0). Proven by timing: no loader 0.8 s; `log_events` 0 0.9 s; walk removed, `log_events`
  6000: 0.94 s. The walk is removed (the bar no longer needs it). Atlas and the drive were ruled out (old atlas
  8.9 s, warm cache 8.9 s). The 21:20 runtime DLL (`evr-runtime.2120.dll`) deadlocks in a lobby load: don't use it.
- Also removed: the EVR_TMP_DUMP resource dump (resources_pc.cpp). Its folder (1.1 GB, 13,479 files, from 21:29) is
  renamed to `plugins\dump.off`, not deleted.
- Estimate gate found (22:25, offline): `+0x28d0` is the player's account id (`<platform>-<n>`, formatter
  0x14060d640; platform types BOT/STM/PSN/XBX/OVR-ORG/OVR/DMO). The queue runs only for types 4/5; our players are
  `DMO-`. New `runtime/src/menu/queue_pc.cpp` NOPs the three JAs (0x1401805be, 0x140168669, 0x140180720; log
  "[EVR.QUEUE] matchmaker queue platform gate: 3 of 3 sites open"). Built and installed, NOT tried in game. Check:
  atlas log "matchmaker queue request" on an Arena find; the handler needs `access_token` and `queue_id` (400
  without them).
- 22:23: CRASHED in game. Gate open (3 of 3), login fine, lobby loaded in 0.93 s, then the game died right after
  "Find social lobby 0x042D9CF9CFDDCF76" / "Requesting search for a public session" (social_2.0 is a queueable
  type; only echo_arena has a matchmaker in Atlas's settings). No atlas queue request was made. `evr_queue::Install()`
  is now commented out in runtime_main.cpp and the DLL without it is installed. Next: find where a find with no
  matching queue (or a DMO id) faults on the path 0x140168681 -> 0x1401b9300 -> 0x1401788f0, or try adding a
  social_2.0 matchmaker in Atlas, before turning the gate on again.
- 22:30: crash explained (offline): after its lookup the join wrapper writes `[rbp+0x68]` (0x1401b9485) where rbp is
  the current queue object, with no null check; a find for a queued type with no matchmaker leaves it null. Atlas
  (`atlas.exe` = `atlas.2230.exe`, previous `atlas.2210.exe`) now gives all 7 queued types their own matchmaker
  (`QueuedMatchTypes`, `DefaultMatchmakers`, test `TestDefaultMatchmakersCoverEveryQueuedType`). The runtime gate is
  back on and installed. Rule: never open the gate against an Atlas that doesn't queue all 7 types. NOT tried in game.
- 22:28: STILL CRASHED at the same point (first social lobby find), with all 7 queues in the login settings (r14 log
  confirms). So the null queue object is not (only) the cause. Gate OFF again (`evr_queue::Install()` commented out),
  that DLL installed. The game writes no minidump ("Unable to find MiniDumpWriteDump"); next step is a runtime crash
  logger (vectored exception handler logging the faulting address and stack) to name the fault before trying again.
  Atlas 2230's queue settings are harmless with the gate off (a DMO client ignores them).
- That run's lobby load was 8.7 s again (launched `-windowed -noovr`, no -mp); the 22:16 timing with the stack walk
  removed was 0.94 s. Unexplained, check again.
- 22:40: crash logger added (`include/evr/crashlog.h`, `src/core/crashlog_pc.cpp`, installed first in
  EvrRuntimeStart): a vectored handler logs `[EVR.CRASH] exception ... at <module>+0x.. (VA 0x..)`, the faulting
  access, registers and the unwound stack (max 6 distinct addresses; first-chance faults the game recovers from also
  show). The queue gate is now behind `"queue_gate": true` in `plugins\config.json` (default off). To reproduce:
  set it, launch, let it crash on the first social find, read the `[EVR.CRASH]` lines, set it back. Installed.
- 22:45: QUEUE WORKS (social lobby, my -noovr -windowed -mp run). Crash 22:36 (logger): Join read [0+0x30] from the
  token source 0x140176370 (null without the Oculus platform). Then the queue went to Oculus: its HTTP client base
  URL is hardcoded "https://graph.oculus.com" (0x1416e3858, set via 0x1401f60c0 from 0x140173eed) -> 400 "Invalid
  OAuth 2.0 Access Token". queue_pc.cpp now (1) hooks the base-URL setter and swaps exactly that string for
  apiservice_host from `_local\config.json` (no gate without it), (2) hooks the token source to return a zeroed
  stand-in (empty access_token), (3) opens the 3 platform jumps. Atlas (`atlas.exe` = `atlas.2245.exe`) accepts an
  empty token, keyed "addr:<host>" (test `TestMatchmakerQueueAcceptsEmptyToken`). Result: join_queue + polls, 200 OK,
  estimate 55 -> 35, lobby joined in ~1 s. `queue_gate: true` is now set in the user's plugins\config.json.
  Not yet seen: an Arena find's bar (should read ~01:00), and the game never sends leave_queue after joining the social
  lobby, so the estimator gets no samples from social; it keeps polling there.
- C: was full (0 bytes) at 22:44; `go clean -cache` freed 6.3 GB. Build atlas with `GOCACHE=H:/EVR/.gocache`.
- 22:47 (user run): Arena find joined queue `echo_arena`, Atlas answered 60 then 55 (game log "matchmaker queue
  response ... 200 OK"), but the bar still shows 00:00. So the queue works; the bar's number comes from somewhere else.
  Known: the reply's estimate is stored at queue+0x60 (0x1401c4978) and raises
  delegate_onmatchmakerqueueestimatedwaittimechanged (0x1416e3700) only on change; the dispatcher 0x1401a9f50 has no
  mode check. The bar library 17d77f27d465760b reads an `estimatedwaittime` pin (code at 0x46f6) from the game's
  NetMatchmakerQueueExpression, which I could not locate (RTTI .?AVCR15NetMatchmakerQueueExpression@... at
  0x14203e7f0 has no COL hit by my search). Five more platform checks exist (0x14015b857, 0x140162157, 0x14017d4bd,
  0x14017dbfd: also need mode 2; 0x1401bb67d: mode 2/3, a find path). Suspect the expression needs mode 2/3 or
  reads another field. Next: find the expression's operator (e.g. the vtable via the type descriptor's COL with a
  wider search, or the bar library's 0x46f6 call target), or log the expression's output at run time.
  The social queue keeps polling after the lobby join (never leaves); harmless. `queue_gate` is still on.
- Estimate 00:00: the game got the queue settings (r14 log 22:03:00) but never joined. A second gate before reading
  `matchmaker_queue_mode` (0x140168661 and 0x140180712: `[[netGame+0x910]->+0x28d0]` handle type & 0xF must be 4 or
  5) is closed on our setup. Not forced: the join then uses that object, which may be null. Next: find what
  +0x28d0 is and when it is set.
- Test launches: `echovr.exe -noovr -windowed -mp` (the user asked for -mp).

**Parties (moved back into this session at the user's request)**
- Design: 10-player parties; the leader queues and all matchmake and join together; in 4v4 public Arena 8 play and 2
  spectate. Client: our own Social stand-in so the stock PARTY tab works (code in EVR-Runtime `src/party/*`,
  `tests/party/*`, `social AccountId()`, party lines in CMake and `runtime_main.cpp`; builds, never run).
- Handed back at ~21:35 (details in `H:\EVR\EVR-Runtime\HANDOFF.md`, "Parties on PC"). Written and built, NOT deployed:
  - EVR-Runtime: `runtime/build/evr-runtime.dll` is a party build of the current tree (80 tests pass, 11 canvas skips).
  - Atlas: `HOWTOBUILD/local/atlas.party.exe`: SNS and RPC party size 10, `party/names` RPC, a public Arena/Combat
    party bigger than 4 skips the matchmaker and gets its own lobby (`lobbyCreateForParty`), seated by `partyRolePlan`
    (leader's team, other team, then spectators; `PartyOverflow` reservations join as spectators). New tests pass;
    the full `go test ./server/` was rerun by the main session (see below).
  - Never run in game: whether the PARTY tab leaves "disabled", the callback ABIs, the dispatcher thread, a null
    SendToPeer buffer, and 10 members following the leader. First test needs two accounts and the party DLL + atlas.

**Not deployed / not done**
- Nothing is committed anywhere. Commit only when the user asks; never commit `HOWTOBUILD/` or this file to atlas.
- Early-quit ladder settings (`enable_early_quit_penalty`) stay off locally by the user's choice; `/lockout` works
  without them.


## What EVR-Runtime is

It updates Echo VR on PCVR and Quest in-game, with no third-party app and no APK redownload.

- **Updates:** signed releases from GitHub. Skip / Update is offered in the menu, and downloads go to a loose-folder loader.
- **Menu extras:** our own rewrite of what nevr-runtime offered (CREATE GAME / JOIN VIA CODE cards), plus the FRIENDS tab and our own party tablets.
- **CDN:** cosmetics and lobby posters only.
- **Rule:** no code is copied from nevr-runtime. Everything is remade our own way. The user's own FriendsMenu files are the exception: they are theirs, and only their nevr-specific parts were rewritten.

Game folder layout (`bin\win10`):

- `hid.dll`: the loader.
- `plugins\`:
  - `config.json`, `state.json`
  - `versions\<ver>\` (each holds `evr-runtime.dll`)
  - `content\friends_panel\`
  - `evr-runtime.log`
- `CDN\`

## Done (proven in game on PC)

1. **Signed manifest** (`crates/evr-manifest`). It is an ed25519 envelope checked against pinned keys and a signature threshold, plus schema, channel, expiry, loader ABI, safe paths, and sha256 and size of every file. 5 tests pass.
2. **Updater** (`crates/evr-updater`). It handles check, decide, skip and stage. It boots with pending promotion and rolls back after 2 failed boots. A version becomes `last_good` only once it is marked `ok`. 3 tests pass.
3. **Signing tool** (`crates/evr-sign`). It has two commands, `keygen` and `build`. `build` writes the flat assets and `manifest.signed.json`.
4. **PC loader** (`crates/evr-loader`, builds `hid.dll`):
   - It forwards all 43 HID exports to System32.
   - It patches the exe entry point so `early()` runs before the game. `early()` runs the updater, then loads `evr-runtime.dll` and calls `EvrRuntimeStart`.
   - Setting `dev: true` in `config.json` turns off boot-failure counting.
   - The full chain works end to end: offer, download, verify, stage, relaunch and rollback.
5. **C++ runtime** (`runtime/`; TDM-GCC 10.3 with MinGW Makefiles, static, MinHook):
   - Core: log, memory checks, `HookVA` with a prologue guard, WinHTTP, paths.
   - `Symbol64`: the game's CSymbol64 hash, verified against 100k strings.
   - Resource hook on `AsyncResourceIOCallback` (0x140FA16D0): transforms and overrides by (type, name).
6. **FRIENDS panel, stage 1:**
   - The user's FriendsMenu sources are in `runtime/src/friends/`, repathed to our core through `evr_compat.h`.
   - All 7 menu resource edits are served.
   - The pictures set is mounted with AddPackage (result 0).
   - The panel renders with a header, 5 empty rows with avatar slots, and INVITE FRIENDS. The user confirmed this with a screenshot.
   - UPDATE NEWS and SUPPORT & SOCIAL are hidden.
7. **Atlas:** `_local\config.json` points at the local Atlas: `ws://127.0.0.1:7350/ws?format=evr&token=...`, with publisher_lock `echovrce`. The headset is linked with a Discord 4-digit code, and login works.
8. **Dev loop:** `tools\dev-run.ps1` kills the game, copies both DLLs, relaunches the game (`-noovr -windowed`) and prints the log. A local feed is served with `python -m http.server 8765` from `dev/upload`.
9. **FRIENDS panel, stage 2:** the token is issued, all three event senders are hooked, the roster loads (0 friends on
   the test account) and INVITE FRIENDS enters select mode. Rows, the profile pop-up and avatars are not seen yet: they
   need an account with at least one friend.
10. **CREATE GAME / JOIN VIA CODE** (details below): the cards show, CREATE issues an Atlas invite code, SHARE CODE
    shows it, and JOIN puts the player into the match through the menu's own connecting screen.
11. **In-menu updates** (details below; replaces the desktop MessageBox): the UPDATE card shows above FRIENDS only
    when a release is offered, its panel opens by itself, and SKIP / UPDATE / RESTART NOW work. The user ran
    offer -> UPDATE -> download -> RESTART NOW -> 0.3.0 booted, and SKIP persisted (2026-11-17).

## Early-quit lockout (2026-11-17 20:25, from the EchoRustServer session)

Our own remake of nevr-runtime's lockout support (no code copied; addresses re-read from this echovr.exe, timestamp
0x6452dff6). Files: `runtime/src/earlyquit/lockout_rules.h` (pure decisions), `lockout.h`, `lockout_pc.cpp`, tests
`runtime/tests/earlyquit/test_lockout_rules.cpp` (7, pass; whole suite 51 pass). Installed from `runtime_main.cpp`
after `evr_login::Install`.

- Why: the stock client never shows the LOCKOUT countdown. Its penalty setter (0x1401ae2e0) always writes -1 as the
  countdown end (netGame+0x64828), the penalty expression (0x140d96190) hard-codes "countdown active" to 0, and the
  feature-flag callback (0x1401618b0) stores `flags & 0xdd`, dropping bit 1 (lockout timer, FIND MATCH lockout) and
  bit 5 (EARLY QUIT INFO pages). Atlas sends 0x23 (bits 0, 1, 5).
- What it does: after the setter takes an update (its stamp gate passed), writes the update's expiry (+0x20) as the
  countdown end, sets flags bit 45 (the game clears it at expiry), and re-raises the penalty event
  (0xA2E48B35C7CD078F, read from the game's own word at 0x1416dcdb0, through 0x1401a9fe0). The expression's active
  output follows the seconds left. The mask byte at 0x1401618b9 is patched from 0xdd to 0xff (one byte, not a
  callback replacement). Only Atlas's stamp-and-expiry layout is read; nevr's legacy layout is not supported.
- Proven: deployed into `plugins\versions\0.8.0\evr-runtime.dll` (old one kept as `evr-runtime.dll.pre-earlyquit`);
  the boot log shows `installed setter=1 expression=1 flags=1 redispatch=1`, and the login update was read.
- Not yet seen: a countdown in game. Test with `/lockout` from Discord (global operators only; the user was added to
  Global Operators on the local atlas). Log lines: `[EVR.EARLYQUIT] penalty update ... lockout_s=N` and
  `countdown on (N s left)`.
- **Proven in game (20:26):** `/lockout` showed LOCKOUT NOTICE with the running timer; level 0 cleared it.
- **Notice OK fix (built 20:34, NOT deployed; the game must be closed to swap the DLL):** the tablet's ReturnToHome
  (script `scripts\18000621255c6ea5.dll`, node `dfb343f6` at RVA 0x8f60) goes home only when feature bit 5 is off;
  with it on it goes to a notice (`b46d4b72` = lockoutcountdownactive: true -> `181010cc` LOCKOUT NOTICE, false ->
  `6a1e8fcb` EARLY QUIT NOTICE). Read from the DLL's next-node switch; the nevr doc had the two notice targets swapped.
  So OK (`right_button_press`, key 0x3375a9c5ccdcc06a, seen in the event log) always came back to the notice. Bit 5
  stays on (it drives scoreboard loyalty and early-quit labels); instead the node's select (`cmove rax,rcx` at
  +0x5a) is swapped for `mov rax,rcx; nop` unless a notice is pending. Pending: set when an update starts a lockout
  or raises the early-quit count; cleared by OK or INFO on the notice (event listener, before the game handles the
  click). Applied every frame against whatever load of the library is current. Tests: 3 more (54 pass).
- **Notice OK proven (20:41):** `/lockout` -> LOCKOUT NOTICE -> OK went home (`notice dismissed (OK)`).
- **Unwanted EARLY QUIT page in the main menu (fix built 20:5x, NOT deployed):** the main menu library
  `c18bafab7b6faa62` shows `earlyquit_quit_notice_page` when feature bit 5 AND `showearlyquitwarning` (flags bit 46)
  are set (read from its node at 0x18000692a: bit 5 via the feature-flag expression, the other input bound to the
  penalty expression's `showearlyquitwarning`). After a failed lobby join (no game servers running, nothing charged
  by Atlas, count still 0) the page appeared, so the client sets bit 46 itself; the setter is not found (the flags
  object tests bits through a virtual call, e.g. echovr.exe 0x1413e87de `mov edx,0x2e; call [rax+0x18]`, so a byte
  scan cannot tell a setter apart). Fix: the frame callback keeps bit 46 only while Atlas has reported a new early
  quit (an update whose early-quit count rose sets it, after the stock setter cleared it); a bit 46 the client set
  on its own is cleared and logged ("set by the client without an early quit from Atlas: cleared"), and the page's
  Continue clearing it ends the report ("acknowledged"). The log line gives the moment the client sets it, if the
  real setter is wanted later. Tests: 55 pass.
- **Loyalty page "0/10 GAMES, QUIT -- / COMPLETE --":** correct for the account (atlas has 0 completed matches). The
  atlas side had a bug: the loyalty tier was never re-derived when matches completed. Fixed in atlas (see
  EchoRustServer HANDOFF 3f).
- Atlas locally has `matchmaking.enable_early_quit_penalty` off, so the lockout is not re-sent at login and real early
  quits are not counted; `/lockout` sends its update directly regardless.

## Started (partly built)

- **UI text writer:** `evr::PostUiText(actor, item, text)` is in use (it calls `SetItemText` at 0x14037D470,
  `runtime/src/core/ui_text_pc.cpp`), and `evr::OnUiTextSet` filters the game's own writes (hooked). Every menu
  canvas shares one pool.
- **Release workflow** (`.github/workflows/release.yml`): the draft is in place. The signing step and the Quest build are TODO.
- **Quest files** (`runtime/src/quest/...`): the user's sources were copied in but not adapted.

## Where we are now (2026-11-17, night)

Login works. The client's `_local\config.json` has the user's own `discordid` and `password`; never print them.

Dev settings: `plugins\config.json` has `"dev": true` and `"log_events": 6000`. The original file is backed up in the
Claude scratchpad. `log_events` > 0 turns on all the dev logs (`[EVR.EVENT] seen`, `[EVR.ACTOR]`, and the full
wizard click lines). Set it back to 0 for normal play and before a release.

### FRIENDS panel, stage 2: working in game

- Events: our own SendComponentEvent hook (`include/evr/events.h`, `src/core/events_pc.cpp`) on the three senders,
  all `(space, const SComponentEvent*, u32 flags)`: `0x14050FB60` (one space), `0x14050FCD0` (all spaces),
  `0x14050FED0` (loaded spaces). SComponentEvent is 0x28 bytes: +0 symbol, +8 `*(space+0x58)`, +0x10 sender word
  `0x0000FFFFFFFF0000|actorIndex`, +0x18 key, +0x20 ~0. Real call sites pass flags = 1.
- Friends from Atlas (`src/social/`): the token from `POST /evr/runtime/token`, the roster from `GET /v2/friend`.
  `CallRpc(id, json)` is a generic `/v2/rpc/<id>?unwrap` call that returns Atlas's refusal message as `error`.
- **Party invite:** the atlas parties + cosmetics session built `POST /v2/rpc/party/invite?unwrap {"user_id"}`
  (uncommitted, in `HOWTOBUILD\local\atlas.new.exe`, which the user swaps in). The runtime already sends exactly this.
  Its refusals are `not_in_game`, `not_in_party` (no SNS party made yet) and `target_offline`; the profile pop-up still
  shows "INVITES SOON" for any of them.

### Actor enable / disable hook (dev aid)

`include/evr/actors.h`, `src/core/actors_pc.cpp`. `Enable(space, ident, u8 reasons)` at `0x1404EDA50`, `Disable` at
`0x1404ECD40` (the thunks at `0x1404EDA40` / `0x1404ECD30` pass reasons = 1, as scripts do). The per-actor disable
mask is a byte at `[space+0x378]+index`; 0 means on. Exposes `SetActorEnabled` and `ActorIndex`. Logs `[EVR.ACTOR]`
while `log_events` > 0.

### CREATE GAME / JOIN VIA CODE: working in game (user confirmed)

`src/menu/`, our own code. nevr-runtime was read for understanding only; nothing was copied.

1. **The cards.** The card actors (`ff5375e5a4dee29d` create, `097b785350a61c5f` join) and their tiles are stock, but
   the stock mnu_master page resource (`CR15UIPage2CR`, name `0xAC360E41E4EDE056`) has no page elements for them.
   - `menu_page.h` (pure) decodes the page format and adds both elements (sprite `tile_create` / `tile_join`, text,
     tip) to **every page that holds PLAY**: pages 24, 26 and 27. Page 24 alone draws nothing.
   - `menu_slots.h` (pure) swaps the poses of PLAY (`a3d3ef315cb999db`) and JOIN in the level's `CTransformCR`, so the
     menu reads PLAY, JOIN VIA CODE, TUTORIALS, CREATE GAME (the user's choice).
2. **The wizard's signal.** The wizard calls no service; its only output is a click-metric JSON line passed to the
   remote-log submit `0x140113960 (line, length, ...)` with `"panel_type":"deep_linking"`. `invite_pc.cpp` hooks it:
   - `create_game_review_settings` / `create_invite` -> Atlas `invite/create`
     `{"experience":"social","region":"default","expires_s":N}`. The code goes into the wizard's code fields (item
     `35e290123dfa9c72` on actors `a9bd0cb4e6f2004c`, `e28a9b96434eeff4`, `fab58d7fa959be3b`), EXPIRES (actor
     `0ec8e16986ef88c8`, item `d38c12d22d3552eb`) shows local time, and the code is copied to the clipboard.
   - `create_game_summary` / `join` -> joins the created match.
   - `join_via_code` / `join` (`join_code`) -> Atlas `invite/resolve {"code"}`, then the same join. **Not tried yet.**
3. **SHARE CODE.** The INVITE LINK button (`98be8277516da28c`) opened a page pointing at a dead link. We write its title
   "SHARE CODE", one line on how to use the code, the code and the expiry (page actor `c3f9f167b2a06043`). CLOSE
   (`evt_close_link_clicked`) brings back the summary, which writes `<notset>` again, so we re-write the code for 90
   frames afterwards.
4. **Joining** (our reading of `BeginMultiplayer` at `0x1400FDA70`; game object static `0x1420A0478`):
   - Stage the lobby GUID at +0x8540 (Windows GUID layout of the match id's uuid part) and team `0xFFFF` at +0x8560.
     Refuse when `(+0x8520 & 0xF) && [+0x8528]`, or the id at +0x8530 is not all zero.
   - Send the home page (`05ce113632359ce3`) the menu's own PLAY: `evt_card_clicked`, key `35ec861316abbb34`. The menu
     shows its connecting page at once, and BeginMultiplayer takes the staged lobby instead of searching.
   - If the stage is still there after 90 frames, call `BeginMultiplayerLobby` (`0x1400FDDD0`) directly.
   - Confirmed: log `the menu's PLAY took the staged lobby`; Atlas logged `LobbyJoinSessionRequest` and
     `Player joining the match`.
5. Atlas refused create with `no_history` until the account had pinged the servers once (pressing PLAY did it).

### Added 2026-11-17 late night (built, NOT tried in game)

- **`evr_runtime` login object** (`src/login/`). The game's login (`0x14017EF10`) fills a CJson and hands it to the
  login-service send `0x14060C370 (service, CJson*)`; that call returns to `0x14017F048`. We hook the send and, only
  for that return address, add `evr_runtime|version/build/commit/platform/channel` (string setter `0x1405FDB30`) and
  `evr_runtime|social` = 0 (integer setter `0x1405FD4F0`). The `a|b` nested path is assumed from the game's own
  `incident_categories[%llu]|id`; check that Atlas's "Login client" line now shows `runtime: evr`. Log line:
  `[EVR.LOGIN] declared evr_runtime ...`. Build/commit come from `git describe` at configure time ("dev" with no
  commits); channel from `plugins\config.json` `"channel"` (default stable). Social stays 0 until the SNS party
  messages are handled.
- **Party invite refusals** in the profile pop-up: `PartyInviteLabel` maps `not_in_game` -> JOIN A GAME FIRST,
  `not_in_party` -> MAKE A PARTY FIRST, `target_offline` -> FRIEND IS OFFLINE, 404 -> INVITES SOON, else TRY AGAIN.
  `InviteToParty` now goes through `CallRpc` and returns its reply.
- **Region:** create calls `invite/regions` and maps the wizard's label to a row's `code` or `location` (case, space
  and punctuation ignored; exact first, then contained), else `default`. The create log shows `region="US WEST" (code)`.
- **REGENERATE:** any deep_linking click whose panel or event name contains `regenerate` revokes the held code
  (`invite/revoke {code}`) and creates a new one with the last wizard settings. The real metric names are a guess:
  confirm with `log_events` on (the `wizard click` line).
- **newunlocks dev probe** (`src/login/unlocks_probe_pc.cpp`, only while `log_events` > 0, lines `[EVR.UNLOCKS]`):
  logs the four client paths that touch newunlocks and the cached server profile. The 2026-11-17 19:13 run showed the
  227 notifications came from `SR15NetNewUnlocks` on lobby join. The cause was EchoRustServer replaying a captured
  blob (`data/new_unlocks.hex`); the atlas session removed that send. It also replays a captured
  `SNSRefreshProfileFromServer` blob, which the client merges into its cached server profile. Remove the probe or keep
  it dev-only before a release.
- **Server-muted red icon: not started.** The `mute/match` contract is known
  (`{match_id, enforced, muted:[{user_id, evr_id, display_name, expires_at, allow_private}]}`), but where the in-match
  player list draws the muted icon has not been found in the exe yet; that reverse-engineering comes first.

### AtlasCDN (started 2026-11-17 evening; built, NOT tried in game)

User's design: Atlas runs a CDN of new cosmetic items and lobby posters, per guild. Uploads are admin-only, and a guild
can allow another guild's assets (e.g. Echo VR Lounge allows EML's tags); the atlas panel shows each asset with a
preview image and description. Atlas signs what it serves; new cosmetics are real new catalog items; PC first, then
Quest. The atlas session (local_2148e307) owns the server side.

- `src/cdn/cdn_index.h` (pure, shared with Quest, 4 tests): the signed index (`{"index","sigs"}`, Ed25519 over the
  exact index bytes), entries `{id, kind poster|cosmetic, platform pc|quest|"", type, name, sha256, size, asset,
  title}`, files up to 256 MiB.
- `src/cdn/cdn_pc.cpp`: every 10 min GET `<apiservice_host>/evr/cdn/index` (Bearer, per guild), falling back to the
  static `/evr/cdn/index.signed.json` that Atlas serves from `EVR_CDN_ROOT` today. It verifies the signature against
  keys compiled in from `keys/cdn_pinned.txt` (DEV now; ATLAS once Atlas has its key), refuses a lower serial than the
  last accepted one per guild (`plugins\cdn\state.json`), checks each file's size and sha256, caches it in
  `plugins\cdn\files`, and serves it through `RegisterResourceTransform`. `plugins\config.json` `"cdn_base"`
  overrides the base URL.
- `evr-sign cdn --key dev/dev.key --label DEV --spec cdn.json --serial N --out <atlas cdn root>` writes the same layout.
- Resource hook fix: transforms now also run on buffer 1 (texture payloads); before, a texture's pixels could not be
  replaced.
- Monocypher 4.0.2 (FetchContent) for Ed25519, BCrypt for SHA-256.
- Lobby posters: one texture array of four, `lobby_poster_array_4k_emi` 0xa240a2bc051b2f23 (2k variant
  0xa240a4bc051b2f23); descriptor type 4a4c32c49300b8a0, payload beac1969cb7b8861; about 130 MiB. (Names read from
  nevr-runtime's symbol table, facts only.) Composing a guild's posters into the array is an open design point.
- Next: one lobby run with `plugins\dump` to capture the stock poster array, build a DEV-signed test set, then new
  catalog items (reverse-engineering the customization item list and loadout), then Quest.
- `plugins\dump` (dev, in resources_pc.cpp marked EVR_TMP_DUMP) now also dumps buffer 1. Delete the folder when done.

### In-menu updates (UPDATE card and panel): working in game (user confirmed)

- **Loader** (`crates/evr-loader`): no dialog any more. `run()` checks the feed once and hands the result to an
  `evr_updater::Session` (`crates/evr-updater/src/session.rs`, 3 tests). The verified offer never leaves the loader;
  staging, signatures, hashes and rollback are unchanged in `Updater`. C exports (listed in `build.rs`'s `.def`):
  `EvrUpdateStatus(buf, cap) -> len` (JSON: state checking|none|offer|downloading|staged|failed, version, title,
  notes, files, bytes, relaunch, skipped, done, total, error), `EvrUpdateStart` (blocking; the runtime calls it on a
  worker), `EvrUpdateSkip`, `EvrUpdateRelaunch` (spawns echovr.exe with the same arguments, then
  `TerminateProcess`, since a normal exit from a worker thread runs every DLL detach under the game), and
  `EvrUpdateBootOk`. The Quest loader must export the same names.
- **Boot ok:** the runtime calls `EvrUpdateBootOk` once the main menu is up. Before this, nothing called `boot_ok`
  outside dev mode, so every non-dev boot counted as failed.
- **Card** (`runtime/src/updates/updates_level.h`, pure, shared with Quest, 3 tests): the stock UPDATE NEWS card
  (`27dfdb284b22a843`), which the friends edits hide, is placed again above the FRIENDS panel at (-3.6, 1.31, 4.4)
  with the panel's rotation. Lessons: the card is **drawn at pointer mesh x transform scale**, and with a transform
  scale below 1 the pointer no longer hits it. So the scale stays 1 and the mesh is 60% of stock (2.1 x 1.05). The
  caption item is `text`. The card actor is enabled only while there is an offer (`evr::SetActorEnabled`).
- **Panel** (`updates_panel.h` pure texts and actions, 7 tests; `updates_pc.cpp` glue): the stock news pop-up
  `777cf8ceec9bfd21` (items `headertext`, `bodytext1` (one wrapped area, 127 chars), `lefttext`, `righttext`). The
  card's click reaches the home page as `fb75d76107e450d8` from relay actor `08324e714b4a21aa`, key
  `6cfc99d2a9bda0df`. We send that same event (new `evr::SendComponentEvent`) about 3 s after the menu first shows, so
  an update the player has not skipped opens the panel by itself. Left (stock LEARN MORE, `evt_left_btn_clicked`) is
  SKIP / LATER: it is rewritten into `evt_right_btn_clicked`, so the link never opens and the panel closes. Right
  (stock CONTINUE, `evt_right_btn_clicked`) is UPDATE -> DOWNLOADING n% -> RESTART NOW, and it is swallowed while it
  has work to do, so the panel stays open.
- **Stock texts:** the pop-up writes its own header, body and button labels on every opening, and "Open the
  hyperlink" as the LEARN MORE tooltip. New `evr::OnUiTextSet` filter: `SetItemText` (0x14037D470) is now hooked, and
  the updates filter swaps those stock strings for ours as they are written (so they never flash) and blanks the
  tooltip while our panel is open. Our own writes call the original directly.
- **Dev loop:** sign a newer version (`evr-sign build ... --version 0.N.0`) into `dev\upload`, then
  `tools\dev-run.ps1 -Version <installed>`. dev-run copies the runtime into `versions\<Version>`, and the loader runs
  `state.json`'s `installed` version, so pass the installed one or the old DLL keeps running. In `-noovr -windowed`
  the desktop mouse drives the menu pointer, so you can click through it.
- **Not done:** the card picture and the panel picture are still the stock art. Quest: the logic headers are ready,
  but the Quest loader exports, the relaunch intent and the hooks backend are not. (The "different picture for a
  moment" report was a mistake, per the user: ignore it.)

### UPDATE art, swapped from the server (designed 2026-11-17; nothing built yet)

The user wants the card's and the panel's pictures served from the server, not baked into the DLL. Found (one menu
boot with `plugins\dump`; texture class `b3ebe9e0bf0eb81f`, descriptor type `4a4c32c49300b8a0` = Make(class,
"Win10"), payload type `beac1969cb7b8861` = Make(class, "Win10GPU"), both checked with the CSymbol64 port):

| Texture | Name | Stock format | Shows |
|---|---|---|---|
| UPDATE card | `86300ac728482baf` | 2048 x 1024, BC7 (DXGI 99), 1 mip; payload 2,097,300 bytes | "UPDATE NEWS" art with four players |
| News pop-up picture (most likely; confirm) | `065104dd7f7388f1` | 1920 x 1180, BC7, 1 mip; payload 2,265,748 bytes | ECHO VR robot, "Check out the Official Mission Logs" |

**Recommended route: the AtlasCDN, with no new client code.** The runtime's CDN client (`src/cdn/`) already serves
any signed index entry `(type, name)` when the game reads that resource. Buffer 0 matches the descriptor type, and
buffer 1 matches the payload type. So an entry pair per texture swaps it:
- payload: `type beac1969cb7b8861`, `name 86300ac728482baf`, a DDS file;
- descriptor: `type 4a4c32c49300b8a0`, same name, 256 bytes. It is only needed if the size or format differs from
  stock. A one-mip B8G8R8A8 texture of any size works with `friends_package.h`'s `Dds()` + `TextureDescriptor()`
  layout. Keep the 2:1 aspect for the card.
- Quest: the same names, with Quest types `e2efe7289d5985b8` / `489bb35d53ca50e9` (248-byte descriptor, raw ASTC 8x8
  payload), as `platform: "quest"` entries.

**What Atlas needs to do (atlas session, local_2148e307):**
1. A way for an admin to upload the UPDATE art (a PNG). It is global, not per guild, so it goes in every guild's
   index (or a global section the client also reads). Kind: a new `"ui"` kind, or `poster`. The client ignores
   `kind`.
2. Convert the PNG at upload to the texture files: PC = DDS (DX10 header, B8G8R8A8 or BC7) + the 256-byte
   descriptor; Quest = ASTC 8x8 + the 248-byte descriptor. Then sign them into the index with the other entries.
   Alternative: `evr-sign cdn` gains a `--png` input and does the conversion offline, and Atlas just hosts the result.
3. Swap DEV for the ATLAS key in `keys/cdn_pinned.txt` once Atlas has its key (already on the CDN list).

**Client follow-ups (this repo):**
- Timing: the menu's textures load about 1.5 s after the CDN worker starts (log: CDN `serving` at +0.2 s, menu
  resources at +1.8 s). A cached file (`plugins\cdn\files`) is ready in time. A first-ever download of a 2 MiB file may
  miss the first menu load and only show on the next boot. If that matters, the CDN worker should load the cached
  index synchronously at Start, before the first refresh (CDN session's files).
- Tie the art to the offer: today the CDN art would show whenever the card shows, which is fine because the card is
  only enabled during an offer. A per-release image (the signed update feed's `changelog.images`) would instead need
  the loader to prefetch images at check time (now only fetched during staging) plus a PNG decoder (none vendored;
  `miniz_oxide` is in Cargo.lock). Not recommended unless per-release pictures are wanted.
- Confirm the pop-up picture is `065104dd7f7388f1`: serve a solid-colour DDS under that name and open the panel.
- `plugins\dump` is still present in the game folder (about 1,500 files per boot); delete it when the CDN poster
  capture is done.

### Next

1. **JOIN VIA CODE with a typed code**: wired but not tried. Look for `[EVR.INVITE] join code_len=5 -> match ...`.
2. Show refusals in the UI with friendly text. Join: `invalid`, `expired`, `closed`, `denied`, `rate` (only logged
   now). Create: `no_history` ("press PLAY once first"), `limit`, `rate` (shown as `ERROR: <token>` now).
3. Check in game that the INVITE LINK button label really reads SHARE CODE (its text item id was a guess).
4. **Region and mode:** `Server_region` ("US WEST") is ignored and `"default"` is sent; map the wizard's labels to
   Atlas's region codes. `game_type` is always sent as `social`. The `Password` field is ignored (Atlas invites have none).
5. REGENERATE on the current-code page: `invite/revoke`, then `invite/create`.
6. `g_menuSpace` is the last space that sent an event from the home page; re-check it after coming back from a match.
7. Party: once `atlas.new.exe` is swapped in, test invites, map the refusal tokens in the profile pop-up, then build
   our own party tablets (stage 3). Notification clearing is with the atlas session.
8. Unit tests for `menu_page.h` and `menu_slots.h` (pure, easy to test with the stock page bytes).
9. Decide whether the `[EVR.ACTOR]` hook stays (dev only) or goes.
10. Long term: device auth (`device/auth/*`) with a stored refresh token instead of the password token route.

## Still to do after that

- UPDATE card and panel art from the server through the AtlasCDN (see "UPDATE art, swapped from the server"; needs
  Atlas's upload + conversion first).
- Networked "update available" notice through Atlas, and an `evr_runtime` login declaration.
- CDN cosmetics and lobby posters.
- Re-verify file hashes at every boot (known security gap).
- Release signing in GitHub Actions.
- Quest port (sentinel and GOT hooks) and the APK version scheme. The menu cards need the same page and transform edits
  there (the Quest page is `mnu_master_lowspec`).

## Key references

- echovr.exe sha256 `b6d08277e5846900c81004b64b298df6acba834b69700a640b758bda94a52043`. All addresses above are for this build. Every hook checks its prologue first.
- Dev signing key: `dev/dev.key` (gitignored). Its public key is in `keys/pinned.txt`.
- Build the runtime with `cmake -S runtime -B runtime/build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release`, then `cmake --build runtime/build -j8`.
- Build the loader with `cargo build --release -p evr-loader`.

### Echo wording and the matchmaking bar (2026-11-17 21:00, built, not deployed)

- `src/menu/wording.h` / `wording_pc.cpp`: an `OnUiTextSet` filter rewording stock texts. First entry: the party tab's
  "You have no Oculus Friends." (library 30b4ca0fb0a1ee02, .rdata RVA 0x3eb18, copied out on each use until the NUL)
  -> "You have no Echo Friends.". Tests in `tests/menu/test_wording.cpp`. Not yet seen in game: if the party tab's
  text skips the hooked setter, patch the .rdata string in place instead.
- The blank FIND MATCH bar was Find Match pressed during a running lockout; Atlas now refuses that find (see the
  EchoRustServer HANDOFF 3h). The bar library 17d77f27d465760b reads feature bit 1 in two bool nodes (RVA 0x251f0 and
  0x25750: `(input || ctx[+0x68]->+0xe2) && bit1`).
- Parties: "Parties are currently disabled." (30b4ca0fb0a1ee02 .rdata 0x3f180, used at 0x180011e50) is the stock PARTY
  tab with no platform Social object. nevr-runtime fills that in with a social facade (`src/runtime/patch/social_facade.cpp`,
  `src/runtime/compat/social_party.h`); ours must be our own code.
- 21:15: the FIND MATCH top bar was still missing with no lockout running. Cause: feature bit 1, which the stock
  client drops and we keep, sends two branch nodes in 17d77f27d465760b (RVA 0x11eb0, 0x12680) down their other pin.
  `ApplyBar` in `lockout_pc.cpp` turns only their CALLs into XOR EAX,EAX (stock behaviour); bit 1 stays for the
  other ~10 libraries that read it. Log: `find-match bar library at ...: 2 of 2 bit-1 nodes set to stock`.
  Installed as 0.8.0's DLL (previous kept as `evr-runtime.2055.dll`); not yet seen in game.
- 21:25: the main menu's EARLY QUIT page shows while a lockout countdown runs (stock: page condition = bit 5 and a
  second pin, and our warning gate logged nothing, so bit 46 was not set). Expected for a lockout, unlike the
  no-lockout case fixed earlier. The actor caller log first named HID.DLL (the loader's frame hook is on the stack):
  now only modules whose path contains "scripts" count. That build is in `runtime/build`, NOT installed (the game was
  running); copy it to `plugins\versions\0.8.0\evr-runtime.dll` with the game closed.
- 21:30: the main menu's EARLY QUIT page is now shown only while Atlas has reported a new early quit this session
  (hidden during a moderator /lockout): `ApplyMenuPage` in `lockout_pc.cpp` swaps one aligned 8-byte word at
  c18bafab7b6faa62 RVA 0x6958 (`kMenuPage*` in `lockout_rules.h`, tests added; 82 pass). Built, NOT installed: the
  build tree also holds the party session's unfinished `src/party` code. Install once that is safe to load.
- 21:35: installed a PARTY-FREE build (copy of the tree in the EchoRustServer session's scratchpad, `src/party/party_pc.cpp`
  and the party test left out of CMake, `evr_party::Install()` commented out; 62 tests pass, no `EVR.PARTY` strings).
  It has the EARLY QUIT page gate and the script-caller actor log. Previous DLL kept as `evr-runtime.2120.dll`.
  Do not install `runtime/build` until the party code has been run once.

## Parties on PC (2026-11-17, built, NOT tested in game)

The goal: the stock tablet PARTY tab works on PC (create, invite, accept, leave, kick, promote, member list). A party holds up to 10 players. A party of 10 queueing public Arena or Combat lands in one match: 8 on the teams, 2 spectators.

### Client: `runtime/src/party/`

- **`party_rules.h`** holds the pure logic, tested by `tests/party/test_party_rules.cpp` (19 tests; the whole `evr_tests` passes, 80 pass and 11 skipped):
  - SHA-1 and UUIDv5. A player's UUID is v5(nil, `OVR-ORG-<account id>`), which matches Atlas's test vector.
  - Builders for the standard (0x28), targeted (0x30) and data-update payloads.
  - The `Party` model. Actions return the requests to send; `Feed(symbol, bytes)` applies Atlas's replies and queues events. `SymbolsAreAtlasTokens` checks the constants against `Symbol64`.
- **`party_pc.cpp`** holds the hooks and our own Social object: 0xBA0 bytes, 75 slots plus 10 logging guard slots.
  - **Social accessor** `0x1406169C0`. Its one argument is the platform module, and its only caller is `CR15NetGame::Initialize` at `0x1401740b1`. When the stock accessor returns null (always on PC), we return our object.
  - **Incoming dispatcher** `0x140F85480`. This is TcpBroadcaster `HandlePeer(this, symbol, peer, data, size)`; on its no-listener path it logs and returns nothing useful.
    - LoginSuccess (`0xa5acc1a90d0cce47`) records the broadcaster and the login peer.
    - SNS party replies are fed to the model and not passed to the game.
    - A LoginSuccess from a different broadcaster or peer drops the party, because Atlas leaves the party when a session closes.
  - **Sending** happens from `OnFrame` through the broadcaster's vtable +0xA8, `SendToPeer(this, peer, symbol, item, size, nullptr, 0)`.
    - `SendToPeer` is at `0x140F8A9C0`. The concrete vtable is `0x141cd1130` (SClientData). Both are checked before use.
    - The arguments, from the disassembly: rdx is the peer, r8 the symbol, r9 the item, then itemSize, buffer and bufferSize on the stack.
  - **CJson functions** (their prologues are checked):
    - load `0x1405F0BD0` (json, text, len) returns 0 on success.
    - clear `0x1405ECE60`.
    - serialize `0x1405F1DC0` (json, block[0x40], 0, ""). The text is at block+0 and its length at +0x30. Call blockReset `0x1400D4E50` when [+0x1C]&6, then blockDestroy `0x1400D2760`.
  - **Object fields:**
    - +0x08: the 15 callback delegates of 0x20 bytes each. A call is `proxy@+0x18(instance@+0, &method@+8, args…)`.
    - +0x1F0: the party CJson. +0x200/+0x204: the local and total member counts.
    - +0x248: pointer to 10 member CJson objects of 16 bytes. +0x250: the member cap (10).
    - +0x260 lobby uuid, +0x270 matchType, +0x278 team, +0x27A lobbyType.
    - +0x27C flags: bit0 party data dirty, bit1 host wants joinable, bit2 creating, bit3 joining, bit4 offline.
    - +0x2A8 room id, +0x2B0 owner index, +0x2B4 join policy (3).
  - **Callback indices:** 0 Created, 1 Joined, 2 JoinFailed(u32), 3 Updated, 4 HostChanged, 5 Left, 6 Kicked, 7 InviteAccepted (a gate checked before any join), 9 MemberJoined(idx), 10 MemberUpdated(idx), 11 MemberLeft(id, name), 14 InviteReceived(idx).
  - **Slots:** the order is the one in the session notes and in `BuildObject()`.
    - Member 0 is always the local player.
    - Friends come from the existing Atlas roster (`evr_social::Friends()`).
    - SendInvite and slot 39 map a friend id to a Nakama user id. Outside a party they first create one, then call Atlas `party/invite` on a worker.
    - Names come from the new Atlas RPC `party/names`; an unknown name shows as "PLAYER".
    - Our own account id comes from the `discordid` in the game's login config (`evr_social::AccountId()`, never logged).
  - **Reset (slot 12) keeps the party, on purpose.** The game calls Reset and then SetLocalUser whenever its social-disabled byte (netGame+0x647E9) returns to 0. Bit 1 of that byte is set at `0x14015a13a` (and 4 similar sites) when a u16 state equals 4, and cleared in matchmaking code (`0x14016204a`, `0x1401b84b8`). So Reset can come around matches. It clears the object's fields only, and the next Update fills them again. Leave is sent only from the Leave slots.
- **Login still declares `social=0`.** Atlas sends party data to any client that has sent an SNS party request (`ReadsPartyData`). Level 1 would also turn on SNS friend-presence and recently-met pushes, and nothing here handles those.

### Atlas (uncommitted; built as `HOWTOBUILD\local\atlas.party.exe`, not deployed)

- **Party size 10.**
  - New constant `SNSPartyMaxSize = 10`, used by `snsPartyCreateRequest`.
  - `PartyMaxDefaultSize` is 10 (it was 4).
- **New RPC `party/names`** (`evr_runtime_rpc_party_names.go`). It takes `{"ids":[…]}` (at most 32) and returns `{"names":{id: display name, else username}}`, found by `users.custom_id`.
- **Oversized parties skip the matchmaker** (`partySkipsMatchmaker`). A public Arena or Combat party bigger than a team gets a new lobby at once: `lobbyCreateForParty` calls `newLobby`, then `joinPartyEntrants`.
- **Seating in `MatchJoinAttempt`** (`partyRolePlan`, `evr_match_party_roles.go`):
  - A public Arena or Combat leader with reservations is seated by the plan: the leader's team, then the other team, then spectators. Each role is checked on its own.
  - Reservations take their planned role. The stands' reservations carry `PartyOverflow`, and consuming one seats the joiner as a spectator (any other reservation's spectator role is still refused).
  - The tests cover a 10-party joining 4+4+2, and a refusal when it does not fit. The full `go test ./server/` result is below.

### Not tested in game

- Whether the PARTY tab leaves "Parties are currently disabled." with the object in place. That message may also read the social-disabled byte or the Ready slot (Ready is 0 outside a party).
- The callback ABIs, in particular MemberLeft(id, name) and the InviteAccepted gate's return.
- Which thread the dispatcher runs on: the model is behind a mutex either way.
- Whether `SendToPeer` with a null buffer is fine.
- The members-follow path for 10 players: the members follow the leader from party data (FollowParty), and their reservations are in the new match.
- Atlas's lock semantics: a locked party queues invite joins for approval. The host's flag bit1 (set at reset) keeps it unlocked.
- 21:30 state: installed DLL = party-free 21:26 build (EARLY QUIT page gate + caller log + bar nodes). Read back from
  the running game: c18bafab+0x6958 holds the "hide" bytes and 17d77f27+0x11eb4 the XOR EAX,EAX, yet the main menu
  EARLY QUIT card still shows during a /lockout, so that node is not the card's condition. The caller field of the
  actor log was empty (no script-library frame on the walked stack). Both bugs are open; details in the EchoRustServer
  HANDOFF section 0.
- 21:40: installed party-free build with (a) the actor log's caller field listing up to 6 frames from any module
  (`by echovr.exe+0x… < …`), and (b) a FIND MATCH workaround: after a lockout was shown, if bar actor
  20207787fbccf88d goes off within 250 ms of going on, the penalty event is raised again on the next frame
  (`[EVR.EARLYQUIT] find-match bar switched off right after it came on: penalty event raised`). Untested.
  Previous DLL kept as `evr-runtime.2126.dll`.
- 21:45 (user): both bugs still open, left for the next agent: the main-menu EARLY QUIT card during a /lockout, and
  the FIND MATCH bar after a lockout. The re-raise workaround fired (21:35:53) and did not bring the bar back. The bar
  actors are toggled by engine code called from echovr.exe+0x1d3742 (VA 0x1401d3742), not by a script. Details and
  next steps: EchoRustServer HANDOFF section 0.

## Appendix A: atlas and EchoRustServer handoff (copied 2026-11-17 21:45)

Status on 2026-11-17. Nothing in this file has been committed. **Do not commit this file to atlas** (atlas's
AGENTS.md forbids handoff documents in that repo). It sits in EchoRustServer because the work started here.

Repos involved:

| Repo | Path | State |
| --- | --- | --- |
| EchoRustServer (Rust game server + launcher) | `J:\EchoVR-Tools-Launcher\EchoRustServer` | uncommitted changes |
| atlas (EchoTools nakama fork, Go) | `J:\EchoVR-Tools-Launcher\atlas` | uncommitted changes |
| EVR-Runtime (new client runtime, another session) | `H:\EVR\EVR-Runtime` | owned by the "Echo VR runtime update system" session |
| EchoRelay (reference only) | `J:\EchoVR-Tools-Launcher\EchoRelay` | read only |

### 0. Current state (2026-11-17 21:30) — read this first

**Running**
- Atlas: `HOWTOBUILD/local/atlas.exe` = `atlas.2130.exe` (PID 16176 since 21:22). Older builds kept as `atlas.2059.exe`,
  `atlas.2037.exe`, `atlas.2011.exe`, `atlas.1944.exe`; their logs as `logs/atlas.<time>.stdout.log`.
- Rust game servers: `dist/windows/echo-rust-server.exe` (20:07 build, `server_mute`), 8 of 8 registered.
- Game: Echo VR PC with EVR-Runtime `plugins\versions\0.8.0\evr-runtime.dll` = a **party-free** build from 21:26
  (made from a copy of the runtime tree in this session's scratchpad with `src/party` left out). Previous DLLs kept as
  `evr-runtime.2120.dll`, `.2115`, `.2055`, `.2034`, `.pre-earlyquit`. **Do not install `runtime/build/evr-runtime.dll`**:
  it contains the party code, which has never run in the game.
- The user is a Global Operator in the local Atlas DB (system group "Global Operators").

**Working (seen in game)**
- `/lockout` (Discord, member autocomplete or username/EVR ID/Discord ID/account ID): LOCKOUT NOTICE with working OK,
  countdown bar on tablet and main menu, and the countdown now also arrives at login after a rejoin.
- Atlas refuses public Arena/Combat finds during a moderator lockout ("You are locked out of matchmaking for …"; the
  client shows KICKED FROM LOBBY GROUP).
- EARLY QUIT INFO loyalty values, server `/mute-player`, "Echo Friends" wording (built in; party tab not re-checked).

**Open bugs (EVR-Runtime, early-quit area)**
1. **FIND MATCH bar after a lockout.** Once a lockout has been shown in a lobby visit (cleared or run out), Find Match
   enables bar actors #32/#31/#30 (20207787fbccf88d / 9ada32d27410ed7b / f4411ba1efc9b76d) and ~35 ms later, right
   after `evt_ar_tablet_enable_started` (bbe776aa91c346e0, sender #16, key b9aee8d7a6daf176), they are disabled. A good
   find gets the bar's own `delegate_onactorenabled` (0d70b6bd637bb91c) first. Not caused by feature bit 1 (the two
   bit-1 nodes in 17d77f27d465760b are already stock in memory). The actor log's caller field (`by <lib>+0x…`) came out
   empty in the 21:26 run: the stack walk finds no frame inside a script library, so a different way to name the
   disabler is needed (e.g. log the return address into echovr.exe and map it, or hook the script's actor-disable
   node). Repro: `/lockout` yourself, clear it, press Find Match in the same lobby.
2. **Main menu EARLY QUIT card during a /lockout.** The user wants it hidden for moderator lockouts. The patch at
   c18bafab7b6faa62 RVA 0x6958 (condition of `earlyquit_quit_notice_page`) is applied in memory (read back from the
   running game at 21:28) but the card still shows, so that node is not what draws this card. Next: find the card's
   own actor/condition (likely another node reading lockoutcountdownactive), or hide it from Atlas's side.

**Status at 21:45, per the user: BOTH early-quit bugs are still open; the next agent should fix them.**
- **EARLY QUIT card still shows on the main menu during a /lockout** (screenshot 21:27). The installed build's gate on
  `earlyquit_quit_notice_page` (c18bafab7b6faa62 RVA 0x6958) is applied ("EARLY QUIT page node found" at 21:33:40, bytes
  read back from memory), so this card is drawn by something else. Find the card's actor and what enables it while
  lockoutcountdownactive is true; hide it unless Atlas reported an early quit (`g_atlasReportedQuit` in lockout_pc.cpp).
- **Lockout/FIND MATCH bar still doesn't show when queueing after a lockout.** The 21:40 workaround (re-raise the
  penalty event when bar actor #32 bounces) fired at 21:35:53.349 but the bar did not come back, so it does not work;
  remove or replace it. New evidence from the caller log: the bar actors are enabled AND disabled by engine code,
  not by a script library: `enable` by echovr.exe+0x4edece and `disable` by echovr.exe+0x4ed17e (the Enable/Disable
  bodies), both called from **echovr.exe+0x1d3742** (VA 0x1401d3742), which is called from +0x51931c < +0x517984 <
  +0x50be59 (the game loop). Next: disassemble around 0x1401d3742 (likely the AR tablet / tablet-enable component's
  per-child enable pass that reapplies a stored visibility state), find the state it reads for #30–#32 and what
  the lockout path left in it, and reset that state when the lockout ends.

**Parties (moved back into this session at the user's request)**
- Design: 10-player parties; the leader queues and all matchmake and join together; in 4v4 public Arena 8 play and 2
  spectate. Client: our own Social stand-in so the stock PARTY tab works (code in EVR-Runtime `src/party/*`,
  `tests/party/*`, `social AccountId()`, party lines in CMake and `runtime_main.cpp`; builds, never run).
- Handed back at ~21:35 (details in `H:\EVR\EVR-Runtime\HANDOFF.md`, "Parties on PC"). Written and built, NOT deployed:
  - EVR-Runtime: `runtime/build/evr-runtime.dll` is a party build of the current tree (80 tests pass, 11 canvas skips).
  - Atlas: `HOWTOBUILD/local/atlas.party.exe`: SNS and RPC party size 10, `party/names` RPC, a public Arena/Combat
    party bigger than 4 skips the matchmaker and gets its own lobby (`lobbyCreateForParty`), seated by `partyRolePlan`
    (leader's team, other team, then spectators; `PartyOverflow` reservations join as spectators). New tests pass;
    the full `go test ./server/` was rerun by the main session (see below).
  - Never run in game: whether the PARTY tab leaves "disabled", the callback ABIs, the dispatcher thread, a null
    SendToPeer buffer, and 10 members following the leader. First test needs two accounts and the party DLL + atlas.

**Not deployed / not done**
- Nothing is committed anywhere. Commit only when the user asks; never commit `HOWTOBUILD/` or this file to atlas.
- Early-quit ladder settings (`enable_early_quit_penalty`) stay off locally by the user's choice; `/lockout` works
  without them.

### 1. Done

#### EchoRustServer

- **Payload HUD and private combat** (`src/engine/combat.rs`, `msg.rs`, `session.rs`; track data in
  `src/engine/data/combat_{fission,gauss}_track_vars.txt` from `tools/gen_track_vars.py`).
  - Private combat waits for both teams to ready up.
  - Payload maps play two halves: the teams swap, and the first attack's distance is the target for the second.
  - Untested in game. agent.md has a section on it.
- **Arena stats to nakama.** At match end `session.rs` sends a `UserServerProfileUpdateRequest` for every
  blue/orange player (`send_arena_profile_updates`, `arena_profile_delta`).
- **Loadout feature flag.** `src/upstream.rs` appends `features=save_loadout` to the backend URL
  (`with_features`), so atlas knows this server saves exact loadouts.
- 117 tests pass. agent.md now has a section on the arena stats and `save_loadout` ("Arena stats to nakama, `save_loadout` feature").

#### atlas

- **Cosmetics not fully saving.**
  - Atlas now skips the client's lossy `item_equipped` remote log for servers that registered `save_loadout`
    (`evr_runtime_event_remotelogset.go`, constant in `evr_pipeline_gameserver_loadout.go`).
  - `LoadoutEquipItem` no longer resets `decal_body` on every equip (`evr_profile_cache.go`).
  - Items tagged restricted or blocked are still stripped from players who don't own them. That is atlas's own
    rule.
- **Stats not working.**
  - Atlas accepts arena stats from non-native servers through `UserServerProfileUpdateRequest`
    (`evr_pipeline_login.go`, `runtime_event_server_profile_update.go`).
  - Private arena and combat matches return quietly instead of "unknown mode".
  - Skill ratings for arena are not updated through this path.
- **EVR-Runtime alongside nevr-runtime and the stock game** (`server/evr/login_request.go`,
  `evr_session_parameters.go`).
  - New `evr_runtime` login object: version, build, commit, platform, channel, social, plugins.
  - The `nevr_*` fields are still read; if both are present, `evr_runtime` wins.
  - The stock game (neither) works as before.
  - `RuntimeKind`, `RuntimeVersion`, `RuntimeBuild` and `RuntimePlugins` serve all three. The "Login client" log
    line gains `runtime` and `runtime_platform`.
  - The game-server protocol (`/ws/nevr`, nevr-api protobuf) and the "nevr" database names are unchanged on
    purpose.
- **Tests.**
  - New: `server/evr_echors_profile_test.go`, `server/evr/login_request_runtime_test.go`.
  - `go build` and `go vet` are clean.
  - The full suite has three failures that are not from these changes:
    - `TestMatchStart_SuccessfulStartCommitsTheLabel` and
      `TestSyncJournalAndProfileWithRetry_KeepsLocalEditOfSharedRecord` fail on repeat runs even without the
      changes.
    - `TestKC1_PollFollowPartyLeader_RefusesPrivateSocialLobby` waits on a timer and passes on its own.

#### Coordination with the EVR-Runtime session

- It was told the login contract:
  - the `evr_runtime` object;
  - `social: 1` only once it handles the SNS party, friend and recently-met messages (tablet parties need
    every member at level 1 or higher);
  - refresh the auth token with `refresh_token` and `expires_in`;
  - use its own HMD serial, not nevr's `nEVR-Wine`.
- It chose the existing SNS party path, so atlas gets no `evr_party_*` RPCs for now.
- It has a stock PC client logging in to the local atlas, and its friends panel shows in the main menu.

#### Local atlas test stack (built and running)

All files are under `J:\EchoVR-Tools-Launcher\atlas\HOWTOBUILD\local\`. `HOWTOBUILD/local/` is in atlas's
`.git/info/exclude`.

| Piece | Where | Notes |
| --- | --- | --- |
| atlas binary | `atlas.exe` | `go build` of the working tree |
| config | `data/nakama.yml` | generated throwaway keys plus the throwaway bot token; Mongo off |
| Postgres 18 + Redis 8 | WSL Ubuntu at `H:\WSL\Ubuntu` | reinstalled; reached from Windows on 127.0.0.1:5432 and :6379 |
| siteproxy | `siteproxy.exe` on :4280 (admin 127.0.0.1:4281) | `data/siteproxy.yaml`; maps `/login /config /matching /transaction /serverdb` onto atlas `/ws?format=evr` |
| host account | `data/host.json` | Discord 800101527305846855, guild 1094260352700653769, host role "Server Hoster" 1572295578082222141, generated password |
| game-server config | `data/gameserver-config.json` | launcher-style config.json for the local atlas |
| logs | `logs/atlas.log`, `logs/siteproxy.log`, `logs/rust-server.log` | |

- **Discord.** The throwaway bot is "Atlas" (app 1572294220494938273) in "he_is_the_cat's server", with the
  Server Members intent on. Reset its token when local testing is done, since it was pasted into chat.
- **Group setting.** The guild group's `roles.server_host` is set to the Server Hoster role.
- **Status endpoint.** The stock client's service-status request is `GET /status/services,news` on 7350
  directly. The siteproxy doesn't serve it, so `apiservice_host` points at `http://192.168.1.157:7350/`.
- **Spark secret.** The Spark app's client secret (app 1094259991013228704) was pasted into chat and needs
  resetting in the Discord portal.

#### atlas: EVR-Runtime REST token (added 2026-11-17, late evening)

- New `server/evr_runtime_token_http.go` (+ `_test.go`, 3 tests pass) and one registration line in
  `server/evr_runtime.go` (next to the service-status route).
- `POST /evr/runtime/token {"discord_id","password"}` checks the password exactly like `session_ws.go`'s
  discordid+password login (bcrypt, disabled accounts refused, password cut to 32), is limited to 6 tries a minute per
  Discord ID, and returns the device-auth token pair (`mintDeviceAuthTokenPair`). No server key or HTTP key needed.
- EVR-Runtime uses it to read the friend list (`/v2/friend`) and, later, party RPCs with a Bearer token. Long term this
  should become the device-auth flow with a stored refresh token.
- Running since 2026-11-17 17:59 (swapped in together with the party/cosmetics build; see below).
- Seen while reading: `RegisterBeforeAuthenticateCustom` is **not** restricted in `evr_runtime_api_perms.go` (commented
  out), so anyone with the socket server key can get a session for any Discord ID through
  `/v2/account/authenticate/custom`. Worth checking for production.
- Party: atlas's `party/*` RPCs are the website's parties; there is no in-game invite. The "Atlas: working parties +
  cosmetics fixes" session owns that; EVR-Runtime calls `POST /v2/rpc/party/invite?unwrap {"user_id"}`.

#### atlas: parties for stock clients, and cosmetics slots (2026-11-17, night)

Uncommitted, in the atlas working tree. `go build ./...` and the scoped `go vet` are clean. **Running locally**
since 17:59 (PID 18144 at the time; previous build kept as `HOWTOBUILD/local/atlas.prev.exe`); all 128 game
servers re-registered. Atlas now logs to `logs/atlas.stdout.log` (`logs/atlas.log` is the old run's).

- **Parties.** The gate was not in the SNS handlers (create/join/invite/kick/leave already serve stock clients). It was
  that atlas sent `SNSPartyDataNotify` only to clients with social level >= 1, and then refused the tablet party at
  matchmaking (`tabletMembersFollow`, `evr_lobby_tablet.go`) because a member that gets no party data is never
  moved to the leader's lobby by the game's FollowParty.
  - New rule: a client that **sends any SNS party request** reads party data from then on
    (`SessionParameters.ReadsPartyData`, `markSNSPartyClient` hooked in `ProcessRequest` next to
    `applyTabletPartyGroup`). Declared social level still counts as before, so nevr-runtime and EVR-Runtime
    with `social >= 1` are unchanged.
  - Both gates use it: the data relay (`partyMembers`, `evr_pipeline_party_data.go`) and `tabletMembersFollow`.
    A party of stock clients, or stock + runtime, now matchmakes as a tablet party.
  - Logged once per session at Info: "Client sent an SNS party request without a declared social level".
  - Tests: `server/evr_lobby_tablet_stock_test.go` (7). The DB-backed party handler tests skip without `TEST_DB_URL`.
- **In-game invite over REST.** New RPC `party/invite` (`server/evr_runtime_rpc_party_invite.go`, + 2 tests),
  asked for by the EVR-Runtime friends-panel session: `POST /v2/rpc/party/invite?unwrap {"user_id"}` with the
  player's Bearer token sends the same `SNSPartyInviteNotify` as the tablet's invite (shared `snsPartyInvite` in
  `evr_pipeline_party.go`), from the caller's game session's SNS party. Refuses with FailedPrecondition
  `not_in_game`, `not_in_party` or `target_offline`. The storage-backed website `party/*` RPCs are separate and
  unchanged.
- **Cosmetics.** Three lossy paths found and fixed:
  1. **The skip for Rust/nevr servers never fired for split-connection clients.** The remote-log equip skip looked
     up the match by the session that sent the remote log, which is the client's *login* connection. Clients play
     on a separate lobby connection, so no match was found and the lossy `item_equipped` equip was applied on
     top of the server's exact save. Now it checks the login session's lobby session too
     (`remoteLogEquipSavedByServer`, `evr_runtime_event_remotelogset.go`).
  2. **`decalborder` and `decalback` were dropped from every GameServerSaveLoadout** (Rust legacy JSON and nevr
     protobuf). Their symbols are not in atlas's symbol table, so the slot came back as hex and was skipped. Slots
     are now resolved against the loadout's own slot names (`loadoutSlotName`, `evr_pipeline_gameserver_loadout.go`).
     Unowned items in those slots are still stripped (to empty, their default).
  3. **Stock servers: a decal border equip failed** as "unknown category". `LoadoutEquipItem` now has `decalborder`.
  - Tests: `server/evr_loadout_slots_test.go` (8), including all 22 slots surviving a save, a one-slot save and the
    login-time sanitize on both server paths. The slot test was checked to fail against the old lookup.
  - **Not fixable on stock servers without slot data:** the client's `item_equipped` remote log names the item,
    never the slot. So an emote fills both `emote` and `secondemote`, a decal both `decal` and `decal_body`, a
    pattern both pattern slots, a tint `tint` and `tint_body` (alignment tints also their alignment slot). Two
    different emotes, or different heraldry and body decals, cannot be kept on a stock server. Rust and nevr
    servers save the exact slots.

#### /mute-player: server voice mute (2026-11-17, late night)

Uncommitted. **Not in the running atlas** (that build is from 17:59) and not in the running launcher. The Rust
release binary was rebuilt at 20:07 (`target/release/echo-rust-server.exe`); atlas has not been rebuilt.

- **What it does.** A guild enforcer (or global operator) runs `/mute-player user duration [reason]
  [allow_private_matches]`. The mute is per guild, like other enforcement, and is enforced by the game server,
  which stops relaying that player's voice to everyone. Only servers that register the `server_mute` feature
  (the Rust server, after this change) can do it. A stock server is never sent the message, so a mute does
  nothing there. The bot's reply says which case applied.
  - **Limit:** the duration is required, at most **30 days** (`ServerMuteMaxDuration`). `0` lifts the mute.
  - **allow_private_matches:** the player can still speak in private matches (`LobbyType == PrivateLobby`).
  - The command is logged to the guild's audit channel, as `/kick-player` is.
- **Saved to the database.** Storage collection `ServerMutes`, key `journal`, one object per user, private (no
  client read or write). It keeps the last 50 records: id, guild, issuer, reason, created, expires,
  allow_private, lifted at/by. A new mute in a guild replaces the active one there.
- **How it reaches the server.** A new atlas message `ERGameServerMuteEntrants`, symbol `0x7777777777770C00`,
  null-terminated JSON `{"entrant_ids":["<uuid>"],"muted":true}`. It is sent:
  - live, to the match the player is in when the mute is issued or lifted (this guild's matches only, so guild
    isolation holds);
  - on every join, after the accept (`lobbyEntrantConnected` → `muteEntrantsOnJoin`), because entrant ids are new
    for each join.
- **atlas files:** `server/evr/gameserver_mute_entrants.go` (registered in `core_packet.go`,
  `core_packet_types.go`, `core_hash_lookup.go`); `server/evr_server_mute.go` (journal, apply);
  `server/evr_discord_appbot_mute.go` (command logic); the command definition and handler in
  `evr_discord_appbot.go`; the permission case in `evr_discord_appbot_handlers.go`; the join hook in
  `evr_pipeline_lobby.go`.
  - Tests: `server/evr_server_mute_test.go` (9 tests), passing. They cover the journal, the duration limit, the
    private option, the wire format, the live mute and lift, stock servers and other guilds being skipped, the
    join-time mute, and the RPC below.
- **Red muted icon in game.** A server can't draw it for a stock client. Instead, a new RPC `mute/match`
  (`server/evr_runtime_rpc_server_mute.go`) returns the server-muted players in the caller's match:
  `{"match_id","enforced":bool,"muted":[{"user_id","evr_id","display_name","expires_at","allow_private"}]}`.
  EVR-Runtime would call it and draw the base muted icon in red by those names. **The EVR-Runtime session has
  not been told yet.**
- **EchoRustServer (compiled and tested 2026-11-17 20:07; 120 tests pass):**
  - `src/upstream.rs`: `FEATURES` is now `"save_loadout,server_mute"`. Both bridges (nakama and ServerDB) pass
    `0x7777_7777_7777_0C00` on to the engine as a text frame (`mute_text` / `mute_texts`).
  - `src/engine/mod.rs`: the engine reads that text frame (`on_mute`, `MuteEntrants`).
  - `src/engine/session.rs`: a `muted` entrant set (`set_muted`), and `on_mailbox` drops VOIP from a muted peer.
  - Tests: `nakama_url_gets_the_save_loadout_feature` expects `save_loadout,server_mute`; new
    `server_mute_tests` in `session.rs` (a muted peer's VOIP reaches nobody, they still hear others, a lift relays
    again, a mute sent before the join applies on join) and `server_mute_frames_reach_the_engine_as_json` in
    `upstream.rs`.
- **Known limits.** A mute that expires mid-match stays in force on that server until the match ends (or a
  moderator runs `/mute-player … 0`). The Discord command list has to be re-registered with Discord for
  `/mute-player` to appear; atlas does that at bot start.

### 2. Started, not finished

- **More than 10 game servers can't register (in production).** Locally it doesn't happen: 128 of 128 register
  (section 3). The cause is something only production has; see Next steps, item 1.
- **/mute-player.** Atlas and Rust sides done and tested, neither deployed. EVR-Runtime red icon not started, and
  that session has not been told the `mute/match` contract (no other session was running at 20:07).
- **Party tablets.** Atlas side done (see "parties for stock clients" above). Not yet checked in game, and atlas
  cannot see whether a pure stock client sends SNS party requests at all (stock party code went through the Oculus
  platform, pnsovr). If it sends none, nothing changes for it; see Next steps, item 3.

### 3. Result of the local registration test (17:11–17:15)

- After setting the password again, **all 16 game servers registered**, then **all 40** on a second run. There
  were no warnings or errors in `atlas.log` or `rust-server.log`, and no bridge closed.
- Atlas's code has no per-host or per-server cap (searched), and neither does the cat-tools siteproxy.
- **Conclusion:** the 10-server limit is not in atlas or the Rust bridge. It comes from something production has
  and this setup doesn't. Candidates, most likely first:
  1. **Per-IP limit in production's front proxy.** A connection or rate limit per client IP (nginx `limit_conn`,
     Cloudflare, etc.) on the `serverdb` sockets. Each game server holds one socket open, so the 11th would be
     refused at the upgrade.
  2. **UDP validation over the internet.** Atlas pings the external IP 3 times with a 500 ms timeout. Locally
     that's the LAN.
  3. **Discord REST limiter.** It's saturated only when error DMs are flowing, which needs failures first.
- **Confirmed with the real launcher** (`dist/windows/echo-rust-server.exe` with 128 servers, log
  `dist/windows/logs/server-1794936006.log`): **128 of 128 registered**, 0 bridges closed. The launcher config
  that works is `atlas/HOWTOBUILD/local/data/gameserver-config.json`.
- **Gotcha seen on the way:** a game-*client* config, with `serverdb_host = ws://…:7350/ws?format=evr&token=<key>`
  and no `discordid`/`password`, makes every game server fail with "unauthenticated session, rejecting". Game
  servers need the host's `discordid` and `password` in `serverdb_host`.

### Next steps

1. **Find the production cause of the 10 limit.**
   - Run the launcher against production with 12 or more servers and read the 11th server's line in
     `dist/windows/logs/server-*.log`. An HTTP status or close at connect means production's front proxy limits
     sockets per IP; a "not accepted" after connecting means atlas.
   - Ask whoever runs production for their proxy config (nginx `limit_conn` / `limit_req`, Cloudflare rules) and
     atlas's log around the refusal.
   - Optional: to check that atlas itself behaves under internet-like probing, register from another machine
     against this PC's LAN address (192.168.1.157:4280).
2. **Test the cosmetics and stats fixes end to end on the local stack.** The running atlas (17:59) has the token
   endpoint, the party/cosmetics changes and `party/invite`. Logs: `HOWTOBUILD/local/logs/atlas.stdout.log`.
   - On a **Rust server**: set every slot to a non-default item, including `decal_body`, decal border, both emotes
     and both tints, then relog. All should come back. `atlas.log` should show no `item_equipped` equip for that
     player (the skip now fires through the lobby session).
   - On a **stock server** (no `save_loadout`): same, and expect the couplings listed above (emote/secondemote etc.).
     Decal border should now save.
   - Try equipping an item the account does not own (a VRML tag): it must come back as the default after relog.
   - Finish an arena public match and check the arena leaderboards fill.
   - Note: the Rust server sends arena stats for every match; **atlas** ignores private arena/combat matches on
     purpose, so only public matches fill the boards.
3. **Party tablets, in game with two clients.** Needs two game clients (two accounts) on the local atlas.
   - Stock + stock, stock + EVR-Runtime, EVR-Runtime + EVR-Runtime: create a party from the tablet, invite, accept,
     kick, leave.
   - Leader finds a social lobby: the member should follow. Leader queues arena: both should land in the same
     match, the member without pressing anything.
   - Watch `atlas.log` for "without a declared social level" (the client spoke SNS party) and "Tablet party is the
     lobby party". If a stock client never logs the first line, it sends no SNS party messages and stock
     parties need client-side work instead.
   - EVR-Runtime's friends-panel INVITE: try it once the runtime calls `party/invite` (below). The caller's game
     client must already be in an SNS party, or the RPC answers `not_in_party`.
4. **Finish /mute-player.**
   - ~~EchoRustServer: tests, features test, VOIP-drop test~~ Done 2026-11-17 20:07 (120 pass).
   - Send EVR-Runtime the `mute/match` contract so it can draw the red muted icon.
   - Rebuild atlas (`HOWTOBUILD/local/atlas.new.exe`). The Rust release binary is built; the launcher's
     `dist/windows/echo-rust-server.exe` is not replaced. Restart only when the user asks.
   - In game: mute a player on a Rust server and check that nobody hears them. Check that `0` lifts it, that
     `allow_private_matches` works in a private match, and that the mute comes back after a rejoin.
   - ~~Write agent.md notes~~ Done 2026-11-17.
5. **Commit when the user asks.**
   - EchoRustServer: payload/private combat, arena stats, `save_loadout`.
   - atlas: cosmetics, stats, EVR-Runtime login, EVR-Runtime token endpoint, stock-client parties, loadout slots,
     /mute-player, tests.
   - EchoRustServer also: server mute (`server_mute`).
   - Never commit `HOWTOBUILD/` or this file to atlas.
6. **Clean up when done.**
   - Reset the throwaway bot's token and the Spark app's client secret in the Discord portal.
   - Stop atlas, siteproxy and the WSL keep-alive.
   - Optionally `wsl --unregister Ubuntu` (it lives on H:).

### 3b. State at 2026-11-17 evening

- Stack is running: atlas.exe, siteproxy.exe, WSL (Postgres/Redis), and the user's launcher (echo-rust-server.exe).
- The test client (H:\EVReady-at-dawn-echo-arena, EVR-Runtime loaded) logs in to the local atlas as
  `he_is_the_cat` (DMO-3296391650685316405). Its `_local\config.json` now carries the user's own
  `discordid`/`password` in the login and serverdb URLs, which fixed "account requires password authentication".
  Note: atlas.log records that query string in clear; treat the log as sensitive.
- That login reports `runtime: ""`: EVR-Runtime does not send the `evr_runtime` login object yet.
- EVR-Runtime's friends panel (rows, clicks, INVITE, avatars) moved to its own session; see H:\EVR\EVR-Runtime\HANDOFF.md.
- Re-checked: Rust `cargo test --release` 117 pass; atlas `go build ./...` clean; `server/evr` and the
  profile/loadout tests in `server` pass.
- Next atlas session's scope (user, 2026-11-17): **working parties** and **cosmetic fixes for both the Rust game
  servers and stock (native) game servers**.

### 3f. Early-quit lockout (2026-11-17 20:25)

- Atlas already has the whole server side (penalty ladder, `/lockout`, `SNSEarlyQuitUpdateNotification` with the
  expiry at +0x20, feature flags 0x23). Early quits are charged only in public arena, from clients' post-match logs,
  so Rust servers need nothing new.
- The missing piece was the client: remade in EVR-Runtime (`runtime/src/earlyquit/`); see H:\EVR\EVR-Runtime\HANDOFF.md.
  Deployed into the game's 0.8.0 folder; hooks install.
- The user's account (he_is_the_cat, c41027c9-a251-4f27-aa70-c00663cdea0b) was added to the "Global Operators" system
  group in the local database (group 96b71419-1bf9-4acd-8e25-9615d4522755, member state 2), so `/lockout` works.
- Still off locally: `matchmaking.enable_early_quit_penalty` (service settings, storage collection "Global") and the
  guild's `enforce_early_quit_penalty` group metadata. Without them the lockout is not re-sent at login and leaving
  a match early charges nothing.

- **Loyalty fix in atlas (20:34, built as `atlas.new.exe`, NOT running):** `SteadyPlayerLevel` (the loyalty tier) was
  only re-derived by the moderator reset RPC, so it never rose with completed matches. Now `applyLadderPenalty`
  re-derives it on every early quit, and both completion paths (`evr_match.go` MatchLoop, post-match remote log)
  call `resolveSteadyLevel` and push an `SNSEarlyQuitUpdateNotification` (`sendLoyaltyUpdate`, skipped in silent
  mode) so the tablet updates without a relog. Tests: `server/evr_earlyquit_loyalty_test.go` (2); the 47 early-quit
  tests pass; `go vet` clean.
- **`/lockout` player picker (20:36, in `atlas.new.exe`, NOT running):** the `player` option is now text with
  autocomplete listing guild members ("Nick (@username)", value = Discord ID), like `/kick-player`'s picker, and still
  takes a typed username, EVR ID (`DMO-…`, any case), Discord ID or mention, or account UUID
  (`server/evr_discord_appbot_lockout.go`, tests in `_test.go`). The option's shape changed, so Discord's client
  may need a reload (Ctrl+R) after atlas re-registers commands.
- `/mute-player` was registered (global commands, 42 incl. mute-player); Discord's client cache hid it until reload.

### 3g. Restart at 2026-11-17 20:37

- Atlas restarted on the build with the loyalty fix and the `/lockout` member autocomplete (PID 21356; previous
  binary kept as `atlas.2011.exe`, its log as `logs/atlas.2011.stdout.log`). Startup clean, 42 commands registered.
- The game was restarted with the EVR-Runtime DLL that has the notice fix; all hooks installed and the tablet
  library's notice node was found (`[EVR.EARLYQUIT] tablet library at ...: notice node found`).
- The launcher's game servers had been stopped (log ends 20:30:59 "— stopped —") before the restart; press Start in
  the launcher. Seen in that run: `server mute ... muted=true changed=1` at 20:29 (the mute reached the Rust server).

### 3e. Restart at 2026-11-17 20:11

- Atlas restarted from the new build (PID 18724, `atlas.exe` = the 20:09 `atlas.new.exe`; the other session's 19:44
  binary is kept as `atlas.1944.exe`). Startup clean, `mute/match` registered, 42 slash commands re-registered with
  Discord. Logs to `logs/atlas.stdout.log` (this overwrote the 17:59–18:40 run's log).
- `dist/windows/echo-rust-server.exe` is now the 20:07 build with `server_mute`; the 19:14 one is kept as
  `echo-rust-server.1914.exe`. Started from the launcher at 20:12: 8 of 8 game servers registered, and atlas logged
  `server_mute` for all 8. Ready for the in-game mute test (Next steps item 4).

### 3d. State at 2026-11-17 20:10

- **Atlas is not running**, and neither is the launcher. Only siteproxy (since 17:03) and WSL (Postgres/Redis) are up.
  Another session ran debug atlas builds after 18:40 (`logs/atlas.debug{,2,3}.stdout.log`, the last ending 20:01)
  and replaced `HOWTOBUILD/local/atlas.exe` at 19:44. That binary's contents were not checked.
- `HOWTOBUILD/local/atlas.new.exe` (20:09) is a fresh `go build` of the whole atlas working tree, including
  /mute-player. The mute, loadout-slot, stock-tablet, party-invite and runtime-token tests in `./server` pass.
- `target/release/echo-rust-server.exe` (20:07) has `server_mute`. To use it, copy it over the launcher's
  `dist/windows/echo-rust-server.exe` while the launcher is stopped.
- To test /mute-player: start atlas from `atlas.new.exe`, start the launcher with the new Rust binary, and run
  through Next steps item 4.

### 3c. State at 2026-11-17 night: EVR-Runtime private matches through atlas invites

All details are in H:\EVR\EVR-Runtime\HANDOFF.md. Nothing is committed.

- EVR-Runtime now shows CREATE GAME and JOIN VIA CODE on the main menu. It turns the wizard's clicks into atlas RPCs
  through `/v2/rpc/<id>?unwrap` with the runtime token:
  - `invite/create {experience:"social", region:"default", expires_s}` -> `{code, expires_at, match_id}`;
  - `invite/resolve {code}` -> `{match_id, mode, level}`.
- Proven in game: codes were issued (MAK6M, DQMSF, NPN4B), and JOIN put the player into the match. atlas logged
  `LobbyJoinSessionRequest` and "Player joining the match".
- atlas refuses `invite/create` with `no_history` until the account has latency history. Only matchmaking ping
  replies write it, so a fresh account must press PLAY once first. Consider seeding history at login, or a softer
  check for `experience:"social"`.
- Not tried yet: resolving a typed code (`invite/resolve`), and `invite/revoke` (REGENERATE).
- The wizard's region ("US WEST") and password are not sent. Mapping the region needs the label -> atlas region code
  list (`invite/regions`).
- From the same test run: loadout saving works, but **notification clearing does not**. That was forwarded to the
  atlas parties + cosmetics session.
- `party/invite` errors (`not_in_game`, `not_in_party`, `target_offline`) are not yet shown in EVR-Runtime's profile
  pop-up.

### 4. How we got here (the earlier failure)

1. **The Rust server runs 16 embedded game servers against the local atlas**, through the proxy.
   - Command: `run_rust.py` in the session scratchpad. Its arguments are the launcher's: `--backend echorelay`,
     `--upstream <serverdb_host>`, `--login-url <loginservice_host>`, `--bind 127.0.0.1:7790`,
     `--udp-port 7100`, `--max-instances 16`, `--min-idle 16`, `--lan-ip 192.168.1.157`.
   - No game launch is needed.
2. **Every registration failed with "Invalid credentials".** `/reset-password` was run from the host's Discord
   account at 17:05:04, which clears the account's password and email (`evr_discord_appbot.go:1857`). The
   generated password in `data/host.json` was set at about 17:02.
3. **Next:**
   - Set the password again (bcrypt of `host.json`'s password plus a placeholder email on the user row).
   - Restart the Rust server.
   - Watch how many of the 16 register, then read `atlas.log` around the 11th registration for the limit.
4. **Don't use `/reset-password`** on the host account while testing. Use a separate Discord account for game
   clients if a password login is needed there.

### Background processes started from the session

- **WSL keep-alive:** `wsl -d Ubuntu -u root -- ... sleep infinity`. Without it WSL idles out and Postgres goes
  down.
- **Running:** `atlas.exe --config data/nakama.yml` and `siteproxy.exe -config data/siteproxy.yaml`.
- **Stopped:** the session's own echo-rust-server test run. The user's launcher now runs the game servers.
- **To restart the stack after a reboot:**
  1. Start WSL with Postgres and Redis.
  2. Start `atlas.exe --config data/nakama.yml` in `atlas/HOWTOBUILD/local`.
  3. Start `siteproxy.exe -config data/siteproxy.yaml` in the same folder.
  4. Start the launcher with `gameserver-config.json`.
- **If registrations fail with "Invalid credentials",** someone ran `/reset-password` on the host account. Set
  the password again from `data/host.json`: bcrypt it into `users.password`, and set `users.email` to
  `<user id>@echonakama.invalid`.

#### 3h. 21:00: matchmaking during a lockout, Echo wording, parties (built, NOT deployed)

- Blank FIND MATCH bar (no time, no cancel): the user had run `/lockout` again at 20:43:59 (120 s) and pressed Find
  Match at 20:44:06 while still locked out. The client showed the lockout bar with matchmaking state underneath, and
  Atlas still matched them: its lockout refusal only covered the ladder (`enable_early_quit_penalty`, off locally) and
  one test user. Fix in atlas `server/evr_lobby_find.go`: a running moderator lockout (`ModeratorPenaltyUntil`) refuses
  Arena/Combat public finds with "You are locked out of matchmaking for <time>", whatever the settings
  (`moderatorLockoutLeft` in `evr_earlyquit.go`, test in `evr_earlyquit_loyalty_test.go`). Built as
  `HOWTOBUILD/local/atlas.2100.exe`; swap it in for atlas.exe on the next restart.
- "You have no Oculus Friends." -> "You have no Echo Friends.": EVR-Runtime `src/menu/wording.h` / `wording_pc.cpp`
  (an `OnUiTextSet` filter). Built, 58 tests pass, not deployed (with the bit-46 fix).
- "Parties are currently disabled.": the stock PARTY tab needs the platform Social object that the Oculus plugin
  supplied on PC. Atlas already speaks the SNS party protocol (`evr_pipeline_party.go`, "tablet party"); the client
  side is missing. The user wants 10-player parties: the leader queues, all 10 matchmake and join together, 8 play
  4v4 and 2 spectate. Not started.

#### 3i. Restart at 2026-11-17 20:59

- Atlas is now the 3h build (`atlas.exe` = `atlas.2100.exe`, PID 22160; previous kept as `atlas.2037.exe`, its log as
  `logs/atlas.2037.stdout.log`). Startup clean, 42 commands; the Rust server's 8 game servers reconnected by themselves.
- `plugins\versions\0.8.0\evr-runtime.dll` is the 20:55 build (early-quit warning fix + Echo wording); previous kept
  as `evr-runtime.2034.dll`. The game was not running, so it picks this up when next launched.

#### 3j. 21:20: lockout after a rejoin, the bar after a lockout

- A player who rejoined during a `/lockout` saw no countdown and got KICKED FROM LOBBY GROUP on Find Match: Atlas sent
  the early-quit messages at login only with `enable_early_quit_penalty` on. Now `loginSendsEarlyQuit` (evr_earlyquit.go)
  also sends them while a moderator lockout runs (test in `evr_earlyquit_loyalty_test.go`). Built as
  `HOWTOBUILD/local/atlas.2130.exe`, not running yet.
- The FIND MATCH bar after any lockout (cleared or run out), in the same lobby visit: Find Match enables bar actors
  20207787fbccf88d/9ada32d27410ed7b/f4411ba1efc9b76d (#32/#31/#30), then `evt_ar_tablet_enable_started` fires and they
  are disabled ~35 ms later. A find with no lockout seen keeps them. Cause not found yet; the runtime's actor log now
  names the script library and offset that enables/disables each actor (`... by <lib>.dll+0x...`), installed in
  0.8.0 (previous kept as `evr-runtime.2115.dll`). Next: reproduce (lockout, clear, Find Match) and read the disabler.
- 21:22: Atlas restarted on `atlas.2130.exe` (PID 16176; previous kept as `atlas.2059.exe`, log as
  `logs/atlas.2059.stdout.log`). Clean startup, 42 commands, 8 game servers reconnected.
- 21:40: full `go test ./server/` with the party changes: one failure, `TestMatchStart_SuccessfulStartCommitsTheLabel`
  ("Started() false"), fails on every run and is not party code (MatchStart sets StartTime = now and `Started()` needs
  `time.Now().After(StartTime)`; Windows clock resolution makes them equal). All else passes.
- Also `TestSyncJournalAndProfileWithRetry_KeepsLocalEditOfSharedRecord` (evr_suspension_profile_occ_test.go:203) is
  flaky: it failed once and passed once when run on its own, and failed in the party session's full run. That's
  suspension/profile code, not party code.

### 2026-11-17 22:57 — FIND MATCH estimate: cause found
- The bar's time is NetMatchmakerQueueExpression (0x140d9bfe0) -> getter 0x140182100 (queue+0x60). The getter returns -1
  unless NetGame state [netGame+0x448] == 3 (menu lobby) and the key [netGame+0x6a0] is in the queue map ([+0x910]+0x50).
- Diagnostic hook (menu/queue_pc.cpp, logs on change) showed: state=5 (in game: finds start inside the social lobby),
  key present, 7 queues -> -1 -> 00:00. Atlas answered echo_arena 15/10/5.
- Fix: JNE after `cmp [rdx+0x448],3` at 0x140182121 NOPed (queue_gate). Awaiting in-game test. The diagnostic hook can go
  once confirmed. Backup before the hook: evr-runtime.2300.dll.
- 22:59: getter now returns 60/54 but the bar still read 00:00. The expression's other output, the position (getter
  0x140182170, queue+0x5c), was -1: same state check, and Atlas sent no queue_position (absent is stored as -1).
- 23:00: position getter's state JNE (0x140182191) NOPed too; Atlas (atlas.2300.exe, running) now sends queue_position
  = 1 + earlier waiting entries of that queue (tests updated). Awaiting in-game test.
- 23:05 user screenshot: the login/menu queue screen now reads "1ST IN LINE FOR MATCHMAKING" (queue_position works). The
  in-lobby FIND MATCH bar still 00:00 although the expression gets 60/55: next, find what the bar shows (estimate vs
  its own elapsed timer mm_interface_matchelapsedtime_* from match_search_start_time).

### 2026-11-17 23:08 — loader: local plugins (dev only)
- crates/evr-loader: with plugins\config.json "dev": true, loads every *.dll in plugins\local\ (name order) after the
  runtime; calls an optional export `EvrPluginStart` (extern "C" fn() -> i32). Logged in evr-loader.log. Tests: 2.
- Installed hid.dll (backup hid.2308.dll). plugins\local\SkipIntro.dll loads (verified 23:08).
- 23:11: test with queue_estimate_scale 1000: getter fed 60000/55000, bar still 00:00. The bar (17d77f27, registers
  R15NetMatchmakerQueueExpression pins queueposition? + estimatedwaittime) does not display that pin. Its 00:00 is likely
  its own timer (mm_interface_matchelapsedtime_* from match_search_start_time). Setting removed from config.json (the
  code reading it stays, default 1).
- 23:16: CAUSE of 00:00: the bar's timer counts from match_search_start_time, read by the bar script (0x12050 ->
  reader 0x25470, R15NetPartyIntExpression) from the PARTY data key "mm|starttime". The game copies its start time
  ([netGame+0x40]+0x8e8, _time64 s) into party data (writer 0x1401bc1a0, also mm|eta float [+0x5fc]) only when a party
  object [netGame+0x647c8] (Oculus Social) exists. No party -> 0 -> 00:00. FIX (menu/queue_pc.cpp, per bar library
  load): the CALL at bar+0x12059 goes to a 14-byte stub at bar+0x11edb (int3 padding) -> StartTimeHook: party value,
  else the game's own start time. evr_earlyquit::NetGame() added (getter `this` as fallback). Verified the patch applies
  ("start time without a party on"); bar not yet seen counting. Backup: evr-runtime.2315.dll.
- 23:18 user screenshot: bar = "SEARCHING FOR ARENA MATCH / Estimated 00:00 / Elapsed 00:02": Elapsed now counts.
  Estimated is the status script 18d18df21fc61b61.dll (ui_ar_mp_mm_status), party real key "mm|eta" (the bar script
  17d77f27 only reads the queue expression's "position" pin, never estimatedwaittime).
- 23:20: hook on R15NetPartyRealExpression's evaluator 0x140d9a4c0 (out {u8 exists; f32 value @+4}, key pin
  Symbol64("key") type 6): no party + key "mm|eta" -> the queue estimate getter's seconds. Installs, lobby loads; not yet
  seen on the bar. Backup: evr-runtime.2320.dll.
- 23:22 still 00:00 with the mm|eta hook (removed again). The status script's Estimated = R15NetMatchmakingStatsExpression
  matchtimeaverageseconds (evaluator 0x140d9c080 -> 0x1401820e0 -> float [lobby+0x5fc], set only by
  event_update_findmatch_estimated_time, handler 0x1401821e0, a server message nobody sends us).
- 23:33: MinHook on 0x1401820e0 (only caller: the stats evaluator): while the field is 0, return the queue estimate
  getter's seconds. Installs, lobby loads; not yet seen on the bar.
- 23:35 user: bar shows "Estimated 00:12 / Elapsed 00:03" (works); estimate steps every 5 s, sat at 0 without a match.
- 23:40 Atlas (atlas.2340.exe, running): queue_heartbeat_interval 1 (was 5); samples only waits that ended in a match
  (LobbyJoinEntrants -> estimator.MatchedAddr(session.ClientIP(), label.Mode) for the "addr:" entries; a leave without
  a match is a cancel, not sampled); past the estimate: median of longer waits minus the wait, else one more typical
  wait (no stuck 0). Queue tests updated + 3 new; pass. Note: token-keyed (Oculus) entries never get matches -> fallback.
- 23:45 user: past 0 the estimate froze at 01:00 (lone player never matched). Atlas atlas.2347.exe (running): with no
  longer waits it counts down typical - (waited-typical) % typical, i.e. fresh cycles. Test extended; pass.

### 2026-11-17 23:50 — committed, party build first run
- All three repos committed on branch CatBranch (EVR-Runtime f6efb33 local only: no remote; atlas 8f461aa and
  EchoRustServer 7c411ec pushed to origin/CatBranch).
- Party runtime built in runtime/build-party (-DEVR_PARTY=ON, ignored by git) and INSTALLED (non-party backup:
  evr-runtime.2349-nonparty.dll). Solo -mp launch to the lobby: no crash; "[EVR.PARTY] installed", "social object
  handed to the game", "social object initialised (max users 10, callbacks set)". PARTY tab not yet opened.

### 2026-11-17 23:59 — party build: lobby find fixed ("Matchmaker is unavailable")
- Cause: the party table's S_MemberId gave the local member's Discord id; the lobby find sends it as its evr id and
  Atlas closed the connection ("mismatched evr id", DMO-<discord> vs DMO-<login number>).
- Fix (`src/party/party_pc.cpp` GameAccountNumber): own member id = the game's login account number, read like the
  lobby does at 0x140180562: user = [[[lobby+0x910]]+0x28D0], then 0x140618690(user, &out) -> {platform, number}
  (prologue checked; address via evr::ResolveVA — a raw VA failed the check). Logs
  `[EVR.PARTY] own account: ... -> 7-<number>` when it changes.
- Verified 23:58: social lobby joined, no mismatch in Atlas. Party build installed as evr-runtime.dll.
- Next: tablet PARTY tab test (create, invite, accept, kick, leave; two clients for invite/accept).
