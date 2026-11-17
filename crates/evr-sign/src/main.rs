//! evr-sign: key generation and signed manifest builder (used by CI and for local testing).
//!
//!   evr-sign keygen <out.key>                 writes a secret key, prints `LABEL PUBKEY_B64` line for keys/pinned.txt
//!   evr-sign build --key <k> --label K1 --version 0.1.0 --channel stable --dir <release dir>
//!                  --base-url <url> --title <t> [--notes <file>] [--days 30] --out <upload dir>
//!
//!   evr-sign cdn --key <k> --label K1 --spec <cdn.json> --serial N [--days 30] --out <cdn root>
//!                  writes <out>/index.signed.json and <out>/files/<sha256>, which Atlas serves as-is from its
//!                  CDN root (EVR_CDN_ROOT) at GET /evr/cdn/index.signed.json and /evr/cdn/files/<sha256>.
//!
//! `cdn.json` (paths relative to the spec's folder; type and name are the game's resource symbols, 16 hex digits):
//!   {"entries":[{"id":"poster_lobby_a","kind":"poster","type":"…","name":"…","file":"posters/a.dds"}]}
//! A texture is two resources of one name (descriptor and DDS payload), so it takes two entries.
//!
//! `--dir` layout: `pc/...` and `quest/...` trees (paths inside become loose-folder paths),
//! `images/...` changelog images. Files under a `live/` subfolder apply live; everything else needs a relaunch.

use base64::{engine::general_purpose::STANDARD as B64, Engine};
use ed25519_dalek::{Signer, SigningKey};
use evr_manifest::*;
use sha2::{Digest, Sha256};
use std::path::Path;
use std::{env, fs, process};

fn die(m: &str) -> ! {
    eprintln!("evr-sign: {m}");
    process::exit(2)
}

fn arg(a: &[String], k: &str) -> Option<String> {
    a.iter().position(|x| x == k).and_then(|i| a.get(i + 1).cloned())
}
fn req(a: &[String], k: &str) -> String {
    arg(a, k).unwrap_or_else(|| die(&format!("missing {k}")))
}

fn walk(root: &Path, dir: &Path, out: &mut Vec<String>) {
    let Ok(rd) = fs::read_dir(dir) else { return };
    let mut es: Vec<_> = rd.flatten().collect();
    es.sort_by_key(|e| e.path());
    for e in es {
        let p = e.path();
        if p.is_dir() {
            walk(root, &p, out)
        } else {
            out.push(p.strip_prefix(root).unwrap().to_string_lossy().replace('\\', "/"))
        }
    }
}

fn entries(dir: &Path, base: &str, prefix: &str) -> Vec<FileEntry> {
    let mut v = Vec::new();
    walk(dir, dir, &mut v);
    v.into_iter()
        .map(|rel| {
            let b = fs::read(dir.join(&rel)).unwrap();
            check_path(&rel).unwrap_or_else(|e| die(&e.to_string()));
            FileEntry {
                url: format!("{base}/{prefix}{}", rel.replace('/', "__")),
                apply: if rel.starts_with("live/") { Apply::Live } else { Apply::Relaunch },
                sha256: hex(&Sha256::digest(&b)),
                size: b.len() as u64,
                path: rel,
            }
        })
        .collect()
}

