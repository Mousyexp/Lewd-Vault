// Google Drive without the Drive app: Settings → Storage → "Google Drive (sign in)".
//
// Google Drive for desktop needs an administrator to install, so this syncs the vault with Drive
// straight over the web instead. You sign in once in your web browser (Google's own page; the app
// only ever sees a token that can reach the files it made itself, the drive.file scope), and the
// vault folder on this PC is kept in step with a "Lewd Vault" folder in your Drive: added, changed
// and deleted files go both ways, every few minutes and on "Sync now". The vault itself still lives
// on this PC (any place from the welcome screen), so it stays fast and works offline; the
// Google Drive for desktop option is unchanged and needs none of this.
//
// Signing in needs a Google "OAuth client" (the key): release builds can carry one
// (LEWD_VAULT_GOOGLE_CLIENT_ID / _SECRET at build time), or you paste your own (Client ID and
// Client secret of a "Desktop app" client from console.cloud.google.com; docs/GUIDE.md shows how).
// Use the same key on every PC: Drive only shows a key the files made with that key.
//
// What's kept on this PC (never in the vault, so nothing secret syncs): %LOCALAPPDATA%\Image Vault\
// drive\<vault>\account.json (the key and the sign-in) and manifest.json (each file as it was at the
// last sync, which is how a change is told apart from a deletion).
//
// Safety: a file is only ever deleted on one side when the other side deleted it and it hadn't
// changed since the last sync (into the Recycle Bin here, into Drive's trash there, both restorable
// for 30 days), never on the first sync, and a sync that would delete more than half the files on
// one side stops instead. When both sides changed a file, this PC's copy wins and Drive's is saved
// next to it as "<name> (from Drive)<ext>"; for index.csv and views.csv that is a conflict copy the
// app folds in by itself (main.rs merge_conflicts).

use super::{err, storage, Res, Vault};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::collections::{BTreeSet, HashMap};
use std::fs;
use std::io::{Read, Write};
use std::net::TcpListener;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const SCOPE: &str = "https://www.googleapis.com/auth/drive.file";
const AUTH_URL: &str = "https://accounts.google.com/o/oauth2/v2/auth";
const TOKEN_URL: &str = "https://oauth2.googleapis.com/token";
const REVOKE_URL: &str = "https://oauth2.googleapis.com/revoke";
const ABOUT_URL: &str = "https://www.googleapis.com/drive/v3/about";
const FILES: &str = "https://www.googleapis.com/drive/v3/files";
const UPLOAD: &str = "https://www.googleapis.com/upload/drive/v3/files";
const FOLDER_MIME: &str = "application/vnd.google-apps.folder";
const ROOT_NAME: &str = "Lewd Vault";
const CHUNK: usize = 8 * 1024 * 1024; // (a multiple of 256 KB, as resumable uploads want)
const EVERY: Duration = Duration::from_secs(10 * 60); // automatic sync
const CONFLICT: &str = " (from Drive)";

const BUILT_IN_ID: Option<&str> = option_env!("LEWD_VAULT_GOOGLE_CLIENT_ID");
const BUILT_IN_SECRET: Option<&str> = option_env!("LEWD_VAULT_GOOGLE_CLIENT_SECRET");

fn yes() -> bool {
    true
}

#[derive(Serialize, Deserialize, Default, Clone)]
struct Account {
    #[serde(default)]
    client_id: String, // your own key ("" = the built-in one)
    #[serde(default)]
    client_secret: String,
    #[serde(default)]
    refresh_token: String,
    #[serde(default)]
    email: String,
    #[serde(default)]
    root_id: String,
    #[serde(default = "yes")]
    auto: bool,
    #[serde(default)]
    last_sync: String,
}

#[derive(Serialize, Deserialize, Default)]
struct Manifest {
    #[serde(default)]
    root_id: String,
    #[serde(default)]
    files: HashMap<String, Synced>,
}

#[derive(Serialize, Deserialize, Clone)]
struct Synced {
    id: String,
    size: u64,
    mtime: u64,
    md5: String,
}

// ---------- what the page sees ----------

fn status_cell() -> &'static Mutex<Value> {
    static S: OnceLock<Mutex<Value>> = OnceLock::new();
    S.get_or_init(|| Mutex::new(json!({ "state": "idle" })))
}
static BUSY: AtomicBool = AtomicBool::new(false);
static CANCEL: AtomicBool = AtomicBool::new(false);
fn token_cell() -> &'static Mutex<Option<(String, Instant)>> {
    static T: OnceLock<Mutex<Option<(String, Instant)>>> = OnceLock::new();
    T.get_or_init(|| Mutex::new(None))
}
fn set_status(v: Value) {
    *status_cell().lock().unwrap() = v;
}
fn bump(key: &str) {
    let mut s = status_cell().lock().unwrap();
    s[key] = json!(s[key].as_u64().unwrap_or(0) + 1);
}

fn dir(vault: &Vault) -> PathBuf {
    storage::local_dir(&vault.root, "drive")
}
fn load<T: for<'a> Deserialize<'a> + Default>(vault: &Vault, name: &str) -> T {
    fs::read_to_string(dir(vault).join(name)).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default()
}
fn save<T: Serialize>(vault: &Vault, name: &str, v: &T) -> Res<()> {
    let d = dir(vault);
    fs::create_dir_all(&d).map_err(err)?;
    let tmp = d.join(format!("{name}.part"));
    fs::write(&tmp, serde_json::to_vec(v).map_err(err)?).map_err(err)?;
    fs::rename(&tmp, d.join(name)).map_err(err)
}

