// Keeping the vault healthy, backed up and up to date:
//   - backup: the whole vault copied into a folder you pick (a USB drive, another disk), only what
//     changed since last time (by size and date), in the background with progress
//   - health check: index rows whose file is gone, previews that are missing, preview clips nothing
//     uses any more; and the fixes for each
//   - updates: the newest release on GitHub, downloaded next to the app and swapped in on restart
use crate::{err, storage, Res, Vault};
use serde_json::{json, Value};
use std::collections::HashSet;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::Duration;

// ---------- backup ----------
fn backup_state() -> &'static Mutex<Value> {
    static S: OnceLock<Mutex<Value>> = OnceLock::new();
    S.get_or_init(|| Mutex::new(json!({ "status": "idle" })))
}

fn walk(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(rd) = fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let p = e.path();
        if p.is_dir() {
            walk(&p, out);
        } else {
            out.push(p);
        }
    }
}

pub fn backup_start(vault: Arc<Vault>, dest: &str) -> Res<Value> {
    let dest = PathBuf::from(dest);
    if dest.as_os_str().is_empty() {
        return Err("Pick a folder to back up into.".into());
    }
    if dest.starts_with(&vault.root) || vault.root.starts_with(&dest) {
        return Err("Pick a folder outside the vault (and not one that holds it).".into());
    }
    {
        let mut s = backup_state().lock().unwrap();
        if s["status"] == "running" {
            return Err("A backup is already running.".into());
        }
        *s = json!({ "status": "running", "done": 0, "total": 0, "copied": 0, "bytes": 0 });
    }
    std::thread::spawn(move || {
        let result = (|| -> Res<(u64, u64)> {
            let target = dest.join("Lewd Vault backup");
            fs::create_dir_all(&target).map_err(err)?;
            let mut files = Vec::new();
            walk(&vault.root, &mut files);
            backup_state().lock().unwrap()["total"] = json!(files.len());
            let (mut copied, mut bytes) = (0u64, 0u64);
            for (k, src) in files.iter().enumerate() {
                let rel = src.strip_prefix(&vault.root).map_err(err)?;
                let to = target.join(rel);
                let meta = fs::metadata(src).map_err(err)?;
                let same = fs::metadata(&to).map(|m| m.len() == meta.len() && m.modified().ok() >= meta.modified().ok()).unwrap_or(false);
                if !same {
                    if let Some(parent) = to.parent() {
                        fs::create_dir_all(parent).map_err(err)?;
                    }
                    fs::copy(src, &to).map_err(|e| format!("Could not copy {}: {e}", rel.display()))?;
                    copied += 1;
                    bytes += meta.len();
                }
                if k % 20 == 0 {
                    let mut s = backup_state().lock().unwrap();
                    s["done"] = json!(k + 1);
                    s["copied"] = json!(copied);
                    s["bytes"] = json!(bytes);
                }
            }
            fs::write(target.join("backup-info.txt"), format!("Lewd Vault backup\nMade: {}\nFrom: {}\n", chrono::Local::now().format("%Y-%m-%d %H:%M"), vault.root.display())).map_err(err)?;
            Ok((copied, bytes))
        })();
        let mut s = backup_state().lock().unwrap();
        match result {
            Ok((copied, bytes)) => {
                let total = s["total"].clone();
                *s = json!({ "status": "done", "done": total, "total": total, "copied": copied, "bytes": bytes, "folder": dest.join("Lewd Vault backup") });
            }
            Err(e) => *s = json!({ "status": e }),
        }
    });
    Ok(Value::Null)
}

pub fn backup_progress() -> Value {
    backup_state().lock().unwrap().clone()
}

// ---------- health check ----------
pub fn health(vault: &Vault) -> Res<Value> {
    let (items, _) = vault.load_index()?;
    let mut missing = Vec::new();
    let mut no_preview = Vec::new();
    let mut used_clips = HashSet::new();
    for i in items.iter() {
        let main = vault.main_path(i);
        if !main.exists() && !vault.resolve(&i.id).map(|p| p.exists()).unwrap_or(false) {
            missing.push(json!({ "id": i.id, "file": i.file, "category": i.category }));
        }
        if !i.preview_id.is_empty() {
            match vault.resolve(&i.preview_id) {
                Ok(p) if p.exists() => {
                    if let Some(n) = p.file_name() {
                        used_clips.insert(n.to_os_string());
                    }
                }
                _ => no_preview.push(json!({ "id": i.id, "file": i.file })),
            }
        }
    }
    let mut orphans = Vec::new();
    if let Ok(rd) = fs::read_dir(vault.app_dir("clips")) {
        for e in rd.flatten() {
            if e.path().is_file() && !used_clips.contains(&e.file_name()) {
                orphans.push(json!({ "name": e.file_name().to_string_lossy(), "bytes": e.metadata().map(|m| m.len()).unwrap_or(0) }));
            }
        }
    }
    Ok(json!({ "items": items.len(), "missing": missing, "noPreview": no_preview, "orphanClips": orphans }))
}

