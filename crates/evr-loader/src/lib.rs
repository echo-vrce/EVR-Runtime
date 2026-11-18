//! EVR loader (PC). The only file a player installs by hand; kept small and stable.
//!
//! Before echovr.exe's entry point runs it:
//! 1. promotes a staged update and counts the boot (rollback after repeated failed boots),
//! 2. loads `evr-runtime.dll` from the active loose folder if the release ships one,
//! 3. in dev mode only (`plugins\config.json` `"dev": true`), loads every `*.dll` in `plugins\local\` by name order.
//!
//! Then a worker thread checks the feed. It never asks anything itself: the runtime's UPDATES card and panel read the
//! offer and drive the update through the `EvrUpdate*` exports below (Update downloads, verifies and stages; Skip
//! saves the skip; Restart now relaunches the game). The verified offer never leaves this DLL; the runtime only sees
//! its status as JSON.
//!
//! Layout next to echovr.exe (`bin\win10`): `hid.dll` (this loader), `plugins\` (updater state, versions,
//! active loose folder), `CDN\` (cosmetic and poster cache, filled by the runtime).

#![cfg(windows)]

use base64::{engine::general_purpose::STANDARD as B64, Engine};
use evr_manifest::PinnedKey;
use evr_updater::{channel, Config, Decision, Session, Updater};
use std::fs::OpenOptions;
use std::io::Write;
use std::path::PathBuf;
use std::sync::OnceLock;
use windows_sys::Win32::Foundation::HINSTANCE;
use windows_sys::Win32::System::LibraryLoader::{DisableThreadLibraryCalls, GetModuleHandleW, GetProcAddress, LoadLibraryW};
use windows_sys::Win32::System::Memory::{VirtualProtect, PAGE_EXECUTE_READWRITE};
use windows_sys::Win32::System::Diagnostics::Debug::{FlushInstructionCache, IMAGE_NT_HEADERS64};
use windows_sys::Win32::System::SystemServices::IMAGE_DOS_HEADER;
use windows_sys::Win32::System::Threading::{GetCurrentProcess, TerminateProcess};
use windows_sys::Win32::System::SystemServices::DLL_PROCESS_ATTACH;

mod pinned {
    include!(concat!(env!("OUT_DIR"), "/pinned.rs"));
}

/// Stable releases (built from the GitHub branch `Main`). Beta testers get their feed from Atlas (see `resolve_channel`).
const DEFAULT_FEED: &str = "https://github.com/echo-vrce/EVR-Runtime/releases/latest/download/manifest.signed.json";

#[derive(serde::Deserialize, Default)]
#[serde(default)]
struct LocalConfig {
    /// Dev override: with `feed` or `channel` set, Atlas is not asked for the channel.
    feed: Option<String>,
    channel: Option<String>,
    /// Set to true to treat a skipped update as new (the panel opens by itself again).
    show_skipped: bool,
    /// Developer machine: never count failed boots (killing the game while iterating would roll back).
    dev: bool,
}

fn game_dir() -> PathBuf {
    std::env::current_exe().ok().and_then(|p| p.parent().map(PathBuf::from)).unwrap_or_default()
}

fn root() -> PathBuf {
    game_dir().join("plugins")
}

fn log(msg: &str) {
    let _ = std::fs::create_dir_all(root());
    if let Ok(mut f) = OpenOptions::new().create(true).append(true).open(root().join("evr-loader.log")) {
        let t = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_secs());
        let _ = writeln!(f, "{t} {msg}");
    }
}

fn keys() -> &'static [PinnedKey] {
    static K: OnceLock<Vec<PinnedKey>> = OnceLock::new();
    K.get_or_init(|| {
        pinned::PINNED
            .iter()
            .filter_map(|(l, k)| {
                let b: [u8; 32] = B64.decode(k).ok()?.try_into().ok()?;
                Some(PinnedKey { label: l, key: b })
            })
            .collect()
    })
}

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}

fn relaunch() {
    let exe = std::env::current_exe().unwrap();
    let args: Vec<String> = std::env::args().skip(1).collect();
    log(&format!("relaunch {} {:?}", exe.display(), args));
    match std::process::Command::new(&exe).args(&args).current_dir(exe.parent().unwrap()).spawn() {
        // End at once: a normal exit from a worker thread runs every DLL's detach while the game's threads still run.
        Ok(_) => unsafe {
            TerminateProcess(GetCurrentProcess(), 0);
        },
        Err(e) => log(&format!("relaunch failed: {e}")),
    }
}