fn client(a: &Account) -> Option<(String, String)> {
    if !a.client_id.trim().is_empty() {
        return Some((a.client_id.trim().into(), a.client_secret.trim().into()));
    }
    let id = BUILT_IN_ID.unwrap_or("").trim();
    (!id.is_empty()).then(|| (id.to_string(), BUILT_IN_SECRET.unwrap_or("").trim().to_string()))
}

pub fn status(vault: &Vault) -> Value {
    let a: Account = load(vault, "account.json");
    let mut s = status_cell().lock().unwrap().clone();
    s["hasKey"] = json!(client(&a).is_some());
    s["ownKey"] = json!(!a.client_id.trim().is_empty());
    s["builtIn"] = json!(BUILT_IN_ID.is_some_and(|k| !k.trim().is_empty()));
    s["clientId"] = json!(a.client_id);
    s["connected"] = json!(!a.refresh_token.is_empty());
    s["email"] = json!(a.email);
    s["auto"] = json!(a.auto);
    s["lastSync"] = json!(a.last_sync);
    s["folderUrl"] = json!(if a.root_id.is_empty() { String::new() } else { format!("https://drive.google.com/drive/folders/{}", a.root_id) });
    s
}

/// "Use your own key": a Desktop-app OAuth client's ID and secret (empty: back to the built-in one).
pub fn set_key(vault: &Vault, id: &str, secret: &str) -> Res<Value> {
    let id = id.trim();
    if !id.is_empty() && !id.ends_with(".apps.googleusercontent.com") {
        return Err("That's not a Client ID: it ends in .apps.googleusercontent.com".into());
    }
    let mut a: Account = load(vault, "account.json");
    if a.client_id != id {
        // another key can't see the files of the old one: start over with it
        a.refresh_token.clear();
        a.email.clear();
        a.root_id.clear();
        let _ = fs::remove_file(dir(vault).join("manifest.json"));
        *token_cell().lock().unwrap() = None;
    }
    a.client_id = id.to_string();
    a.client_secret = secret.trim().to_string();
    save(vault, "account.json", &a)?;
    Ok(status(vault))
}

pub fn set_auto(vault: &Vault, on: bool) -> Res<Value> {
    let mut a: Account = load(vault, "account.json");
    a.auto = on;
    save(vault, "account.json", &a)?;
    Ok(status(vault))
}

pub fn sign_out(vault: &Vault) -> Res<Value> {
    let mut a: Account = load(vault, "account.json");
    if !a.refresh_token.is_empty() {
        let _ = agent().post(REVOKE_URL).send_form(&[("token", a.refresh_token.as_str())]);
    }
    a.refresh_token.clear();
    a.email.clear();
    a.root_id.clear();
    a.last_sync.clear();
    save(vault, "account.json", &a)?;
    let _ = fs::remove_file(dir(vault).join("manifest.json"));
    *token_cell().lock().unwrap() = None;
    set_status(json!({ "state": "idle" }));
    Ok(status(vault))
}

pub fn cancel() -> Value {
    CANCEL.store(true, Ordering::Relaxed);
    json!(true)
}

/// Sign in, in the web browser; the page polls status() meanwhile (it shows "authUrl" in case the
/// browser didn't open).
pub fn sign_in(vault: Arc<Vault>) -> Res<Value> {
    let a: Account = load(&vault, "account.json");
    if client(&a).is_none() {
        return Err("Add a Google key first (Client ID and secret).".into());
    }
    if BUSY.swap(true, Ordering::SeqCst) {
        return Err("Google Drive is busy; try again in a moment.".into());
    }
    CANCEL.store(false, Ordering::Relaxed);
    set_status(json!({ "state": "signing-in" }));
    std::thread::spawn(move || {
        let r = sign_in_flow(&vault);
        BUSY.store(false, Ordering::SeqCst);
        match r {
            Ok(()) => {
                set_status(json!({ "state": "idle", "message": "Signed in" }));
                let _ = sync_start(vault); // the first sync right away
            }
            Err(e) => set_status(json!({ "state": "error", "message": e })),
        }
    });
    Ok(json!(true))
}

/// Starts a sync in the background (nothing when one is running already).
pub fn sync_start(vault: Arc<Vault>) -> Res<Value> {
    let a: Account = load(&vault, "account.json");
    if a.refresh_token.is_empty() {
        return Err("Sign in to Google Drive first.".into());
    }
    if BUSY.swap(true, Ordering::SeqCst) {
        return Ok(json!(false));
    }
    CANCEL.store(false, Ordering::Relaxed);
    set_status(json!({ "state": "syncing", "done": 0, "total": 0, "uploaded": 0, "downloaded": 0, "deletedHere": 0, "deletedThere": 0, "conflicts": 0 }));
    std::thread::spawn(move || {
        let r = sync(&vault);
        BUSY.store(false, Ordering::SeqCst);
        let mut s = status_cell().lock().unwrap();
        match r {
            Ok(()) => s["state"] = json!("done"),
            Err(e) => {
                s["state"] = json!("error");
                s["message"] = json!(e);
            }
        }
        s["finished"] = json!(now_ms());
    });
    Ok(json!(true))
}

