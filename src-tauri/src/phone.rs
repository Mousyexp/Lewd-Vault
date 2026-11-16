// The phones' native pieces, for the phone layer (mobile/ui/phone.js, phone-plus.js). One command,
// `phone_native(what, arg)`, so the page needs no plugin permissions of its own:
//
//   haptic      a tap felt in the hand: "select", "light", "medium", "heavy", "success", "warning", "error"
//   bioStatus   whether Face ID / Touch ID / a fingerprint can unlock the app, and which
//   bioAuth     ask for it (the reason is shown by the system); Ok(true) when it was you
//   inbox       pictures and videos shared to the app (the share sheet, "Open in"), waiting to be added
//   inboxDone   one of them is in the vault now (or was refused): remove it from the inbox
//   icon        the app's icon (and on Android its name): "default" or a preset (see ICONS)
//   secure      Android: no screenshots or screen recording of the app (FLAG_SECURE) while on
//   captured    iOS: is the screen being recorded or mirrored right now?
//   openPhotos  open the phone's Photos (iOS) or gallery (Android), to delete what was just added
//
// On the PC every one of them answers "not here" (null), and the page doesn't offer them.
//
// How the inbox gets filled: on Android the app's activity (mobile/android/MainActivity.kt, copied in
// by .github/workflows/android.yml) copies whatever is shared to it into files/share-inbox; on iOS a
// file opened with the app arrives as a RunEvent::Opened (see `opened`) and is copied into
// Documents/share-inbox, and the share extension (mobile/ios/ShareExtension) writes into the app
// group's share-inbox. The page asks for the list when it comes to the front and adds them through
// the same Add flow as a picked file, reading each from the media server (/inbox/<name>).
use serde_json::{json, Value};
use std::fs;
use std::path::PathBuf;

/// The presets for the app's icon (the files: art/icons/<id>.png, made by tools/make-icons.mjs).
/// On Android each is an activity alias with its own name; on iOS an alternate app icon (the name
/// can't change there).
pub const ICONS: &[(&str, &str)] = &[
    ("default", "Lewd Vault"), ("violet", "Lewd Vault"), ("mint", "Lewd Vault"), ("midnight", "Lewd Vault"), ("mono", "Lewd Vault"),
    ("calculator", "Calculator"), ("notes", "Notes"), ("weather", "Weather"), ("clock", "Clock"), ("files", "Files"), ("compass", "Compass"),
];

#[tauri::command]
pub async fn phone_native(app: tauri::AppHandle, what: String, arg: Value) -> Result<Value, String> {
    let _ = &app;
    match what.as_str() {
        "inbox" => Ok(inbox_list()),
        "inboxDone" => {
            let name = arg.as_str().unwrap_or("");
            if let Some(p) = inbox_file(name) {
                let _ = fs::remove_file(p);
            }
            Ok(json!(true))
        }
        #[cfg(mobile)]
        "haptic" => {
            use tauri_plugin_haptics::{HapticsExt, ImpactFeedbackStyle, NotificationFeedbackType};
            let h = app.haptics();
            let r = match arg.as_str().unwrap_or("select") {
                "light" => h.impact_feedback(ImpactFeedbackStyle::Light),
                "medium" => h.impact_feedback(ImpactFeedbackStyle::Medium),
                "heavy" => h.impact_feedback(ImpactFeedbackStyle::Heavy),
                "success" => h.notification_feedback(NotificationFeedbackType::Success),
                "warning" => h.notification_feedback(NotificationFeedbackType::Warning),
                "error" => h.notification_feedback(NotificationFeedbackType::Error),
                _ => h.selection_feedback(),
            };
            r.map(|_| json!(true)).map_err(|e| e.to_string())
        }
        #[cfg(mobile)]
        "bioStatus" => {
            use tauri_plugin_biometric::{BiometricExt, BiometryType};
            match app.biometric().status() {
                Ok(s) => Ok(json!({
                    "available": s.is_available,
                    "kind": match s.biometry_type { BiometryType::FaceID => "face", BiometryType::TouchID => "touch", BiometryType::None => "" },
                    "error": s.error,
                })),
                Err(e) => Ok(json!({ "available": false, "kind": "", "error": e.to_string() })),
            }
        }
        #[cfg(mobile)]
        "bioAuth" => {
            use tauri_plugin_biometric::{AuthOptions, BiometricExt};
            let reason = arg.as_str().unwrap_or("Open Lewd Vault").to_string();
            let options = AuthOptions { allow_device_credential: false, cancel_title: Some("Use PIN".into()), fallback_title: Some("Use PIN".into()), ..Default::default() };
            app.biometric().authenticate(reason, options).map(|_| json!(true)).map_err(|e| e.to_string())
        }
        "icon" => {
            let id = arg.as_str().unwrap_or("default").to_string();
            if !ICONS.iter().any(|(k, _)| *k == id) {
                return Err("No such icon".into());
            }
            #[cfg(target_os = "ios")]
            {
                let name = if id == "default" { None } else { Some(format!("AppIcon-{id}")) };
                app.run_on_main_thread(move || ios::set_icon(name.as_deref())).map_err(|e| e.to_string())?;
                return Ok(json!(true));
            }
            #[cfg(target_os = "android")]
            {
                // the activity reads it when the app goes to the background (switching the launcher
                // entry while the app is in front would close it)
                return native_flag("icon", json!(id)).map(|_| json!(true));
            }
            #[allow(unreachable_code)]
            Ok(Value::Null)
        }
        "secure" => {
            #[cfg(target_os = "android")]
            {
                return native_flag("secure", json!(arg.as_bool().unwrap_or(false))).map(|_| json!(true));
            }
            #[allow(unreachable_code)]
            Ok(Value::Null)
        }
        "captured" => {
            #[cfg(target_os = "ios")]
            {
                let (tx, rx) = std::sync::mpsc::channel();
                app.run_on_main_thread(move || { let _ = tx.send(ios::captured()); }).map_err(|e| e.to_string())?;
                return Ok(json!(rx.recv_timeout(std::time::Duration::from_secs(2)).unwrap_or(false)));
            }
            #[allow(unreachable_code)]
            Ok(json!(false))
        }
        "openPhotos" => {
            #[cfg(target_os = "ios")]
            return tauri_plugin_opener::open_url("photos-redirect://", None::<&str>).map(|_| json!(true)).map_err(|e| e.to_string());
            #[cfg(target_os = "android")]
            return tauri_plugin_opener::open_url("content://media/external/images/media", None::<&str>).map(|_| json!(true)).map_err(|e| e.to_string());
            #[allow(unreachable_code)]
            Ok(Value::Null)
        }
        _ => Ok(Value::Null),
    }
}

