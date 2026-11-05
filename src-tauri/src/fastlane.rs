// The fast lane for saving files. Pieces sent through Tauri's own channel (the upload command) pass
// through WebView2's request interception, which manages about 40 MB/s: a 1 GB video took 25 s just
// to cross over. This is a plain HTTP server on 127.0.0.1 (a free port, chosen at start) that takes
// the same pieces as fast as the disk can write them.
//
// Only the app's page can use it: every request must carry the key made at start (128 random bits,
// handed to the page by the "fastLane" call), browsers only let pages from the app's own origin read
// the answers, and nothing outside this PC can reach 127.0.0.1. It accepts upload pieces and nothing
// else. If it can't start, the page keeps using the upload command.

use super::{Res, Vault};
use std::collections::hash_map::RandomState;
use std::hash::{BuildHasher, Hasher};
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::{Arc, OnceLock};

static LANE: OnceLock<(u16, String)> = OnceLock::new();
const ORIGINS: [&str; 2] = ["http://tauri.localhost", "https://tauri.localhost"];
const MAX_PIECE: u64 = 256 * 1024 * 1024;

pub fn start(vault: Arc<Vault>) {
    let Ok(listener) = TcpListener::bind("127.0.0.1:0") else { return };
    let Ok(port) = listener.local_addr().map(|a| a.port()) else { return };
    // (RandomState's keys come from the system's random source; two of them make 128 bits)
    let key: String = (0..2).map(|_| format!("{:016x}", RandomState::new().build_hasher().finish())).collect();
    let _ = LANE.set((port, key));
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

// {port, key} for the page, or null when the lane isn't running
pub fn info() -> serde_json::Value {
    LANE.get().map_or(serde_json::Value::Null, |(port, key)| serde_json::json!({ "port": port, "key": key }))
}

fn handle(vault: &Vault, conn: TcpStream) -> std::io::Result<()> {
    let mut out = conn.try_clone()?;
    let mut reader = BufReader::with_capacity(1 << 20, conn);
    loop {
        // the request line and headers (at most 64 lines of 8 KB)
        let mut line = String::new();
        if reader.by_ref().take(8192).read_line(&mut line)? == 0 {
            return Ok(());
        }
        let mut parts = line.split_whitespace();
        let (method, path) = (parts.next().unwrap_or("").to_string(), parts.next().unwrap_or("").to_string());
        let mut headers = Vec::new();
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
        let header = |k: &str| headers.iter().find(|(n, _)| n == k).map(|(_, v)| v.clone()).unwrap_or_default();
        let origin = header("origin");
        let cors = if ORIGINS.contains(&origin.as_str()) {
            format!("Access-Control-Allow-Origin: {origin}\r\nVary: Origin\r\n")
        } else {
            String::new()
        };
        let reply = |out: &mut TcpStream, status: &str, body: &str, extra: &str| {
            write!(out, "HTTP/1.1 {status}\r\n{cors}{extra}Content-Type: application/json\r\nContent-Length: {}\r\n\r\n{body}", body.len())
                .and_then(|_| out.flush())
        };
        if method == "OPTIONS" {
            reply(&mut out, "204 No Content", "", "Access-Control-Allow-Methods: POST\r\nAccess-Control-Allow-Headers: x-key, x-token, x-first, x-done, x-name, x-parent, x-pos, content-type\r\nAccess-Control-Allow-Private-Network: true\r\nAccess-Control-Max-Age: 600\r\n")?;
            continue;
        }
        let len: u64 = header("content-length").parse().unwrap_or(u64::MAX);
        let key_ok = LANE.get().is_some_and(|(_, key)| same(key, &header("x-key")));
        if method != "POST" || path != "/upload" || !key_ok || cors.is_empty() || len > MAX_PIECE {
            reply(&mut out, "403 Forbidden", "\"Not allowed\"", "Connection: close\r\n")?;
            return Ok(());
        }
        let decoded = |k: &str| percent_encoding::percent_decode_str(&header(k)).decode_utf8_lossy().into_owned();
        let mut body = reader.by_ref().take(len);
        let result: Res<String> = super::write_piece(vault, &decoded("x-token"), header("x-first") == "1",
            header("x-pos").parse().ok(), &mut body)
            .and_then(|_| if header("x-done") == "1" { super::finish_piece(vault, &decoded("x-token"), &decoded("x-name"), &decoded("x-parent")) } else { Ok(String::new()) });
        // (whatever of the body wasn't read, so the next request on this connection starts clean)
        std::io::copy(&mut body, &mut std::io::sink())?;
        match result {
            Ok(id) => reply(&mut out, "200 OK", &serde_json::to_string(&id).unwrap_or_default(), "")?,
            Err(e) => reply(&mut out, "500 Internal Server Error", &serde_json::to_string(&e).unwrap_or_default(), "")?,
        }
    }
}

// compares without stopping at the first difference
fn same(a: &str, b: &str) -> bool {
    a.len() == b.len() && a.bytes().zip(b.bytes()).fold(0u8, |d, (x, y)| d | (x ^ y)) == 0
}
