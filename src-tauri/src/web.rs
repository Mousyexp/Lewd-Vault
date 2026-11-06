// Discover: the sites you add as sources (boorus, Reddit, Bing images, video sites, ...).
// Lewd Vault never opens their web pages for browsing: the page asks each site's API for posts
// (json / post) and shows the pictures through the vault:// protocol (/web?u=...), fetched here. So
// there are no ads, popups or redirects to block, and the sites only ever see this app.
//
// API calls (json, post) only reach the sites you added (their domain and its subdomains, e.g.
// cdn.donmai.us for danbooru.donmai.us) and a few services the app itself uses (Reddit's login,
// RedGifs' token, SauceNAO and IQDB for "Find the source"). Pictures and videos (file) can come
// from any public web address, because search results and Reddit posts link everywhere; addresses
// on this PC or the local network are refused. Pictures are cached on this PC in
// %LOCALAPPDATA%\Image Vault\web (at most about 1.5 GB; the oldest go first). Sources, keys and
// tokens are kept on this PC too, in %LOCALAPPDATA%\Image Vault\sources.json, never in the
// (possibly cloud-synced) vault.

use super::{err, md5_hex, Res};
use serde_json::{json, Value};
use std::fs;
use std::io::{Read, Write};
use std::path::PathBuf;
use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

const USER_AGENT: &str = "LewdVault/0.2 (personal desktop app)";
const CACHE_MAX: u64 = 1_500_000_000;
const CACHE_KEEP: u64 = 1_000_000_000;
const MAX_BYTES: u64 = 300_000_000; // one picture or video
const STREAM_PIECE: u64 = 4 * 1024 * 1024; // what a video player gets per request while it plays (stream)
const MAX_DOWNLOAD: u64 = 8_000_000_000; // one long video saved into the vault (download_start)

/// A long video being saved: written to %LOCALAPPDATA%\Image Vault\downloads, then read by the page.
#[derive(Default)]
pub struct Download {
    done: AtomicU64,
    total: AtomicU64,
    state: Mutex<String>, // "" while it runs, "done", or the error
}
// services the app talks to on its own (besides your sources): Discover's, Videos' and Audio's
const SERVICES: [&str; 13] = ["reddit.com", "redgifs.com", "saucenao.com", "iqdb.org", "bing.com", "eporner.com", "pornhub.com", "soundgasm.net", "rule34video.com",
    "audiodesires.com", "archive.org", "freesound.org", "hypnotube.com"];
// request headers the page may set
const HEADERS: [&str; 3] = ["authorization", "accept", "content-type"];

pub struct Web {
    domains: Mutex<Vec<String>>, // the sources' domains, e.g. "donmai.us"
    writes: AtomicU64,
    agent: ureq::Agent,
    downloads: Mutex<HashMap<String, Arc<Download>>>,
}

// IPv4 addresses first: some sites (IQDB) publish an IPv6 address that doesn't answer on many
// networks, and a connection would wait on it until Windows gives up
fn agent() -> ureq::Agent {
    use std::net::ToSocketAddrs;
    ureq::AgentBuilder::new()
        .resolver(|addr: &str| {
            let mut v: Vec<std::net::SocketAddr> = addr.to_socket_addrs()?.collect();
            v.sort_by_key(|a| !a.is_ipv4());
            Ok(v)
        })
        .timeout_connect(Duration::from_secs(10))
        .build()
}

// (%LOCALAPPDATA%\Image Vault; in Untraceable mode inside the app's hidden folder, see storage.rs, and
// everything written there is encrypted, see crypt.rs)
fn base_dir() -> PathBuf {
    super::storage::local_base()
}
fn sources_path() -> PathBuf {
    base_dir().join("sources.json")
}