// ---------- the inbox ----------
fn inbox_dirs() -> Vec<PathBuf> {
    #[allow(unused_mut)]
    let mut dirs: Vec<PathBuf> = Vec::new();
    #[cfg(mobile)]
    dirs.push(crate::storage::phone_documents().join("share-inbox"));
    #[cfg(target_os = "ios")]
    {
        // "Open in" / "Copy to" from another app, the way iOS hands files over without the share extension
        dirs.push(crate::storage::phone_documents().join("Inbox"));
        if let Some(group) = ios::group_dir() {
            dirs.push(group.join("share-inbox"));
        }
    }
    dirs
}
const MEDIA: &[&str] = &["jpg", "jpeg", "png", "gif", "webp", "heic", "heif", "avif", "bmp", "mp4", "mov", "m4v", "webm", "mkv"];
fn inbox_list() -> Value {
    let mut out = Vec::new();
    for dir in inbox_dirs() {
        let Ok(rd) = fs::read_dir(&dir) else { continue };
        for e in rd.flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            let ext = name.rsplit('.').next().unwrap_or("").to_ascii_lowercase();
            if name.starts_with('.') || !MEDIA.contains(&ext.as_str()) {
                continue;
            }
            let size = e.metadata().map(|m| m.len()).unwrap_or(0);
            if size == 0 {
                continue; // (still being written)
            }
            out.push(json!({ "name": name, "size": size }));
        }
    }
    json!(out)
}
/// One file in the inbox, by its bare name (nothing that could step outside it).
pub fn inbox_file(name: &str) -> Option<PathBuf> {
    if name.is_empty() || name.contains('/') || name.contains('\\') || name.starts_with('.') {
        return None;
    }
    inbox_dirs().into_iter().map(|d| d.join(name)).find(|p| p.is_file())
}
pub fn inbox_type(name: &str) -> &'static str {
    match name.rsplit('.').next().unwrap_or("").to_ascii_lowercase().as_str() {
        "jpg" | "jpeg" => "image/jpeg",
        "png" => "image/png",
        "gif" => "image/gif",
        "webp" => "image/webp",
        "heic" | "heif" => "image/heic",
        "avif" => "image/avif",
        "bmp" => "image/bmp",
        "mp4" | "m4v" => "video/mp4",
        "mov" => "video/quicktime",
        "webm" => "video/webm",
        "mkv" => "video/x-matroska",
        _ => "application/octet-stream",
    }
}

/// iOS: files opened with the app ("Open in Lewd Vault", a file tapped in Files): copied into the
/// inbox, where the page picks them up.
#[allow(dead_code)]
pub fn opened(urls: Vec<tauri::Url>) {
    #[cfg(target_os = "ios")]
    for u in urls {
        ios::take(&u);
    }
    #[cfg(not(target_os = "ios"))]
    let _ = urls;
}

