// Where the vault lives. On first start (no config yet) the page shows a welcome screen with the
// places this PC can keep it: a cloud service's synced folder (Google Drive for desktop, OneDrive,
// Dropbox, iCloud), this PC next to the app, or any folder. In a cloud service the app makes its own
// folder, "Image Vault Desktop", and the service's own app uploads and syncs it, so the vault still
// reads from the local disk at full speed and works offline.
//
// The choice is kept in "Image Vault.config.json" next to the .exe (portable), or, when that folder
// can't be written, in %APPDATA%\Image Vault. IMAGE_VAULT_DATA still overrides everything (testing),
// IMAGE_VAULT_CONFIG points the config file elsewhere.
//
// Untraceable (crypt.rs): the vault in a hidden "data.temp" folder, encrypted: next to the .exe for
// the portable copy, in Documents for the installed one (where it can't write next to itself), or a
// place you pick; everything this PC would otherwise keep elsewhere (caches, the browser engine's
// data) goes in there too. Choosing it can also clear what the app left on this PC
// before ("scrub", done at the next start, when nothing holds those folders open).

use super::{err, md5_hex, parse_rows, Res, Vault, INDEX_NAME};
use serde_json::{json, Value};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;

pub const CLOUD_FOLDER: &str = "Image Vault Desktop";
const LOCAL_FOLDER: &str = "Image Vault data";
pub const HIDDEN: &str = "data.temp";

/// The untraceable vault in use: <somewhere>\data.temp\vault (set at start, see main).
static HIDDEN_AT: std::sync::RwLock<Option<PathBuf>> = std::sync::RwLock::new(None);
pub fn set_hidden(dir: PathBuf) {
    *HIDDEN_AT.write().unwrap() = Some(dir);
}
/// Untraceable mode's folder ("data.temp"): the one in use, else where a new one goes by default:
/// next to the app (the portable copy), or in Documents when the app can't write there (installed).
pub fn hidden_dir() -> PathBuf {
    HIDDEN_AT.read().unwrap().clone().unwrap_or_else(default_hidden)
}
fn default_hidden() -> PathBuf {
    if exe_dir_writable() {
        return exe_dir().join(HIDDEN);
    }
    env_path("USERPROFILE").map(|u| u.join("Documents")).filter(|d| d.is_dir())
        .or_else(|| env_path("LOCALAPPDATA"))
        .unwrap_or_else(exe_dir)
        .join(HIDDEN)
}
/// Where an untraceable vault goes in a folder you picked: its data.temp, or the folder itself
/// when you picked a data.temp (or the vault inside one).
pub fn untraceable_in(picked: &Path) -> PathBuf {
    let name = |p: &Path| p.file_name().map(|n| n.to_string_lossy().to_lowercase()).unwrap_or_default();
    if name(picked) == "vault" && picked.parent().is_some_and(|p| name(p) == HIDDEN) {
        return picked.to_path_buf();
    }
    if name(picked) == HIDDEN {
        return picked.join("vault");
    }
    picked.join(HIDDEN).join("vault")
}
/// Where this PC keeps the app's own things (caches, Discover's sources, the Drive sign-in).
pub fn local_base() -> PathBuf {
    if crate::crypt::untraceable() {
        return hidden_dir().join("local");
    }
    env_path("LOCALAPPDATA").unwrap_or_else(std::env::temp_dir).join("Image Vault")
}
/// Hidden and "system", so Explorer doesn't show it even with hidden files shown.
fn hide(p: &Path) {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        let _ = std::process::Command::new("attrib").args(["+h", "+s"]).arg(p).creation_flags(0x0800_0000).status();
    }
    #[cfg(not(windows))]
    let _ = p;
}

