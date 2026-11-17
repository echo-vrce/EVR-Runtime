//! Signed release manifest: the one document the loader trusts.
//!
//! The wire form is `{"manifest": <canonical JSON string>, "sigs": [{"key": "K1", "sig": "<b64>"}]}`.
//! The signature covers the exact bytes of `manifest`, so no JSON canonicalisation is needed.
//! Shared by the PC and Quest builds; no platform code here.

use base64::{engine::general_purpose::STANDARD as B64, Engine};
use ed25519_dalek::{Signature, Verifier, VerifyingKey};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::cmp::Ordering;

pub const SCHEMA: u32 = 1;

#[derive(Debug, thiserror::Error, PartialEq)]
pub enum Error {
    #[error("malformed envelope or manifest: {0}")]
    Malformed(String),
    #[error("no valid signature from a pinned key")]
    BadSignature,
    #[error("schema {0} not supported")]
    Schema(u32),
    #[error("manifest is for channel {got}, expected {want}")]
    Channel { got: String, want: String },
    #[error("manifest expired")]
    Expired,
    #[error("manifest platform {0:?} not listed")]
    Platform(Platform),
    #[error("needs loader ABI {need}, this loader is {have}")]
    LoaderAbi { need: u32, have: u32 },
    #[error("file {0}: hash or size mismatch")]
    FileMismatch(String),
    #[error("unsafe path {0}")]
    UnsafePath(String),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Platform {
    Pc,
    Quest,
}

impl Platform {
    pub const fn current() -> Self {
        if cfg!(target_os = "android") { Platform::Quest } else { Platform::Pc }
    }
}

/// How a file takes effect once staged.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Apply {
    /// Picked up live (CDN-style asset served on next resource read).
    Live,
    /// Needs a game relaunch (code, packages, manifests).
    Relaunch,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct FileEntry {
    /// Path relative to the loose folder, forward slashes.
    pub path: String,
    pub url: String,
    pub sha256: String,
    pub size: u64,
    pub apply: Apply,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PlatformBuild {
    pub platform: Platform,
    pub loader_abi: u32,
    pub files: Vec<FileEntry>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Changelog {
    pub title: String,
    /// Plain text lines, rendered as bullets in the in-game panel.
    pub notes: Vec<String>,
    /// Image entries (also hash-checked) shown in the panel.
    #[serde(default)]
    pub images: Vec<FileEntry>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Manifest {
    pub schema: u32,
    pub version: Version,
    pub channel: String,
    /// Git commit the release was built from.
    pub commit: String,
    /// Unix seconds; the manifest is refused after this.
    pub expires: u64,
    pub changelog: Changelog,
    pub builds: Vec<PlatformBuild>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SignedEnvelope {
    pub manifest: String,
    pub sigs: Vec<Sig>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Sig {
    pub key: String,
    pub sig: String,
}

pub struct PinnedKey {
    pub label: &'static str,
    pub key: [u8; 32],
}

/// Semver-ish `major.minor.patch`, serialised as a string.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct Version(pub u32, pub u32, pub u32);

impl Version {
    pub fn parse(s: &str) -> Option<Self> {
        let s = s.strip_prefix('v').unwrap_or(s);
        let mut it = s.split('.').map(|p| p.parse::<u32>().ok());
        let v = Version(it.next()??, it.next()??, it.next()??);
        it.next().is_none().then_some(v)
    }
}
impl std::fmt::Display for Version {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}.{}.{}", self.0, self.1, self.2)
    }
}
impl Ord for Version {
    fn cmp(&self, o: &Self) -> Ordering {
        (self.0, self.1, self.2).cmp(&(o.0, o.1, o.2))
    }
}
impl PartialOrd for Version {
    fn partial_cmp(&self, o: &Self) -> Option<Ordering> {
        Some(self.cmp(o))
    }
}
impl Serialize for Version {
    fn serialize<S: serde::Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        s.serialize_str(&self.to_string())
    }
}
impl<'de> Deserialize<'de> for Version {
    fn deserialize<D: serde::Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        let s = String::deserialize(d)?;
        Version::parse(&s).ok_or_else(|| serde::de::Error::custom("bad version"))
    }
}

/// What the client checks against.
pub struct Policy<'a> {
    pub keys: &'a [PinnedKey],
    /// Distinct pinned keys that must sign (1 for releases, 2 for key rotation).
    pub threshold: usize,
    pub channel: &'a str,
    pub platform: Platform,
    pub loader_abi: u32,
    pub now: u64,
}

/// Verify an envelope and return the manifest plus this platform's build.
pub fn verify(envelope_json: &[u8], p: &Policy) -> Result<(Manifest, PlatformBuild), Error> {
    let env: SignedEnvelope =
        serde_json::from_slice(envelope_json).map_err(|e| Error::Malformed(e.to_string()))?;

    let mut ok: Vec<&str> = Vec::new();
    for s in &env.sigs {
        let Some(pk) = p.keys.iter().find(|k| k.label == s.key) else { continue };
        if ok.contains(&pk.label) {
            continue;
        }
        let Ok(vk) = VerifyingKey::from_bytes(&pk.key) else { continue };
        let Ok(raw) = B64.decode(&s.sig) else { continue };
        let Ok(sig) = Signature::from_slice(&raw) else { continue };
        if vk.verify(env.manifest.as_bytes(), &sig).is_ok() {
            ok.push(pk.label);
        }
    }
    if ok.len() < p.threshold.max(1) {
        return Err(Error::BadSignature);
    }

    let m: Manifest =
        serde_json::from_str(&env.manifest).map_err(|e| Error::Malformed(e.to_string()))?;
    if m.schema != SCHEMA {
        return Err(Error::Schema(m.schema));
    }
    if m.channel != p.channel {
        return Err(Error::Channel { got: m.channel, want: p.channel.into() });
    }
    if m.expires <= p.now {
        return Err(Error::Expired);
    }
    let b = m
        .builds
        .iter()
        .find(|b| b.platform == p.platform)
        .cloned()
        .ok_or(Error::Platform(p.platform))?;
    if b.loader_abi != p.loader_abi {
        return Err(Error::LoaderAbi { need: b.loader_abi, have: p.loader_abi });
    }
    for f in b.files.iter().chain(&m.changelog.images) {
        check_path(&f.path)?;
    }
    Ok((m, b))
}

/// Refuse absolute paths, `..`, backslashes and drive letters so a manifest can only write inside the loose folder.
pub fn check_path(p: &str) -> Result<(), Error> {
    let bad = p.is_empty()
        || p.starts_with('/')
        || p.contains('\\')
        || p.contains(':')
        || p.split('/').any(|c| c.is_empty() || c == "." || c == "..");
    if bad { Err(Error::UnsafePath(p.into())) } else { Ok(()) }
}

pub fn check_file(f: &FileEntry, bytes: &[u8]) -> Result<(), Error> {
    let h = hex(&Sha256::digest(bytes));
    if bytes.len() as u64 != f.size || !h.eq_ignore_ascii_case(&f.sha256) {
        return Err(Error::FileMismatch(f.path.clone()));
    }
    Ok(())
}

pub fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

#[cfg(test)]
mod tests;