/// The (feed, channel) Atlas gave this boot; unset until the worker has asked.
static RESOLVED: OnceLock<(String, String)> = OnceLock::new();

fn updater(lc: &LocalConfig) -> Updater {
    let (feed, channel) = match (&lc.feed, &lc.channel, RESOLVED.get()) {
        (None, None, Some((f, c))) => (f.clone(), c.clone()),
        _ => (
            lc.feed.clone().unwrap_or_else(|| DEFAULT_FEED.into()),
            lc.channel.clone().unwrap_or_else(|| "stable".into()),
        ),
    };
    Updater::new(Config { root: root(), feed, channel, keys: keys() })
}

/// Worker thread, before the feed check: asks Atlas whether this player is a beta tester (`/beta` in Discord) and
/// remembers the beta feed if so. Skipped when `plugins\config.json` sets `feed` or `channel` (dev override).
/// Anything going wrong means stable.
fn resolve_channel(lc: &LocalConfig) {
    if lc.feed.is_some() || lc.channel.is_some() {
        log("channel: feed/channel set in config.json, Atlas not asked");
        return;
    }
    let path = game_dir().join("..").join("..").join("_local").join("config.json");
    let Some(login) = std::fs::read(&path).ok().and_then(|b| channel::parse_game_config(&b)) else {
        log("channel: stable (no Atlas login in _local\\config.json)");
        return;
    };
    match channel::ask_atlas(&login) {
        Ok(channel::Channel::Beta { feed }) => {
            log(&format!("channel: beta (Atlas) feed {feed}"));
            let _ = RESOLVED.set((feed, "beta".into()));
        }
        Ok(channel::Channel::Stable) => log("channel: stable (Atlas)"),
        Err(e) => log(&format!("channel: stable (could not ask Atlas: {e})")),
    }
}

fn local_config() -> LocalConfig {
    std::fs::read(root().join("config.json")).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default()
}

/// Runs on the game's main thread, before echovr.exe's entry point: promote a staged update, then load and start the
/// runtime so its engine hooks are in place before the engine initialises.
fn early() {
    let _ = std::fs::create_dir_all(game_dir().join("CDN"));
    log(&format!("loader start v{} pid {}", env!("CARGO_PKG_VERSION"), std::process::id()));
    let lc = local_config();
    let u = updater(&lc);
    if lc.dev {
        log("dev mode: boot failure counting off");
    }
    let booted = u.boot();
    if lc.dev {
        let _ = u.boot_ok();
    }
    match booted {
        Ok(s) => log(&format!("boot installed={:?} last_good={:?} fails={}", s.installed, s.last_good, s.boot_fails)),
        Err(e) => log(&format!("boot error: {e}")),
    }
    load_runtime(&u);
    if lc.dev {
        load_local_plugins();
    }
}

/// `*.dll` files directly in `dir` (any case of the extension), sorted by file name; empty when `dir` is missing.
fn local_plugin_paths(dir: &std::path::Path) -> Vec<PathBuf> {
    let Ok(entries) = std::fs::read_dir(dir) else { return Vec::new() };
    let mut out: Vec<PathBuf> = entries
        .filter_map(|e| e.ok())
        .filter(|e| e.file_type().map(|t| t.is_file()).unwrap_or(false))
        .map(|e| e.path())
        .filter(|p| p.extension().map(|x| x.eq_ignore_ascii_case("dll")).unwrap_or(false))
        .collect();
    out.sort_by_key(|p| p.file_name().map(|n| n.to_ascii_lowercase()));
    out
}

/// Dev mode only: your own DLLs from `plugins\local\`, after the runtime (so its hooks are in place first). Each is
/// loaded with LoadLibrary (its DllMain runs); a DLL that exports `EvrPluginStart` (extern "C" fn() -> i32) has it
/// called once. Off outside dev mode: otherwise any DLL dropped there would run inside every player's game.
fn load_local_plugins() {
    let dir = root().join("local");
    let _ = std::fs::create_dir_all(&dir);
    for p in local_plugin_paths(&dir) {
        let h = unsafe { LoadLibraryW(wide(&p.to_string_lossy()).as_ptr()) };
        if h.is_null() {
            log(&format!("local plugin {} loaded=false", p.display()));
            continue;
        }
        match unsafe { GetProcAddress(h, b"EvrPluginStart\0".as_ptr()) } {
            Some(f) => {
                let f: extern "C" fn() -> i32 = unsafe { std::mem::transmute(f) };
                log(&format!("local plugin {} loaded=true start -> {}", p.display(), f()));
            }
            None => log(&format!("local plugin {} loaded=true", p.display())),
        }
    }
}

