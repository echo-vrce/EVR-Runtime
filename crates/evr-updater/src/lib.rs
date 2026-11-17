//! Update client: query the feed, offer, skip, download + verify + stage, and promote at boot.
//!
//! On-disk layout under `root` (PC `%LOCALAPPDATA%\evr`, Quest `<files>/evr`):
//! ```text
//! state.json               installed / skipped / pending / last_good / boot_fails
//! versions/<ver>/...       one fully verified tree per version (the loose folder)
//! staging/<ver>/...        download in progress, never loaded
//! ```

use evr_manifest::{self as m, Apply, FileEntry, Manifest, PinnedKey, Platform, PlatformBuild, Policy, Version};
use serde::{Deserialize, Serialize};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

pub mod session;
pub use session::{Phase, Session, Status};

pub const LOADER_ABI: u32 = 1;
/// Boots that fail to reach the menu before the loader reverts to `last_good`.
pub const MAX_BOOT_FAILS: u32 = 2;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("network: {0}")]
    Net(String),
    #[error("manifest: {0}")]
    Manifest(#[from] m::Error),
    #[error("io: {0}")]
    Io(#[from] std::io::Error),
}

#[derive(Debug, Default, Clone, PartialEq, Serialize, Deserialize)]
pub struct State {
    pub installed: Option<Version>,
    pub skipped: Option<Version>,
    /// Staged and verified; becomes `installed` at next boot.
    pub pending: Option<Version>,
    pub last_good: Option<Version>,
    pub boot_fails: u32,
    /// Versions that failed to boot; never offered again.
    #[serde(default)]
    pub bad: Vec<Version>,
    /// Versions that have reached the menu at least once; only these become `last_good`.
    #[serde(default)]
    pub ok: Vec<Version>,
}

pub struct Config {
    pub root: PathBuf,
    /// URL of `manifest.signed.json` (GitHub: `.../releases/latest/download/manifest.signed.json`).
    pub feed: String,
    pub channel: String,
    pub keys: &'static [PinnedKey],
}

#[derive(Debug, Clone)]
pub struct Offer {
    pub manifest: Manifest,
    pub build: PlatformBuild,
    pub envelope: Vec<u8>,
}

impl Offer {
    pub fn needs_relaunch(&self) -> bool {
        self.build.files.iter().any(|f| f.apply == Apply::Relaunch)
    }
}

pub enum Decision {
    /// Show the panel.
    Prompt(Offer),
    /// Newer exists but the player skipped it; the menu button can still open it.
    Skipped(Offer),
    UpToDate,
}

pub struct Updater {
    pub cfg: Config,
}

fn now() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

fn get(url: &str) -> Result<Vec<u8>, Error> {
    let r = ureq::get(url)
        .set("User-Agent", "evr-runtime")
        .timeout(std::time::Duration::from_secs(30))
        .call()
        .map_err(|e| Error::Net(e.to_string()))?;
    let mut buf = Vec::new();
    std::io::Read::read_to_end(&mut r.into_reader().take(512 << 20), &mut buf)?;
    Ok(buf)
}
use std::io::Read as _;

impl Updater {
    pub fn new(cfg: Config) -> Self {
        Self { cfg }
    }

    fn state_path(&self) -> PathBuf {
        self.cfg.root.join("state.json")
    }

    pub fn state(&self) -> State {
        fs::read(self.state_path()).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default()
    }

    pub fn save(&self, s: &State) -> Result<(), Error> {
        fs::create_dir_all(&self.cfg.root)?;
        let tmp = self.cfg.root.join("state.json.tmp");
        fs::write(&tmp, serde_json::to_vec_pretty(s).unwrap())?;
        fs::rename(tmp, self.state_path())?;
        Ok(())
    }

    /// The loose folder the game should read from this boot, if any.
    pub fn active_dir(&self) -> Option<PathBuf> {
        self.state().installed.map(|v| self.cfg.root.join("versions").join(v.to_string()))
    }

    pub fn check(&self) -> Result<Decision, Error> {
        let env = get(&self.cfg.feed)?;
        self.decide(env, now())
    }

    pub fn decide(&self, envelope: Vec<u8>, now: u64) -> Result<Decision, Error> {
        let p = Policy {
            keys: self.cfg.keys,
            threshold: 1,
            channel: &self.cfg.channel,
            platform: Platform::current(),
            loader_abi: LOADER_ABI,
            now,
        };
        let (manifest, build) = m::verify(&envelope, &p)?;
        let st = self.state();
        let v = manifest.version;
        let have = st.pending.or(st.installed);
        if have.is_some_and(|h| v <= h) || st.bad.contains(&v) {
            return Ok(Decision::UpToDate);
        }
        let offer = Offer { manifest, build, envelope };
        Ok(if st.skipped == Some(v) { Decision::Skipped(offer) } else { Decision::Prompt(offer) })
    }

    pub fn skip(&self, v: Version) -> Result<(), Error> {
        let mut s = self.state();
        s.skipped = Some(v);
        self.save(&s)
    }

    /// Download every file, verify, and move the tree into `versions/`. `progress(done, total)` in bytes.
    pub fn stage(&self, o: &Offer, mut progress: impl FnMut(u64, u64)) -> Result<PathBuf, Error> {
        self.stage_with(o, &mut progress, |f| get(&f.url))
    }

    pub fn stage_with(
        &self,
        o: &Offer,
        progress: &mut dyn FnMut(u64, u64),
        mut fetch: impl FnMut(&FileEntry) -> Result<Vec<u8>, Error>,
    ) -> Result<PathBuf, Error> {
        let ver = o.manifest.version.to_string();
        let staging = self.cfg.root.join("staging").join(&ver);
        let dest = self.cfg.root.join("versions").join(&ver);
        let _ = fs::remove_dir_all(&staging);
        let files: Vec<&FileEntry> = o.build.files.iter().chain(&o.manifest.changelog.images).collect();
        let total: u64 = files.iter().map(|f| f.size).sum();
        let mut done = 0;
        for f in files {
            m::check_path(&f.path)?;
            let bytes = fetch(f)?;
            m::check_file(f, &bytes)?;
            let p = staging.join(&f.path);
            fs::create_dir_all(p.parent().unwrap())?;
            fs::write(p, &bytes)?;
            done += f.size;
            progress(done, total);
        }
        fs::write(staging.join("manifest.signed.json"), &o.envelope)?;
        let _ = fs::remove_dir_all(&dest);
        fs::create_dir_all(dest.parent().unwrap())?;
        fs::rename(&staging, &dest)?;
        let mut s = self.state();
        s.pending = Some(o.manifest.version);
        s.skipped = None;
        self.save(&s)?;
        Ok(dest)
    }

    /// Called by the loader at process start, before anything from the loose folder loads.
    /// Promotes a pending version, counts the boot, and reverts after repeated failures.
    pub fn boot(&self) -> Result<State, Error> {
        let mut s = self.state();
        if let Some(p) = s.pending.take() {
            if s.installed.is_some_and(|v| s.ok.contains(&v)) {
                s.last_good = s.installed;
            }
            s.installed = Some(p);
            s.boot_fails = 0;
        }
        if s.installed.is_some() {
            s.boot_fails += 1;
            if s.boot_fails > MAX_BOOT_FAILS {
                if let Some(bad) = s.installed {
                    s.bad.push(bad);
                }
                s.installed = s.last_good.take();
                s.boot_fails = 1;
            }
        }
        self.save(&s)?;
        Ok(s)
    }

    /// Called once the main menu is up: this version is good.
    pub fn boot_ok(&self) -> Result<(), Error> {
        let mut s = self.state();
        s.boot_fails = 0;
        if let Some(v) = s.installed {
            if !s.ok.contains(&v) {
                s.ok.push(v);
            }
        }
        self.save(&s)
    }
}

pub fn dir_size(p: &Path) -> u64 {
    fs::read_dir(p)
        .into_iter()
        .flatten()
        .flatten()
        .map(|e| {
            let md = e.metadata().ok();
            if md.as_ref().is_some_and(|m| m.is_dir()) { dir_size(&e.path()) } else { md.map_or(0, |m| m.len()) }
        })
        .sum()
}

#[cfg(test)]
mod tests;