pub(crate) fn exe_dir() -> PathBuf {
    std::env::current_exe().ok().and_then(|p| p.parent().map(Path::to_path_buf)).unwrap_or_default()
}
/// Can this app write next to itself? Not when installed (Program Files): then the vault and the
/// setting live in the user's own folders instead.
pub(crate) fn exe_dir_writable() -> bool {
    let probe = exe_dir().join(format!(".lewd-vault-probe-{}", std::process::id()));
    let ok = fs::write(&probe, b"").is_ok();
    let _ = fs::remove_file(&probe);
    ok
}
/// Where "This PC" keeps the vault: next to the app (portable), else Documents\Image Vault data.
fn local_home() -> PathBuf {
    if exe_dir_writable() {
        return exe_dir();
    }
    env_path("USERPROFILE").map(|u| u.join("Documents")).filter(|d| d.is_dir())
        .or_else(|| env_path("LOCALAPPDATA"))
        .unwrap_or_else(exe_dir)
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
    pub untraceable: bool,
    pub drive_setup: bool, // chosen with "Google Drive in your browser": the page offers the sign-in
}
pub fn start() -> Start {
    if let Some(root) = env_path("IMAGE_VAULT_DATA") {
        return Start { root, first_run: false, untraceable: false, drive_setup: false };
    }
    for p in config_paths() {
        let Some(v) = fs::read_to_string(&p).ok().and_then(|t| serde_json::from_str::<Value>(&t).ok()) else { continue };
        let Some(root) = v["dataFolder"].as_str().map(PathBuf::from) else { continue };
        let untraceable = v["untraceable"] == json!(true);
        let scrub: Vec<PathBuf> = v["scrub"].as_array().into_iter().flatten().filter_map(|x| x.as_str().map(PathBuf::from)).collect();
        if !scrub.is_empty() {
            std::thread::spawn(move || clear_traces(scrub));
        }
        return Start { root, first_run: false, untraceable, drive_setup: v["driveSetup"] == json!(true) };
    }
    Start { root: local_home().join(LOCAL_FOLDER), first_run: true, untraceable: false, drive_setup: false }
}

/// A folder that may be removed wholesale: not a drive or the user folder, not one holding the app.
fn removable(p: &Path) -> bool {
    p.components().count() >= 3 && !exe_dir().starts_with(p) && env_path("USERPROFILE").map_or(true, |h| !h.starts_with(p))
}
/// The "scrub" a switch to Untraceable asked for: retried for a minute (the last copy of the app and
/// its browser engine may still be closing), then taken off the config.
fn clear_traces(mut list: Vec<PathBuf>) {
    for _ in 0..20 {
        list.retain(|p| p.exists() && removable(p) && fs::remove_dir_all(p).is_err());
        if list.is_empty() {
            break;
        }
        std::thread::sleep(std::time::Duration::from_secs(3));
    }
    edit_config(|v| {
        if list.is_empty() {
            v.as_object_mut().map(|o| o.remove("scrub"));
        } else {
            v["scrub"] = json!(list.iter().map(|p| p.display().to_string()).collect::<Vec<_>>());
        }
    });
}

/// Changes the config in use where it is (its other settings stay).
pub fn edit_config(change: impl FnOnce(&mut Value)) {
    for p in config_paths() {
        let Some(mut v) = fs::read_to_string(&p).ok().and_then(|t| serde_json::from_str::<Value>(&t).ok()) else { continue };
        change(&mut v);
        let hidden = v["untraceable"] == json!(true);
        if hidden {
            let _ = fs::remove_file(&p);
        }
        if fs::write(&p, v.to_string()).is_ok() && hidden {
            hide(&p);
        }
        return;
    }
}

fn save_config(root: &Path, untraceable: bool, scrub: &[PathBuf], drive_setup: bool) -> Res<()> {
    let mut v = json!({ "dataFolder": root.display().to_string() });
    if drive_setup {
        v["driveSetup"] = json!(true);
    }
    if untraceable {
        v["untraceable"] = json!(true);
    }
    if !scrub.is_empty() {
        v["scrub"] = json!(scrub.iter().map(|p| p.display().to_string()).collect::<Vec<_>>());
    }
    let text = v.to_string();
    let mut last = String::from("no place to keep the setting");
    // (next to the app when it can write there, the portable copy; installed, in %APPDATA%)
    for p in config_paths() {
        if let Some(dir) = p.parent() {
            let _ = fs::create_dir_all(dir);
        }
        if untraceable {
            let _ = fs::remove_file(&p); // (a hidden file can't be overwritten on Windows)
        }
        match fs::write(&p, &text) {
            Ok(()) => {
                if untraceable {
                    hide(&p);
                }
                return Ok(());
            }
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
    let text = crate::crypt::read_to_string(&folder.join(INDEX_NAME)).ok()?; // (an untraceable vault: only when it's the open one)
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
        "exists": folder.as_deref().map_or(false, |f| f.join(INDEX_NAME).exists()),
        "download": DOWNLOADS.iter().find(|(k, _)| *k == id).map(|(_, u)| *u)
    })
}