fn host_of(url: &str) -> Option<String> {
    Some(url.split("://").nth(1)?.split(['/', '?', '#']).next()?.rsplit('@').next()?.split(':').next()?.to_lowercase())
}
// "https://api.rule34.xxx/..." -> "rule34.xxx": the last two parts of the host name
fn domain_of(url: &str) -> Option<String> {
    let host = host_of(url)?;
    let parts: Vec<&str> = host.split('.').filter(|p| !p.is_empty()).collect();
    (parts.len() >= 2).then(|| parts[parts.len() - 2..].join("."))
}
// this PC, the local network, and names that only mean something here
fn private_host(host: &str) -> bool {
    if host.is_empty() || host == "localhost" || host.ends_with(".localhost") || host.ends_with(".local") || host.ends_with(".internal") || !host.contains('.') {
        return true;
    }
    if host.starts_with('[') {
        return true; // no IPv6 literals
    }
    if let Ok(ip) = host.parse::<std::net::Ipv4Addr>() {
        return ip.is_private() || ip.is_loopback() || ip.is_link_local() || ip.is_unspecified() || ip.is_broadcast() || ip.octets()[0] == 0 || ip.octets()[0] >= 224;
    }
    false
}

impl Web {
    pub fn new() -> Web {
        let w = Web { domains: Mutex::new(Vec::new()), writes: AtomicU64::new(0), agent: agent(), downloads: Mutex::new(HashMap::new()) };
        // what was left from a download the app didn't finish (it was closed)
        let _ = fs::remove_dir_all(base_dir().join("downloads"));
        if let Some(v) = w.load_sources() {
            w.learn(&v);
        }
        w
    }

    // the domains your sources (Discover) and your own sites (Videos, Audio: "sites") are on
    pub(crate) fn learn(&self, sources: &Value) {
        let mut list = sources["sources"].as_array().cloned().unwrap_or_default();
        list.extend(sources["sites"].as_array().cloned().unwrap_or_default());
        *self.domains.lock().unwrap() = list.iter().filter_map(|s| s["base"].as_str().and_then(domain_of)).collect();
    }

    /// A site you're making in the site builder (sites.js), before it's saved: its address may be read
    /// until the sources are next saved.
    pub fn allow(&self, url: &str) -> Res<Value> {
        let d = Self::checked(url)?;
        let mut list = self.domains.lock().unwrap();
        if !list.contains(&d) {
            list.push(d);
        }
        Ok(Value::Bool(true))
    }

    pub fn load_sources(&self) -> Option<Value> {
        super::crypt::read_to_string(&sources_path()).ok().and_then(|t| serde_json::from_str(&t).ok())
    }

    pub fn save_sources(&self, v: &Value) -> Res<Value> {
        fs::create_dir_all(base_dir()).map_err(err)?;
        super::crypt::write(&sources_path(), v.to_string().as_bytes()).map_err(err)?;
        self.learn(v);
        Ok(Value::Bool(true))
    }

    fn checked(url: &str) -> Res<String> {
        if !url.starts_with("https://") && !url.starts_with("http://") {
            return Err("Not a web address".into());
        }
        let host = host_of(url).ok_or("Not a web address")?;
        if private_host(&host) {
            return Err("Addresses on this PC or network can't be reached from Discover".into());
        }
        domain_of(url).ok_or_else(|| "Not a web address".into())
    }

    // API calls: your sources and the app's own services only
    fn allowed_api(&self, url: &str) -> Res<()> {
        let d = Self::checked(url)?;
        if SERVICES.contains(&d.as_str()) || self.domains.lock().unwrap().iter().any(|s| *s == d) {
            Ok(())
        } else {
            Err(format!("{d} isn't one of your sources"))
        }
    }

    fn request(&self, method: &str, url: &str, headers: &Value) -> ureq::Request {
        let origin: String = url.split('/').take(3).collect::<Vec<_>>().join("/");
        let mut r = self.agent.request(method, url).set("User-Agent", USER_AGENT).set("Referer", &(origin + "/")).timeout(Duration::from_secs(40));
        if let Some(h) = headers.as_object() {
            for (k, v) in h {
                if let (true, Some(v)) = (HEADERS.contains(&k.to_lowercase().as_str()), v.as_str()) {
                    r = r.set(k, v);
                }
            }
        }
        r
    }

