// The media server (iOS): the vault's files, served over loopback HTTP.
//
// A custom URL scheme (vault://, spelled http://vault.localhost on WebView2) is what the desktop
// app uses, but on WKWebView a custom scheme does not feed <img>, <video> and <audio> reliably
// (WebKit bugs 146351, 324711): images come out blank and video won't play or seek. A plain HTTP
// server on 127.0.0.1 does, and it also answers byte ranges, which the video streamer needs.
//
// It is the same `serve` the desktop's protocol handler uses, so every path (/t/, /stream, /web,
// /dl/, an item id) behaves identically. Only the page can use it: every request must carry the
// token made at start, which the page gets through window.__VAULT_ORIGIN; nothing off this device
// can reach 127.0.0.1, and the port is picked at random each launch.
//
// On the desktop none of this runs (media.rs is only compiled for iOS): vault:// stays.

use super::{serve, Vault};
use std::collections::hash_map::RandomState;
use std::hash::{BuildHasher, Hasher};
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::{Arc, OnceLock};

static AT: OnceLock<(u16, String)> = OnceLock::new();

/// Where the page reaches the vault: http://127.0.0.1:<port>/<token> (the token is part of the base,
/// so every URL the page builds carries it). Empty until start() has run.
pub fn origin() -> String {
    AT.get().map(|(port, token)| format!("http://127.0.0.1:{port}/{token}")).unwrap_or_default()
}

/// Starts the server and remembers where it listens. Called once, at start, before the window.
pub fn start(vault: Arc<Vault>) {
    let Ok(listener) = TcpListener::bind("127.0.0.1:0") else { return };
    let Ok(port) = listener.local_addr().map(|a| a.port()) else { return };
    // (RandomState's keys come from the system's random source; two of them make 128 bits)
    let token: String = (0..2).map(|_| format!("{:016x}", RandomState::new().build_hasher().finish())).collect();
    let _ = AT.set((port, token));
    std::thread::spawn(move || {
        for conn in listener.incoming().flatten() {
            let vault = vault.clone();
            std::thread::spawn(move || {
                let _ = conn.set_read_timeout(Some(std::time::Duration::from_secs(60)));
                let _ = handle(&vault, conn);
            });
        }
    });
}

fn handle(vault: &Vault, conn: TcpStream) -> std::io::Result<()> {
    let mut out = conn.try_clone()?;
    let mut reader = BufReader::with_capacity(1 << 20, conn);
    loop {
        let mut line = String::new();
        if reader.by_ref().take(8192).read_line(&mut line)? == 0 {
            return Ok(());
        }
        let mut parts = line.split_whitespace();
        let (method, target) = (parts.next().unwrap_or("").to_string(), parts.next().unwrap_or("").to_string());
        let mut headers: Vec<(String, String)> = Vec::new();
        for _ in 0..64 {
            let mut h = String::new();
            reader.by_ref().take(8192).read_line(&mut h)?;
            let h = h.trim_end();
            if h.is_empty() {
                break;
            }
            if let Some((k, v)) = h.split_once(':') {
                headers.push((k.trim().to_ascii_lowercase(), v.trim().to_string()));
            }
        }
        let head_only = method == "HEAD";
        if method != "GET" && !head_only {
            return reply(&mut out, 405, &[], b"", true);
        }
        // the token is the first path piece; the rest is what serve() knows
        let Some((_, token)) = AT.get() else { return reply(&mut out, 503, &[], b"", true) };
        let rest = match target.strip_prefix(&format!("/{token}")) {
            Some(r) if r.is_empty() || r.starts_with('/') || r.starts_with('?') => r,
            _ => return reply(&mut out, 403, &[], b"", true),
        };
        let mut req = tauri::http::Request::builder().method("GET").uri(format!("http://127.0.0.1{rest}"));
        for (k, v) in &headers {
            req = req.header(k.as_str(), v.as_str());
        }
        let res = match req.body(Vec::new()) {
            Ok(r) => serve(vault, &r),
            Err(_) => return reply(&mut out, 400, &[], b"", true),
        };
        let extra: Vec<(String, String)> = res.headers().iter()
            .map(|(k, v)| (k.to_string(), v.to_str().unwrap_or("").to_string()))
            .filter(|(k, _)| k != "content-length")
            .collect();
        reply(&mut out, res.status().as_u16(), &extra, if head_only { b"" } else { res.body() }, false)?;
        if head_only {
            return Ok(());
        }
    }
}

fn reply(out: &mut TcpStream, status: u16, headers: &[(String, String)], body: &[u8], close: bool) -> std::io::Result<()> {
    let mut head = format!("HTTP/1.1 {status} X\r\nContent-Length: {}\r\n", body.len());
    for (k, v) in headers {
        head.push_str(&format!("{k}: {v}\r\n"));
    }
    head.push_str(if close { "Connection: close\r\n\r\n" } else { "\r\n" });
    out.write_all(head.as_bytes()).and_then(|_| out.write_all(body)).and_then(|_| out.flush())
}