/// The places the welcome screen offers, with what's already there.
pub fn options(current: &Path) -> Value {
    let local = local_home().join(LOCAL_FOLDER);
    let mut list = vec![
        option("gdrive", "Google Drive", google_drive(), None, current),
        option("onedrive", "OneDrive", onedrive(), None, current),
        option("dropbox", "Dropbox", dropbox(), None, current),
        option("icloud", "iCloud Drive", icloud(), None, current),
        option("local", "This PC", Some(local_home()), Some(local.clone()), current),
    ];
    // Untraceable: the one in use, or a new one next to the app / in Documents (another place can be
    // picked on its page)
    let hidden = hidden_dir();
    let mut untr = option("untraceable", "Untraceable", hidden.parent().map(Path::to_path_buf), Some(hidden.join("vault")), current);
    untr["synced"] = json!(synced_folder(&hidden));
    list.push(untr);
    // a folder chosen earlier that's none of these
    if !list.iter().any(|o| o["current"] == json!(true)) {
        list.push(option("custom", "Your folder", current.parent().map(Path::to_path_buf), Some(current.to_path_buf()), current));
    }
    json!({ "options": list, "current": current.display().to_string(), "currentItems": count_items(current).unwrap_or(0),
            "untraceable": crate::crypt::untraceable() })
}

/// What a folder picked with "Another folder…" would hold the vault as: the folder itself when it
/// already has a vault (or is empty), otherwise an "Image Vault Desktop" folder inside it.
/// Whether a folder is inside one a cloud service uploads (OneDrive often backs up Documents).
fn synced_folder(p: &Path) -> bool {
    let s = p.display().to_string().to_lowercase();
    ["onedrive", "dropbox", "google drive", "my drive", "icloud"].iter().any(|k| s.contains(k))
}

/// "Another place…" on the Untraceable page.
pub fn untraceable_for_picked(picked: &Path, current: &Path) -> Value {
    let folder = untraceable_in(picked);
    let mut o = option("untraceable", "Untraceable", folder.parent().and_then(Path::parent).map(Path::to_path_buf), Some(folder.clone()), current);
    o["synced"] = json!(synced_folder(&folder));
    o
}

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

fn copy_library(from: &Path, to: &Path, state: &CopyState, encrypt: Option<&[u8; 32]>) -> Res<()> {
    let mut list = Vec::new();
    files(from, from, &mut list);
    state.total.store(list.iter().map(|(_, n)| n).sum(), Ordering::Relaxed);
    state.done.store(0, Ordering::Relaxed);
    for (p, n) in list {
        let dest = to.join(p.strip_prefix(from).map_err(err)?);
        if let Some(dir) = dest.parent() {
            fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
        }
        // (read decrypted when it comes from an untraceable vault; encrypted when it goes into one)
        crate::crypt::copy(&p, &dest, encrypt).map_err(|e| format!("Could not copy {}: {e}", p.display()))?;
        state.done.fetch_add(n, Ordering::Relaxed);
    }
    Ok(())
}