    fn answer(result: Result<ureq::Response, ureq::Error>) -> Res<ureq::Response> {
        match result {
            Ok(r) => Ok(r),
            Err(ureq::Error::Status(code, r)) => {
                // "too many requests": when the site says its limit resets, the page waits just that long
                let reset = r.header("x-ratelimit-reset").or(r.header("retry-after")).filter(|s| s.trim().parse::<f64>().is_ok())
                    .map(|s| format!(" [retry {}]", s.trim())).unwrap_or_default();
                let body = r.into_string().unwrap_or_default();
                let short: String = body.chars().take(200).collect();
                Err(format!("{code}{reset}: {}", short.trim()))
            }
            Err(e) => Err(format!("Couldn't reach the site: {e}")),
        }
    }

    /// An API answer, as text (the page parses it). `headers` may set Authorization and Accept.
    pub fn json(&self, url: &str, headers: &Value) -> Res<Value> {
        self.allowed_api(url)?;
        let r = Self::answer(self.request("GET", url, headers).call())?;
        Ok(Value::String(r.into_string().map_err(err)?))
    }

    /// A form POST (e.g. a login token), as text.
    pub fn post(&self, url: &str, form: &Value, headers: &Value) -> Res<Value> {
        self.allowed_api(url)?;
        let pairs: Vec<(String, String)> = form.as_object().map(|o| o.iter().map(|(k, v)| (k.clone(), v.as_str().map(String::from).unwrap_or_else(|| v.to_string()))).collect()).unwrap_or_default();
        let refs: Vec<(&str, &str)> = pairs.iter().map(|(k, v)| (k.as_str(), v.as_str())).collect();
        let r = Self::answer(self.request("POST", url, headers).send_form(&refs))?;
        Ok(Value::String(r.into_string().map_err(err)?))
    }

    /// A search form that answers through a session (Hypnotube): the form is sent, its session cookie
    /// kept, and the results page `then` read with it. The page as text.
    pub fn form_then_get(&self, url: &str, form: &Value, then: &str) -> Res<Value> {
        self.allowed_api(url)?;
        self.allowed_api(then)?;
        let pairs: Vec<(String, String)> = form.as_object().map(|o| o.iter().map(|(k, v)| (k.clone(), v.as_str().map(String::from).unwrap_or_else(|| v.to_string()))).collect()).unwrap_or_default();
        let refs: Vec<(&str, &str)> = pairs.iter().map(|(k, v)| (k.as_str(), v.as_str())).collect();
        // (its own agent, not following the form's redirect: the cookie comes with the redirect)
        let once = ureq::AgentBuilder::new().redirects(0).timeout(Duration::from_secs(40)).build();
        let origin: String = url.split('/').take(3).collect::<Vec<_>>().join("/");
        let r = Self::answer(once.post(url).set("User-Agent", USER_AGENT).set("Referer", &(origin + "/")).send_form(&refs))?;
        let cookies: Vec<String> = r.all("set-cookie").iter().filter_map(|c| c.split(';').next()).map(|c| c.trim().to_string()).collect();
        let r = Self::answer(self.request("GET", then, &Value::Null).set("Cookie", &cookies.join("; ")).call())?;
        Ok(Value::String(r.into_string().map_err(err)?))
    }

    /// A picture as a multipart upload (SauceNAO, IQDB), with extra form fields; the answer as text.
    pub fn upload(&self, url: &str, field: &str, bytes: &[u8], fields: &Value) -> Res<Value> {
        self.allowed_api(url)?;
        let boundary = format!("----LewdVault{}", md5_hex(&bytes[..bytes.len().min(4096)]));
        let mut body = Vec::with_capacity(bytes.len() + 1024);
        if let Some(o) = fields.as_object() {
            for (k, v) in o {
                let v = v.as_str().map(String::from).unwrap_or_else(|| v.to_string());
                write!(body, "--{boundary}\r\nContent-Disposition: form-data; name=\"{k}\"\r\n\r\n{v}\r\n").map_err(err)?;
            }
        }
        write!(body, "--{boundary}\r\nContent-Disposition: form-data; name=\"{field}\"; filename=\"picture.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n").map_err(err)?;
        body.extend_from_slice(bytes);
        write!(body, "\r\n--{boundary}--\r\n").map_err(err)?;
        let r = Self::answer(
            // (IQDB queues searches when it's busy: up to 2 minutes)
            self.request("POST", url, &Value::Null).timeout(Duration::from_secs(120)).set("Content-Type", &format!("multipart/form-data; boundary={boundary}")).send_bytes(&body),
        )?;
        Ok(Value::String(r.into_string().map_err(err)?))
    }

