//! EVR loader (Quest / Android): the counterpart of evr-loader's `hid.dll` on PC.
//!
//! It is built as `libevr_loader.so` for aarch64-linux-android and loaded into the r15 app process
//! (`com.readyatdawn.r15`) by the Quest patcher. Its ELF constructor runs [`early`] when the library loads:
//! 1. promote a staged update and count the boot (rollback after repeated failed boots),
//! 2. load `evr-runtime.so` from the active loose folder if the release ships one and call `EvrRuntimeStart`.
//!
//! Then a worker thread checks the feed. Like the PC loader it never asks anything itself: the runtime's UPDATES card
//! drives the update through the `EvrUpdate*` exports (identical names and ABI to the PC loader), and the verified
//! offer never leaves this library.
//!
//! The loose folder lives under the r15 app's external files dir:
//!   `/sdcard/Android/media/com.readyatdawn.r15/files/EVR`   (override with the env var `EVR_ROOT`).
//!
//! This loader manages updates and loads the runtime only. Hooking the game (libr15.so) is the runtime's job, and
//! needs an ARM64 hooking path plus addresses re-pinned against the Quest binary — not done here. The whole
//! Android-specific body is behind `cfg(target_os = "android")` so the crate still builds as a stub on a PC host
//! (so `cargo build/test --workspace` passes); only the Android build produces a working loader.

use base64::{engine::general_purpose::STANDARD as B64, Engine};
use evr_manifest::PinnedKey;
use evr_updater::{Config, Decision, Session, Updater};
use std::path::PathBuf;
use std::sync::OnceLock;

mod pinned {
    include!(concat!(env!("OUT_DIR"), "/pinned.rs"));
}

/// Stable releases (built from the GitHub branch `Main`). Beta resolution via Atlas on Quest is not wired yet
/// (the Quest login config path differs from PC's `_local\config.json`); set `feed`/`channel` in `config.json`
/// under the root to override until then.
const DEFAULT_FEED: &str = "https://github.com/echo-vrce/EVR-Runtime/releases/latest/download/manifest.signed.json";

/// The r15 app's external files dir holds the loose folder, as on a real headset.
const DEFAULT_ROOT: &str = "/sdcard/Android/media/com.readyatdawn.r15/files/EVR";

#[derive(serde::Deserialize, Default)]
#[serde(default)]
struct LocalConfig {
    feed: Option<String>,
    channel: Option<String>,
    show_skipped: bool,
    dev: bool,
}

fn root() -> PathBuf {
    std::env::var("EVR_ROOT").map(PathBuf::from).unwrap_or_else(|_| PathBuf::from(DEFAULT_ROOT))
}

fn log(msg: &str) {
    use std::io::Write;
    let _ = std::fs::create_dir_all(root());
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(root().join("evr-loader.log")) {
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

fn local_config() -> LocalConfig {
    std::fs::read(root().join("config.json")).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default()
}

fn updater(lc: &LocalConfig) -> Updater {
    Updater::new(Config {
        root: root(),
        feed: lc.feed.clone().unwrap_or_else(|| DEFAULT_FEED.into()),
        channel: lc.channel.clone().unwrap_or_else(|| "stable".into()),
        keys: keys(),
    })
}

fn session() -> &'static Session {
    static S: OnceLock<Session> = OnceLock::new();
    S.get_or_init(Session::new)
}

/// Promote a staged update, then load and start the runtime so its hooks are in place before the engine initialises.
fn early() {
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
}

/// Worker thread: check the feed once and hand the result to the session the runtime reads.
fn run() {
    let lc = local_config();
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

// ---- runtime load (dlopen the Quest runtime .so) ----

#[cfg(target_os = "android")]
fn load_runtime(u: &Updater) {
    use std::ffi::CString;
    let Some(dir) = u.active_dir() else { return };
    let rt = dir.join("evr-runtime.so");
    if !rt.exists() {
        return;
    }
    let path = match CString::new(rt.to_string_lossy().as_bytes()) {
        Ok(p) => p,
        Err(_) => return,
    };
    let h = unsafe { libc::dlopen(path.as_ptr(), libc::RTLD_NOW | libc::RTLD_GLOBAL) };
    if h.is_null() {
        log(&format!("runtime {} dlopen failed", rt.display()));
        return;
    }
    let sym = CString::new("EvrRuntimeStart").unwrap();
    let p = unsafe { libc::dlsym(h, sym.as_ptr()) };
    if p.is_null() {
        log("runtime has no EvrRuntimeStart");
        return;
    }
    let start: extern "C" fn(*const u8) -> i32 = unsafe { std::mem::transmute(p) };
    let ver = format!("{}\0", u.state().installed.map(|v| v.to_string()).unwrap_or_default());
    log(&format!("runtime {} start -> {}", rt.display(), start(ver.as_ptr())));
}

#[cfg(not(target_os = "android"))]
fn load_runtime(_u: &Updater) {} // host stub: the real loader only runs on the Quest

// ---- ELF constructor: run early() when the library is loaded into the r15 process ----

#[cfg(target_os = "android")]
#[used]
#[link_section = ".init_array"]
static CTOR: extern "C" fn() = {
    extern "C" fn ctor() {
        let _ = std::panic::catch_unwind(|| {
            early();
            std::thread::spawn(|| {
                let _ = std::panic::catch_unwind(run);
            });
        });
    }
    ctor
};

// ---- the runtime's interface (C ABI; identical to the PC loader so the same runtime UI drives both) ----

/// Writes the update status as UTF-8 JSON (no NUL) into `buf` when it fits; returns its length either way.
#[no_mangle]
pub extern "C" fn EvrUpdateStatus(buf: *mut u8, cap: usize) -> usize {
    let json = session().status_json();
    if !buf.is_null() && json.len() <= cap {
        unsafe { std::ptr::copy_nonoverlapping(json.as_ptr(), buf, json.len()) };
    }
    json.len()
}

/// Writes this boot's update channel ("stable"/"beta") into `buf` when it fits; returns its length either way.
/// On Quest the channel is whatever `config.json` set (default "stable"): Atlas beta resolution is not wired yet.
#[no_mangle]
pub extern "C" fn EvrUpdateChannel(buf: *mut u8, cap: usize) -> usize {
    let c = local_config().channel.unwrap_or_else(|| "stable".into());
    if !buf.is_null() && c.len() <= cap {
        unsafe { std::ptr::copy_nonoverlapping(c.as_ptr(), buf, c.len()) };
    }
    c.len()
}

/// Download, verify and stage the offer (blocks; call from a worker thread). 0 started, 1 nothing to do, -1 panicked.
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

/// A staged update applies at the next app launch on Quest (there is no in-process relaunch). Logs and ends the
/// process so reopening the app promotes the staged version; returns only if nothing is staged (1).
#[no_mangle]
pub extern "C" fn EvrUpdateRelaunch() -> i32 {
    let st = session().status();
    if st.state != evr_updater::Phase::Staged || !st.relaunch {
        return 1;
    }
    log("relaunch requested: exiting so the next app launch promotes the staged update");
    std::process::exit(0);
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