pub fn health_fix(vault: &Vault, what: &str) -> Res<Value> {
    let report = health(vault)?;
    let ids = |key: &str| report[key].as_array().map(|a| a.iter().filter_map(|x| x["id"].as_str().map(String::from)).collect::<HashSet<_>>()).unwrap_or_default();
    match what {
        // rows for files that no longer exist: out of the index (nothing to bin)
        "missing" => {
            let gone = ids("missing");
            vault.change_index(|items| items.retain(|i| !gone.contains(&i.id)))
        }
        // previews that point at nothing: cleared, so tiles show the still picture instead of a broken clip
        "noPreview" => {
            let gone = ids("noPreview");
            vault.change_index(|items| {
                for i in items.iter_mut().filter(|i| gone.contains(&i.id)) {
                    i.preview_id.clear();
                }
            })
        }
        // clips no item uses: to the Recycle Bin
        "orphanClips" => {
            let dir = vault.app_dir("clips");
            let mut n = 0;
            for o in report["orphanClips"].as_array().cloned().unwrap_or_default() {
                if let Some(name) = o["name"].as_str() {
                    if trash::delete(dir.join(name)).is_ok() {
                        n += 1;
                    }
                }
            }
            Ok(json!({ "binned": n }))
        }
        _ => Err("Unknown fix".into()),
    }
}

// ---------- updates ----------
const RELEASES: &str = "https://api.github.com/repos/Mousyexp/Lewd-Vault/releases/latest";
// the release's files (GitHub writes spaces in file names as dots): the portable app as a zip, and the
// installer. A portable copy updates from the zip (its exe swapped in); an installed one (it can't write
// next to itself) runs the new installer.
fn pick_asset(rel: &Value, installed: bool) -> String {
    let assets = rel["assets"].as_array().cloned().unwrap_or_default();
    let want = |name: &str| if installed { name.ends_with(".msi") } else { name.contains("Portable") && name.ends_with(".zip") };
    assets.iter().find(|a| a["name"].as_str().map(want).unwrap_or(false))
        .and_then(|a| a["browser_download_url"].as_str()).unwrap_or("").to_string()
}

fn version_parts(v: &str) -> Vec<u64> {
    v.trim_start_matches('v').split(|c: char| !c.is_ascii_digit()).filter(|s| !s.is_empty()).filter_map(|s| s.parse().ok()).collect()
}

fn agent() -> ureq::Agent {
    ureq::AgentBuilder::new().timeout_connect(Duration::from_secs(15)).user_agent("Lewd-Vault-updater").build()
}

pub fn update_check() -> Res<Value> {
    let r = agent().get(RELEASES).set("Accept", "application/vnd.github+json").timeout(Duration::from_secs(30)).call().map_err(|e| match e {
        ureq::Error::Status(404, _) => "No releases have been published yet.".to_string(),
        e => format!("Couldn't reach GitHub: {e}"),
    })?;
    let rel: Value = serde_json::from_str(&r.into_string().map_err(err)?).map_err(err)?;
    let tag = rel["tag_name"].as_str().unwrap_or("").to_string();
    let installed = !storage::exe_dir_writable();
    let url = pick_asset(&rel, installed);
    let current = env!("CARGO_PKG_VERSION");
    let newer = version_parts(&tag) > version_parts(current);
    Ok(json!({ "current": current, "latest": tag, "newer": newer && !url.is_empty(), "notes": rel["body"], "url": url, "page": rel["html_url"], "installed": installed }))
}

fn update_state() -> &'static Mutex<Value> {
    static S: OnceLock<Mutex<Value>> = OnceLock::new();
    S.get_or_init(|| Mutex::new(json!({ "status": "idle" })))
}

fn new_exe() -> PathBuf {
    storage::exe_dir().join("Lewd Vault.new.exe")
}
fn new_msi() -> PathBuf {
    std::env::temp_dir().join("Lewd Vault update.msi")
}

