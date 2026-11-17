# Rough-start test (PC)

Game: `H:\EVR\ready-at-dawn-echo-arena\bin\win10`. Only `hid.dll` is added to the stock build. Every export forwards
to `System32\hid.dll`.

```
bin\win10\hid.dll                 loader (target\release\hid.dll)
bin\win10\plugins\config.json     {"feed": "...manifest.signed.json", "channel": "stable", "show_skipped": false}
bin\win10\plugins\state.json      installed / skipped / pending / last_good / boot_fails / bad
bin\win10\plugins\versions\<v>\   verified loose folder per version
bin\win10\plugins\evr-loader.log  what happened
bin\win10\CDN\                    cosmetic and poster cache (runtime, later)
```

## Loop

1. `cargo build --release`, then copy `target\release\hid.dll` into `bin\win10`.
2. Make a release: put files under `dev\release-src\pc\...`. Paths under `pc\live\` apply live, and everything else
   needs a relaunch. Then build and sign it:
   `cargo run --release -p evr-sign -- build --key dev\dev.key --label DEV --version 0.1.0 --channel stable --dir dev\release-src --base-url http://127.0.0.1:8765 --title "..." --notes dev\notes.txt --out dev\upload`
3. Serve it: `python -m http.server 8765 --bind 127.0.0.1` in `dev\upload`.
4. Launch `echovr.exe` (or `tools\dev-run.ps1 -Version <installed version>`). Nothing asks on the desktop. On the
   main menu the **UPDATE** card shows above FRIENDS and its panel opens by itself:
   - **UPDATE:** download, verify and stage, with progress on the button, then **RESTART NOW** relaunches the game.
   - **SKIP:** saves the skip. The card stays and still opens the panel (SKIP becomes LATER), but the panel no longer
     opens by itself.
   - With no newer release, there is no card and no panel.
5. `"show_skipped": true` in `config.json` treats a skipped version as new (the panel opens by itself again).
6. To test rollback, ship a build whose `evr-runtime.dll` crashes before the main menu. A version is marked good when
   the menu comes up (the runtime calls `EvrUpdateBootOk`). After 2 boots that never reach it, the loader reverts to
   the last good version and never offers the bad one again. `"dev": true` in `config.json` turns boot-failure
   counting off.
