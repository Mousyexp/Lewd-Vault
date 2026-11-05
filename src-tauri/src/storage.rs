// Where the vault lives. On first start (no config yet) the page shows a welcome screen with the
// places this PC can keep it: a cloud service's synced folder (Google Drive for desktop, OneDrive,
// Dropbox, iCloud), this PC next to the app, or any folder. In a cloud service the app makes its own
// folder, "Image Vault Desktop", and the service's own app uploads and syncs it, so the vault still
// reads from the local disk at full speed and works offline.
//
// The choice is kept in "Image Vault.config.json" next to the .exe (portable), or, when that folder
// can't be written, in %APPDATA%\Image Vault. IMAGE_VAULT_DATA still overrides everything (testing),
// IMAGE_VAULT_CONFIG points the config file elsewhere.

use super::{err, md5_hex, parse_rows, Res, Vault, INDEX_NAME};
use serde_json::{json, Value};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;

pub const CLOUD_FOLDER: &str = "Image Vault Desktop";
const LOCAL_FOLDER: &str = "Image Vault data";

pub(crate) fn exe_dir() -> PathBuf {
    std::env::current_exe().ok().and_then(|p| p.parent().map(Path::to_path_buf)).unwrap_or_default()
}
fn env_path(key: &str) -> Option<PathBuf> {
    std::env::var_os(key).map(PathBuf::from).filter(|p| !p.as_os_str().is_empty())
}
fn appdata_config() -> Option<PathBuf> {
    env_path("APPDATA").map(|a| a.join("Image Vault").join("config.json"))
}
fn config_paths() -> Vec<PathBuf> {
    if let Some(p) = env_path("IMAGE_VAULT_CONFIG") {
        return vec![p];
    }
    let mut v = vec![exe_dir().join("Image Vault.config.json")];
    v.extend(appdata_config());
    v
}

/// The data folder to open, and whether the welcome screen should ask where it goes.
pub struct Start {
    pub root: PathBuf,
    pub first_run: bool,
}
pub fn start() -> Start {
    if let Some(root) = env_path("IMAGE_VAULT_DATA") {
        return Start { root, first_run: false };
    }
    for p in config_paths() {
        let folder = fs::read_to_string(&p).ok().and_then(|t| serde_json::from_str::<Value>(&t).ok())
            .and_then(|v| v["dataFolder"].as_str().map(PathBuf::from));
        if let Some(root) = folder {
            return Start { root, first_run: false };
        }
    }
    Start { root: exe_dir().join(LOCAL_FOLDER), first_run: true }
}

fn save_config(root: &Path) -> Res<()> {
    let text = json!({ "dataFolder": root.display().to_string() }).to_string();
    let mut last = String::from("no place to keep the setting");
    for p in config_paths() {
        if let Some(dir) = p.parent() {
            let _ = fs::create_dir_all(dir);
        }
        match fs::write(&p, &text) {
            Ok(()) => return Ok(()),
            Err(e) => last = format!("{}: {e}", p.display()),
        }
    }
    Err(format!("Could not save where the vault lives ({last})"))
}

// ---------- cloud services' folders on this PC ----------

fn google_drive() -> Option<PathBuf> {
    // Google Drive for desktop: a drive letter with "My Drive" (streaming), or a mirrored folder
    for letter in b'D'..=b'Z' {
        let p = PathBuf::from(format!("{}:\\My Drive", letter as char));
        if p.is_dir() {
            return Some(p);
        }
    }
    let home = env_path("USERPROFILE")?;
    [home.join("My Drive"), home.join("Google Drive").join("My Drive"), home.join("Google Drive")].into_iter().find(|p| p.is_dir())
}
fn onedrive() -> Option<PathBuf> {
    ["OneDriveConsumer", "OneDrive", "OneDriveCommercial"].iter().filter_map(|k| env_path(k)).find(|p| p.is_dir())
        .or_else(|| env_path("USERPROFILE").map(|h| h.join("OneDrive")).filter(|p| p.is_dir()))
}
fn dropbox() -> Option<PathBuf> {
    for base in [env_path("LOCALAPPDATA"), env_path("APPDATA")].into_iter().flatten() {
        let info = fs::read_to_string(base.join("Dropbox").join("info.json")).ok().and_then(|t| serde_json::from_str::<Value>(&t).ok());
        if let Some(info) = info {
            for kind in ["personal", "business"] {
                if let Some(p) = info[kind]["path"].as_str().map(PathBuf::from).filter(|p| p.is_dir()) {
                    return Some(p);
                }
            }
        }
    }
    env_path("USERPROFILE").map(|h| h.join("Dropbox")).filter(|p| p.is_dir())
}
fn icloud() -> Option<PathBuf> {
    env_path("USERPROFILE").map(|h| h.join("iCloudDrive")).filter(|p| p.is_dir())
}

pub const DOWNLOADS: [(&str, &str); 4] = [
    ("gdrive", "https://www.google.com/drive/download/"),
    ("onedrive", "https://www.microsoft.com/microsoft-365/onedrive/download"),
    ("dropbox", "https://www.dropbox.com/install"),
    ("icloud", "https://apps.microsoft.com/detail/9pktq5699m62"),
];

/// How many items the vault in `folder` has, if there is one.
pub fn count_items(folder: &Path) -> Option<usize> {
    let text = fs::read_to_string(folder.join(INDEX_NAME)).ok()?;
    Some(parse_rows(&text).1.len())
}

fn same(a: &Path, b: &Path) -> bool {
    let canon = |p: &Path| fs::canonicalize(p).unwrap_or_else(|_| p.to_path_buf());
    canon(a) == canon(b)
}