/// The app out of the portable zip: its one .exe.
pub(crate) fn exe_from_zip(zip_path: &Path, to: &Path) -> Res<()> {
    let mut z = zip::ZipArchive::new(fs::File::open(zip_path).map_err(err)?).map_err(err)?;
    for k in 0..z.len() {
        let mut f = z.by_index(k).map_err(err)?;
        if f.is_file() && f.name().to_ascii_lowercase().ends_with(".exe") {
            let mut out = fs::File::create(to).map_err(err)?;
            std::io::copy(&mut f, &mut out).map_err(|e| format!("Couldn't unpack the update: {e}"))?;
            return Ok(());
        }
    }
    Err("The update has no app in it.".into())
}

pub fn update_download(url: &str) -> Res<Value> {
    // only GitHub's own release files
    if !(url.starts_with("https://github.com/Mousyexp/Lewd-Vault/releases/download/")) {
        return Err("That isn't a Lewd Vault release.".into());
    }
    *update_state().lock().unwrap() = json!({ "status": "running", "done": 0, "total": 0 });
    let url = url.to_string();
    std::thread::spawn(move || {
        let result = (|| -> Res<()> {
            let r = agent().get(&url).timeout(Duration::from_secs(600)).call().map_err(err)?;
            let total = r.header("content-length").and_then(|v| v.parse::<u64>().ok()).unwrap_or(0);
            update_state().lock().unwrap()["total"] = json!(total);
            let is_msi = url.ends_with(".msi");
            let part = if is_msi { new_msi().with_extension("part") } else { new_exe().with_extension("part") };
            let mut out = fs::File::create(&part).map_err(err)?;
            let mut reader = r.into_reader();
            let mut buf = vec![0u8; 256 * 1024];
            let mut done = 0u64;
            loop {
                let n = std::io::Read::read(&mut reader, &mut buf).map_err(err)?;
                if n == 0 {
                    break;
                }
                std::io::Write::write_all(&mut out, &buf[..n]).map_err(err)?;
                done += n as u64;
                update_state().lock().unwrap()["done"] = json!(done);
            }
            drop(out);
            if done < 1_000_000 {
                let _ = fs::remove_file(&part);
                return Err("The download was too small to be the app.".into());
            }
            if is_msi {
                // an installer starts with the Windows "compound file" signature
                let mut head = [0u8; 8];
                std::io::Read::read_exact(&mut fs::File::open(&part).map_err(err)?, &mut head).map_err(err)?;
                if head != [0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1] {
                    let _ = fs::remove_file(&part);
                    return Err("The download wasn't the installer.".into());
                }
                let _ = fs::remove_file(new_msi());
                fs::rename(&part, new_msi()).map_err(err)?;
            } else {
                // the zip's app; a Windows program starts with "MZ"
                let exe = new_exe();
                let result = exe_from_zip(&part, &exe);
                let _ = fs::remove_file(&part);
                result?;
                let mut head = [0u8; 2];
                std::io::Read::read_exact(&mut fs::File::open(&exe).map_err(err)?, &mut head).map_err(err)?;
                if &head != b"MZ" {
                    let _ = fs::remove_file(&exe);
                    return Err("The update wasn't the app.".into());
                }
            }
            Ok(())
        })();
        *update_state().lock().unwrap() = match result {
            Ok(()) => json!({ "status": "ready" }),
            Err(e) => json!({ "status": e }),
        };
    });
    Ok(Value::Null)
}

pub fn update_progress() -> Value {
    update_state().lock().unwrap().clone()
}

// swaps the downloaded app in once this one has closed, then starts it again
#[tauri::command]
pub fn install_update(app: tauri::AppHandle) -> Res<()> {
    let exe = std::env::current_exe().map_err(err)?;
    let pid = std::process::id();
    let wait = format!(":wait\r\ntasklist /FI \"PID eq {pid}\" | find \"{pid}\" >nul && (timeout /t 1 /nobreak >nul & goto wait)\r\n");
    let body = if new_msi().exists() {
        // installed: the new installer runs (it asks Windows for permission), then the app starts again
        format!("start \"\" /wait msiexec /i \"{}\" /passive /norestart\r\ndel \"{}\"\r\nstart \"\" \"{}\"\r\n", new_msi().display(), new_msi().display(), exe.display())
    } else if new_exe().exists() {
        format!("move /Y \"{}\" \"{}\" >nul\r\nstart \"\" \"{}\"\r\n", new_exe().display(), exe.display(), exe.display())
    } else {
        return Err("The update hasn't been downloaded.".into());
    };
    let script = std::env::temp_dir().join("lewd-vault-update.cmd");
    fs::write(&script, format!("@echo off\r\n{wait}{body}del \"%~f0\"\r\n")).map_err(err)?;
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        std::process::Command::new("cmd").args(["/C", &script.to_string_lossy()]).creation_flags(0x0800_0000).spawn().map_err(err)?;
    }
    app.exit(0);
    Ok(())
}