/// The automatic sync: shortly after start, then every EVERY, while signed in with it on.
pub fn start(vault: Arc<Vault>) {
    std::thread::spawn(move || {
        std::thread::sleep(Duration::from_secs(20));
        loop {
            let a: Account = load(&vault, "account.json");
            if a.auto && !a.refresh_token.is_empty() && client(&a).is_some() {
                let _ = sync_start(vault.clone());
            }
            std::thread::sleep(EVERY);
        }
    });
}

// ---------- signing in ----------

fn agent() -> ureq::Agent {
    // (no redirects: an unfinished resumable upload answers 308 "Resume Incomplete", which isn't one)
    ureq::AgentBuilder::new().timeout_connect(Duration::from_secs(15)).timeout_read(Duration::from_secs(120)).redirects(0).user_agent("Lewd-Vault").build()
}

fn random_token(bytes: usize) -> String {
    use base64::Engine;
    let mut buf = vec![0u8; bytes];
    if getrandom::getrandom(&mut buf).is_err() {
        // (never expected; the time and process are still unguessable enough for a one-off state)
        let seed = format!("{:?}{}", SystemTime::now(), std::process::id());
        buf = super::md5_hex(seed.as_bytes()).into_bytes();
    }
    base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(buf)
}

fn open_browser(url: &str) -> Res<()> {
    #[cfg(windows)]
    let r = std::process::Command::new("rundll32").args(["url.dll,FileProtocolHandler", url]).spawn();
    #[cfg(target_os = "macos")]
    let r = std::process::Command::new("open").arg(url).spawn();
    #[cfg(not(any(windows, target_os = "macos")))]
    let r = std::process::Command::new("xdg-open").arg(url).spawn();
    r.map(|_| ()).map_err(err)
}

fn query_of(target: &str) -> HashMap<String, String> {
    let q = target.split_once('?').map(|(_, q)| q).unwrap_or("");
    q.split('&').filter_map(|kv| kv.split_once('='))
        .map(|(k, v)| (k.to_string(), percent_encoding::percent_decode_str(&v.replace('+', " ")).decode_utf8_lossy().into_owned()))
        .collect()
}

fn page(title: &str, text: &str) -> String {
    let body = format!("<!doctype html><meta charset=utf-8><title>Lewd Vault</title><body style=\"font-family:Segoe UI,system-ui,sans-serif;background:#2a2226;color:#f7e8ec;display:grid;place-items:center;height:100vh;margin:0\"><div style=\"text-align:center\"><h1 style=\"font-weight:600\">{title}</h1><p>{text}</p></div>");
    format!("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}", body.len())
}

