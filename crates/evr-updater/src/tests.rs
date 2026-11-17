use super::*;
use evr_manifest::{Changelog, Sig, SignedEnvelope};
use ed25519_dalek::{Signer, SigningKey};
use std::sync::OnceLock;

const BODY: &[u8] = b"runtime-bytes";

fn sk() -> SigningKey {
    SigningKey::from_bytes(&[7; 32])
}
fn keys() -> &'static [PinnedKey] {
    static K: OnceLock<Vec<PinnedKey>> = OnceLock::new();
    K.get_or_init(|| vec![PinnedKey { label: "K1", key: sk().verifying_key().to_bytes() }])
}

fn env(v: Version) -> Vec<u8> {
    use sha2::Digest;
    let m = Manifest {
        schema: 1,
        version: v,
        channel: "stable".into(),
        commit: "c".into(),
        expires: u64::MAX,
        changelog: Changelog { title: "t".into(), notes: vec![], images: vec![] },
        builds: vec![PlatformBuild {
            platform: Platform::current(),
            loader_abi: LOADER_ABI,
            files: vec![FileEntry {
                path: "evr-runtime.dll".into(),
                url: "mem://".into(),
                sha256: evr_manifest::hex(&sha2::Sha256::digest(BODY)),
                size: BODY.len() as u64,
                apply: Apply::Relaunch,
            }],
        }],
    };
    let s = serde_json::to_string(&m).unwrap();
    let sig = base64::Engine::encode(&base64::engine::general_purpose::STANDARD, sk().sign(s.as_bytes()).to_bytes());
    serde_json::to_vec(&SignedEnvelope { manifest: s, sigs: vec![Sig { key: "K1".into(), sig }] }).unwrap()
}

fn up(dir: &Path) -> Updater {
    Updater::new(Config { root: dir.into(), feed: String::new(), channel: "stable".into(), keys: keys() })
}

fn offer(u: &Updater, v: Version) -> Offer {
    match u.decide(env(v), 0).unwrap() {
        Decision::Prompt(o) => o,
        _ => panic!("expected prompt"),
    }
}

#[test]
fn skip_then_menu_button_still_sees_it() {
    let d = tempfile::tempdir().unwrap();
    let u = up(d.path());
    let o = offer(&u, Version(1, 0, 0));
    u.skip(o.manifest.version).unwrap();
    assert!(matches!(u.decide(env(Version(1, 0, 0)), 0).unwrap(), Decision::Skipped(_)));
    // a newer release prompts again
    assert!(matches!(u.decide(env(Version(1, 0, 1)), 0).unwrap(), Decision::Prompt(_)));
}

#[test]
fn stage_boot_and_rollback() {
    let d = tempfile::tempdir().unwrap();
    let u = up(d.path());
    let ok = |_: &FileEntry| Ok(BODY.to_vec());

    u.stage_with(&offer(&u, Version(1, 0, 0)), &mut |_, _| {}, ok).unwrap();
    assert!(matches!(u.decide(env(Version(1, 0, 0)), 0).unwrap(), Decision::UpToDate));
    assert_eq!(u.boot().unwrap().installed, Some(Version(1, 0, 0)));
    u.boot_ok().unwrap();
    assert!(u.active_dir().unwrap().join("evr-runtime.dll").exists());

    assert!(u.boot().is_ok()); // a later quick restart before the menu must not forget 1.0.0 was good
    u.stage_with(&offer(&u, Version(2, 0, 0)), &mut |_, _| {}, ok).unwrap();
    for _ in 0..MAX_BOOT_FAILS {
        assert_eq!(u.boot().unwrap().installed, Some(Version(2, 0, 0)));
    }
    let s = u.boot().unwrap(); // third boot without reaching the menu
    assert_eq!(s.installed, Some(Version(1, 0, 0)));
    assert!(s.bad.contains(&Version(2, 0, 0)));
    assert!(matches!(u.decide(env(Version(2, 0, 0)), 0).unwrap(), Decision::UpToDate));
}

#[test]
fn bad_download_never_lands() {
    let d = tempfile::tempdir().unwrap();
    let u = up(d.path());
    let r = u.stage_with(&offer(&u, Version(1, 0, 0)), &mut |_, _| {}, |_| Ok(b"evil".to_vec()));
    assert!(r.is_err());
    assert!(!d.path().join("versions/1.0.0").exists());
    assert_eq!(u.state().pending, None);
}

#[test]
fn session_reports_offer_progress_and_staged() {
    let d = tempfile::tempdir().unwrap();
    let u = up(d.path());
    let s = Session::new();
    assert_eq!(s.status().state, Phase::Checking);
    s.set_checked(u.decide(env(Version(1, 0, 0)), 0));
    let st = s.status();
    assert_eq!((st.state, st.version.as_str(), st.files, st.relaunch), (Phase::Offer, "1.0.0", 1, true));
    assert!(s.status_json().contains("\"state\":\"offer\""));

    let o = s.begin().unwrap();
    assert!(s.begin().is_none()); // one download at a time
    let r = u.stage_with(&o, &mut |d, t| s.progress(d, t), |_| Ok(BODY.to_vec())).map(|_| ());
    s.finish(r);
    let st = s.status();
    assert_eq!((st.state, st.done, st.total), (Phase::Staged, BODY.len() as u64, BODY.len() as u64));
    assert_eq!(u.state().pending, Some(Version(1, 0, 0)));
}

#[test]
fn session_skip_persists_and_failure_can_retry() {
    let d = tempfile::tempdir().unwrap();
    let u = up(d.path());
    let s = Session::new();
    s.set_checked(u.decide(env(Version(1, 0, 0)), 0));
    let o = s.begin().unwrap();
    s.finish(u.stage_with(&o, &mut |_, _| {}, |_| Ok(b"evil".to_vec())).map(|_| ()));
    assert_eq!(s.status().state, Phase::Failed);
    assert!(s.begin().is_some()); // TRY AGAIN
    s.finish(Err(Error::Net("x".into())));
    s.skip(&u).unwrap();
    let st = s.status();
    assert!(st.skipped && st.state == Phase::Offer && st.error.is_empty());
    assert_eq!(u.state().skipped, Some(Version(1, 0, 0)));
    // next launch: the card still offers it, flagged as skipped
    let s2 = Session::new();
    s2.set_checked(u.decide(env(Version(1, 0, 0)), 0));
    assert!(s2.status().skipped);
}

#[test]
fn session_up_to_date_and_errors_offer_nothing() {
    let s = Session::new();
    s.set_checked(Ok(Decision::UpToDate));
    assert_eq!(s.status().state, Phase::None);
    s.set_checked(Err(Error::Net("offline".into())));
    assert_eq!(s.status().state, Phase::None);
    assert!(s.begin().is_none());
}