/// Use `target` from now on. When it has no vault yet and `copy` is set, the current library is
/// copied there first (the old folder is left as it was, as a backup, unless Untraceable was asked
/// to delete it). Returns whether the app has to restart to open it.
/// `opts` (the Untraceable choice): { untraceable, password, scrub, deleteOld }.
pub fn use_folder(vault: &Vault, target: &Path, copy: bool, opts: &Value) -> Res<Value> {
    let untraceable = opts["untraceable"] == json!(true);
    let drive_setup = opts["driveSetup"] == json!(true);
    if same(target, &vault.root) {
        save_config(target, crate::crypt::untraceable(), &[], drive_setup)?;
        return Ok(json!({ "restart": drive_setup }));
    }
    let mut key = None;
    if untraceable {
        if !target.parent().is_some_and(|p| p.file_name().is_some_and(|n| n.to_string_lossy().eq_ignore_ascii_case(HIDDEN))) {
            return Err("An untraceable vault lives in a data.temp folder".into());
        }
        let pw = opts["password"].as_str().unwrap_or("");
        if pw.chars().count() < 6 {
            return Err("Use a password of at least 6 characters.".into());
        }
        let k = crate::crypt::derive(pw);
        if let Ok(b) = fs::read(target.join("_app").join("key.check")) {
            if !crate::crypt::check_ok(&k, &b) {
                return Err("There's already an untraceable vault here, with another password.".into());
            }
        }
        key = Some(k);
    }
    fs::create_dir_all(target).map_err(|e| format!("Could not make {}: {e}", target.display()))?;
    if untraceable {
        hide(target.parent().unwrap());
    }
    let has_vault = target.join(INDEX_NAME).exists();
    let mut copied = false;
    if copy && !has_vault && count_items(&vault.root).unwrap_or(0) > 0 {
        *vault.copy.status.lock().unwrap() = "copying".into();
        if let Err(e) = copy_library(&vault.root, target, &vault.copy, key.as_ref()) {
            *vault.copy.status.lock().unwrap() = e.clone();
            return Err(e);
        }
        copied = true;
    }
    // a new place without a vault still takes this one's settings (theme, the emergency key...)
    let settings = vault.app_dir("settings.json");
    if !has_vault && !copied && settings.exists() {
        let to = target.join("_app").join("settings.json");
        if fs::create_dir_all(to.parent().unwrap()).is_ok() {
            let _ = crate::crypt::copy(&settings, &to, key.as_ref());
        }
    }
    let mut scrub = Vec::new();
    if let Some(k) = key {
        let check = target.join("_app").join("key.check");
        if !check.exists() {
            fs::create_dir_all(check.parent().unwrap()).map_err(err)?;
            fs::write(&check, crate::crypt::check_bytes_for(&k)).map_err(err)?;
        }
        if opts["scrub"] == json!(true) {
            // what the app kept on this PC in the normal modes: caches, Discover's sources, the Drive
            // sign-in, the browser engine's data (with its cache of pictures), the other config
            scrub.extend(env_path("LOCALAPPDATA").map(|l| l.join("Image Vault")));
            scrub.extend(env_path("LOCALAPPDATA").map(|l| l.join("local.imagevault.desktop")));
            // (%APPDATA%\Image Vault only when the config goes next to the app: installed, it lives there)
            if exe_dir_writable() {
                scrub.extend(appdata_config().and_then(|c| c.parent().map(Path::to_path_buf)));
            }
        }
        if opts["deleteOld"] == json!(true) && copied && !crate::crypt::untraceable() && removable(&vault.root) {
            scrub.push(vault.root.clone());
        }
    }
    save_config(target, untraceable, &scrub, drive_setup)?;
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
    let base = local_base();
    let key = &md5_hex(root.display().to_string().as_bytes())[..12];
    base.join(what).join(key)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn untraceable_places() {
        let base = Path::new("x").join("Docs");
        assert_eq!(untraceable_in(&base), base.join("data.temp").join("vault"));
        assert_eq!(untraceable_in(&base.join("data.temp")), base.join("data.temp").join("vault"));
        assert_eq!(untraceable_in(&base.join("data.temp").join("vault")), base.join("data.temp").join("vault"));
        assert!(synced_folder(Path::new("C:/Users/a/OneDrive - Acme/Documents/data.temp")));
        assert!(!synced_folder(Path::new("C:/Users/a/Documents/data.temp")));
    }
}