fn sign_in_flow(vault: &Vault) -> Res<()> {
    use base64::Engine;
    use sha2::Digest;
    let mut a: Account = load(vault, "account.json");
    let (id, secret) = client(&a).ok_or("No Google key")?;
    let listener = TcpListener::bind("127.0.0.1:0").map_err(err)?;
    let redirect = format!("http://127.0.0.1:{}", listener.local_addr().map_err(err)?.port());
    let verifier = random_token(48);
    let challenge = base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(sha2::Sha256::digest(verifier.as_bytes()));
    let state = random_token(16);
    let enc = |s: &str| percent_encoding::utf8_percent_encode(s, percent_encoding::NON_ALPHANUMERIC).to_string();
    let url = format!("{AUTH_URL}?client_id={}&redirect_uri={}&response_type=code&scope={}&code_challenge={challenge}&code_challenge_method=S256&state={state}&access_type=offline&prompt=consent",
        enc(&id), enc(&redirect), enc(SCOPE));
    status_cell().lock().unwrap()["authUrl"] = json!(url);
    open_browser(&url)?;

    // Google sends the browser back to us with ?code=… (or ?error=…)
    listener.set_nonblocking(true).map_err(err)?;
    let deadline = Instant::now() + Duration::from_secs(300);
    let code = loop {
        if CANCEL.load(Ordering::Relaxed) {
            return Err("Sign-in cancelled".into());
        }
        if Instant::now() > deadline {
            return Err("Sign-in timed out (5 minutes). Try again.".into());
        }
        let (mut conn, _) = match listener.accept() {
            Ok(c) => c,
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => { std::thread::sleep(Duration::from_millis(200)); continue; }
            Err(e) => return Err(err(e)),
        };
        let _ = conn.set_nonblocking(false);
        let _ = conn.set_read_timeout(Some(Duration::from_secs(5)));
        let mut buf = [0u8; 8192];
        let n = conn.read(&mut buf).unwrap_or(0);
        let head = String::from_utf8_lossy(&buf[..n]);
        let target = head.split_whitespace().nth(1).unwrap_or("").to_string();
        let q = query_of(&target);
        if q.get("state").map(String::as_str) != Some(state.as_str()) {
            let _ = conn.write_all(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            continue; // (the favicon, or something that isn't Google's answer)
        }
        if let Some(e) = q.get("error") {
            let _ = conn.write_all(page("Not connected", "You can close this tab and go back to Lewd Vault.").as_bytes());
            return Err(if e == "access_denied" { "Sign-in cancelled in the browser".into() } else { format!("Google said: {e}") });
        }
        let Some(code) = q.get("code").cloned() else { continue };
        let _ = conn.write_all(page("Lewd Vault is connected 💖", "Google Drive sync is on. You can close this tab and go back to the app.").as_bytes());
        break code;
    };

    let mut form = vec![("code", code.as_str()), ("client_id", id.as_str()), ("redirect_uri", redirect.as_str()),
                        ("grant_type", "authorization_code"), ("code_verifier", verifier.as_str())];
    if !secret.is_empty() {
        form.push(("client_secret", secret.as_str()));
    }
    let tok: Value = match agent().post(TOKEN_URL).send_form(&form) {
        Ok(r) => r.into_string().map_err(err).and_then(|t| serde_json::from_str::<Value>(&t).map_err(err))?,
        Err(ureq::Error::Status(_, r)) => {
            let v: Value = r.into_string().ok().and_then(|t| serde_json::from_str::<Value>(&t).ok()).unwrap_or_default();
            return Err(format!("Google refused the sign-in: {}", v["error_description"].as_str().or(v["error"].as_str()).unwrap_or("unknown error")));
        }
        Err(e) => return Err(format!("Could not reach Google: {e}")),
    };
    let refresh = tok["refresh_token"].as_str().ok_or("Google sent no lasting sign-in (refresh token); try again")?;
    let access = tok["access_token"].as_str().unwrap_or("").to_string();
    let life = tok["expires_in"].as_u64().unwrap_or(3600);
    *token_cell().lock().unwrap() = Some((access.clone(), Instant::now() + Duration::from_secs(life.saturating_sub(120))));
    let about: Value = agent().get(ABOUT_URL).query("fields", "user(emailAddress)").set("Authorization", &format!("Bearer {access}"))
        .call().ok().and_then(|r| r.into_string().ok()).and_then(|t| serde_json::from_str::<Value>(&t).ok()).unwrap_or_default();
    a.refresh_token = refresh.to_string();
    a.email = about["user"]["emailAddress"].as_str().unwrap_or("").to_string();
    save(vault, "account.json", &a)
}

fn access_token(vault: &Vault) -> Res<String> {
    if let Some((t, until)) = token_cell().lock().unwrap().clone() {
        if Instant::now() < until {
            return Ok(t);
        }
    }
    let mut a: Account = load(vault, "account.json");
    let (id, secret) = client(&a).ok_or("No Google key")?;
    if a.refresh_token.is_empty() {
        return Err("Sign in to Google Drive first.".into());
    }
    let mut form = vec![("client_id", id.as_str()), ("refresh_token", a.refresh_token.as_str()), ("grant_type", "refresh_token")];
    if !secret.is_empty() {
        form.push(("client_secret", secret.as_str()));
    }
    let tok: Value = match agent().post(TOKEN_URL).send_form(&form) {
        Ok(r) => r.into_string().map_err(err).and_then(|t| serde_json::from_str::<Value>(&t).map_err(err))?,
        Err(ureq::Error::Status(400 | 401, r)) => {
            let v: Value = r.into_string().ok().and_then(|t| serde_json::from_str::<Value>(&t).ok()).unwrap_or_default();
            if v["error"] == "invalid_grant" {
                // revoked, expired (a key in "Testing" mode signs out after 7 days) or the password changed
                a.refresh_token.clear();
                let _ = save(vault, "account.json", &a);
                return Err("Google signed you out. Sign in again to keep syncing.".into());
            }
            return Err(format!("Google refused the sign-in: {}", v["error_description"].as_str().or(v["error"].as_str()).unwrap_or("unknown error")));
        }
        Err(e) => return Err(format!("Could not reach Google: {e}")),
    };
    let t = tok["access_token"].as_str().ok_or("Google sent no access token")?.to_string();
    let life = tok["expires_in"].as_u64().unwrap_or(3600);
    *token_cell().lock().unwrap() = Some((t.clone(), Instant::now() + Duration::from_secs(life.saturating_sub(120))));
    Ok(t)
}

// ---------- talking to Drive ----------

/// One Drive request, with the token, and again after a pause when Drive is busy (429, 5xx, rate
/// limits), the network dropped, or the token ran out (401).
fn call(vault: &Vault, make: &dyn Fn(&str) -> Result<ureq::Response, ureq::Error>) -> Res<ureq::Response> {
    let mut wait = 1u64;
    let mut refreshed = false;
    for attempt in 0..6 {
        let token = access_token(vault)?;
        let last = attempt == 5;
        match make(&token) {
            Ok(r) => return Ok(r),
            Err(ureq::Error::Status(401, _)) if !refreshed => {
                *token_cell().lock().unwrap() = None;
                refreshed = true;
                continue;
            }
            Err(ureq::Error::Status(code, r)) => {
                let body = r.into_string().unwrap_or_default();
                let busy = code == 429 || code >= 500 || (code == 403 && body.contains("ateLimitExceeded"));
                if !busy || last {
                    let v: Value = serde_json::from_str(&body).unwrap_or_default();
                    let msg = v["error"]["message"].as_str().unwrap_or("").to_string();
                    if code == 403 && body.contains("storageQuotaExceeded") {
                        return Err("Your Google Drive is full.".into());
                    }
                    return Err(format!("Google Drive error {code}{}", if msg.is_empty() { String::new() } else { format!(": {msg}") }));
                }
            }
            Err(e) if last => return Err(format!("Could not reach Google Drive: {e}")),
            Err(_) => {}
        }
        if CANCEL.load(Ordering::Relaxed) {
            return Err("Sync stopped".into());
        }
        std::thread::sleep(Duration::from_secs(wait));
        wait *= 2;
    }
    Err("Google Drive didn't answer".into())
}

fn bearer(t: &str) -> String {
    format!("Bearer {t}")
}

#[derive(Clone)]
struct Remote {
    id: String,
    md5: String,
    size: u64,
}

fn drive_q(s: &str) -> String {
    s.replace('\\', "\\\\").replace('\'', "\\'")
}

/// The app's own "Lewd Vault" folder in My Drive (made the first time).
fn ensure_root(vault: &Vault, a: &mut Account) -> Res<String> {
    if !a.root_id.is_empty() {
        let id = a.root_id.clone();
        let r = call(vault, &|t| agent().get(&format!("{FILES}/{id}")).query("fields", "id,trashed").set("Authorization", &bearer(t)).call());
        match r {
            Ok(r) => {
                let v: Value = r.into_string().map_err(err).and_then(|t| serde_json::from_str::<Value>(&t).map_err(err))?;
                if v["trashed"] != json!(true) {
                    return Ok(id);
                }
            }
            Err(e) if e.contains("error 404") => {}
            Err(e) => return Err(e),
        }
    }
    let q = format!("name = '{}' and mimeType = '{FOLDER_MIME}' and 'root' in parents and trashed = false", drive_q(ROOT_NAME));
    let found: Value = call(vault, &|t| agent().get(FILES).query("q", &q).query("fields", "files(id)").query("orderBy", "createdTime")
        .set("Authorization", &bearer(t)).call())?.into_string().map_err(err).and_then(|t| serde_json::from_str::<Value>(&t).map_err(err))?;
    let id = match found["files"][0]["id"].as_str() {
        Some(id) => id.to_string(),
        None => make_folder(vault, ROOT_NAME, "root")?,
    };
    a.root_id = id.clone();
    save(vault, "account.json", a)?;
    Ok(id)
}

fn make_folder(vault: &Vault, name: &str, parent: &str) -> Res<String> {
    let meta = json!({ "name": name, "mimeType": FOLDER_MIME, "parents": [parent] });
    let v: Value = call(vault, &|t| agent().post(FILES).query("fields", "id").set("Authorization", &bearer(t)).set("Content-Type", "application/json").send_string(&meta.clone().to_string()))?
        .into_string().map_err(err).and_then(|t| serde_json::from_str::<Value>(&t).map_err(err))?;
    v["id"].as_str().map(str::to_string).ok_or_else(|| "Drive made no folder".into())
}

/// Everything under the root folder, by path ("Media/000123_x.webp"), and the folders ("Media").
fn list_remote(vault: &Vault, root: &str) -> Res<(HashMap<String, Remote>, HashMap<String, String>)> {
    struct Node { name: String, parent: String, folder: bool, md5: String, size: u64 }
    let mut nodes: HashMap<String, Node> = HashMap::new();
    let mut page_token = String::new();
    loop {
        let pt = page_token.clone();
        let v: Value = call(vault, &|t| {
            let mut r = agent().get(FILES).query("q", "trashed = false").query("pageSize", "1000").query("spaces", "drive")
                .query("fields", "nextPageToken,files(id,name,mimeType,parents,md5Checksum,size)").set("Authorization", &bearer(t));
            if !pt.is_empty() {
                r = r.query("pageToken", &pt);
            }
            r.call()
        })?.into_string().map_err(err).and_then(|t| serde_json::from_str::<Value>(&t).map_err(err))?;
        for f in v["files"].as_array().into_iter().flatten() {
            let id = f["id"].as_str().unwrap_or("").to_string();
            nodes.insert(id, Node {
                name: f["name"].as_str().unwrap_or("").to_string(),
                parent: f["parents"][0].as_str().unwrap_or("").to_string(),
                folder: f["mimeType"] == FOLDER_MIME,
                md5: f["md5Checksum"].as_str().unwrap_or("").to_string(),
                size: f["size"].as_str().and_then(|s| s.parse().ok()).unwrap_or(0),
            });
        }
        match v["nextPageToken"].as_str() {
            Some(t) => page_token = t.to_string(),
            None => break,
        }
    }
    // each node's path below the root (None: somewhere else in Drive)
    let mut memo: HashMap<String, Option<String>> = HashMap::new();
    fn path_of(id: &str, root: &str, nodes: &HashMap<String, Node>, memo: &mut HashMap<String, Option<String>>, depth: usize) -> Option<String> {
        if id == root {
            return Some(String::new());
        }
        if let Some(p) = memo.get(id) {
            return p.clone();
        }
        let n = nodes.get(id)?;
        let p = if depth > 64 { None } else {
            path_of(&n.parent, root, nodes, memo, depth + 1).map(|up| if up.is_empty() { n.name.clone() } else { format!("{up}/{}", n.name) })
        };
        memo.insert(id.to_string(), p.clone());
        p
    }
    let (mut files, mut folders) = (HashMap::new(), HashMap::new());
    let mut ids: Vec<&String> = nodes.keys().collect();
    ids.sort(); // (two files with one path: always the same one wins)
    for id in ids {
        let Some(p) = path_of(id, root, &nodes, &mut memo, 0) else { continue };
        let n = &nodes[id];
        if n.folder {
            folders.entry(p).or_insert_with(|| id.clone());
        } else if !n.md5.is_empty() && ok_path(&p) {
            files.entry(p).or_insert_with(|| Remote { id: id.clone(), md5: n.md5.clone(), size: n.size });
        }
    }
    Ok((files, folders))
}

fn folder_for(vault: &Vault, rel_dir: &str, root: &str, folders: &mut HashMap<String, String>) -> Res<String> {
    if rel_dir.is_empty() {
        return Ok(root.to_string());
    }
    if let Some(id) = folders.get(rel_dir) {
        return Ok(id.clone());
    }
    let (up, name) = rel_dir.rsplit_once('/').unwrap_or(("", rel_dir));
    let parent = folder_for(vault, up, root, folders)?;
    let id = make_folder(vault, name, &parent)?;
    folders.insert(rel_dir.to_string(), id.clone());
    Ok(id)
}

/// Uploads a file (new: into `parent`; else over `existing`) in resumable pieces. Returns its Drive
/// id and md5.
fn upload(vault: &Vault, path: &Path, name: &str, parent: &str, existing: Option<&str>) -> Res<(String, String)> {
    let mut tries = 0;
    loop {
        match upload_once(vault, path, name, parent, existing) {
            Ok(r) => return Ok(r),
            // (a piece that failed even after call()'s retries: once more from the start, twice at most)
            Err(e) if tries < 2 && !CANCEL.load(Ordering::Relaxed) && !e.contains("full") && !e.contains("signed you out") => tries += 1,
            Err(e) => return Err(e),
        }
    }
}

fn upload_once(vault: &Vault, path: &Path, name: &str, parent: &str, existing: Option<&str>) -> Res<(String, String)> {
    let size = fs::metadata(path).map_err(err)?.len();
    let mime = super::mime(path).split(';').next().unwrap_or("application/octet-stream").to_string();
    let session = call(vault, &|t| {
        let req = match existing {
            Some(id) => agent().request("PATCH", &format!("{UPLOAD}/{id}")),
            None => agent().post(UPLOAD),
        };
        let meta = match existing { Some(_) => json!({}), None => json!({ "name": name, "parents": [parent] }) };
        req.query("uploadType", "resumable").query("fields", "id,md5Checksum").set("Authorization", &bearer(t))
            .set("X-Upload-Content-Type", &mime).set("X-Upload-Content-Length", &size.to_string()).set("Content-Type", "application/json").send_string(&meta.to_string())
    })?;
    let at = session.header("Location").ok_or("Drive gave no upload address")?.to_string();
    let mut file = fs::File::open(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let mut sent = 0u64;
    let mut buf = vec![0u8; CHUNK];
    loop {
        if CANCEL.load(Ordering::Relaxed) {
            return Err("Sync stopped".into());
        }
        let mut n = 0;
        while n < CHUNK {
            let k = file.read(&mut buf[n..]).map_err(err)?;
            if k == 0 {
                break;
            }
            n += k;
        }
        let range = if size == 0 { "bytes */0".to_string() } else { format!("bytes {}-{}/{size}", sent, sent + n as u64 - 1) };
        let body = &buf[..n];
        let r = call(vault, &|t| agent().put(&at).set("Authorization", &bearer(t)).set("Content-Range", &range).send_bytes(body))?;
        sent += n as u64;
        if r.status() == 308 {
            if sent >= size {
                return Err("Drive wanted more of the file than there is (it changed while uploading?)".into());
            }
            continue;
        }
        let v: Value = r.into_string().map_err(err).and_then(|t| serde_json::from_str::<Value>(&t).map_err(err))?;
        return Ok((v["id"].as_str().unwrap_or("").to_string(), v["md5Checksum"].as_str().unwrap_or("").to_string()));
    }
}

fn download(vault: &Vault, id: &str, to: &Path) -> Res<()> {
    let r = call(vault, &|t| agent().get(&format!("{FILES}/{id}")).query("alt", "media").set("Authorization", &bearer(t)).call())?;
    if let Some(d) = to.parent() {
        fs::create_dir_all(d).map_err(err)?;
    }
    let mut out = fs::File::create(to).map_err(|e| format!("{}: {e}", to.display()))?;
    let mut body = r.into_reader();
    let mut buf = vec![0u8; 256 * 1024];
    loop {
        if CANCEL.load(Ordering::Relaxed) {
            drop(out);
            let _ = fs::remove_file(to);
            return Err("Sync stopped".into());
        }
        let n = body.read(&mut buf).map_err(|e| { let _ = fs::remove_file(to); format!("Download cut off: {e}") })?;
        if n == 0 {
            break;
        }
        out.write_all(&buf[..n]).map_err(err)?;
    }
    Ok(())
}

fn trash_remote(vault: &Vault, id: &str) -> Res<()> {
    match call(vault, &|t| agent().request("PATCH", &format!("{FILES}/{id}")).query("fields", "id").set("Authorization", &bearer(t)).set("Content-Type", "application/json").send_string(&json!({ "trashed": true }).to_string())) {
        Ok(_) => Ok(()),
        Err(e) if e.contains("error 404") => Ok(()), // (gone already)
        Err(e) => Err(e),
    }
}

// ---------- the vault folder on this PC ----------

/// What syncs: everything but what's made again anyway or only means something on this PC.
fn ok_path(rel: &str) -> bool {
    !(rel == "_app/cache" || rel.starts_with("_app/cache/") || rel.starts_with("_app/.incoming") || rel.starts_with("_app/backups")
        || rel.ends_with(".part") || rel.contains(CONFLICT) || rel.split('/').any(|p| p.starts_with(".lewd-vault-probe") || p.eq_ignore_ascii_case("desktop.ini")))
}

fn now_ms() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0)
}
fn stat(p: &Path) -> Option<(u64, u64)> {
    let m = fs::metadata(p).ok()?;
    m.is_file().then(|| (m.len(), m.modified().ok().and_then(|t| t.duration_since(UNIX_EPOCH).ok()).map(|d| d.as_millis() as u64).unwrap_or(0)))
}
fn local_files(root: &Path) -> HashMap<String, (u64, u64)> {
    fn walk(dir: &Path, root: &Path, out: &mut HashMap<String, (u64, u64)>) {
        for e in fs::read_dir(dir).into_iter().flatten().flatten() {
            let p = e.path();
            let rel = p.strip_prefix(root).unwrap_or(&p).to_string_lossy().replace('\\', "/");
            if !ok_path(&rel) {
                continue;
            }
            if p.is_dir() {
                walk(&p, root, out);
            } else if let Some(s) = stat(&p) {
                out.insert(rel, s);
            }
        }
    }
    let mut out = HashMap::new();
    walk(root, root, &mut out);
    out
}
fn file_md5(p: &Path) -> String {
    let Ok(mut f) = fs::File::open(p) else { return String::new() };
    let mut ctx = md5::Context::new();
    let mut buf = vec![0u8; 1024 * 1024];
    while let Ok(n) = f.read(&mut buf) {
        if n == 0 {
            break;
        }
        ctx.consume(&buf[..n]);
    }
    format!("{:x}", ctx.compute())
}
fn conflict_path(p: &Path) -> PathBuf {
    let name = p.file_name().unwrap_or_default().to_string_lossy();
    let (stem, ext) = match name.rfind('.') { Some(k) if k > 0 => (&name[..k], &name[k..]), _ => (&name[..], "") };
    p.with_file_name(format!("{stem}{CONFLICT}{ext}"))
}