fn load_runtime(u: &Updater) {
    let Some(dir) = u.active_dir() else { return };
    let rt = dir.join("evr-runtime.dll");
    if !rt.exists() {
        return;
    }
    let h = unsafe { LoadLibraryW(wide(&rt.to_string_lossy()).as_ptr()) };
    log(&format!("runtime {} loaded={}", rt.display(), !h.is_null()));
    if h.is_null() {
        return;
    }
    let Some(start) = (unsafe { GetProcAddress(h, b"EvrRuntimeStart\0".as_ptr()) }) else {
        return log("runtime has no EvrRuntimeStart");
    };
    let start: extern "C" fn(*const u8) -> i32 = unsafe { std::mem::transmute(start) };
    let ver = format!("{}\0", u.state().installed.map(|v| v.to_string()).unwrap_or_default());
    log(&format!("runtime start -> {}", start(ver.as_ptr())));
}

fn session() -> &'static Session {
    static S: OnceLock<Session> = OnceLock::new();
    S.get_or_init(Session::new)
}

/// Worker thread: check the feed once and hand the result to the session the runtime reads.
fn run() {
    let lc = local_config();
    resolve_channel(&lc);
    let u = updater(&lc);
    let r = match u.check() {
        Ok(Decision::Skipped(o)) if lc.show_skipped => Ok(Decision::Prompt(o)),
        r => r,
    };
    match &r {
        Ok(Decision::Prompt(o)) => log(&format!("offer {} ({} files)", o.manifest.version, o.build.files.len())),
        Ok(Decision::Skipped(o)) => log(&format!("update {} skipped by player (card still offers it)", o.manifest.version)),
        Ok(Decision::UpToDate) => log("up to date"),
        Err(e) => log(&format!("check failed: {e}")),
    }
    session().set_checked(r);
}

// ---- the runtime's interface (C ABI; the Quest loader exports the same names) ----

/// Writes the update status as UTF-8 JSON (no NUL) into `buf` when it fits; returns its length either way.
/// `{"state": "checking|none|offer|downloading|staged|failed", "version", "title", "notes": [..], "files", "bytes",
/// "relaunch", "skipped", "done", "total", "error"}`.
#[no_mangle]
pub extern "C" fn EvrUpdateStatus(buf: *mut u8, cap: usize) -> usize {
    let json = session().status_json();
    if !buf.is_null() && json.len() <= cap {
        unsafe { std::ptr::copy_nonoverlapping(json.as_ptr(), buf, json.len()) };
    }
    json.len()
}

/// Download, verify and stage the offer (blocks until done; call it from a worker thread). 0 started, 1 nothing to do.
#[no_mangle]
pub extern "C" fn EvrUpdateStart() -> i32 {
    let u = updater(&local_config());
    match std::panic::catch_unwind(|| session().start(&u)) {
        Ok(true) => {
            let st = session().status();
            log(&format!("update {}: {:?} {}", st.version, st.state, st.error));
            0
        }
        Ok(false) => 1,
        Err(_) => {
            log("update panicked");
            -1
        }
    }
}

/// Save `skipped = <offered version>`. 0 ok, -1 failed.
#[no_mangle]
pub extern "C" fn EvrUpdateSkip() -> i32 {
    let u = updater(&local_config());
    match session().skip(&u) {
        Ok(()) => {
            log(&format!("skip {}", session().status().version));
            0
        }
        Err(e) => {
            log(&format!("skip failed: {e}"));
            -1
        }
    }
}

/// Relaunch the game (same arguments) once a staged update needs it, then end this process. Returns only on failure.
#[no_mangle]
pub extern "C" fn EvrUpdateRelaunch() -> i32 {
    let st = session().status();
    if st.state != evr_updater::Phase::Staged || !st.relaunch {
        return 1;
    }
    relaunch();
    -1
}

/// The main menu is up: the installed version boots (it can become `last_good`).
#[no_mangle]
pub extern "C" fn EvrUpdateBootOk() -> i32 {
    let u = updater(&local_config());
    match u.boot_ok() {
        Ok(()) => {
            log("boot ok (main menu reached)");
            0
        }
        Err(e) => {
            log(&format!("boot ok failed: {e}"));
            -1
        }
    }
}

// ---- entry-point hook: run `early()` on the main thread before echovr.exe's own entry ----

