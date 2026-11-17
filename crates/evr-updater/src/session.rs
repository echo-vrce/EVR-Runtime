//! The in-game update flow, shared by every loader (PC `hid.dll`, Quest `.so`).
//!
//! The loader checks the feed once and hands the result to a [`Session`]. The runtime (the menu's UPDATES card and
//! panel) reads [`Session::status_json`] and asks for [`Session::start`] (download, verify, stage) or
//! [`Session::skip`]. Staging, signature checks and rollback stay in [`Updater`]; the session only holds the verified
//! offer and reports progress.

use crate::{Decision, Offer, Updater};
use std::sync::{Arc, Mutex};

#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Phase {
    /// The feed check has not finished yet.
    Checking,
    /// Nothing newer (or the check failed: `error` says why).
    None,
    /// A newer release is offered (`skipped` when the player skipped it before).
    Offer,
    Downloading,
    /// Staged; it applies at the next launch (`relaunch`) or is already live.
    Staged,
    /// Download or staging failed; the offer can be tried again.
    Failed,
}

/// What the runtime shows. Serialised as the JSON the runtime reads.
#[derive(Debug, Clone, PartialEq, serde::Serialize)]
pub struct Status {
    pub state: Phase,
    pub version: String,
    pub title: String,
    pub notes: Vec<String>,
    pub files: usize,
    pub bytes: u64,
    pub relaunch: bool,
    pub skipped: bool,
    pub done: u64,
    pub total: u64,
    pub error: String,
}

impl Default for Status {
    fn default() -> Self {
        Self {
            state: Phase::Checking,
            version: String::new(),
            title: String::new(),
            notes: Vec::new(),
            files: 0,
            bytes: 0,
            relaunch: false,
            skipped: false,
            done: 0,
            total: 0,
            error: String::new(),
        }
    }
}

#[derive(Default)]
struct Inner {
    status: Status,
    offer: Option<Offer>,
}

/// One per process. Cheap to clone (shared state).
#[derive(Clone, Default)]
pub struct Session(Arc<Mutex<Inner>>);

impl Session {
    pub fn new() -> Self {
        Self::default()
    }

    fn with<R>(&self, f: impl FnOnce(&mut Inner) -> R) -> R {
        let mut g = self.0.lock().unwrap_or_else(|e| e.into_inner());
        f(&mut g)
    }

    pub fn status(&self) -> Status {
        self.with(|i| i.status.clone())
    }

    pub fn status_json(&self) -> String {
        serde_json::to_string(&self.status()).unwrap_or_else(|_| "{}".into())
    }

    /// Records the feed check's result.
    pub fn set_checked(&self, r: Result<Decision, crate::Error>) {
        self.with(|i| match r {
            Ok(Decision::Prompt(o)) => offer(i, o, false),
            Ok(Decision::Skipped(o)) => offer(i, o, true),
            Ok(Decision::UpToDate) => {
                i.offer = None;
                i.status = Status { state: Phase::None, ..Status::default() };
            }
            Err(e) => {
                i.offer = None;
                i.status = Status { state: Phase::None, error: e.to_string(), ..Status::default() };
            }
        })
    }

    /// Saves `skipped = <offered version>`. The offer stays readable (the card can still open it).
    pub fn skip(&self, u: &Updater) -> Result<(), crate::Error> {
        let v = self.with(|i| i.offer.as_ref().map(|o| o.manifest.version));
        let Some(v) = v else { return Ok(()) };
        u.skip(v)?;
        self.with(|i| {
            if matches!(i.status.state, Phase::Offer | Phase::Failed) {
                i.status.state = Phase::Offer;
                i.status.skipped = true;
                i.status.error.clear();
            }
        });
        Ok(())
    }

    /// Takes the offer for downloading; false when there is none or one is already running or staged.
    pub fn begin(&self) -> Option<Offer> {
        self.with(|i| {
            if !matches!(i.status.state, Phase::Offer | Phase::Failed) {
                return None;
            }
            let o = i.offer.clone()?;
            i.status.state = Phase::Downloading;
            i.status.done = 0;
            i.status.total = i.status.bytes;
            i.status.error.clear();
            Some(o)
        })
    }

    pub fn progress(&self, done: u64, total: u64) {
        self.with(|i| {
            i.status.done = done;
            i.status.total = total;
        })
    }

    pub fn finish(&self, r: Result<(), crate::Error>) {
        self.with(|i| match r {
            Ok(()) => {
                i.status.state = Phase::Staged;
                i.status.skipped = false;
            }
            Err(e) => {
                i.status.state = Phase::Failed;
                i.status.error = e.to_string();
            }
        })
    }

    /// Download, verify and stage the offer on this thread (the loader runs it on a worker).
    pub fn start(&self, u: &Updater) -> bool {
        let Some(o) = self.begin() else { return false };
        let r = u.stage(&o, |d, t| self.progress(d, t)).map(|_| ());
        self.finish(r);
        true
    }
}

fn offer(i: &mut Inner, o: Offer, skipped: bool) {
    let m = &o.manifest;
    let files: Vec<_> = o.build.files.iter().chain(&m.changelog.images).collect();
    i.status = Status {
        state: Phase::Offer,
        version: m.version.to_string(),
        title: m.changelog.title.clone(),
        notes: m.changelog.notes.clone(),
        files: files.len(),
        bytes: files.iter().map(|f| f.size).sum(),
        relaunch: o.needs_relaunch(),
        skipped,
        ..Status::default()
    };
    i.offer = Some(o);
}