/// Puts a downloaded file in place. Under the vault's lock, so the app's own writes of index.csv
/// and views.csv never cross it; if the file changed here after the scan, Drive's copy goes next to
/// it as a conflict copy instead. Returns whether it replaced the file.
fn put_in_place(vault: &Vault, tmp: &Path, dest: &Path, seen: Option<(u64, u64)>) -> Res<bool> {
    let _g = vault.lock.lock().unwrap();
    if stat(dest) != seen {
        fs::rename(tmp, conflict_path(dest)).map_err(err)?;
        return Ok(false);
    }
    fs::rename(tmp, dest).map_err(|e| format!("Could not write {} (is it open in another program?): {e}", dest.display()))?;
    Ok(true)
}

// ---------- one sync ----------

enum Act { Up, Down, DelHere, DelThere, Conflict, Same, Forget }

fn sync(vault: &Vault) -> Res<()> {
    let mut a: Account = load(vault, "account.json");
    let root = ensure_root(vault, &mut a)?;
    let mut m: Manifest = load(vault, "manifest.json");
    if m.root_id != root {
        // a new (or another) Drive folder: nothing is known about it, so nothing is deleted this time
        m = Manifest { root_id: root.clone(), files: HashMap::new() };
    }
    let (remote, mut folders) = list_remote(vault, &root)?;
    let local = local_files(&vault.root);

    let paths: BTreeSet<String> = local.keys().chain(remote.keys()).chain(m.files.keys()).cloned().collect();
    let mut plan: Vec<(String, Act)> = Vec::new();
    for p in paths {
        let (l, r, k) = (local.get(&p), remote.get(&p), m.files.get(&p));
        let l_changed = l.is_some_and(|l| k.map_or(true, |k| k.size != l.0 || k.mtime != l.1));
        let r_changed = r.is_some_and(|r| k.map_or(true, |k| k.md5 != r.md5));
        let act = match (l, r, k) {
            (None, None, _) => Act::Forget,
            (Some(_), None, Some(_)) if !l_changed => Act::DelHere,
            (Some(_), None, _) => Act::Up,
            (None, Some(_), Some(_)) if !r_changed => Act::DelThere,
            (None, Some(_), _) => Act::Down,
            (Some(_), Some(_), _) if !l_changed && !r_changed => continue,
            (Some(_), Some(_), _) if l_changed && !r_changed => Act::Up,
            (Some(_), Some(_), _) if r_changed && !l_changed => Act::Down,
            (Some(_), Some(_), _) => Act::Conflict, // (both changed, or the first sync: same content is just noted)
        };
        if matches!(act, Act::Forget) {
            m.files.remove(&p);
            continue;
        }
        plan.push((p, act));
    }
    // a sync that would delete most of one side is a mistake somewhere (a folder emptied, a USB stick
    // with another vault...): stop and say so
    let here = plan.iter().filter(|(_, a)| matches!(a, Act::DelHere)).count();
    let there = plan.iter().filter(|(_, a)| matches!(a, Act::DelThere)).count();
    if here > 20 && here * 2 > local.len() {
        return Err(format!("Stopped: this would delete {here} of the {} files on this PC because they're gone from Drive. If that's right, delete them here too; otherwise sign out and in again to start over.", local.len()));
    }
    if there > 20 && there * 2 > remote.len() {
        return Err(format!("Stopped: this would delete {there} of the {} files in Drive because they're gone from this PC. Is the vault folder empty or on a missing drive?", remote.len()));
    }

    status_cell().lock().unwrap()["total"] = json!(plan.len());
    let mut since_save = 0;
    for (p, act) in plan {
        if CANCEL.load(Ordering::Relaxed) {
            save(vault, "manifest.json", &m)?;
            return Err("Sync stopped".into());
        }
        let path = vault.root.join(&p);
        let parent_dir = p.rsplit_once('/').map(|(d, _)| d).unwrap_or("");
        let name = p.rsplit('/').next().unwrap_or(&p).to_string();
        let r = remote.get(&p);
        let seen = local.get(&p).copied();
        let mut act = act;
        if matches!(act, Act::Conflict) {
            let rr = r.unwrap();
            if seen.map(|s| s.0) == Some(rr.size) && file_md5(&path) == rr.md5 {
                act = Act::Same;
            }
        }
        let step: Res<()> = (|| {
            match act {
                Act::Same => {
                    let rr = r.unwrap();
                    let (size, mtime) = seen.unwrap();
                    m.files.insert(p.clone(), Synced { id: rr.id.clone(), size, mtime, md5: rr.md5.clone() });
                }
                Act::Up => {
                    let (size, mtime) = seen.unwrap();
                    let folder = folder_for(vault, parent_dir, &root, &mut folders)?;
                    let (id, md5) = upload(vault, &path, &name, &folder, r.map(|r| r.id.as_str()))?;
                    m.files.insert(p.clone(), Synced { id, size, mtime, md5 });
                    bump("uploaded");
                }
                Act::Down => {
                    let rr = r.unwrap();
                    let tmp = path.with_file_name(format!("{name}.drive.part"));
                    download(vault, &rr.id, &tmp)?;
                    if put_in_place(vault, &tmp, &path, seen)? {
                        let (size, mtime) = stat(&path).unwrap_or((rr.size, 0));
                        m.files.insert(p.clone(), Synced { id: rr.id.clone(), size, mtime, md5: rr.md5.clone() });
                    } else {
                        bump("conflicts");
                    }
                    bump("downloaded");
                }
                Act::Conflict => {
                    // this PC's copy wins; Drive's is kept beside it
                    let rr = r.unwrap();
                    let tmp = path.with_file_name(format!("{name}.drive.part"));
                    download(vault, &rr.id, &tmp)?;
                    fs::rename(&tmp, conflict_path(&path)).map_err(err)?;
                    let (size, mtime) = seen.unwrap();
                    let (id, md5) = upload(vault, &path, &name, "", Some(&rr.id))?;
                    m.files.insert(p.clone(), Synced { id, size, mtime, md5 });
                    bump("conflicts");
                }
                Act::DelHere => {
                    if trash::delete(&path).is_err() {
                        let _ = fs::remove_file(&path);
                    }
                    m.files.remove(&p);
                    bump("deletedHere");
                }
                Act::DelThere => {
                    trash_remote(vault, &r.unwrap().id)?;
                    m.files.remove(&p);
                    bump("deletedThere");
                }
                Act::Forget => {}
            }
            Ok(())
        })();
        if let Err(e) = step {
            if e.contains("Drive is full") || e.contains("signed you out") || e.contains("stopped") {
                save(vault, "manifest.json", &m)?;
                return Err(e);
            }
            // one file that can't be done (open in another program...) doesn't stop the rest
            let mut s = status_cell().lock().unwrap();
            s["failed"] = json!(s["failed"].as_u64().unwrap_or(0) + 1);
            s["message"] = json!(format!("{p}: {e}"));
        }
        bump("done");
        since_save += 1;
        if since_save >= 25 {
            save(vault, "manifest.json", &m)?;
            since_save = 0;
        }
    }
    save(vault, "manifest.json", &m)?;
    let mut a: Account = load(vault, "account.json");
    a.last_sync = chrono::Local::now().to_rfc3339();
    save(vault, "account.json", &a)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn paths() {
        assert!(ok_path("index.csv") && ok_path("Media/000123_x.webp") && ok_path("_app/thumbs/1.webp"));
        assert!(!ok_path("_app/cache/x") && !ok_path("_app/backups/2026-11-06/index.csv") && !ok_path("index.csv.part"));
        assert!(!ok_path("index (from Drive).csv"));
        assert_eq!(conflict_path(Path::new("v/index.csv")), PathBuf::from("v/index (from Drive).csv"));
        assert!(super::super::is_conflict_copy("index (from drive).csv", "index"));
        let q = query_of("/?state=abc&code=4%2F0Ab+c");
        assert_eq!(q["code"], "4/0Ab c");
    }
}