fn main() {
    let a: Vec<String> = env::args().collect();
    match a.get(1).map(String::as_str) {
        Some("keygen") => {
            let out = a.get(2).unwrap_or_else(|| die("keygen <out.key>"));
            let k = SigningKey::generate(&mut rand::rngs::OsRng);
            fs::write(out, B64.encode(k.to_bytes())).unwrap();
            println!("K1 {}", B64.encode(k.verifying_key().to_bytes()));
        }
        Some("build") => {
            let raw = B64.decode(fs::read_to_string(req(&a, "--key")).unwrap().trim()).unwrap();
            let sk = SigningKey::from_bytes(&raw.try_into().unwrap_or_else(|_| die("bad key")));
            let dir = std::path::PathBuf::from(req(&a, "--dir"));
            let base = req(&a, "--base-url").trim_end_matches('/').to_string();
            let days: u64 = arg(&a, "--days").map_or(30, |d| d.parse().unwrap());
            let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_secs();
            let notes = arg(&a, "--notes")
                .map(|p| fs::read_to_string(p).unwrap().lines().filter(|l| !l.trim().is_empty()).map(|l| l.trim_start_matches(['-', '*', ' ']).to_string()).collect())
                .unwrap_or_default();
            let mut builds = Vec::new();
            for (name, plat) in [("pc", Platform::Pc), ("quest", Platform::Quest)] {
                if dir.join(name).is_dir() {
                    builds.push(PlatformBuild { platform: plat, loader_abi: 1, files: entries(&dir.join(name), &base, &format!("{name}__")) });
                }
            }
            let images = if dir.join("images").is_dir() { entries(&dir.join("images"), &base, "images__") } else { vec![] };
            let m = Manifest {
                schema: SCHEMA,
                version: Version::parse(&req(&a, "--version")).unwrap_or_else(|| die("bad version")),
                channel: req(&a, "--channel"),
                commit: arg(&a, "--commit").unwrap_or_else(|| "local".into()),
                expires: now + days * 86400,
                changelog: Changelog { title: req(&a, "--title"), notes, images },
                builds,
            };
            let s = serde_json::to_string(&m).unwrap();
            let label = arg(&a, "--label").unwrap_or_else(|| "K1".into());
            let env = SignedEnvelope { sigs: vec![Sig { key: label, sig: B64.encode(sk.sign(s.as_bytes()).to_bytes()) }], manifest: s };
            // --out is a directory holding exactly what gets uploaded: flat asset names + the manifest.
            let out = std::path::PathBuf::from(req(&a, "--out"));
            fs::create_dir_all(&out).unwrap();
            for (name, b) in m.builds.iter().map(|b| (if b.platform == Platform::Pc { "pc" } else { "quest" }, &b.files)).chain([("images", &m.changelog.images)]) {
                for f in b {
                    let flat = f.url.rsplit('/').next().unwrap();
                    fs::copy(dir.join(name).join(&f.path), out.join(flat)).unwrap();
                }
            }
            fs::write(out.join("manifest.signed.json"), serde_json::to_vec_pretty(&env).unwrap()).unwrap();
            eprintln!("signed {} ({} builds)", m.version, m.builds.len());
        }
        Some("cdn") => {
            let raw = B64.decode(fs::read_to_string(req(&a, "--key")).unwrap().trim()).unwrap();
            let sk = SigningKey::from_bytes(&raw.try_into().unwrap_or_else(|_| die("bad key")));
            let spec_path = std::path::PathBuf::from(req(&a, "--spec"));
            let spec_dir = spec_path.parent().map(Path::to_path_buf).unwrap_or_default();
            let spec: serde_json::Value = serde_json::from_slice(&fs::read(&spec_path).unwrap()).unwrap_or_else(|e| die(&format!("spec: {e}")));
            let serial: u64 = req(&a, "--serial").parse().unwrap_or_else(|_| die("bad --serial"));
            let days: u64 = arg(&a, "--days").map_or(30, |d| d.parse().unwrap());
            let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_secs();
            let out = std::path::PathBuf::from(req(&a, "--out"));
            fs::create_dir_all(out.join("files")).unwrap();
            let is_sym = |s: &str| s.len() == 16 && s.bytes().all(|c| c.is_ascii_hexdigit() && !c.is_ascii_uppercase());
            let mut entries = Vec::new();
            for e in spec["entries"].as_array().unwrap_or_else(|| die("spec has no entries")) {
                let field = |k: &str| e[k].as_str().unwrap_or_else(|| die(&format!("entry missing {k}"))).to_string();
                let (id, kind, ty, name, file) = (field("id"), field("kind"), field("type"), field("name"), field("file"));
                if kind != "poster" && kind != "cosmetic" { die(&format!("{id}: kind must be poster or cosmetic")) }
                if !is_sym(&ty) || !is_sym(&name) { die(&format!("{id}: type and name are 16 lowercase hex digits")) }
                let b = fs::read(spec_dir.join(&file)).unwrap_or_else(|e| die(&format!("{id}: {file}: {e}")));
                let sha = hex(&Sha256::digest(&b));
                fs::write(out.join("files").join(&sha), &b).unwrap();
                let platform = e["platform"].as_str().unwrap_or("").to_string();
                if !platform.is_empty() && platform != "pc" && platform != "quest" { die(&format!("{id}: platform must be pc or quest")) }
                entries.push(serde_json::json!({"id": id, "kind": kind, "platform": platform, "type": ty, "name": name, "sha256": sha, "size": b.len()}));
            }
            let index = serde_json::json!({"schema": 1, "serial": serial, "expires": now + days * 86400, "entries": entries});
            let s = serde_json::to_string(&index).unwrap();
            let label = arg(&a, "--label").unwrap_or_else(|| "K1".into());
            let env = serde_json::json!({"index": s, "sigs": [{"key": label, "sig": B64.encode(sk.sign(s.as_bytes()).to_bytes())}]});
            fs::write(out.join("index.signed.json"), serde_json::to_vec_pretty(&env).unwrap()).unwrap();
            eprintln!("cdn index serial {serial}: {} entries", entries.len());
        }
        _ => die("usage: evr-sign keygen|build|cdn ..."),
    }
}