// ---------- Android: settings the activity reads (files/phone-native.json) ----------
#[cfg(target_os = "android")]
fn native_flag(key: &str, value: Value) -> Result<(), String> {
    let path = crate::storage::phone_documents().join("phone-native.json");
    let mut v: Value = fs::read_to_string(&path).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_else(|| json!({}));
    v[key] = value;
    if let Some(dir) = path.parent() {
        fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    fs::write(&path, v.to_string()).map_err(|e| e.to_string())
}

// ---------- iOS: UIKit through the Objective-C runtime ----------
#[cfg(target_os = "ios")]
mod ios {
    use objc2::msg_send;
    use objc2::runtime::{AnyClass, AnyObject, Bool};
    use objc2_foundation::NSString;
    use std::path::PathBuf;

    pub const GROUP: &str = "group.local.imagevault.desktop";

    pub fn set_icon(name: Option<&str>) {
        unsafe {
            let Some(cls) = AnyClass::get(c"UIApplication") else { return };
            let app: *mut AnyObject = msg_send![cls, sharedApplication];
            if app.is_null() {
                return;
            }
            let ok: Bool = msg_send![app, supportsAlternateIcons];
            if !ok.as_bool() {
                return;
            }
            let ns = name.map(NSString::from_str);
            let ptr: *const NSString = ns.as_deref().map_or(std::ptr::null(), |s| s as *const NSString);
            let done: Option<&block2::Block<dyn Fn(*mut AnyObject)>> = None;
            let _: () = msg_send![app, setAlternateIconName: ptr, completionHandler: done];
        }
    }
    pub fn captured() -> bool {
        unsafe {
            let Some(cls) = AnyClass::get(c"UIScreen") else { return false };
            let screen: *mut AnyObject = msg_send![cls, mainScreen];
            if screen.is_null() {
                return false;
            }
            let b: Bool = msg_send![screen, isCaptured];
            b.as_bool()
        }
    }
    /// The app group's folder, shared with the share extension (None when the build has no group).
    /// A sideloading tool may rename the group (adding its team ID): the name this build was signed
    /// with is in its provisioning profile, so that one is tried first.
    pub fn group_dir() -> Option<PathBuf> {
        let mut ids = vec![GROUP.to_string()];
        if let Some(signed) = profile_group() {
            ids.insert(0, signed);
        }
        ids.into_iter().find_map(|id| container(&id))
    }
    fn profile_group() -> Option<String> {
        let bundle = std::env::current_exe().ok()?.parent()?.to_path_buf();
        let bytes = std::fs::read(bundle.join("embedded.mobileprovision")).ok()?;
        let text: String = bytes.iter().map(|&b| b as char).collect();
        let key = text.find("<key>com.apple.security.application-groups</key>")?;
        let rest = &text[key..];
        let open = rest.find("<string>")? + "<string>".len();
        let close = rest[open..].find("</string>")?;
        Some(rest[open..open + close].trim().to_string())
    }
    fn container(id: &str) -> Option<PathBuf> {
        unsafe {
            let cls = AnyClass::get(c"NSFileManager")?;
            let fm: *mut AnyObject = msg_send![cls, defaultManager];
            let id = NSString::from_str(id);
            let url: *mut AnyObject = msg_send![fm, containerURLForSecurityApplicationGroupIdentifier: &*id];
            if url.is_null() {
                return None;
            }
            let path: *mut NSString = msg_send![url, path];
            if path.is_null() {
                return None;
            }
            Some(PathBuf::from((*path).to_string()))
        }
    }
    /// Copy a file opened with the app into the inbox (it may be outside the app's container, so
    /// with the security-scoped access the system grants for it).
    pub fn take(u: &tauri::Url) {
        let Ok(src) = u.to_file_path() else { return };
        let dir = crate::storage::phone_documents().join("share-inbox");
        if std::fs::create_dir_all(&dir).is_err() {
            return;
        }
        let name = src.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| "shared".into());
        let dst = dir.join(format!("{}-{}", std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis()).unwrap_or(0), name));
        unsafe {
            let Some(cls) = AnyClass::get(c"NSURL") else { return };
            let s = NSString::from_str(u.as_str());
            let nsurl: *mut AnyObject = msg_send![cls, URLWithString: &*s];
            let scoped: Bool = if nsurl.is_null() { Bool::NO } else { msg_send![nsurl, startAccessingSecurityScopedResource] };
            let _ = std::fs::copy(&src, &dst);
            if scoped.as_bool() {
                let _: () = msg_send![nsurl, stopAccessingSecurityScopedResource];
            }
        }
        // a file iOS put into Documents/Inbox for us is ours to tidy away
        if src.starts_with(crate::storage::phone_documents().join("Inbox")) {
            let _ = std::fs::remove_file(&src);
        }
    }
}