    /// A picture or video, from the cache or the web: (bytes, content type). Any public address.
    pub fn file(&self, url: &str) -> Res<(Vec<u8>, String)> {
        Self::checked(url)?;
        let dir = base_dir().join("web");
        let key = md5_hex(url.as_bytes());
        let (body_path, type_path) = (dir.join(format!("{key}.bin")), dir.join(format!("{key}.type")));
        if let (Ok(b), Ok(t)) = (super::crypt::read(&body_path), super::crypt::read_to_string(&type_path)) {
            return Ok((b, t));
        }
        let r = Self::answer(self.request("GET", url, &Value::Null).call())?;
        let kind = r.content_type().to_string();
        let mut bytes = Vec::new();
        r.into_reader().take(MAX_BYTES).read_to_end(&mut bytes).map_err(err)?;
        if fs::create_dir_all(&dir).is_ok() {
            let _ = super::crypt::write(&body_path, &bytes);
            let _ = super::crypt::write(&type_path, kind.as_bytes());
            if self.writes.fetch_add(1, Ordering::Relaxed) % 200 == 199 {
                trim(&dir);
            }
        }
        Ok((bytes, kind))
    }

    /// A piece of a video on the web, for playing it (/stream): the player's Range is passed on, at most
    /// STREAM_PIECE at a time (the player asks for the next as it plays), nothing is cached. Gives
    /// (status, content type, content range, bytes).
    pub fn stream(&self, url: &str, range: Option<&str>) -> Res<(u16, String, Option<String>, Vec<u8>)> {
        Self::checked(url)?;
        let spec = range.and_then(|v| v.trim().strip_prefix("bytes=")).unwrap_or("0-");
        let (a, b) = spec.split_once('-').unwrap_or(("0", ""));
        let start: u64 = a.trim().parse().unwrap_or(0);
        let end = b.trim().parse::<u64>().ok().map(|e| e.min(start + STREAM_PIECE - 1)).unwrap_or(start + STREAM_PIECE - 1);
        let r = Self::answer(self.request("GET", url, &Value::Null).timeout(Duration::from_secs(60)).set("Range", &format!("bytes={start}-{end}")).call())?;
        let status = r.status();
        let kind = r.content_type().to_string();
        let total = r.header("content-length").and_then(|v| v.parse::<u64>().ok());
        let content_range = r.header("content-range").map(String::from);
        let mut bytes = Vec::new();
        r.into_reader().take(STREAM_PIECE).read_to_end(&mut bytes).map_err(err)?;
        if status == 206 {
            return Ok((206, kind, content_range, bytes));
        }
        // a site that ignores Range sends the whole file: its start, as if it had answered the range
        if start == 0 {
            let n = bytes.len() as u64;
            return Ok((206, kind, Some(format!("bytes 0-{}/{}", n.saturating_sub(1), total.unwrap_or(n))), bytes));
        }
        Err("That site doesn't let a player jump into its videos".into())
    }