fn option(id: &str, name: &str, base: Option<PathBuf>, folder: Option<PathBuf>, current: &Path) -> Value {
    let folder = folder.or_else(|| base.as_ref().map(|b| b.join(CLOUD_FOLDER)));
    let items = folder.as_deref().and_then(count_items);
    json!({
        "id": id, "name": name, "found": folder.is_some(),
        "base": base.map(|b| b.display().to_string()),
        "folder": folder.as_ref().map(|f| f.display().to_string()),
        "items": items,
        "current": folder.as_deref().map_or(false, |f| same(f, current)),
        "download": DOWNLOADS.iter().find(|(k, _)| *k == id).map(|(_, u)| *u)
    })
}

/// The places the welcome screen offers, with what's already there.
pub fn options(current: &Path) -> Value {
    let local = exe_dir().join(LOCAL_FOLDER);
    let mut list = vec![
        option("gdrive", "Google Drive", google_drive(), None, current),
        option("onedrive", "OneDrive", onedrive(), None, current),
        option("dropbox", "Dropbox", dropbox(), None, current),
        option("icloud", "iCloud Drive", icloud(), None, current),
        option("local", "This PC", Some(exe_dir()), Some(local.clone()), current),
    ];
    // a folder chosen earlier that's none of these
    if !list.iter().any(|o| o["current"] == json!(true)) {
        list.push(option("custom", "Your folder", current.parent().map(Path::to_path_buf), Some(current.to_path_buf()), current));
    }
    json!({ "options": list, "current": current.display().to_string(), "currentItems": count_items(current).unwrap_or(0) })
}

/// What a folder picked with "Another folder…" would hold the vault as: the folder itself when it
/// already has a vault (or is empty), otherwise an "Image Vault Desktop" folder inside it.
pub fn for_picked(picked: &Path, current: &Path) -> Value {
    let empty = fs::read_dir(picked).map(|mut d| d.next().is_none()).unwrap_or(true);
    let folder = if count_items(picked).is_some() || empty { picked.to_path_buf() } else { picked.join(CLOUD_FOLDER) };
    option("custom", "Your folder", picked.parent().map(Path::to_path_buf), Some(folder), current)
}

// ---------- moving the vault ----------

#[derive(Default)]
pub struct CopyState {
    pub done: AtomicU64,
    pub total: AtomicU64,
    pub status: Mutex<String>, // "", "copying", "done" or the error
}

fn files(dir: &Path, root: &Path, out: &mut Vec<(PathBuf, u64)>) {
    let Ok(entries) = fs::read_dir(dir) else { return };
    for e in entries.flatten() {
        let p = e.path();
        let rel = p.strip_prefix(root).unwrap_or(&p).to_string_lossy().replace('\\', "/");
        // never copied: the thumbnail cache (made again) and unfinished uploads
        if rel == "_app/cache" || rel == "_app/.incoming" || rel.ends_with(".part") {
            continue;
        }
        if p.is_dir() {
            files(&p, root, out);
        } else if let Ok(m) = e.metadata() {
            out.push((p, m.len()));
        }
    }
}

fn copy_library(from: &Path, to: &Path, state: &CopyState) -> Res<()> {
    let mut list = Vec::new();
    files(from, from, &mut list);
    state.total.store(list.iter().map(|(_, n)| n).sum(), Ordering::Relaxed);
    state.done.store(0, Ordering::Relaxed);
    for (p, n) in list {
        let dest = to.join(p.strip_prefix(from).map_err(err)?);
        if let Some(dir) = dest.parent() {
            fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
        }
        fs::copy(&p, &dest).map_err(|e| format!("Could not copy {}: {e}", p.display()))?;
        state.done.fetch_add(n, Ordering::Relaxed);
    }
    Ok(())
}

/// Use `target` from now on. When it has no vault yet and `copy` is set, the current library is
/// copied there first (the old folder is left as it was, as a backup). Returns whether the app
/// has to restart to open it.
pub fn use_folder(vault: &Vault, target: &Path, copy: bool) -> Res<Value> {
    if same(target, &vault.root) {
        save_config(target)?;
        return Ok(json!({ "restart": false }));
    }
    fs::create_dir_all(target).map_err(|e| format!("Could not make {}: {e}", target.display()))?;
    let has_vault = count_items(target).is_some();
    if copy && !has_vault && count_items(&vault.root).unwrap_or(0) > 0 {
        *vault.copy.status.lock().unwrap() = "copying".into();
        if let Err(e) = copy_library(&vault.root, target, &vault.copy) {
            *vault.copy.status.lock().unwrap() = e.clone();
            return Err(e);
        }
    }
    save_config(target)?;
    *vault.copy.status.lock().unwrap() = "done".into();
    Ok(json!({ "restart": true }))
}

pub fn copy_progress(vault: &Vault) -> Value {
    json!({
        "done": vault.copy.done.load(Ordering::Relaxed), "total": vault.copy.total.load(Ordering::Relaxed),
        "status": vault.copy.status.lock().unwrap().clone()
    })
}

/// Where this PC keeps what's made again anyway (card-size thumbnails, unfinished uploads): never
/// inside the vault, so a cloud service doesn't sync it.
pub fn local_dir(root: &Path, what: &str) -> PathBuf {
    let base = env_path("LOCALAPPDATA").unwrap_or_else(std::env::temp_dir).join("Image Vault");
    let key = &md5_hex(root.display().to_string().as_bytes())[..12];
    base.join(what).join(key)
}