static mut ENTRY: usize = 0;
static mut SAVED: [u8; 14] = [0; 14];

unsafe fn write_code(at: usize, bytes: &[u8]) {
    let mut old = 0u32;
    VirtualProtect(at as _, bytes.len(), PAGE_EXECUTE_READWRITE, &mut old);
    std::ptr::copy_nonoverlapping(bytes.as_ptr(), at as *mut u8, bytes.len());
    VirtualProtect(at as _, bytes.len(), old, &mut old);
    FlushInstructionCache(GetCurrentProcess(), at as _, bytes.len());
}

extern "system" fn entry_hook() -> u32 {
    unsafe {
        let entry = ENTRY;
        write_code(entry, &*std::ptr::addr_of!(SAVED));
        if std::panic::catch_unwind(early).is_err() {
            log("early panicked");
        }
        std::thread::spawn(|| {
            if std::panic::catch_unwind(run).is_err() {
                log("loader panicked");
            }
        });
        let original: extern "system" fn() -> u32 = std::mem::transmute(entry);
        original()
    }
}

unsafe fn hook_entry() -> bool {
    let base = GetModuleHandleW(std::ptr::null()) as usize;
    let dos = &*(base as *const IMAGE_DOS_HEADER);
    let nt = &*((base + dos.e_lfanew as usize) as *const IMAGE_NT_HEADERS64);
    let entry = base + nt.OptionalHeader.AddressOfEntryPoint as usize;
    ENTRY = entry;
    std::ptr::copy_nonoverlapping(entry as *const u8, std::ptr::addr_of_mut!(SAVED) as *mut u8, 14);
    let mut jmp = [0xFFu8, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]; // jmp [rip+0]; dq target
    jmp[6..].copy_from_slice(&(entry_hook as usize as u64).to_le_bytes());
    write_code(entry, &jmp);
    true
}

/// The game's own executable names, lowercased. echovr.exe is the stock live build; echovr_openxr.exe is EchoXR's
/// patched copy (it renames the game so its OpenXR runtime loads), and echoarena.exe / echocombat.exe are the
/// classic-build names. A tool that loads hid.dll under any other name is left alone, so the loader never patches a
/// process whose entry point is not the game's.
fn is_game_exe(name: &str) -> bool {
    matches!(name, "echovr.exe" | "echovr_openxr.exe" | "echoarena.exe" | "echocombat.exe")
}

#[no_mangle]
extern "system" fn DllMain(h: HINSTANCE, reason: u32, _: *mut core::ffi::c_void) -> i32 {
    if reason == DLL_PROCESS_ATTACH {
        unsafe {
            DisableThreadLibraryCalls(h);
            // Only patch the entry when loaded by the game itself (not a tool that loads hid.dll).
            let exe = std::env::current_exe().ok().and_then(|p| p.file_name().map(|n| n.to_string_lossy().to_lowercase()));
            if exe.as_deref().map(is_game_exe).unwrap_or(false) {
                hook_entry();
            }
        }
    }
    1
}

#[cfg(test)]
mod game_exe_tests {
    use super::is_game_exe;

    #[test]
    fn matches_game_executables() {
        for n in ["echovr.exe", "echovr_openxr.exe", "echoarena.exe", "echocombat.exe"] {
            assert!(is_game_exe(n), "{n} should be a game exe");
        }
    }

    #[test]
    fn rejects_tools_and_launchers() {
        for n in ["echoxr.exe", "echoxrhands.exe", "echoxrsettings.exe", "echoquest.exe", "notepad.exe", ""] {
            assert!(!is_game_exe(n), "{n} should not be a game exe");
        }
    }
}

#[cfg(test)]
mod local_plugin_tests {
    use super::local_plugin_paths;

    #[test]
    fn lists_only_dlls_in_name_order() {
        let dir = std::env::temp_dir().join(format!("evr-local-plugins-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(dir.join("sub.dll")).unwrap();
        for f in ["b.dll", "A.DLL", "notes.txt", "c.dll.off"] {
            std::fs::write(dir.join(f), b"x").unwrap();
        }
        let names: Vec<String> =
            local_plugin_paths(&dir).iter().map(|p| p.file_name().unwrap().to_string_lossy().into_owned()).collect();
        let _ = std::fs::remove_dir_all(&dir);
        assert_eq!(names, ["A.DLL", "b.dll"]);
    }

    #[test]
    fn missing_folder_is_empty() {
        assert!(local_plugin_paths(std::path::Path::new("Z:/no/such/evr/folder")).is_empty());
    }
}