    /// Starts saving a long video to this PC (a file in "downloads"); gives its id. The page asks
    /// download_progress(id), then reads it from vault://dl/<id> and adds it to the vault.
    pub fn download_start(&self, url: &str) -> Res<Value> {
        Self::checked(url)?;
        let id = md5_hex(format!("{url}{:?}", std::time::SystemTime::now()).as_bytes())[..16].to_string();
        let dir = base_dir().join("downloads");
        fs::create_dir_all(&dir).map_err(err)?;
        let path = dir.join(format!("{id}.part"));
        let d = Arc::new(Download::default());
        self.downloads.lock().unwrap().insert(id.clone(), d.clone());
        let req = self.request("GET", url, &Value::Null).timeout(Duration::from_secs(4 * 3600));
        std::thread::spawn(move || {
            let result = (|| -> Res<()> {
                let r = Self::answer(req.call())?;
                if let Some(n) = r.header("content-length").and_then(|v| v.parse::<u64>().ok()) {
                    d.total.store(n, Ordering::Relaxed);
                }
                let mut out = super::crypt::Writer::create(&path).map_err(err)?;
                let mut reader = r.into_reader().take(MAX_DOWNLOAD);
                let mut buf = vec![0u8; 512 * 1024];
                loop {
                    let n = reader.read(&mut buf).map_err(err)?;
                    if n == 0 {
                        break;
                    }
                    out.write_all(&buf[..n]).map_err(err)?;
                    d.done.fetch_add(n as u64, Ordering::Relaxed);
                    if d.state.lock().unwrap().as_str() == "cancelled" {
                        return Err("cancelled".into());
                    }
                }
                Ok(())
            })();
            let mut state = d.state.lock().unwrap();
            if state.as_str() != "cancelled" {
                *state = match result { Ok(()) => "done".into(), Err(e) => e };
            }
        });
        Ok(Value::String(id))
    }

    /// How far a download is: { done, total (0 when the site doesn't say), state: "", "done" or the error }.
    pub fn download_progress(&self, id: &str) -> Res<Value> {
        let d = self.downloads.lock().unwrap().get(id).cloned().ok_or("No such download")?;
        let state = d.state.lock().unwrap().clone();
        Ok(json!({ "done": d.done.load(Ordering::Relaxed), "total": d.total.load(Ordering::Relaxed), "state": state }))
    }

    /// A finished download's file (for vault://dl/<id>).
    pub fn download_path(&self, id: &str) -> Option<PathBuf> {
        let d = self.downloads.lock().unwrap().get(id).cloned()?;
        let done = d.state.lock().unwrap().as_str() == "done";
        let path = base_dir().join("downloads").join(format!("{id}.part"));
        (done && id.chars().all(|c| c.is_ascii_hexdigit())).then_some(path)
    }

    /// Cancels a download, or forgets a finished one: its file goes.
    pub fn download_drop(&self, id: &str) -> Res<Value> {
        if let Some(d) = self.downloads.lock().unwrap().remove(id) {
            let mut state = d.state.lock().unwrap();
            if state.is_empty() {
                *state = "cancelled".into();
            }
        }
        if id.chars().all(|c| c.is_ascii_hexdigit()) {
            let _ = fs::remove_file(base_dir().join("downloads").join(format!("{id}.part")));
        }
        Ok(Value::Bool(true))
    }

