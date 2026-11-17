use super::*;
use ed25519_dalek::{Signer, SigningKey};

fn key(seed: u8) -> SigningKey {
    SigningKey::from_bytes(&[seed; 32])
}

fn manifest(channel: &str, expires: u64, path: &str) -> Manifest {
    let body = b"hello";
    Manifest {
        schema: SCHEMA,
        version: Version(1, 2, 3),
        channel: channel.into(),
        commit: "abc".into(),
        expires,
        changelog: Changelog { title: "t".into(), notes: vec!["n".into()], images: vec![] },
        builds: vec![PlatformBuild {
            platform: Platform::Pc,
            loader_abi: 1,
            files: vec![FileEntry {
                path: path.into(),
                url: "https://x/y".into(),
                sha256: hex(&Sha256::digest(body)),
                size: body.len() as u64,
                apply: Apply::Relaunch,
            }],
        }],
    }
}

fn envelope(m: &Manifest, signers: &[(&str, &SigningKey)]) -> Vec<u8> {
    let s = serde_json::to_string(m).unwrap();
    let sigs = signers
        .iter()
        .map(|(l, k)| Sig { key: (*l).into(), sig: B64.encode(k.sign(s.as_bytes()).to_bytes()) })
        .collect();
    serde_json::to_vec(&SignedEnvelope { manifest: s, sigs }).unwrap()
}

fn pins() -> Vec<PinnedKey> {
    vec![
        PinnedKey { label: "K1", key: key(1).verifying_key().to_bytes() },
        PinnedKey { label: "K2", key: key(2).verifying_key().to_bytes() },
    ]
}

fn policy<'a>(keys: &'a [PinnedKey], threshold: usize) -> Policy<'a> {
    Policy { keys, threshold, channel: "stable", platform: Platform::Pc, loader_abi: 1, now: 100 }
}

#[test]
fn accepts_good() {
    let k = pins();
    let (m, b) = verify(&envelope(&manifest("stable", 200, "runtime/evr.dll"), &[("K1", &key(1))]), &policy(&k, 1)).unwrap();
    assert_eq!(m.version, Version(1, 2, 3));
    check_file(&b.files[0], b"hello").unwrap();
    assert!(check_file(&b.files[0], b"hellO").is_err());
}

#[test]
fn rejects_unpinned_and_tampered() {
    let k = pins();
    let m = manifest("stable", 200, "a");
    assert_eq!(verify(&envelope(&m, &[("K1", &key(9))]), &policy(&k, 1)).unwrap_err(), Error::BadSignature);
    let mut env: SignedEnvelope = serde_json::from_slice(&envelope(&m, &[("K1", &key(1))])).unwrap();
    env.manifest = env.manifest.replace("1.2.3", "9.9.9");
    assert_eq!(verify(&serde_json::to_vec(&env).unwrap(), &policy(&k, 1)).unwrap_err(), Error::BadSignature);
}

#[test]
fn threshold_needs_distinct_keys() {
    let k = pins();
    let m = manifest("stable", 200, "a");
    let same_twice = envelope(&m, &[("K1", &key(1)), ("K1", &key(1))]);
    assert_eq!(verify(&same_twice, &policy(&k, 2)).unwrap_err(), Error::BadSignature);
    verify(&envelope(&m, &[("K1", &key(1)), ("K2", &key(2))]), &policy(&k, 2)).unwrap();
}

#[test]
fn rejects_rules() {
    let k = pins();
    let p = policy(&k, 1);
    let e = |m: Manifest| verify(&envelope(&m, &[("K1", &key(1))]), &p).unwrap_err();
    assert!(matches!(e(manifest("beta", 200, "a")), Error::Channel { .. }));
    assert_eq!(e(manifest("stable", 100, "a")), Error::Expired);
    for bad in ["../x", "/abs", "a\\b", "C:/x", "a//b", "./a"] {
        assert!(matches!(e(manifest("stable", 200, bad)), Error::UnsafePath(_)), "{bad}");
    }
}

#[test]
fn version_order() {
    assert!(Version::parse("v1.10.0").unwrap() > Version::parse("1.9.9").unwrap());
    assert!(Version::parse("1.2").is_none());
    assert!(Version::parse("1.2.3.4").is_none());
}
