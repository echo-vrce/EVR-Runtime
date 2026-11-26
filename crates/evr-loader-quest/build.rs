use std::{env, fs, path::PathBuf};

// Generates the pinned CDN/release verification keys into OUT_DIR/pinned.rs, exactly as the PC loader does
// (crates/evr-loader/build.rs), so both loaders accept the same signed manifests. CI release builds set
// EVR_RELEASE_BUILD=1 to drop the DEV key; local builds keep it for the dev feed.
fn main() {
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let keys_txt = manifest_dir.join("../../keys/pinned.txt");
    println!("cargo:rerun-if-changed={}", keys_txt.display());
    println!("cargo:rerun-if-env-changed=EVR_RELEASE_BUILD");
    let release = env::var("EVR_RELEASE_BUILD").as_deref() == Ok("1");
    let mut src = String::from("pub static PINNED: &[(&str, &str)] = &[\n");
    let mut pinned = 0;
    for l in fs::read_to_string(&keys_txt).unwrap().lines() {
        let l = l.trim();
        if l.is_empty() || l.starts_with('#') {
            continue;
        }
        let (label, key) = l.split_once(' ').expect("LABEL KEY");
        if release && label == "DEV" {
            continue;
        }
        src += &format!("    ({label:?}, {:?}),\n", key.trim());
        pinned += 1;
    }
    assert!(pinned > 0, "keys/pinned.txt has no key for this build (release builds need a non-DEV key)");
    src += "];\n";
    fs::write(out.join("pinned.rs"), src).unwrap();
}
