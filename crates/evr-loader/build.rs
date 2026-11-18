use std::{env, fs, path::PathBuf};

fn main() {
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());

    // Pinned keys -> generated Rust.
    let keys_txt = manifest_dir.join("../../keys/pinned.txt");
    println!("cargo:rerun-if-changed={}", keys_txt.display());
    // CI release builds (the hid.dll players install) set EVR_RELEASE_BUILD=1: the DEV key is left out, so only
    // manifests signed in the release workflow are accepted. Local builds keep DEV for the dev feed.
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

    // Export forwarders to the real hid.dll (absolute path so we never forward to ourselves).
    if env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        let ex = manifest_dir.join("hid_exports.txt");
        println!("cargo:rerun-if-changed={}", ex.display());
        let mut def = String::from("LIBRARY hid\nEXPORTS\n");
        for n in fs::read_to_string(ex).unwrap().lines().filter(|l| !l.is_empty()) {
            def += &format!("    {n}=C:\\Windows\\System32\\hid.{n}\n");
        }
        // The runtime's interface (src/lib.rs): the UPDATES card and panel drive the update through these.
        for n in ["EvrUpdateStatus", "EvrUpdateStart", "EvrUpdateSkip", "EvrUpdateRelaunch", "EvrUpdateBootOk"] {
            def += &format!("    {n}
");
        }
        let p = out.join("hid.def");
        fs::write(&p, def).unwrap();
        println!("cargo:rustc-cdylib-link-arg=/DEF:{}", p.display());
    }
}