    /// Reddit login: opens Reddit's "Allow" page in the browser and waits (up to 3 minutes) for it
    /// to come back to http://127.0.0.1:65010/reddit with a code, which is traded for tokens. Needs
    /// your own Reddit app's client id ("installed app", redirect uri exactly as above).
    pub fn reddit_login(&self, client_id: &str) -> Res<Value> {
        if client_id.is_empty() || !client_id.chars().all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_') {
            return Err("Add your Reddit app's client id first".into());
        }
        let redirect = "http://127.0.0.1:65010/reddit";
        let state = md5_hex(format!("{:?}", std::time::SystemTime::now()).as_bytes());
        let listener = std::net::TcpListener::bind("127.0.0.1:65010").map_err(|e| format!("Couldn't wait for Reddit's answer: {e}"))?;
        listener.set_nonblocking(true).map_err(err)?;
        let url = format!(
            "https://www.reddit.com/api/v1/authorize?client_id={client_id}&response_type=code&state={state}&redirect_uri={}&duration=permanent&scope=read%20identity%20mysubreddits%20history",
            percent_encoding::utf8_percent_encode(redirect, percent_encoding::NON_ALPHANUMERIC)
        );
        std::process::Command::new("explorer").arg(&url).spawn().map_err(err)?;
        let started = std::time::Instant::now();
        let code = loop {
            if started.elapsed() > Duration::from_secs(180) {
                return Err("Reddit didn't answer in time. Try again.".into());
            }
            match listener.accept() {
                Ok((mut stream, _)) => {
                    let _ = stream.set_nonblocking(false);
                    let mut buf = [0u8; 4096];
                    let n = stream.read(&mut buf).unwrap_or(0);
                    let req = String::from_utf8_lossy(&buf[..n]).into_owned();
                    let line = req.lines().next().unwrap_or("");
                    let query = line.split_whitespace().nth(1).and_then(|p| p.split_once('?')).map(|(_, q)| q.to_string()).unwrap_or_default();
                    let get = |k: &str| query.split('&').find_map(|kv| kv.strip_prefix(&format!("{k}="))).map(|v| percent_encoding::percent_decode_str(v).decode_utf8_lossy().into_owned());
                    let page = |msg: &str| format!("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n<!doctype html><meta charset=utf-8><title>Lewd Vault</title><body style=\"font:16px system-ui;background:#2a2226;color:#f7e8ec;display:grid;place-items:center;height:90vh\"><p>{msg}</p></body>");
                    if !line.contains("/reddit") {
                        let _ = stream.write_all(b"HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
                        continue;
                    }
                    if let Some(e) = get("error") {
                        let _ = stream.write_all(page("Reddit login cancelled. You can close this tab.").as_bytes());
                        return Err(format!("Reddit said: {e}"));
                    }
                    if get("state").as_deref() != Some(state.as_str()) {
                        let _ = stream.write_all(page("That wasn't the login Lewd Vault started. Try again.").as_bytes());
                        return Err("Login answer didn't match; try again".into());
                    }
                    let _ = stream.write_all(page("&#9829; Logged in. You can close this tab and go back to Lewd Vault.").as_bytes());
                    break get("code").ok_or("No code from Reddit")?;
                }
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => std::thread::sleep(Duration::from_millis(150)),
                Err(e) => return Err(err(e)),
            }
        };
        let basic = format!("Basic {}", base64::Engine::encode(&base64::engine::general_purpose::STANDARD, format!("{client_id}:")));
        let text = self.post(
            "https://www.reddit.com/api/v1/access_token",
            &json!({ "grant_type": "authorization_code", "code": code, "redirect_uri": redirect }),
            &json!({ "Authorization": basic }),
        )?;
        serde_json::from_str(text.as_str().unwrap_or("")).map_err(err)
    }
}

// keeps the cache under CACHE_MAX: past it, the oldest files go until it's down to CACHE_KEEP
fn trim(dir: &PathBuf) {
    let Ok(entries) = fs::read_dir(dir) else { return };
    let mut files: Vec<(std::time::SystemTime, u64, PathBuf)> = entries.flatten()
        .filter_map(|e| { let m = e.metadata().ok()?; Some((m.modified().ok()?, m.len(), e.path())) }).collect();
    let mut total: u64 = files.iter().map(|f| f.1).sum();
    if total <= CACHE_MAX {
        return;
    }
    files.sort_by_key(|f| f.0);
    for (_, n, p) in files {
        if total <= CACHE_KEEP {
            break;
        }
        if fs::remove_file(&p).is_ok() {
            total -= n;
        }
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn domains() {
        assert_eq!(super::domain_of("https://danbooru.donmai.us/posts.json"), Some("donmai.us".into()));
        assert_eq!(super::domain_of("https://api.rule34.xxx/index.php?x"), Some("rule34.xxx".into()));
        assert_eq!(super::domain_of("https://yande.re"), Some("yande.re".into()));
    }
    #[test]
    fn private_hosts() {
        for h in ["localhost", "vault.localhost", "127.0.0.1", "10.1.2.3", "192.168.0.10", "172.16.4.4", "169.254.1.1", "router", "[::1]"] {
            assert!(super::private_host(h), "{h}");
        }
        for h in ["i.redd.it", "cdn.donmai.us", "8.8.8.8", "media.redgifs.com"] {
            assert!(!super::private_host(h), "{h}");
        }
        assert_eq!(super::host_of("https://user@Example.com:8080/x"), Some("example.com".into()));
    }
}
