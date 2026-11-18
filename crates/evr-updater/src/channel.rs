//! Which update channel this player gets, asked from Atlas at boot.
//!
//! Every player is on `stable` (releases built from the GitHub branch `Main`). Players a global operator added with
//! Atlas's Discord command `/beta` are on `beta` (releases built from `DevTest`). The loader asks Atlas with the
//! game's own login from `_local\config.json` (the same `discordid` + `password` the game logs in with):
//!
//! 1. `POST <apiservice_host>/evr/runtime/token {"discord_id","password"}` -> `access_token`
//! 2. `GET <apiservice_host>/evr/runtime/channel` (Bearer) -> `{"channel":"stable"}` or `{"channel":"beta","feed":"<url>"}`
//!
//! Any failure (no config, Atlas down, old Atlas without the route) means `stable`. The manifest's own `channel` is
//! still checked against the channel chosen here, so a beta manifest is never taken on stable and the other way round.

use std::time::Duration;

/// The game's Atlas login, read from `_local\config.json`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GameLogin {
    /// `apiservice_host` without the trailing `/`.
    pub base: String,
    pub discord_id: String,
    pub password: String,
}

/// What Atlas answered.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Channel {
    Stable,
    Beta { feed: String },
}

fn percent_decode(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        match b[i] {
            b'%' if i + 2 < b.len() => match std::str::from_utf8(&b[i + 1..i + 3]).ok().and_then(|h| u8::from_str_radix(h, 16).ok()) {
                Some(v) => {
                    out.push(v);
                    i += 3;
                    continue;
                }
                None => out.push(b'%'),
            },
            b'+' => out.push(b' '),
            c => out.push(c),
        }
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// The value of `key` in the query string of `url`, decoded; empty when missing.
fn query_value(url: &str, key: &str) -> String {
    let Some((_, q)) = url.split_once('?') else { return String::new() };
    q.split('&')
        .filter_map(|pair| pair.split_once('='))
        .find(|(k, _)| *k == key)
        .map(|(_, v)| percent_decode(v))
        .unwrap_or_default()
}

/// Reads the game's `_local\config.json` text: `apiservice_host`, and `discordid` (or `discord_id`) and `password`
/// from the query of `loginservice_host`. None when any of them is missing.
pub fn parse_game_config(text: &[u8]) -> Option<GameLogin> {
    let j: serde_json::Value = serde_json::from_slice(text).ok()?;
    let base = j.get("apiservice_host")?.as_str()?.trim_end_matches('/').to_string();
    let login = j.get("loginservice_host")?.as_str()?;
    let mut discord_id = query_value(login, "discordid");
    if discord_id.is_empty() {
        discord_id = query_value(login, "discord_id");
    }
    let password = query_value(login, "password");
    if base.is_empty() || discord_id.is_empty() || password.is_empty() {
        return None;
    }
    Some(GameLogin { base, discord_id, password })
}

/// Reads Atlas's `/evr/runtime/channel` reply. Beta needs an http(s) feed; anything else is stable.
pub fn parse_channel_reply(body: &[u8]) -> Channel {
    let Ok(j) = serde_json::from_slice::<serde_json::Value>(body) else { return Channel::Stable };
    let feed = j.get("feed").and_then(|v| v.as_str()).unwrap_or("");
    match j.get("channel").and_then(|v| v.as_str()) {
        Some("beta") if feed.starts_with("https://") || feed.starts_with("http://") => {
            Channel::Beta { feed: feed.to_string() }
        }
        _ => Channel::Stable,
    }
}

/// Asks Atlas for this player's channel (two short requests). Err when Atlas could not be asked.
pub fn ask_atlas(login: &GameLogin) -> Result<Channel, String> {
    let agent = ureq::AgentBuilder::new().timeout(Duration::from_secs(8)).user_agent("evr-runtime").build();
    let token: serde_json::Value = agent
        .post(&format!("{}/evr/runtime/token", login.base))
        .send_json(serde_json::json!({"discord_id": login.discord_id, "password": login.password}))
        .map_err(|e| format!("token: {e}"))?
        .into_json()
        .map_err(|e| format!("token reply: {e}"))?;
    let access = token.get("access_token").and_then(|v| v.as_str()).ok_or("token reply has no access_token")?;
    let mut body = Vec::new();
    agent
        .get(&format!("{}/evr/runtime/channel", login.base))
        .set("Authorization", &format!("Bearer {access}"))
        .call()
        .map_err(|e| format!("channel: {e}"))?
        .into_reader()
        .take(64 * 1024)
        .read_to_end(&mut body)
        .map_err(|e| format!("channel reply: {e}"))?;
    Ok(parse_channel_reply(&body))
}
use std::io::Read as _;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_the_game_login() {
        let cfg = br#"{"apiservice_host":"http://127.0.0.1:7350/","loginservice_host":"ws://127.0.0.1:7350/ws?format=evr&discordid=800000000000000001&password=a%20b%2Bc"}"#;
        assert_eq!(
            parse_game_config(cfg),
            Some(GameLogin {
                base: "http://127.0.0.1:7350".into(),
                discord_id: "800000000000000001".into(),
                password: "a b+c".into()
            })
        );
    }

    #[test]
    fn accepts_discord_id_spelling_and_refuses_missing_parts() {
        let ok = br#"{"apiservice_host":"http://a","loginservice_host":"ws://a/ws?discord_id=1&password=p"}"#;
        assert_eq!(parse_game_config(ok).unwrap().discord_id, "1");
        for bad in [
            &br#"{"loginservice_host":"ws://a/ws?discordid=1&password=p"}"#[..],
            br#"{"apiservice_host":"http://a","loginservice_host":"ws://a/ws?discordid=1"}"#,
            br#"{"apiservice_host":"http://a","loginservice_host":"ws://a/ws"}"#,
            b"not json",
        ] {
            assert_eq!(parse_game_config(bad), None);
        }
    }

    #[test]
    fn channel_reply() {
        assert_eq!(
            parse_channel_reply(br#"{"channel":"beta","feed":"https://x/devtest/manifest.signed.json"}"#),
            Channel::Beta { feed: "https://x/devtest/manifest.signed.json".into() }
        );
        assert_eq!(parse_channel_reply(br#"{"channel":"stable"}"#), Channel::Stable);
        assert_eq!(parse_channel_reply(br#"{"channel":"beta"}"#), Channel::Stable, "beta without a feed");
        assert_eq!(parse_channel_reply(br#"{"channel":"beta","feed":"file:///c:/x"}"#), Channel::Stable);
        assert_eq!(parse_channel_reply(b"<html>"), Channel::Stable);
    }
}
