#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

// Image Vault desktop backend: the same calls as Code.gs, over a plain folder next to the .exe
// instead of Google Drive. The layout and file names are the ones Code.gs uses, so the folder
// stays readable without the app (and a copy of the Drive folder can be opened by it):
//   Image Vault data/
//     index.csv, views.csv
//     <category>/000123_261016_v073_<category>.mp4
//     _app/thumbs/000123.webp, _app/thumbs/pack-*.bin, _app/clips/000123.mp4, _app/streams/000123.json
//     _app/settings.json, _app/state.json (the next unique number)
//
// File ids the page sees (Drive file ids in the web version) say where the file is:
//   item-000123               the item with that number (found through index.csv, so renames keep the id)
//   clips-<base64url name>    a file in _app/clips (likewise thumbs-, streams-)
//   file-<base64url path>     any other file under the data folder
//   anything else             a Drive id from a copied Drive folder, found through the index row naming it
// Files are read through the vault:// protocol (http://vault.localhost/<id> in WebView2), which
// answers byte-range requests for the video streamer and the thumbnail packs.

use base64::{engine::general_purpose::URL_SAFE_NO_PAD as B64, Engine};
use chrono::{DateTime, Local, SecondsFormat, Utc};
use percent_encoding::{percent_decode_str, utf8_percent_encode, NON_ALPHANUMERIC};
use serde::Serialize;
use serde_json::{json, Value};
use std::collections::{HashMap, HashSet};
use std::fs;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Component, Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::SystemTime;
use tauri::http::{response::Builder as ResponseBuilder, Request as HttpRequest, Response};
use tauri::{Manager, State};

mod discreet;
mod crypt;
mod dupes;
mod excel;
mod fastlane;
mod gdrive;
mod maint;
mod pack;
mod storage;
mod web;

const INDEX_NAME: &str = "index.csv";
const BACKUP_DAYS: usize = 14;
const VIEWS_NAME: &str = "views.csv";
const APP_NAME: &str = "_app";
// Discreet names (discreet.rs): every item in one neutral folder, named by its number only
static DISCREET: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
const MEDIA_FOLDER: &str = "Media";

const APP_FOLDERS: [&str; 4] = ["clips", "thumbs", "streams", "audio"]; // (audio: the Audio section's files)
const HEADERS: [&str; 21] = [
    "id", "uid", "file", "original_name", "category", "kind", "nsfw", "quality", "added", "width", "height",
    "duration", "preview_id", "thumb", "thumbhash", "stream_id", "thumb_size", "collection", "collection_cover",
    "tags", "source",
];
const VIEW_HEADERS: [&str; 7] = ["id", "uid", "views", "last_viewed", "appreciations", "last_appreciated", "seconds_viewed"];
const KINDS: [&str; 3] = ["image", "gif", "video"];

type Res<T> = Result<T, String>;
fn err<E: std::fmt::Display>(e: E) -> String {
    e.to_string()
}

#[derive(Clone, Default, Serialize)]
#[serde(rename_all = "camelCase")]
struct Item {
    id: String,
    uid: String,
    file: String,
    name: String,
    category: String,
    kind: String,
    nsfw: i64,
    quality: i64,
    added: String,
    width: i64,
    height: i64,
    duration: f64,
    preview_id: String,
    thumb: String,
    thumbhash: String,
    stream_id: String,
    thumb_size: String,
    collection: String,
    collection_cover: bool,
    tags: String, // booru-style: space-separated, e.g. "artist:someone blue_hair smile"
    source: String, // where it came from (a Discover post's page), if anywhere
}

#[derive(Clone, Default, Serialize)]
#[serde(rename_all = "camelCase")]
struct ViewRow {
    id: String,
    uid: String,
    views: i64,
    last: String,
    likes: i64,
    last_like: String,
    seconds: f64, // time the item was on screen (full view or slideshow), see record_time
}

// ---------- CSV (RFC 4180, UTF-8 with a byte-order mark, like csvText_ in Code.gs) ----------

fn parse_csv(text: &str) -> Vec<Vec<String>> {
    let text = text.strip_prefix('\u{feff}').unwrap_or(text);
    let (mut rows, mut row, mut cell, mut quoted) = (Vec::new(), Vec::new(), String::new(), false);
    let mut chars = text.chars().peekable();
    while let Some(ch) = chars.next() {
        if quoted {
            if ch == '"' {
                if chars.peek() == Some(&'"') {
                    cell.push('"');
                    chars.next();
                } else {
                    quoted = false;
                }
            } else {
                cell.push(ch);
            }
        } else if ch == '"' {
            quoted = true;
        } else if ch == ',' {
            row.push(std::mem::take(&mut cell));
        } else if ch == '\r' || ch == '\n' {
            if ch == '\r' && chars.peek() == Some(&'\n') {
                chars.next();
            }
            row.push(std::mem::take(&mut cell));
            rows.push(std::mem::take(&mut row));
        } else {
            cell.push(ch);
        }
    }
    if !cell.is_empty() || !row.is_empty() {
        row.push(cell);
        rows.push(row);
    }
    rows
}

// The header (column name -> position) and the rows, so columns are looked up by name and older
// files with fewer columns still read fine.
fn parse_rows(text: &str) -> (HashMap<String, usize>, Vec<Vec<String>>) {
    let mut rows = parse_csv(text).into_iter();
    let head = rows.next().unwrap_or_default().into_iter().enumerate().map(|(k, h)| (h, k)).collect();
    (head, rows.filter(|r| r.first().map_or(false, |c| !c.is_empty())).collect())
}
struct Row<'a> {
    head: &'a HashMap<String, usize>,
    cells: Vec<String>,
}
impl Row<'_> {
    fn get(&self, column: &str) -> Option<&String> {
        self.head.get(column).and_then(|&k| self.cells.get(k))
    }
}

fn csv_text(rows: &[Vec<String>]) -> String {
    let cell = |s: &String| {
        if s.contains(['"', ',', '\r', '\n']) { format!("\"{}\"", s.replace('"', "\"\"")) } else { s.clone() }
    };
    let lines: Vec<String> = rows.iter().map(|r| r.iter().map(cell).collect::<Vec<_>>().join(",")).collect();
    format!("\u{feff}{}\r\n", lines.join("\r\n"))
}

fn header_row(h: &[&str]) -> Vec<String> {
    h.iter().map(|s| s.to_string()).collect()
}

// Numbers written the way JavaScript prints them (12 not 12.0), empty for 0 like `x || ''`.
fn num_or_empty(f: f64) -> String {
    if f == 0.0 { String::new() } else if f.fract() == 0.0 { format!("{}", f as i64) } else { format!("{}", f) }
}

fn to_csv(items: &[Item]) -> String {
    let mut rows = vec![header_row(&HEADERS)];
    for i in items {
        rows.push(vec![
            i.id.clone(), i.uid.clone(), i.file.clone(), i.name.clone(), i.category.clone(), i.kind.clone(),
            i.nsfw.to_string(), i.quality.to_string(), i.added.clone(), num_or_empty(i.width as f64),
            num_or_empty(i.height as f64), num_or_empty(i.duration), i.preview_id.clone(), i.thumb.clone(),
            i.thumbhash.clone(), i.stream_id.clone(), i.thumb_size.clone(), i.collection.clone(),
            if i.collection_cover { "1".into() } else { String::new() },
            i.tags.clone(),
            i.source.clone(),
        ]);
    }
    csv_text(&rows)
}

fn views_csv(rows: &[ViewRow]) -> String {
    let mut out = vec![header_row(&VIEW_HEADERS)];
    for v in rows {
        out.push(vec![v.id.clone(), v.uid.clone(), v.views.to_string(), v.last.clone(), v.likes.to_string(), v.last_like.clone(), num_or_empty((v.seconds * 10.0).round() / 10.0)]);
    }
    csv_text(&out)
}

fn md5_hex(bytes: &[u8]) -> String {
    format!("{:x}", md5::compute(bytes))
}

// ---------- values from the page ----------

fn jnum(v: &Value) -> f64 {
    match v {
        Value::Number(n) => n.as_f64().unwrap_or(f64::NAN),
        Value::String(s) if s.trim().is_empty() => 0.0,
        Value::String(s) => s.trim().parse().unwrap_or(f64::NAN),
        Value::Bool(b) => *b as i64 as f64,
        Value::Null => 0.0,
        _ => f64::NAN,
    }
}
fn int_of(s: &str) -> i64 {
    s.trim().parse::<f64>().ok().filter(|f| f.is_finite()).map_or(0, |f| f as i64)
}
fn str_of(v: &Value) -> String {
    match v {
        Value::String(s) => s.clone(),
        Value::Number(n) => n.to_string(),
        Value::Bool(b) => b.to_string(),
        _ => String::new(),
    }
}
fn field(v: &Value, k: &str) -> String {
    str_of(v.get(k).unwrap_or(&Value::Null))
}
fn arr(v: &Value) -> Vec<Value> {
    v.as_array().cloned().unwrap_or_default()
}
fn id_ok(s: &str) -> bool {
    !s.is_empty() && s.len() <= 512 && s.chars().all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-')
}
fn id_or_empty(s: String) -> String {
    if id_ok(&s) { s } else { String::new() }
}
fn is_uid(s: &str) -> bool {
    s.len() >= 6 && s.chars().all(|c| c.is_ascii_digit())
}
fn iso(s: &str) -> Option<String> {
    DateTime::parse_from_rfc3339(s.trim()).ok().map(|d| d.with_timezone(&Utc).to_rfc3339_opts(SecondsFormat::Millis, true))
}
fn b64(s: &str) -> String {
    B64.encode(s.as_bytes())
}
fn unb64(s: &str) -> Res<String> {
    B64.decode(s).ok().and_then(|b| String::from_utf8(b).ok()).ok_or_else(|| "Bad file id".to_string())
}
fn plain_name(name: &str) -> Res<&str> {
    if name.is_empty() || name == "." || name == ".." || name.contains(['/', '\\', ':']) || name.chars().any(|c| (c as u32) < 0x20) {
        return Err(format!("Bad file name: {name}"));
    }
    Ok(name)
}

// A category as a folder / file-name part: characters Windows doesn't allow become "-" (fileName_ in Code.gs).
fn safe_name(s: &str) -> String {
    let r: String = s.chars().map(|c| if "<>:\"/\\|?*".contains(c) || (c as u32) < 0x20 { '-' } else { c }).collect();
    r.trim_end_matches(['.', ' ']).to_string()
}

fn clean_category(value: &str) -> Res<String> {
    let c = value.trim();
    if c.is_empty() {
        return Err("Pick or type a category.".into());
    }
    if c.contains(['/', '\\']) {
        return Err("Category names cannot contain / or \\.".into());
    }
    if c.starts_with('_') {
        return Err("Category names cannot start with _.".into());
    }
    Ok(c.to_string())
}

fn clean(fields: &Value) -> Res<(String, i64, i64)> {
    let category = clean_category(&field(fields, "category"))?;
    let nsfw = jnum(fields.get("nsfw").unwrap_or(&Value::Null));
    let quality = jnum(fields.get("quality").unwrap_or(&Value::Null));
    if !(nsfw.fract() == 0.0 && (1.0..=10.0).contains(&nsfw)) {
        return Err("NSFW must be a whole number from 1 to 10.".into());
    }
    if ![1.0, 2.0, 3.0].contains(&quality) {
        return Err("Rating must be 1, 2 or 3.".into());
    }
    Ok((category, nsfw as i64, quality as i64))
}

fn collection_name(v: &str) -> String {
    v.split_whitespace().collect::<Vec<_>>().join(" ").chars().take(100).collect()
}

// Booru-style tags: lowercase, words joined by "_", separated by spaces (or commas when typed).
// An optional type prefix colours them: artist:, character:, series: (also copyright:), meta:.
// "-" and "~" at the start and "*" are search operators, so they're left out of tag names.
const TAG_TYPES: [&str; 4] = ["artist", "character", "series", "meta"];
fn clean_tag(raw: &str) -> String {
    let t = raw.trim().to_lowercase();
    let t = t.trim_start_matches(['-', '~']).replace('*', "");
    let (kind, name) = match t.split_once(':') {
        Some((k, n)) if k == "copyright" => ("series", n),
        Some((k, n)) if TAG_TYPES.contains(&k) => (k, n),
        _ => ("", t.as_str()),
    };
    let name: String = name.trim_matches('_').chars().filter(|c| !c.is_control()).take(64).collect();
    if name.is_empty() { String::new() } else if kind.is_empty() { name } else { format!("{kind}:{name}") }
}
fn clean_tags(v: &str) -> String {
    let mut seen = HashSet::new();
    v.split(|c: char| c.is_whitespace() || c == ',')
        .map(clean_tag)
        .filter(|t| !t.is_empty() && seen.insert(t.clone()))
        .take(300)
        .collect::<Vec<_>>()
        .join(" ")
}

fn thumb_size(v: &str) -> String {
    let ok = v.split_once('x').map_or(false, |(w, h)| {
        [w, h].iter().all(|p| (1..=5).contains(&p.len()) && p.chars().all(|c| c.is_ascii_digit()))
    });
    if ok { v.to_string() } else { String::new() }
}

fn ext_of(name: &str) -> String {
    match name.rfind('.') {
        Some(k) if (1..=5).contains(&(name.len() - k - 1)) && name[k + 1..].chars().all(|c| c.is_ascii_alphanumeric()) => {
            name[k..].to_lowercase()
        }
        _ => String::new(),
    }
}

// 000123_261016_v073_Street Style.mp4: number, date added (local time), kind, NSFW, rating, category.
fn file_name(uid: &str, added: &str, kind: &str, category: &str, nsfw: i64, quality: i64, ext: &str) -> String {
    if DISCREET.load(std::sync::atomic::Ordering::Relaxed) {
        return format!("{uid}_file{ext}"); // 000123_file.webp
    }
    let day = DateTime::parse_from_rfc3339(added).map(|d| d.with_timezone(&Local).format("%y%m%d").to_string()).unwrap_or_default();
    let code = match kind { "gif" => "g", "video" => "v", _ => "i" };
    format!("{uid}_{day}_{code}{nsfw:02}{quality}_{}{ext}", safe_name(category))
}

fn for_page(i: &Item) -> Value {
    let mut v = serde_json::to_value(i).unwrap_or(Value::Null);
    v["url"] = json!(format!("http://vault.localhost/{}", utf8_percent_encode(&i.id, NON_ALPHANUMERIC)));
    v
}

fn items_from_csv(text: &str) -> Vec<Item> {
    let (head, table) = parse_rows(text);
    table
        .into_iter()
        .map(|cells| {
            let r = Row { head: &head, cells };
            let g = |k: &str| r.get(k).cloned().unwrap_or_default();
            Item {
                id: g("id"), uid: g("uid"), file: g("file"), name: g("original_name"), category: g("category"),
                kind: Some(g("kind")).filter(|k| !k.is_empty()).unwrap_or_else(|| "image".into()),
                nsfw: int_of(&g("nsfw")), quality: int_of(&g("quality")), added: g("added"),
                width: int_of(&g("width")), height: int_of(&g("height")),
                duration: g("duration").trim().parse().unwrap_or(0.0), preview_id: g("preview_id"), thumb: g("thumb"),
                thumbhash: g("thumbhash"), stream_id: g("stream_id"), thumb_size: g("thumb_size"),
                collection: g("collection"), collection_cover: g("collection_cover") == "1",
                tags: clean_tags(&g("tags")),
                source: g("source"),
            }
        })
        .collect()
}

fn views_from_csv(text: &str) -> Vec<ViewRow> {
    let (head, table) = parse_rows(text);
    table
        .into_iter()
        .map(|cells| {
            let r = Row { head: &head, cells };
            let g = |k: &str| r.get(k).cloned().unwrap_or_default();
            ViewRow { id: g("id"), uid: g("uid"), views: int_of(&g("views")), last: g("last_viewed"),
                      likes: int_of(&g("appreciations")), last_like: g("last_appreciated"),
                      seconds: g("seconds_viewed").trim().parse().unwrap_or(0.0) }
        })
        .collect()
}

// A copy a cloud service made when two PCs changed the same file at once: "index (1).csv",
// "index-DESKTOP-4F2.csv", "index (Sam's conflicted copy 2026-10-20).csv". Not "index.csv" itself,
// and not "indexes.csv".
fn is_conflict_copy(name: &str, base: &str) -> bool {
    let n = name.to_lowercase();
    n != format!("{base}.csv") && n.ends_with(".csv") && n.starts_with(base)
        && !n[base.len()..].starts_with(|c: char| c.is_alphanumeric() || c == '.')
}

fn write_atomic(path: &Path, bytes: &[u8]) -> Res<()> {
    let tmp = path.with_extension(format!("{}.part", path.extension().and_then(|e| e.to_str()).unwrap_or("")));
    crypt::write(&tmp, bytes).map_err(|e| format!("Could not write {}: {e}", path.display()))?;
    fs::rename(&tmp, path).map_err(|e| {
        let _ = fs::remove_file(&tmp);
        format!("Could not write {} (is it open in another program?): {e}", path.display())
    })
}

// ---------- the vault folder ----------

struct IndexCache {
    stamp: (SystemTime, u64),
    items: Arc<Vec<Item>>,
    md5: String,
}

struct Vault {
    root: PathBuf,
    lock: Mutex<()>,                 // every write to index.csv / views.csv / state.json
    cache: Mutex<Option<IndexCache>>, // the parsed index, while the file is unchanged
    packs: Mutex<HashMap<String, PathBuf>>, // Drive pack id -> local pack file (see drive_pack)
    resizing: (Mutex<usize>, std::sync::Condvar), // grid thumbnails being made right now (at most one per core)
    first_run: std::sync::atomic::AtomicBool, // no place chosen yet: the page shows the welcome screen
    copy: storage::CopyState,                 // copying the library to a new place (storage.rs)
    web: web::Web,                            // Discover's sources and their pictures (web.rs)
    dupes: dupes::Dupes,                      // Clean up → Find duplicates (dupes.rs)
    moving: discreet::Moving,                 // Discreet names being switched (discreet.rs)
}

impl Vault {
    fn new(root: PathBuf, first_run: bool) -> Vault {
        let v = Vault { root, lock: Mutex::new(()), cache: Mutex::new(None), packs: Mutex::new(HashMap::new()),
                      resizing: (Mutex::new(0), std::sync::Condvar::new()),
                      first_run: first_run.into(), copy: Default::default(), web: web::Web::new(), dupes: Default::default(), moving: Default::default() };
        // (an untraceable vault waits for its password: see unlock)
        if !crypt::locked() {
            v.init();
        }
        v
    }

    fn init(&self) {
        discreet::load(self);
        let _ = fs::remove_dir_all(self.incoming_dir()); // pieces of uploads cut short last time
        for f in APP_FOLDERS {
            let _ = fs::create_dir_all(self.app_dir(f));
        }
        let _ = self.read_text(INDEX_NAME, &to_csv(&[]));
        let _ = self.read_text(VIEWS_NAME, &views_csv(&[]));
        self.backup_daily();
    }

    // unfinished uploads: on this PC, not in the vault (which a cloud service may be syncing)
    fn incoming_dir(&self) -> PathBuf {
        storage::local_dir(&self.root, "incoming")
    }

    fn app_dir(&self, name: &str) -> PathBuf {
        self.root.join(APP_NAME).join(name)
    }

    fn category_dir(&self, category: &str) -> PathBuf {
        if DISCREET.load(std::sync::atomic::Ordering::Relaxed) {
            return self.root.join(MEDIA_FOLDER);
        }
        self.root.join(safe_name(category))
    }

    fn main_path(&self, i: &Item) -> PathBuf {
        self.category_dir(&i.category).join(&i.file)
    }

    // A root-level text file, created with `empty` if it isn't there.
    fn read_text(&self, name: &str, empty: &str) -> Res<(String, String)> {
        let p = self.root.join(name);
        if !p.exists() {
            fs::create_dir_all(&self.root).map_err(err)?;
            write_atomic(&p, empty.as_bytes())?;
        }
        let bytes = crypt::read(&p).map_err(err)?;
        Ok((String::from_utf8_lossy(&bytes).into_owned(), md5_hex(&bytes)))
    }

    fn load_index(&self) -> Res<(Arc<Vec<Item>>, String)> {
        let p = self.root.join(INDEX_NAME);
        let stamp = fs::metadata(&p).ok().map(|m| (m.modified().unwrap_or(SystemTime::UNIX_EPOCH), m.len()));
        let mut cache = self.cache.lock().unwrap();
        if let (Some(c), Some(s)) = (cache.as_ref(), stamp) {
            if c.stamp == s {
                return Ok((c.items.clone(), c.md5.clone()));
            }
        }
        let (text, md5) = self.read_text(INDEX_NAME, &to_csv(&[]))?;
        let items = Arc::new(items_from_csv(&text));
        let stamp = fs::metadata(&p).ok().map(|m| (m.modified().unwrap_or(SystemTime::UNIX_EPOCH), m.len()));
        *cache = stamp.map(|stamp| IndexCache { stamp, items: items.clone(), md5: md5.clone() });
        Ok((items, md5))
    }

    // Rolling backups: at each start and before the first change of a day, index.csv, views.csv and
    // the settings are copied into _app/backups/<date>/. A bad sync or a cut-off write is undone by
    // copying them back (with the app closed). The newest BACKUP_DAYS days are kept.
    fn backup_daily(&self) {
        let backups = self.app_dir("backups");
        let dir = backups.join(Local::now().format("%Y-%m-%d").to_string());
        if dir.exists() || fs::create_dir_all(&dir).is_err() {
            return;
        }
        for (from, name) in [(self.root.join(INDEX_NAME), INDEX_NAME), (self.root.join(VIEWS_NAME), VIEWS_NAME),
                             (self.app_dir("settings.json"), "settings.json")] {
            // (a file cut to nothing is no backup)
            if fs::metadata(&from).map(|m| m.len() > 0).unwrap_or(false) {
                let _ = fs::copy(&from, dir.join(name));
            }
        }
        let mut days: Vec<PathBuf> = fs::read_dir(&backups).into_iter().flatten().flatten()
            .map(|e| e.path()).filter(|p| p.is_dir() && p.file_name().is_some_and(|n| n.to_string_lossy().starts_with("20")))
            .collect();
        days.sort();
        while days.len() > BACKUP_DAYS {
            let _ = fs::remove_dir_all(days.remove(0));
        }
    }

    // Folds conflict copies of index.csv and views.csv into the real ones: items only the copy lists
    // (and whose file is in the vault) are added, and each item's counts take the higher of the two.
    // Where both list an item, this PC's details win. The copy then moves to _app/backups/conflicts.
    fn merge_conflicts(&self) -> Res<Value> {
        let copies: Vec<(PathBuf, bool)> = fs::read_dir(&self.root).map_err(err)?.flatten()
            .filter(|e| e.path().is_file())
            .filter_map(|e| {
                let name = e.file_name().to_string_lossy().into_owned();
                if is_conflict_copy(&name, "index") { Some((e.path(), true)) }
                else if is_conflict_copy(&name, "views") { Some((e.path(), false)) }
                else { None }
            })
            .collect();
        if copies.is_empty() {
            return Ok(json!({ "files": [], "added": 0, "counts": 0 }));
        }
        let _g = self.lock.lock().unwrap();
        self.backup_daily();
        let (mut added, mut counts, mut files) = (0usize, 0usize, Vec::new());
        for (path, is_index) in copies {
            let Ok(bytes) = crypt::read(&path) else { continue }; // (still being synced: next time)
            let text = String::from_utf8_lossy(&bytes);
            if is_index {
                let (ours, _) = self.load_index()?;
                let ids: HashSet<&str> = ours.iter().map(|i| i.id.as_str()).collect();
                let uids: HashSet<&str> = ours.iter().map(|i| i.uid.as_str()).filter(|u| !u.is_empty()).collect();
                let new: Vec<Item> = items_from_csv(&text).into_iter()
                    .filter(|i| !i.id.is_empty() && !ids.contains(i.id.as_str()) && !uids.contains(i.uid.as_str()))
                    .filter(|i| self.main_path(i).exists() || (is_uid(&i.uid) && self.item_path(&i.uid).is_ok()))
                    .collect();
                if !new.is_empty() {
                    added += new.len();
                    let mut all = (*ours).clone();
                    all.extend(new);
                    self.write_index(&all)?;
                }
            } else {
                let (mut ours, _) = self.load_views()?;
                let mut changed = 0;
                for t in views_from_csv(&text) {
                    match ours.iter_mut().find(|o| o.id == t.id) {
                        Some(o) => {
                            let before = (o.views, o.likes, o.seconds);
                            o.views = o.views.max(t.views);
                            o.likes = o.likes.max(t.likes);
                            o.seconds = o.seconds.max(t.seconds);
                            if t.last > o.last { o.last = t.last; }
                            if t.last_like > o.last_like { o.last_like = t.last_like; }
                            if before != (o.views, o.likes, o.seconds) { changed += 1; }
                        }
                        None if !t.id.is_empty() => { ours.push(t); changed += 1; }
                        None => {}
                    }
                }
                if changed > 0 {
                    counts += changed;
                    self.write_views(&ours)?;
                }
            }
            let name = path.file_name().unwrap_or_default().to_string_lossy().into_owned();
            let dir = self.app_dir("backups").join("conflicts");
            let _ = fs::create_dir_all(&dir);
            let to = dir.join(format!("{}_{name}", Local::now().format("%Y%m%d-%H%M%S")));
            if fs::rename(&path, &to).is_err() && fs::copy(&path, &to).is_ok() {
                let _ = fs::remove_file(&path);
            }
            files.push(name);
        }
        Ok(json!({ "files": files, "added": added, "counts": counts }))
    }

    // Returns the checksum of what was written.
    fn write_index(&self, items: &[Item]) -> Res<String> {
        self.backup_daily();
        let text = to_csv(items);
        *self.cache.lock().unwrap() = None;
        write_atomic(&self.root.join(INDEX_NAME), text.as_bytes())?;
        Ok(md5_hex(text.as_bytes()))
    }

    fn load_views(&self) -> Res<(Vec<ViewRow>, String)> {
        let (text, md5) = self.read_text(VIEWS_NAME, &views_csv(&[]))?;
        Ok((views_from_csv(&text), md5))
    }

    fn write_views(&self, rows: &[ViewRow]) -> Res<String> {
        self.backup_daily();
        let text = views_csv(rows);
        write_atomic(&self.root.join(VIEWS_NAME), text.as_bytes())?;
        Ok(md5_hex(text.as_bytes()))
    }

    // Applies a change to the index under the lock; returns the checksums before and after.
    fn change_index(&self, change: impl FnOnce(&mut Vec<Item>)) -> Res<Value> {
        let _g = self.lock.lock().unwrap();
        let (items, prev) = self.load_index()?;
        let mut items = (*items).clone();
        change(&mut items);
        Ok(json!({ "prev": prev, "md5": self.write_index(&items)? }))
    }

    // ---------- file ids ----------

    fn folder_dir(&self, folder_id: &str) -> Res<PathBuf> {
        if let Some(f) = folder_id.strip_prefix("app-").filter(|f| APP_FOLDERS.contains(f)) {
            return Ok(self.app_dir(f));
        }
        if let Some(c) = folder_id.strip_prefix("cat-") {
            return Ok(self.category_dir(&clean_category(&unb64(c)?)?));
        }
        Err(format!("Unknown folder: {folder_id}"))
    }

    fn resolve(&self, id: &str) -> Res<PathBuf> {
        if let Some(uid) = id.strip_prefix("item-") {
            return self.item_path(uid);
        }
        for f in APP_FOLDERS {
            if let Some(rest) = id.strip_prefix(f).and_then(|r| r.strip_prefix('-')) {
                return Ok(self.app_dir(f).join(plain_name(&unb64(rest)?)?));
            }
        }
        if let Some(rest) = id.strip_prefix("file-") {
            let rel = PathBuf::from(unb64(rest)?);
            if !rel.components().all(|c| matches!(c, Component::Normal(_))) {
                return Err("Bad file id".into());
            }
            return Ok(self.root.join(rel));
        }
        // a Drive id, from a copy of the Drive folder: the index row that names it says which file it is
        let (items, _) = self.load_index()?;
        for i in items.iter() {
            let uid = if i.uid.is_empty() { &i.id } else { &i.uid };
            if i.id == id {
                return Ok(self.main_path(i));
            } else if i.preview_id == id {
                return Ok(self.app_dir("clips").join(format!("{uid}.mp4")));
            } else if i.stream_id == id {
                return Ok(self.app_dir("streams").join(format!("{uid}.json")));
            } else if i.thumb == id {
                return Ok(self.app_dir("thumbs").join(format!("{uid}.webp")));
            }
        }
        if let Some(p) = self.drive_pack(id, &items) {
            return Ok(p);
        }
        Err(format!("Unknown file: {id}"))
    }

    // A thumbnail pack from a Drive copy is named pack-<time>.bin and known to the index only by its
    // Drive id. Its size is the end of its last thumbnail, and every slice is a WebP ("RIFF"): that
    // finds the matching local file. Remembered once found.
    fn drive_pack(&self, id: &str, items: &[Item]) -> Option<PathBuf> {
        if let Some(p) = self.packs.lock().unwrap().get(id) {
            return Some(p.clone());
        }
        let prefix = format!("{id}:");
        let slices: Vec<(u64, u64)> = items
            .iter()
            .filter_map(|i| i.thumb.strip_prefix(&prefix))
            .filter_map(|r| r.split_once(':'))
            .filter_map(|(o, l)| Some((o.parse().ok()?, l.parse().ok()?)))
            .collect();
        let size = slices.iter().map(|(o, l)| o + l).max()?;
        for f in fs::read_dir(self.app_dir("thumbs")).ok()?.flatten() {
            let path = f.path();
            if path.extension().and_then(|e| e.to_str()) != Some("bin") {
                continue;
            }
            let Ok(mut file) = crypt::Reader::open(&path) else { continue };
            if file.len() != size {
                continue;
            }
            let webp = slices.iter().take(20).all(|(o, _)| {
                let mut head = [0u8; 4];
                file.seek(SeekFrom::Start(*o)).and_then(|_| file.read_exact(&mut head)).is_ok() && &head == b"RIFF"
            });
            if webp {
                self.packs.lock().unwrap().insert(id.to_string(), path.clone());
                return Some(path);
            }
        }
        None
    }

    // The item's file: where index.csv says, else (just saved, not listed yet) found by its number.
    fn item_path(&self, uid: &str) -> Res<PathBuf> {
        if !is_uid(uid) {
            return Err("Bad file id".into());
        }
        let (items, _) = self.load_index()?;
        if let Some(i) = items.iter().find(|i| i.uid == uid) {
            let p = self.main_path(i);
            if p.exists() {
                return Ok(p);
            }
        }
        let prefix = format!("{uid}_");
        for dir in fs::read_dir(&self.root).map_err(err)?.flatten() {
            if !dir.path().is_dir() || dir.file_name().to_string_lossy().starts_with('_') {
                continue;
            }
            for f in fs::read_dir(dir.path()).map_err(err)?.flatten() {
                if f.file_name().to_string_lossy().starts_with(&prefix) {
                    return Ok(f.path());
                }
            }
        }
        Err(format!("No file for item {uid}"))
    }

    fn save_upload(&self, name: &str, parent: &str, bytes: &[u8]) -> Res<String> {
        let name = plain_name(name)?;
        let dir = self.folder_dir(parent)?;
        fs::create_dir_all(&dir).map_err(err)?;
        write_atomic(&dir.join(name), bytes)?;
        self.upload_id(name, parent, &dir)
    }

    // The last piece of a chunked upload arrived: the gathered file moves to its place.
    fn finish_upload(&self, name: &str, parent: &str, part: &Path) -> Res<String> {
        let name = plain_name(name)?;
        let dir = self.folder_dir(parent)?;
        fs::create_dir_all(&dir).map_err(err)?;
        let to = dir.join(name);
        let _ = fs::remove_file(&to); // a retry of the same file
        // (the pieces wait on this PC; the vault may be on another drive, where a rename can't reach)
        if fs::rename(part, &to).is_err() {
            fs::copy(part, &to).map_err(|e| format!("Could not save {name}: {e}"))?;
            let _ = fs::remove_file(part);
        }
        self.upload_id(name, parent, &dir)
    }

    fn upload_id(&self, name: &str, parent: &str, dir: &Path) -> Res<String> {
        if let Some(f) = parent.strip_prefix("app-") {
            return Ok(format!("{f}-{}", b64(name)));
        }
        match name.split_once('_') {
            Some((uid, _)) if is_uid(uid) => Ok(format!("item-{uid}")),
            _ => Ok(format!("file-{}", b64(&format!("{}/{name}", dir.file_name().unwrap_or_default().to_string_lossy())))),
        }
    }

    // ---------- the calls Code.gs offers ----------

    // The category folders, by the category name the index uses for them (a name with characters
    // Windows doesn't allow in folder names, like "Test: Cat", lives in the folder "Test- Cat").
    fn list_categories(&self) -> Res<Vec<String>> {
        let (items, _) = self.load_index()?;
        if DISCREET.load(std::sync::atomic::Ordering::Relaxed) {
            // (the folders say nothing: the categories are the index's)
            let mut names: Vec<String> = items.iter().map(|i| i.category.clone()).collect::<HashSet<_>>().into_iter().collect();
            names.sort_by_key(|n| n.to_lowercase());
            return Ok(names);
        }
        let by_folder: HashMap<String, &str> = items.iter().map(|i| (safe_name(&i.category), i.category.as_str())).collect();
        let mut names: Vec<String> = fs::read_dir(&self.root)
            .map_err(err)?
            .flatten()
            .filter(|e| e.path().is_dir())
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .filter(|n| !n.starts_with('_'))
            .map(|n| by_folder.get(&n).map_or(n, |c| c.to_string()))
            .collect();
        names.sort_by_key(|n| n.to_lowercase());
        Ok(names)
    }

    fn load_settings(&self) -> Value {
        crypt::read_to_string(&self.app_dir("settings.json")).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or(json!({}))
    }

    fn get_setup(&self) -> Res<Value> {
        let categories = self.list_categories()?;
        let folders: serde_json::Map<String, Value> = categories.iter().map(|c| (c.clone(), json!(format!("cat-{}", b64(c))))).collect();
        Ok(json!({
            "token": "local", "indexId": "index", "viewsId": "views", "settings": self.load_settings(),
            "categories": categories, "categoryFolders": folders, "timeZone": "",
            "folders": { "clips": "app-clips", "thumbs": "app-thumbs", "streams": "app-streams" },
            "dataFolder": self.root.display().to_string(),
            "firstRun": self.first_run.load(std::sync::atomic::Ordering::Relaxed),
            "discreet": DISCREET.load(std::sync::atomic::Ordering::Relaxed), "discreetChosen": discreet::chosen(self),
            "untraceable": crypt::untraceable()
        }))
    }

    fn category_folder(&self, category: &str) -> Res<String> {
        let c = clean_category(category)?;
        fs::create_dir_all(self.category_dir(&c)).map_err(err)?;
        Ok(format!("cat-{}", b64(&c)))
    }

    // The next `count` unique numbers (call with the lock held). If the counter is missing it
    // carries on from the highest number in the index.
    fn reserve_locked(&self, count: u64) -> Res<Vec<String>> {
        let state_path = self.app_dir("state.json");
        let mut state: Value = crypt::read_to_string(&state_path).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or(json!({}));
        let mut n = state["nextUid"].as_u64().unwrap_or(0);
        if n == 0 {
            let (items, _) = self.load_index()?;
            n = items.iter().map(|i| i.uid.parse::<u64>().unwrap_or(0)).max().unwrap_or(0) + 1;
        }
        state["nextUid"] = json!(n + count);
        write_atomic(&state_path, state.to_string().as_bytes())?;
        Ok((0..count).map(|k| format!("{:06}", n + k)).collect())
    }

    fn reserve_uids(&self, count: &Value) -> Res<Value> {
        let n = jnum(count).floor();
        let n = if n.is_finite() && n >= 1.0 { (n as u64).min(50) } else { 1 };
        let _g = self.lock.lock().unwrap();
        Ok(json!(self.reserve_locked(n)?))
    }

    fn uploaded_item(info: &Value) -> Res<Item> {
        let (category, nsfw, quality) = clean(info)?;
        let kind = Some(field(info, "kind")).filter(|k| KINDS.contains(&k.as_str())).unwrap_or_else(|| "image".into());
        let uid = field(info, "uid");
        let added = iso(&field(info, "added")).filter(|_| is_uid(&uid)).ok_or("Upload was not prepared.")?;
        let id = id_or_empty(field(info, "id"));
        if id.is_empty() {
            return Err("Upload has no file.".into());
        }
        let whole = |k: &str| { let f = jnum(info.get(k).unwrap_or(&Value::Null)); if f.is_finite() { f as i64 } else { 0 } };
        let duration = jnum(info.get("duration").unwrap_or(&Value::Null));
        let thumbhash = field(info, "thumbhash");
        let hash_ok = (1..=64).contains(&thumbhash.len()) && thumbhash.chars().all(|c| c.is_ascii_alphanumeric() || "+/=".contains(c));
        Ok(Item {
            file: file_name(&uid, &added, &kind, &category, nsfw, quality, &ext_of(&field(info, "file"))),
            id, uid, name: field(info, "name"), category, kind, nsfw, quality, added,
            width: whole("width"), height: whole("height"),
            duration: if duration.is_finite() { (duration * 100.0).round() / 100.0 } else { 0.0 },
            preview_id: id_or_empty(field(info, "previewId")), thumb: id_or_empty(field(info, "thumb")),
            thumbhash: if hash_ok { thumbhash } else { String::new() }, stream_id: id_or_empty(field(info, "streamId")),
            thumb_size: thumb_size(&field(info, "thumbSize")), collection: collection_name(&field(info, "collection")),
            collection_cover: false,
            tags: clean_tags(&field(info, "tags")),
            source: Some(field(info, "source")).filter(|u| (u.starts_with("https://") || u.starts_with("http://")) && u.len() <= 500).unwrap_or_default(),
        })
    }

    fn loose_thumbs(items: &[Item]) -> usize {
        items.iter().filter(|i| !i.thumb.is_empty() && !i.thumb.contains(':')).count()
    }

    // Adds saved files to index.csv in one rewrite; files already listed (a retry) are skipped.
    fn register_uploads(&self, list: &[Value]) -> Res<Value> {
        let wanted = list.iter().map(Vault::uploaded_item).collect::<Res<Vec<_>>>()?;
        let _g = self.lock.lock().unwrap();
        let (items, prev) = self.load_index()?;
        let mut items = (*items).clone();
        let mut have: HashSet<String> = items.iter().map(|i| i.id.clone()).collect();
        let mut added = Vec::new();
        for (item, info) in wanted.into_iter().zip(list) {
            if !have.insert(item.id.clone()) {
                continue;
            }
            // the page names files in its own time zone; if its idea of the date differs, fix the name
            let given = field(info, "file");
            if !given.is_empty() && given != item.file {
                let dir = self.category_dir(&item.category);
                let _ = fs::rename(dir.join(&given), dir.join(&item.file));
            }
            added.push(item);
        }
        items.extend(added.iter().cloned());
        let md5 = if added.is_empty() { prev.clone() } else { self.write_index(&items)? };
        Ok(json!({ "items": added.iter().map(for_page).collect::<Vec<_>>(), "looseThumbs": Vault::loose_thumbs(&items), "prev": prev, "md5": md5 }))
    }

    // Many small thumbnail files were combined into one pack: point the rows at their slice
    // ("<pack id>:<offset>:<length>") and remove the separate files (their bytes are in the pack).
    fn register_pack(&self, pack_id: &str, entries: &[Value]) -> Res<Value> {
        let by_id: HashMap<String, &Value> = entries.iter().map(|e| (field(e, "id"), e)).collect();
        let pack = id_or_empty(pack_id.to_string());
        let mut done = Vec::new();
        let result = {
            let _g = self.lock.lock().unwrap();
            let (items, prev) = self.load_index()?;
            let mut items = (*items).clone();
            for i in items.iter_mut() {
                let Some(e) = by_id.get(&i.id) else { continue };
                let loose = field(e, "loose");
                if loose.is_empty() || i.thumb != loose {
                    continue;
                }
                if let Ok(p) = self.resolve(&loose) {
                    done.push(p);
                }
                i.thumb = format!("{pack}:{}:{}", jnum(&e["offset"]) as i64, jnum(&e["length"]) as i64);
            }
            json!({ "count": done.len(), "prev": prev, "md5": self.write_index(&items)? })
        };
        for p in done {
            let _ = fs::remove_file(p);
        }
        Ok(result)
    }

    // Thumbnail packs keep the bytes of thumbnails whose item was deleted or got a sharper thumbnail.
    // A pack where more than a quarter (and over 256 KB) is such leftovers is written again with only
    // the thumbnails still used, and a pack nothing uses any more goes to the Recycle Bin. Packs
    // changed in the last 10 minutes are left alone (one may be on its way into the index).
    fn compact_packs(&self) -> Res<Value> {
        let _g = self.lock.lock().unwrap();
        let (items, prev) = self.load_index()?;
        let mut items = (*items).clone();
        // pack id → its slices (item index, offset, length)
        let mut packs: HashMap<String, Vec<(usize, u64, u64)>> = HashMap::new();
        for (k, i) in items.iter().enumerate() {
            let mut parts = i.thumb.rsplitn(3, ':');
            let (Some(len), Some(off), Some(pack)) = (parts.next(), parts.next(), parts.next()) else { continue };
            let (Ok(off), Ok(len)) = (off.parse::<u64>(), len.parse::<u64>()) else { continue };
            packs.entry(pack.to_string()).or_default().push((k, off, len));
        }
        let fresh = |p: &Path| fs::metadata(p).and_then(|m| m.modified()).ok()
            .and_then(|t| t.elapsed().ok()).map_or(true, |age| age.as_secs() < 600);
        let (mut used, mut unresolved, mut freed, mut rewritten, mut trashed) = (HashSet::new(), false, 0u64, 0, 0);
        let mut old_files = Vec::new();
        let stamp = Local::now().format("%Y%m%d%H%M%S").to_string();
        for (n, (pack, mut slices)) in packs.into_iter().enumerate() {
            let Ok(path) = self.resolve(&pack) else { unresolved = true; continue };
            used.insert(path.clone());
            let Ok(size) = fs::metadata(&path).map(|m| m.len()) else { unresolved = true; continue };
            slices.sort_by_key(|s| s.1);
            let live: u64 = slices.iter().map(|s| s.2).sum();
            let waste = size.saturating_sub(live);
            if waste * 4 <= size || waste < 256 * 1024 || fresh(&path) {
                continue;
            }
            let Ok(bytes) = crypt::read(&path) else { continue };
            if slices.iter().any(|&(_, o, l)| o + l > bytes.len() as u64) {
                continue; // (not the file the index means)
            }
            let mut out = Vec::with_capacity(live as usize);
            let name = format!("pack-{stamp}-{n}.bin");
            let id = format!("thumbs-{}", b64(&name));
            let mut moved = Vec::new();
            for &(k, o, l) in &slices {
                moved.push((k, format!("{id}:{}:{l}", out.len())));
                out.extend_from_slice(&bytes[o as usize..(o + l) as usize]);
            }
            write_atomic(&self.app_dir("thumbs").join(&name), &out)?;
            for (k, thumb) in moved {
                items[k].thumb = thumb;
            }
            old_files.push(path);
            freed += waste;
            rewritten += 1;
        }
        let changed = rewritten > 0;
        let md5 = if changed { self.write_index(&items)? } else { prev.clone() };
        for p in old_files {
            self.packs.lock().unwrap().retain(|_, v| *v != p);
            if !crypt::trash(&p) {
                let _ = fs::remove_file(&p);
            }
        }
        // packs nothing in the index points at (only when every pack the index names was found)
        if !unresolved {
            for f in fs::read_dir(self.app_dir("thumbs")).map_err(err)?.flatten() {
                let path = f.path();
                let name = f.file_name().to_string_lossy().into_owned();
                if !(name.starts_with("pack-") && name.ends_with(".bin")) || used.contains(&path) || fresh(&path) {
                    continue;
                }
                let size = f.metadata().map(|m| m.len()).unwrap_or(0);
                if crypt::trash(&path) {
                    freed += size;
                    trashed += 1;
                }
            }
        }
        Ok(json!({ "rewritten": rewritten, "trashed": trashed, "freed": freed, "prev": prev, "md5": md5 }))
    }

    // Sharper thumbnails for older items: [{ id, thumb, thumbSize }]; the old separate ones are removed.
    // New thumbnails ({id, thumb, thumbSize}) and, on the desktop, new preview clips ({id, previewId});
    // the files they replace are deleted.
    fn update_thumbs(&self, list: &[Value]) -> Res<Value> {
        let by_id: HashMap<String, (String, String)> = list
            .iter()
            .filter_map(|t| {
                let thumb = id_or_empty(field(t, "thumb"));
                (!thumb.is_empty()).then(|| (field(t, "id"), (thumb, thumb_size(&field(t, "thumbSize")))))
            })
            .collect();
        let clips: HashMap<String, String> = list
            .iter()
            .filter_map(|t| {
                let clip = id_or_empty(field(t, "previewId"));
                (!clip.is_empty()).then(|| (field(t, "id"), clip))
            })
            .collect();
        let mut old = Vec::new();
        let result = {
            let _g = self.lock.lock().unwrap();
            let (items, prev) = self.load_index()?;
            let mut items = (*items).clone();
            let mut count = 0;
            for i in items.iter_mut() {
                if let Some(clip) = clips.get(&i.id) {
                    if !i.preview_id.is_empty() && &i.preview_id != clip {
                        if let Ok(p) = self.resolve(&i.preview_id) {
                            old.push(p);
                        }
                    }
                    i.preview_id = clip.clone();
                    count += 1;
                }
                let Some((thumb, size)) = by_id.get(&i.id) else { continue };
                if !i.thumb.is_empty() && !i.thumb.contains(':') && &i.thumb != thumb {
                    if let Ok(p) = self.resolve(&i.thumb) {
                        old.push(p);
                    }
                }
                i.thumb = thumb.clone();
                i.thumb_size = size.clone();
                count += 1;
            }
            let md5 = if count > 0 { self.write_index(&items)? } else { prev.clone() };
            json!({ "count": count, "looseThumbs": Vault::loose_thumbs(&items), "prev": prev, "md5": md5 })
        };
        for p in old {
            let _ = fs::remove_file(p);
        }
        Ok(result)
    }

    fn record_views(&self, views: &[Value]) -> Res<Value> {
        let _g = self.lock.lock().unwrap();
        let (mut rows, prev) = self.load_views()?;
        let mut at_row: HashMap<String, usize> = rows.iter().enumerate().map(|(k, r)| (r.id.clone(), k)).collect();
        for v in views {
            let id = id_or_empty(field(v, "id"));
            let Some(at) = iso(&field(v, "at")) else { continue };
            if id.is_empty() {
                continue;
            }
            let k = *at_row.entry(id.clone()).or_insert_with(|| {
                let uid = field(v, "uid");
                rows.push(ViewRow { id, uid: if is_uid(&uid) { uid } else { String::new() }, ..Default::default() });
                rows.len() - 1
            });
            let row = &mut rows[k];
            if field(v, "type") == "like" {
                row.likes += 1;
                if at > row.last_like { row.last_like = at; }
            } else {
                row.views += 1;
                if at > row.last { row.last = at; }
            }
        }
        Ok(json!({ "prev": prev, "md5": self.write_views(&rows)? }))
    }

    // Time spent looking at items: [{ id, uid, seconds }], added to views.csv's seconds_viewed.
    fn record_time(&self, list: &[Value]) -> Res<Value> {
        let _g = self.lock.lock().unwrap();
        let (mut rows, prev) = self.load_views()?;
        let mut at_row: HashMap<String, usize> = rows.iter().enumerate().map(|(k, r)| (r.id.clone(), k)).collect();
        for t in list {
            let id = id_or_empty(field(t, "id"));
            let secs = jnum(t.get("seconds").unwrap_or(&Value::Null));
            if id.is_empty() || !secs.is_finite() || secs <= 0.0 {
                continue;
            }
            let k = *at_row.entry(id.clone()).or_insert_with(|| {
                let uid = field(t, "uid");
                rows.push(ViewRow { id, uid: if is_uid(&uid) { uid } else { String::new() }, ..Default::default() });
                rows.len() - 1
            });
            rows[k].seconds += secs.min(6.0 * 3600.0); // one batch never adds more than 6 hours
        }
        Ok(json!({ "prev": prev, "md5": self.write_views(&rows)? }))
    }

    // New category and scores: the file moves to the category's folder and gets its new name.
    fn update_image(&self, id: &str, fields: &Value) -> Res<Value> {
        let (category, nsfw, quality) = clean(fields)?;
        let _g = self.lock.lock().unwrap();
        let (items, prev) = self.load_index()?;
        let mut items = (*items).clone();
        let k = items.iter().position(|i| i.id == id).ok_or("That item is not in the index any more.")?;
        let mut from = self.main_path(&items[k]);
        if !from.exists() {
            from = self.resolve(id)?;
        }
        if items[k].uid.is_empty() {
            items[k].uid = self.reserve_locked(1)?.remove(0); // an item saved before numbered names existed
        }
        let item = &mut items[k];
        item.category = category;
        item.nsfw = nsfw;
        item.quality = quality;
        item.file = file_name(&item.uid, &item.added, &item.kind, &item.category, nsfw, quality, &ext_of(&item.file));
        let to = self.main_path(item);
        let page = for_page(item);
        if to != from {
            fs::create_dir_all(to.parent().unwrap()).map_err(err)?;
            fs::rename(&from, &to).map_err(|e| format!("Could not move the file: {e}"))?;
        }
        match self.write_index(&items) {
            Ok(md5) => Ok(json!({ "item": page, "prev": prev, "md5": md5 })),
            Err(e) => {
                let _ = fs::rename(&to, &from);
                Err(e)
            }
        }
    }

    // Moves the files and their helper files to the Recycle Bin and takes them out of
    // index.csv and views.csv. Packed thumbnails stay in their pack.
    fn trash_images(&self, ids: &[String]) -> Res<Value> {
        let gone: HashSet<&str> = ids.iter().map(String::as_str).collect();
        let mut paths = Vec::new();
        let result = {
            let _g = self.lock.lock().unwrap();
            let (items, prev) = self.load_index()?;
            for i in items.iter().filter(|i| gone.contains(i.id.as_str())) {
                let main = self.main_path(i);
                paths.push(if main.exists() { main } else { self.resolve(&i.id).unwrap_or(main) });
                let thumb = if i.thumb.contains(':') { "" } else { &i.thumb };
                for helper in [&i.preview_id, &i.stream_id, &thumb.to_string()] {
                    if !helper.is_empty() {
                        if let Ok(p) = self.resolve(helper) {
                            paths.push(p);
                        }
                    }
                }
            }
            let kept: Vec<Item> = items.iter().filter(|i| !gone.contains(i.id.as_str())).cloned().collect();
            let md5 = self.write_index(&kept)?;
            let (views, views_prev) = self.load_views()?;
            let views_md5 = if views.iter().any(|v| gone.contains(v.id.as_str())) {
                self.write_views(&views.into_iter().filter(|v| !gone.contains(v.id.as_str())).collect::<Vec<_>>())?
            } else {
                views_prev.clone()
            };
            json!({ "count": ids.len(), "prev": prev, "md5": md5, "viewsPrev": views_prev, "viewsMd5": views_md5 })
        };
        for p in paths.into_iter().filter(|p| p.exists()) {
            // the Recycle Bin, or (a cloud drive may not have one; untraceable: the bin is a trace) the
            // vault's own _trash folder
            if crypt::untraceable() || trash::delete(&p).is_err() {
                let bin = self.root.join("_trash");
                let _ = fs::create_dir_all(&bin);
                let to = bin.join(p.file_name().unwrap_or_default());
                if fs::rename(&p, &to).is_err() && fs::copy(&p, &to).is_ok() {
                    let _ = fs::remove_file(&p);
                }
            }
        }
        Ok(result)
    }

    fn kv_path(&self, name: &str) -> Res<PathBuf> {
        if name.is_empty() || name.len() > 40 || !name.chars().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-' || c == '_') {
            return Err("Bad document name".into());
        }
        Ok(self.app_dir("kv").join(format!("{name}.json")))
    }

    // A copy of an item's file goes to the Recycle Bin (or _trash) before the file is replaced (Compact
    // videos), so the original can still be restored from there.
    fn backup_to_bin(&self, id: &str) -> Res<Value> {
        let from = self.resolve(id)?;
        let name = from.file_name().map(|n| n.to_string_lossy().into_owned()).ok_or("No file")?;
        let dir = self.incoming_dir().join("bin");
        fs::create_dir_all(&dir).map_err(err)?;
        let copy = dir.join(format!("original {name}"));
        fs::copy(&from, &copy).map_err(|e| format!("Could not back up {name}: {e}"))?;
        if crypt::untraceable() || trash::delete(&copy).is_err() {
            let bin = self.root.join("_trash");
            fs::create_dir_all(&bin).map_err(err)?;
            let to = bin.join(copy.file_name().unwrap_or_default());
            if fs::rename(&copy, &to).is_err() {
                fs::copy(&copy, &to).map_err(err)?;
                let _ = fs::remove_file(&copy);
            }
        }
        Ok(Value::Bool(true))
    }

    fn save_settings(&self, settings: &Value) -> Res<Value> {
        let text = if settings.is_object() { settings.to_string() } else { "{}".into() };
        if text.len() > 8000 {
            return Err("Settings too large.".into());
        }
        write_atomic(&self.app_dir("settings.json"), text.as_bytes())?;
        Ok(json!(true))
    }

    // index.csv or views.csv as text with its checksum (the page parses it like the Drive copy).
    fn read_tracked(&self, which: &str) -> Res<Value> {
        let (text, md5) = match which {
            "index" => self.read_text(INDEX_NAME, &to_csv(&[]))?,
            "views" => self.read_text(VIEWS_NAME, &views_csv(&[]))?,
            _ => return Err(format!("Unknown list: {which}")),
        };
        Ok(json!({ "md5": md5, "text": text }))
    }

    // A thumbnail's bytes: "<pack id>:<offset>:<length>" (a slice of a pack) or a file id.
    fn thumb_bytes(&self, thumb: &str) -> Res<Vec<u8>> {
        let parts: Vec<&str> = thumb.rsplitn(3, ':').collect();
        if let [len, off, pack] = parts[..] {
            let (off, len): (u64, usize) = (off.parse().map_err(err)?, len.parse().map_err(err)?);
            let mut f = crypt::Reader::open(&self.resolve(pack)?).map_err(err)?;
            let mut buf = vec![0u8; len];
            f.seek(SeekFrom::Start(off)).and_then(|_| f.read_exact(&mut buf)).map_err(err)?;
            return Ok(buf);
        }
        crypt::read(&self.resolve(thumb)?).map_err(err)
    }

    // The thumbnail at most `width` px wide, for the Library's cards. Thumbnails are made 900 x 1200
    // so they fill a phone screen; a desktop card is a few hundred px, and decoding the big one for
    // every card costs time and memory. Made once and kept in _app/cache/w<width>/ as JPEG.
    fn grid_thumb(&self, thumb: &str, width: u32) -> Res<(Vec<u8>, &'static str)> {
        let key = format!("{:x}", md5::compute(thumb.as_bytes()));
        let path = storage::local_dir(&self.root, "cache").join(format!("w{width}")).join(format!("{key}.jpg"));
        if let Ok(bytes) = crypt::read(&path) {
            return Ok((bytes, "image/jpeg"));
        }
        let source = self.thumb_bytes(thumb)?;
        let _slot = self.resize_slot();
        let img = match image::load_from_memory(&source) {
            Ok(img) if img.width() > width => img,
            _ => return Ok((source, "image/webp")), // small enough already, or not something to resize
        };
        let height = ((img.height() as u64 * width as u64) / img.width() as u64).max(1) as u32;
        let small = img.resize_exact(width, height, image::imageops::FilterType::Triangle).to_rgb8();
        let mut out = Vec::new();
        image::codecs::jpeg::JpegEncoder::new_with_quality(&mut out, 86).encode_image(&small).map_err(err)?;
        if fs::create_dir_all(path.parent().unwrap()).is_ok() {
            let _ = write_atomic(&path, &out);
        }
        Ok((out, "image/jpeg"))
    }

    // Waits until fewer resizes than cores are running; the slot is given back when dropped.
    fn resize_slot(&self) -> impl Drop + '_ {
        struct Slot<'a>(&'a (Mutex<usize>, std::sync::Condvar));
        impl Drop for Slot<'_> {
            fn drop(&mut self) {
                *self.0 .0.lock().unwrap() -= 1;
                self.0 .1.notify_one();
            }
        }
        let cores = std::thread::available_parallelism().map_or(4, |n| n.get());
        let (count, freed) = &self.resizing;
        let mut n = count.lock().unwrap();
        while *n >= cores {
            n = freed.wait(n).unwrap();
        }
        *n += 1;
        Slot(&self.resizing)
    }

    // Opens the file with its default program (Photos, Media Player, VLC...).
    fn open_file(&self, id: &str) -> Res<Value> {
        if crypt::untraceable() {
            // (another app would need a decrypted copy on the disk: exactly what this mode avoids)
            return Err("Opening in another app isn't possible in Untraceable mode".into());
        }
        let path = self.resolve(id)?;
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            std::process::Command::new("explorer").raw_arg(format!("\"{}\"", path.display())).spawn().map_err(err)?;
        }
        Ok(json!(true))
    }

    fn show_in_folder(&self, id: &str) -> Res<Value> {
        let path = self.resolve(id)?;
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            std::process::Command::new("explorer").raw_arg(format!("/select,\"{}\"", path.display())).spawn().map_err(err)?;
        }
        Ok(json!(true))
    }
}

// ---------- commands ----------

#[tauri::command(async)]
fn api(vault: State<'_, Arc<Vault>>, name: String, args: Vec<Value>) -> Res<Value> {
    let a = |k: usize| args.get(k).cloned().unwrap_or(Value::Null);
    let strings = |v: Value| arr(&v).iter().map(str_of).collect::<Vec<_>>();
    if crypt::locked() && !matches!(name.as_str(), "lockState" | "appVersion") {
        return Err("Locked: enter your password first".into());
    }
    match name.as_str() {
        "lockState" => Ok(json!({ "untraceable": crypt::untraceable(), "locked": crypt::locked() })),
        "getSetup" => vault.get_setup(),
        "getToken" => Ok(json!("local")),
        "listCategories" => Ok(json!(vault.list_categories()?)),
        "categoryFolder" => Ok(json!(vault.category_folder(&str_of(&a(0)))?)),
        "reserveUids" => vault.reserve_uids(&a(0)),
        "registerUploads" => vault.register_uploads(&arr(&a(0))),
        "registerPack" => vault.register_pack(&str_of(&a(0)), &arr(&a(1))),
        "updateThumbs" => vault.update_thumbs(&arr(&a(0))),
        "setCollection" => {
            let (ids, want) = (strings(a(0)).into_iter().collect::<HashSet<_>>(), collection_name(&str_of(&a(1))));
            vault.change_index(|items| {
                for i in items.iter_mut().filter(|i| ids.contains(&i.id)) {
                    i.collection = want.clone();
                    i.collection_cover = false;
                }
            })
        }
        "renameCollection" => {
            let (from, to) = (str_of(&a(0)), collection_name(&str_of(&a(1))));
            if to.is_empty() {
                return Err("Give the collection a name.".into());
            }
            vault.change_index(|items| {
                let merging = items.iter().any(|i| i.collection == to && i.collection_cover);
                for i in items.iter_mut().filter(|i| i.collection == from) {
                    i.collection = to.clone();
                    if merging { i.collection_cover = false; }
                }
            })
        }
        "setCollectionCover" => {
            let (name, id) = (str_of(&a(0)), str_of(&a(1)));
            vault.change_index(|items| {
                for i in items.iter_mut().filter(|i| i.collection == name) {
                    i.collection_cover = i.id == id;
                }
            })
        }
        "listImages" => Ok(json!(vault.load_index()?.0.iter().map(for_page).collect::<Vec<_>>())),
        "listViews" => Ok(json!(vault.load_views()?.0)),
        "recordViews" => vault.record_views(&arr(&a(0))),
        "recordTime" => vault.record_time(&arr(&a(0))),
        "updateImage" => vault.update_image(&str_of(&a(0)), &a(1)),
        "trashImages" => vault.trash_images(&strings(a(0))),
        "saveSettings" => vault.save_settings(&a(0)),
        "getThumb" => Ok(json!(format!("http://vault.localhost/{}", utf8_percent_encode(&str_of(&a(0)), NON_ALPHANUMERIC)))),
        "readTracked" => vault.read_tracked(&str_of(&a(0))),
        "showInFolder" => vault.show_in_folder(&str_of(&a(0))),
        // small JSON documents kept with the vault (_app/kv/<name>.json): resume positions, watch later,
        // the audio list, your profile... They travel with the library, like settings.json.
        // how much space the vault takes on disk (everything in its folder: items, previews, thumbnails)
        "mergeConflicts" => vault.merge_conflicts(),
        "fastLane" => Ok(fastlane::info()),
        "compactPacks" => vault.compact_packs(),
        "vaultSize" => {
            fn walk(d: &Path) -> u64 {
                fs::read_dir(d).map(|rd| rd.flatten().map(|e| match e.file_type() {
                    Ok(t) if t.is_dir() => walk(&e.path()),
                    Ok(_) => e.metadata().map(|m| m.len()).unwrap_or(0),
                    _ => 0,
                }).sum()).unwrap_or(0)
            }
            Ok(json!(walk(&vault.root)))
        }
        "kvGet" => {
            let path = vault.kv_path(&str_of(&a(0)))?;
            Ok(crypt::read_to_string(&path).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or(Value::Null))
        }
        "kvSet" => {
            let path = vault.kv_path(&str_of(&a(0)))?;
            fs::create_dir_all(path.parent().unwrap()).map_err(err)?;
            write_atomic(&path, a(1).to_string().as_bytes())?;
            Ok(json!(true))
        }
        // Clean up: duplicates (dupes.rs), and compacting videos
        "backupStart" => maint::backup_start(vault.inner().clone(), &str_of(&a(0))),
        "backupProgress" => Ok(maint::backup_progress()),
        "healthCheck" => maint::health(&vault),
        "healthFix" => maint::health_fix(&vault, &str_of(&a(0))),
        "updateCheck" => maint::update_check(),
        "updateDownload" => maint::update_download(&str_of(&a(0))),
        "updateProgress" => Ok(maint::update_progress()),
        "appVersion" => Ok(json!(env!("CARGO_PKG_VERSION"))),
        "findDuplicates" => dupes::start(vault.inner().clone()),
        "duplicateProgress" => Ok(dupes::progress(&vault)),
        "setDiscreet" => discreet::start(vault.inner().clone(), a(0).as_bool() == Some(true)),
        "discreetProgress" => Ok(discreet::progress(&vault)),
        "tidyPack" => pack::tidy(&vault, &str_of(&a(0))),
        "backupToBin" => vault.backup_to_bin(&str_of(&a(0))),
        // an Audio file to the Recycle Bin (or _trash)
        "trashAudio" => {
            let id = str_of(&a(0));
            if !id.starts_with("audio-") {
                return Err("Not an audio file".into());
            }
            let p = vault.resolve(&id)?;
            if p.exists() && trash::delete(&p).is_err() {
                let bin = vault.root.join("_trash");
                fs::create_dir_all(&bin).map_err(err)?;
                fs::rename(&p, bin.join(p.file_name().unwrap_or_default())).map_err(err)?;
            }
            Ok(json!(true))
        }
        // a checked copy (in _app/streams) takes the place of an item's file: videos made seekable
        "replaceItemFile" => {
            let (id, from_id) = (str_of(&a(0)), str_of(&a(1)));
            let from = vault.resolve(&from_id)?;
            if !from.starts_with(vault.app_dir("streams")) {
                return Err("Only a copy made by the app can replace a file".into());
            }
            let (items, _) = vault.load_index()?;
            let item = items.iter().find(|i| i.id == id).ok_or("That item isn't in the library")?.clone();
            let to = vault.main_path(&item);
            if !to.exists() {
                return Err("The item's file isn't there".into());
            }
            if fs::rename(&from, &to).is_err() {
                fs::copy(&from, &to).map_err(err)?;
                let _ = fs::remove_file(&from);
            }
            let old_stream = item.stream_id.clone();
            if !old_stream.is_empty() {
                if let Ok(p) = vault.resolve(&old_stream) {
                    let _ = fs::remove_file(p);
                }
            }
            vault.change_index(|items| items.iter_mut().filter(|i| i.id == id).for_each(|i| i.stream_id.clear()))
        }
        "clearStream" => {
            let id = str_of(&a(0));
            vault.change_index(|items| items.iter_mut().filter(|i| i.id == id).for_each(|i| i.stream_id.clear()))
        }
        "openFile" => vault.open_file(&str_of(&a(0))),
        // where the vault lives (storage.rs)
        "storageOptions" => Ok(storage::options(&vault.root)),
        "storageForPicked" => Ok(storage::for_picked(Path::new(&str_of(&a(0))), &vault.root)),
        "untraceableForPicked" => Ok(storage::untraceable_for_picked(Path::new(&str_of(&a(0))), &vault.root)),
        "useStorage" => {
            // (a(2): { untraceable, password, scrub, deleteOld } for the Untraceable choice)
            let r = storage::use_folder(&vault, Path::new(&str_of(&a(0))), a(1) == json!(true), &a(2))?;
            vault.first_run.store(false, std::sync::atomic::Ordering::Relaxed);
            Ok(r)
        }
        "copyProgress" => Ok(storage::copy_progress(&vault)),
        // Google Drive over the web, for PCs without Google Drive for desktop (gdrive.rs)
        "driveStatus" => Ok(gdrive::status(&vault)),
        "driveSetKey" => gdrive::set_key(&vault, &str_of(&a(0)), &str_of(&a(1))),
        "driveSignIn" => gdrive::sign_in(vault.inner().clone()),
        "driveSignOut" => gdrive::sign_out(&vault),
        "driveSync" => gdrive::sync_start(vault.inner().clone()),
        "driveCancel" => Ok(gdrive::cancel()),
        "driveAsked" => Ok(gdrive::asked()),
        "driveAuto" => gdrive::set_auto(&vault, a(0).as_bool() == Some(true)),
        // Discover (web.rs)
        "loadSources" => Ok(vault.web.load_sources().unwrap_or(Value::Null)),
        "saveSources" => vault.web.save_sources(&a(0)),
        "webAllow" => vault.web.allow(&str_of(&a(0))),
        "webJson" => vault.web.json(&str_of(&a(0)), &a(1)),
        // long videos from Videos' sites, saved into the vault (web.rs)
        "downloadStart" => vault.web.download_start(&str_of(&a(0))),
        "downloadProgress" => vault.web.download_progress(&str_of(&a(0))),
        "downloadDrop" => vault.web.download_drop(&str_of(&a(0))),
        "webPost" => vault.web.post(&str_of(&a(0)), &a(1), &a(2)),
        "webFormThenGet" => vault.web.form_then_get(&str_of(&a(0)), &a(1), &str_of(&a(2))),
        "redditLogin" => vault.web.reddit_login(&str_of(&a(0))),
        // Find the source of a library item: its thumbnail goes to SauceNAO (with your key) or IQDB
        "reverseSearch" => {
            let (service, id, key) = (str_of(&a(0)), str_of(&a(1)), str_of(&a(2)));
            let (items, _) = vault.load_index()?;
            let item = items.iter().find(|i| i.id == id).ok_or("That item isn't in the library")?;
            if item.thumb.is_empty() {
                return Err("This item has no thumbnail to search with".into());
            }
            let (bytes, _) = vault.grid_thumb(&item.thumb, 720)?;
            match service.as_str() {
                "saucenao" => vault.web.upload(
                    &format!("https://saucenao.com/search.php?output_type=2&numres=8&db=999&api_key={}", utf8_percent_encode(&key, NON_ALPHANUMERIC)),
                    "file", &bytes, &Value::Null),
                "iqdb" => vault.web.upload("https://iqdb.org/", "file", &bytes, &json!({ "MAX_FILE_SIZE": "8388608" })),
                _ => Err("Unknown search service".into()),
            }
        }
        // a post's page or its original source, in the normal browser (web addresses only)
        "openLink" => {
            let url = str_of(&a(0));
            if !(url.starts_with("https://") || url.starts_with("http://")) || url.contains(['"', ' ']) {
                return Err("Not a web address".into());
            }
            std::process::Command::new("explorer").arg(&url).spawn().map_err(err)?;
            Ok(json!(true))
        }
        "openDownload" => {
            let id = str_of(&a(0));
            let (_, url) = storage::DOWNLOADS.iter().find(|(k, _)| *k == id).ok_or("Unknown service")?;
            std::process::Command::new("explorer").arg(url).spawn().map_err(err)?;
            Ok(json!(true))
        }
        "openDataFolder" => {
            #[cfg(windows)]
            {
                use std::os::windows::process::CommandExt;
                std::process::Command::new("explorer").raw_arg(format!("\"{}\"", vault.root.display())).spawn().map_err(err)?;
            }
            Ok(json!(true))
        }
        // tags of many items at once: mode "set" replaces them, "add" / "remove" change only those given
        "setTags" => {
            let (ids, tags, mode) = (strings(a(0)).into_iter().collect::<HashSet<_>>(), clean_tags(&str_of(&a(1))), str_of(&a(2)));
            let given: Vec<&str> = tags.split(' ').filter(|t| !t.is_empty()).collect();
            vault.change_index(|items| {
                for i in items.iter_mut().filter(|i| ids.contains(&i.id)) {
                    let mut have: Vec<String> = i.tags.split(' ').filter(|t| !t.is_empty()).map(String::from).collect();
                    match mode.as_str() {
                        "add" => have.extend(given.iter().map(|t| t.to_string())),
                        "remove" => have.retain(|t| !given.contains(&t.as_str())),
                        _ => have = given.iter().map(|t| t.to_string()).collect(),
                    }
                    i.tags = clean_tags(&have.join(" "));
                }
            })
        }
        _ => Err(format!("Unknown call: {name}")),
    }
}

// The page sends a file in pieces (so a big video isn't held in memory twice and progress can be
// shown): each piece is the raw request body, and headers say which upload it belongs to (x-token),
// whether it's the first (x-first: 1) or last (x-done: 1), and where the file goes (x-name,
// x-parent). Returns the file id after the last piece, "" before that.
#[tauri::command(async)]
fn upload(vault: State<'_, Arc<Vault>>, request: tauri::ipc::Request<'_>) -> Res<String> {
    let tauri::ipc::InvokeBody::Raw(bytes) = request.body() else {
        return Err("Expected the file's bytes".into());
    };
    let header = |k: &str| {
        request.headers().get(k).and_then(|v| v.to_str().ok()).map(|s| percent_decode_str(s).decode_utf8_lossy().into_owned()).unwrap_or_default()
    };
    let token = header("x-token");
    if token.is_empty() {
        return vault.save_upload(&header("x-name"), &header("x-parent"), bytes); // all in one piece
    }
    write_piece(&vault, &token, header("x-first") == "1", header("x-pos").parse().ok(), &mut &bytes[..])?;
    if header("x-done") != "1" {
        return Ok(String::new());
    }
    finish_piece(&vault, &token, &header("x-name"), &header("x-parent"))
}

fn upload_part(vault: &Vault, token: &str) -> Res<PathBuf> {
    if !(8..=64).contains(&token.len()) || !token.chars().all(|c| c.is_ascii_alphanumeric()) {
        return Err("Bad upload token".into());
    }
    let incoming = vault.incoming_dir();
    fs::create_dir_all(&incoming).map_err(err)?;
    Ok(incoming.join(token))
}

// One piece of an upload (from the upload command or the fast lane, fastlane.rs). x-pos: a piece
// for a given place in the file (a video being rewritten writes its index last, at the front);
// without it, pieces follow each other.
fn write_piece(vault: &Vault, token: &str, first: bool, pos: Option<u64>, body: &mut dyn Read) -> Res<()> {
    let part = upload_part(vault, token)?;
    if first {
        let _ = fs::remove_file(&part);
    }
    crypt::write_piece(&part, first, pos, body).map_err(err)
}

fn finish_piece(vault: &Vault, token: &str, name: &str, parent: &str) -> Res<String> {
    let part = upload_part(vault, token)?;
    vault.finish_upload(name, parent, &part)
}

// ---------- vault:// — files by id, with byte ranges ----------

fn mime(path: &Path) -> &'static str {
    match path.extension().and_then(|e| e.to_str()).map(|e| e.to_ascii_lowercase()).as_deref() {
        Some("webp") => "image/webp",
        Some("gif") => "image/gif",
        Some("png") => "image/png",
        Some("jpg" | "jpeg") => "image/jpeg",
        Some("mp4" | "m4v") => "video/mp4",
        Some("m4a" | "aac") => "audio/mp4",
        Some("mp3") => "audio/mpeg",
        Some("ogg" | "oga") => "audio/ogg",
        Some("opus") => "audio/ogg",
        Some("flac") => "audio/flac",
        Some("wav") => "audio/wav",
        Some("webm") => "video/webm",
        Some("mov") => "video/quicktime",
        Some("json") => "application/json",
        Some("csv") => "text/csv; charset=utf-8",
        _ => "application/octet-stream",
    }
}

// "bytes=a-b", "bytes=a-" or "bytes=-n" -> [start, end) within the file, None if unsatisfiable.
const RANGE_PIECE: u64 = 4 * 1024 * 1024;

fn parse_range(v: &str, size: u64) -> Option<(u64, u64)> {
    let (a, b) = v.trim().strip_prefix("bytes=")?.split(',').next()?.split_once('-')?;
    let (start, end) = if a.trim().is_empty() {
        let n: u64 = b.trim().parse().ok()?;
        (size.saturating_sub(n), size)
    } else {
        let start: u64 = a.trim().parse().ok()?;
        let end = if b.trim().is_empty() { size } else { b.trim().parse::<u64>().ok()?.saturating_add(1).min(size) };
        (start, end)
    };
    (start < end).then_some((start, end))
}

fn serve(vault: &Vault, req: &HttpRequest<Vec<u8>>) -> Response<Vec<u8>> {
    // the page (http://tauri.localhost) reads these cross-origin
    let cors = |status: u16| {
        ResponseBuilder::new()
            .status(status)
            .header("Access-Control-Allow-Origin", "*")
            .header("Access-Control-Expose-Headers", "Content-Range, Content-Length, Accept-Ranges")
    };
    let fail = |status: u16, msg: String| cors(status).header("Content-Type", "text/plain").body(msg.into_bytes()).unwrap();
    if req.method() == "OPTIONS" {
        return cors(204).header("Access-Control-Allow-Headers", "Range").header("Access-Control-Allow-Methods", "GET, HEAD").body(Vec::new()).unwrap();
    }
    // /web?u=<address>: a picture from one of Discover's sources (web.rs), cached on this PC
    if req.uri().path() == "/web" {
        let url = req.uri().query().and_then(|q| q.strip_prefix("u=")).map(|u| percent_decode_str(u).decode_utf8_lossy().into_owned()).unwrap_or_default();
        return match vault.web.file(&url) {
            Ok((body, kind)) => cors(200).header("Content-Type", if kind.is_empty() { "application/octet-stream".to_string() } else { kind })
                .header("Cache-Control", "max-age=604800").header("Content-Length", body.len().to_string()).body(body).unwrap(),
            Err(e) => fail(502, e),
        };
    }
    // /t/<thumb>?w=<width>: a thumbnail for the grid (see grid_thumb); never changes, so cached for good
    if let Some(rest) = req.uri().path().strip_prefix("/t/") {
        let thumb = percent_decode_str(rest).decode_utf8_lossy().into_owned();
        let width = req.uri().query().and_then(|q| q.strip_prefix("w=")).and_then(|w| w.parse::<u32>().ok()).map(|w| w.clamp(64, 2400));
        let got = match width {
            Some(w) => vault.grid_thumb(&thumb, w),
            None => vault.thumb_bytes(&thumb).map(|b| (b, "image/webp")),
        };
        return match got {
            Ok((body, kind)) => cors(200).header("Content-Type", kind).header("Cache-Control", "max-age=31536000, immutable")
                .header("Content-Length", body.len().to_string()).body(body).unwrap(),
            Err(e) => fail(404, e),
        };
    }
    // /stream?u=<address>: a video on the web, played through here a piece at a time (web.rs stream)
    if req.uri().path() == "/stream" {
        let url = req.uri().query().and_then(|q| q.strip_prefix("u=")).map(|u| percent_decode_str(u).decode_utf8_lossy().into_owned()).unwrap_or_default();
        let range = req.headers().get("range").and_then(|v| v.to_str().ok());
        return match vault.web.stream(&url, range) {
            Ok((status, kind, content_range, body)) => {
                let mut res = cors(status).header("Content-Type", if kind.is_empty() { "video/mp4".to_string() } else { kind })
                    .header("Accept-Ranges", "bytes").header("Content-Length", body.len().to_string());
                if let Some(cr) = content_range {
                    res = res.header("Content-Range", cr);
                }
                res.body(body).unwrap()
            }
            Err(e) => fail(502, e),
        };
    }
    let id = percent_decode_str(req.uri().path().trim_start_matches('/')).decode_utf8_lossy().into_owned();
    // /dl/<id>: a long video just downloaded (web.rs download_start), read once by the page
    let path = match id.strip_prefix("dl/") {
        Some(d) => match vault.web.download_path(d) {
            Some(p) => p,
            None => return fail(404, "That download isn't finished".into()),
        },
        None => match vault.resolve(&id) {
            Ok(p) => p,
            Err(e) => return fail(404, e),
        },
    };
    let mut file = match crypt::Reader::open(&path) {
        Ok(f) => f,
        Err(e) => return fail(404, format!("{}: {e}", path.display())),
    };
    let size = file.len();
    let wanted = req.headers().get("range").and_then(|v| v.to_str().ok());
    let (start, end) = match wanted.map(|v| parse_range(v, size)) {
        // a ranged answer is at most RANGE_PIECE long: a video player asks for "from here to the end"
        // and then for more as it plays. Answering the whole rest each time read hundreds of MB per
        // seek (slow jumps, big memory spikes).
        Some(Some((start, end))) => (start, end.min(start + RANGE_PIECE)),
        Some(None) => return cors(416).header("Content-Range", format!("bytes */{size}")).body(Vec::new()).unwrap(),
        None => (0, size),
    };
    let mut body = vec![0u8; (end - start) as usize];
    if let Err(e) = file.seek(SeekFrom::Start(start)).and_then(|_| file.read_exact(&mut body)) {
        return fail(500, e.to_string());
    }
    let mut res = cors(if wanted.is_some() { 206 } else { 200 })
        .header("Content-Type", mime(&path))
        .header("Accept-Ranges", "bytes")
        .header("Content-Length", body.len().to_string());
    if wanted.is_some() {
        res = res.header("Content-Range", format!("bytes {}-{}/{size}", start, end - 1));
    }
    res.body(body).unwrap()
}

// ---------- the emergency key ----------
const PANIC_KEY: &str = "Ctrl+Shift+Space";

// What the key does (Settings → Privacy → "The emergency key shows"):
//   0 "screensaver" / 1 "video": the window stays where it is and the page covers itself with a
//     screensaver or a (cat) video, full screen; press the key again to come back. The app never
//     disappears, so it can't get "lost" in the background.
//   2 "hide": the window disappears, from the taskbar too. Pressing the key again (or starting the
//     app again) brings it back.
static PANIC_MODE: std::sync::atomic::AtomicU8 = std::sync::atomic::AtomicU8::new(0);
static COVERED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
static WAS_FULLSCREEN: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

fn panic_mode_of(mode: &str) -> u8 {
    match mode { "video" => 1, "hide" => 2, _ => 0 }
}

fn panic_toggle(app: &tauri::AppHandle) {
    use std::sync::atomic::Ordering::Relaxed;
    use tauri::Emitter;
    let Some(w) = app.get_webview_window("main") else { return };
    let visible = w.is_visible().unwrap_or(true);
    let minimized = w.is_minimized().unwrap_or(false);
    let mode = PANIC_MODE.load(Relaxed);
    if mode == 2 && !COVERED.load(Relaxed) {
        // hide: the page is told (it pauses and mutes everything) and the window disappears
        let shown = visible && !minimized;
        if shown {
            let _ = w.emit("panic", json!({ "on": true, "mode": "hide" }));
            let _ = w.hide();
        } else {
            bring_back(&w);
            let _ = w.emit("panic", json!({ "on": false, "mode": "hide" }));
        }
        return;
    }
    // cover (also when the mode was changed while covered: this uncovers)
    let covering = !COVERED.load(Relaxed);
    COVERED.store(covering, Relaxed);
    if covering {
        let _ = w.emit("panic", json!({ "on": true, "mode": if mode == 1 { "video" } else { "screensaver" } }));
        // a hidden or minimized window stays out of sight (it opens on the cover if someone restores it)
        if visible && !minimized {
            WAS_FULLSCREEN.store(w.is_fullscreen().unwrap_or(false), Relaxed);
            let _ = w.set_fullscreen(true);
        }
    } else {
        let _ = w.emit("panic", json!({ "on": false, "mode": "cover" }));
        if !WAS_FULLSCREEN.swap(false, Relaxed) {
            let _ = w.set_fullscreen(false);
        }
        bring_back(&w);
    }
}

fn bring_back(w: &tauri::WebviewWindow) {
    let _ = w.show();
    let _ = w.unminimize();
    let _ = w.set_focus();
}

// Untraceable mode (crypt.rs): the password, from the page's lock screen. Wrong: an error; right:
// the vault opens, and what waited for its settings (the emergency key) is set up.
#[tauri::command(async)]
fn unlock(app: tauri::AppHandle, vault: State<'_, Arc<Vault>>, password: String) -> Res<bool> {
    if !crypt::locked() {
        return Ok(true);
    }
    if password.is_empty() {
        return Err("Type your password".into());
    }
    let key = crypt::derive(&password);
    let check = vault.app_dir("key.check");
    match fs::read(&check) {
        Ok(bytes) => {
            if !crypt::check_ok(&key, &bytes) {
                return Err("Wrong password".into());
            }
            crypt::set_key(Some(key));
        }
        Err(_) => {
            // a new vault, or its check file went missing: a vault already there must open with it
            crypt::set_key(Some(key));
            let index = vault.root.join(INDEX_NAME);
            if index.exists() && !crypt::read(&index).map(|b| b.starts_with(HEADERS[0].as_bytes())).unwrap_or(false) {
                crypt::set_key(None);
                return Err("Wrong password".into());
            }
            fs::create_dir_all(check.parent().unwrap()).map_err(err)?;
            fs::write(&check, crypt::check_bytes_for(&key)).map_err(err)?;
        }
    }
    vault.init();
    if let Some(v) = vault.web.load_sources() {
        vault.web.learn(&v); // (read before the password: nothing then)
    }
    let settings = vault.load_settings();
    set_panic_mode(settings["panicMode"].as_str().unwrap_or("screensaver").to_string());
    if settings["panicOn"].as_bool() != Some(false) {
        let k = settings["panicKey"].as_str().filter(|k| !k.is_empty()).unwrap_or(PANIC_KEY).to_string();
        let _ = set_panic_key(app, k);
    }
    // number-only names too: the file names give nothing away either
    if !DISCREET.load(std::sync::atomic::Ordering::Relaxed) {
        let _ = discreet::start(vault.inner().clone(), true);
    }
    Ok(true)
}

// Settings → Privacy: "screensaver", "video" or "hide".
#[tauri::command]
fn set_panic_mode(mode: String) {
    PANIC_MODE.store(panic_mode_of(&mode), std::sync::atomic::Ordering::Relaxed);
}

// Settings → Privacy → "Pick a video…": the video the cover plays, copied to _app/cover.<ext>.
// Returns its file id (file-<base64 path>), or "" when the dialog was cancelled.
#[tauri::command(async)]
fn pick_cover_video(app: tauri::AppHandle, vault: State<'_, Arc<Vault>>) -> Res<String> {
    use tauri_plugin_dialog::DialogExt;
    let Some(path) = app.dialog().file().set_title("A video to show when you press the emergency key")
        .add_filter("Video", &["mp4", "webm", "m4v", "mov"]).blocking_pick_file().and_then(|f| f.into_path().ok()) else {
        return Ok(String::new());
    };
    let ext = ext_of(&path.file_name().unwrap_or_default().to_string_lossy());
    let ext = if ext.is_empty() { ".mp4".to_string() } else { ext };
    let dir = vault.root.join(APP_NAME);
    for old in fs::read_dir(&dir).into_iter().flatten().flatten() {
        if old.file_name().to_string_lossy().starts_with("cover.") {
            let _ = fs::remove_file(old.path());
        }
    }
    let name = format!("cover{ext}");
    crypt::copy(&path, &dir.join(&name), crypt::current_key().as_ref()).map_err(|e| format!("Could not copy the video: {e}"))?;
    Ok(format!("file-{}", b64(&format!("{APP_NAME}/{name}"))))
}

// Sets the key (e.g. "Ctrl+Shift+Space", "Alt+F12"); an empty one turns it off.
#[tauri::command]
fn set_panic_key(app: tauri::AppHandle, key: String) -> Res<String> {
    use tauri_plugin_global_shortcut::{GlobalShortcutExt, Shortcut};
    let gs = app.global_shortcut();
    gs.unregister_all().map_err(err)?;
    if key.trim().is_empty() {
        return Ok(String::new());
    }
    let sc: Shortcut = key.parse().map_err(|e| format!("{key} is not a key combination: {e}"))?;
    gs.register(sc).map_err(|e| format!("{key} can not be used (another app may have it): {e}"))?;
    Ok(key)
}

// Settings → Storage → "Export to Excel": the library as a styled spreadsheet (excel.rs)
#[tauri::command(async)]
fn export_excel(app: tauri::AppHandle, vault: State<'_, Arc<Vault>>) -> Res<String> {
    use tauri_plugin_dialog::DialogExt;
    let path = app.dialog().file().set_title("Save your library as an Excel file").set_file_name("Lewd Vault library.xlsx")
        .add_filter("Excel workbook", &["xlsx"]).blocking_save_file().and_then(|f| f.into_path().ok()).ok_or("Cancelled")?;
    excel::export(&vault, &path)?;
    Ok(path.display().to_string())
}

// You → Share: a share pack out (a save dialog), a friend's pack in (an open dialog); see pack.rs
#[tauri::command(async)]
fn export_pack(app: tauri::AppHandle, vault: State<'_, Arc<Vault>>, ids: Vec<String>, card: String, title: String) -> Res<Value> {
    use tauri_plugin_dialog::DialogExt;
    let safe: String = title.chars().map(|c| if c.is_alphanumeric() || c == ' ' || c == '-' { c } else { ' ' }).collect::<String>().trim().to_string();
    let path = app.dialog().file().set_title("Save the share pack").set_file_name(format!("{} (Lewd Vault pack).zip", if safe.is_empty() { "Share" } else { &safe }))
        .add_filter("Share pack", &["zip"]).blocking_save_file().and_then(|f| f.into_path().ok()).ok_or("Cancelled")?;
    let png = base64::Engine::decode(&B64S, card.trim_start_matches("data:image/png;base64,")).unwrap_or_default();
    pack::export(&vault, &ids, &png, &title, &path)
}
#[tauri::command(async)]
fn open_pack(app: tauri::AppHandle, vault: State<'_, Arc<Vault>>) -> Res<Value> {
    use tauri_plugin_dialog::DialogExt;
    let path = app.dialog().file().set_title("Open a share pack").add_filter("Share pack", &["zip"])
        .blocking_pick_file().and_then(|f| f.into_path().ok()).ok_or("Cancelled")?;
    pack::open(&vault, &path)
}
const B64S: base64::engine::GeneralPurpose = base64::engine::general_purpose::STANDARD;

// "Another folder…" on the welcome screen: the system's folder picker.
#[tauri::command(async)]
fn pick_folder(app: tauri::AppHandle) -> Option<String> {
    use tauri_plugin_dialog::DialogExt;
    app.dialog().file().set_title("Where should Lewd Vault keep your library?").blocking_pick_folder()
        .and_then(|f| f.into_path().ok()).map(|p| p.display().to_string())
}

// After choosing a new place: start again on it.
#[tauri::command]
fn restart_app(app: tauri::AppHandle) {
    app.restart();
}

// The page has drawn itself: show the window (it starts hidden, see main).
#[tauri::command]
fn window_ready(window: tauri::WebviewWindow) {
    let _ = window.show();
    let _ = window.set_focus();
}

// The page's own title bar buttons. Returns whether the window is maximized.
#[tauri::command]
fn window_action(window: tauri::WebviewWindow, action: String) -> Res<bool> {
    match action.as_str() {
        "minimize" => window.minimize().map_err(err)?,
        "maximize" => {
            if window.is_maximized().map_err(err)? { window.unmaximize().map_err(err)? } else { window.maximize().map_err(err)? }
        }
        "close" => window.close().map_err(err)?,
        // the video player's full screen: the whole monitor, no titlebar
        "fullscreen" => { let on = window.is_fullscreen().map_err(err)?; window.set_fullscreen(!on).map_err(err)?; return Ok(!on); }
        "unfullscreen" => { window.set_fullscreen(false).map_err(err)?; return Ok(false); }
        "hide" => window.hide().map_err(err)?,
        "show" => { window.show().map_err(err)?; window.unminimize().map_err(err)?; window.set_focus().map_err(err)?; }
        // Settings → Look → Size: the whole page larger (text, icons, strokes, everything), "zoom:1.1"
        z if z.starts_with("zoom:") => {
            let scale: f64 = z[5..].parse().map_err(|_| "Bad size".to_string())?;
            window.set_zoom(scale.clamp(0.8, 1.6)).map_err(err)?;
        }
        _ => {}
    }
    window.is_maximized().map_err(err)
}

// Each theme's background (--bg in Index.html / desktop.css), for the moment before the page draws.
fn theme_background(theme: &str) -> tauri::window::Color {
    let hex = match theme {
        "gray" => 0x2b2d31, "white" => 0xf6f6f8, "pink" => 0xfff0f6, "lavender" => 0xf5f0ff, "mint" => 0xecfbf4,
        "peach" => 0xfff4ec, "sky" => 0xeef6ff, "berry" => 0x1c1220, "dark" => 0x0d0d10, _ => 0x2a2226, // rose, the default
    };
    tauri::window::Color((hex >> 16) as u8, (hex >> 8 & 255) as u8, (hex & 255) as u8, 255)
}

fn main() {
    // where the vault lives: chosen on the welcome screen, see storage.rs
    let start = storage::start();
    crypt::set_untraceable(start.untraceable);
    if start.untraceable {
        if let Some(hidden) = start.root.parent() {
            storage::set_hidden(hidden.to_path_buf()); // (its caches and browser data go beside it)
        }
    }
    gdrive::ask_sign_in(start.drive_setup);
    let vault = Arc::new(Vault::new(start.root, start.first_run));
    let for_files = vault.clone();
    let for_window = vault.clone();
    let mut builder = tauri::Builder::default();
    // a second start brings the open window forward instead (two copies would both write index.csv);
    // test copies (IMAGE_VAULT_DEBUG_PORT, with their own IMAGE_VAULT_DATA) run beside the real one
    if std::env::var("IMAGE_VAULT_DEBUG_PORT").is_err() {
        builder = builder.plugin(tauri_plugin_single_instance::init(|app, _args, _cwd| {
            // (show too: after the emergency key in "hide" mode the window is hidden, not minimized,
            // and starting the app again is the natural way to get it back)
            if let Some(w) = app.get_webview_window("main") {
                bring_back(&w);
            }
        }));
    }
    builder
        .plugin(tauri_plugin_dialog::init())
        // the emergency key (Settings → Privacy): hides everything at once, and brings it back
        .plugin(tauri_plugin_global_shortcut::Builder::new()
            .with_handler(|app, _shortcut, event| {
                if event.state() == tauri_plugin_global_shortcut::ShortcutState::Pressed {
                    panic_toggle(app);
                }
            })
            .build())
        .manage({ fastlane::start(vault.clone()); gdrive::start(vault.clone()); vault })
        .register_asynchronous_uri_scheme_protocol("vault", move |_ctx, request, responder| {
            let vault = for_files.clone();
            std::thread::spawn(move || responder.respond(serve(&vault, &request)));
        })
        .invoke_handler(tauri::generate_handler![api, upload, window_ready, window_action, pick_folder, restart_app, set_panic_key, set_panic_mode, pick_cover_video, unlock, export_excel, export_pack, open_pack, maint::install_update])
        .setup(move |app| {
            // the window starts hidden (no white flash) in the theme's background colour; the page
            // shows it once it has drawn itself (window_ready), or after 3 s whatever happens
            let theme = for_window.load_settings()["theme"].as_str().unwrap_or("rose").to_string();
            let mut window = tauri::WebviewWindowBuilder::from_config(app.handle(), &app.config().app.windows[0])?
                .background_color(theme_background(&theme));
            // Untraceable: the browser engine keeps nothing (private mode), and what it must have on
            // disk goes inside the app's hidden folder instead of %LOCALAPPDATA%
            if crypt::untraceable() {
                // (and a dull name on the taskbar and in Alt+Tab)
                window = window.incognito(true).data_directory(storage::hidden_dir().join("web")).title("Notes");
            }
            // WebView2's own defaults, plus (Settings → Library → "Use the faster graphics card", off by
            // default) drawing with the NVIDIA/AMD card when the PC has two. Measured on this laptop it made
            // no difference to scrolling and costs battery, hence off; IMAGE_VAULT_GPU=low|high overrides it.
            // IMAGE_VAULT_DEBUG_PORT=9222 opens WebView2's DevTools protocol on that port (for testing).
            let fast_gpu = match std::env::var("IMAGE_VAULT_GPU").as_deref() {
                Ok("low") => false,
                Ok("high") => true,
                _ => for_window.load_settings()["fastGpu"].as_bool() == Some(true),
            };
            // (autoplay: the emergency key's cover video starts with its sound, without a click first)
            let mut args = String::from("--disable-features=msWebOOUI,msPdfOOUI,msSmartScreenProtection --autoplay-policy=no-user-gesture-required");
            if fast_gpu {
                args.push_str(" --force_high_performance_gpu");
            }
            if let Ok(port) = std::env::var("IMAGE_VAULT_DEBUG_PORT") {
                args.push_str(&format!(" --remote-debugging-port={port}"));
            }
            window = window.additional_browser_args(&args);
            let window = window.build()?;
            let settings = for_window.load_settings();
            set_panic_mode(settings["panicMode"].as_str().unwrap_or("screensaver").to_string());
            if settings["panicOn"].as_bool() != Some(false) {
                let key = settings["panicKey"].as_str().filter(|k| !k.is_empty()).unwrap_or(PANIC_KEY).to_string();
                if let Err(e) = set_panic_key(app.handle().clone(), key) {
                    eprintln!("emergency key not set: {e}");
                }
            }
            std::thread::spawn(move || {
                std::thread::sleep(std::time::Duration::from_secs(3));
                let _ = window.show();
            });
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running Lewd Vault");
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn csv_round_trip() {
        let rows = vec![header_row(&["a", "b"]), vec!["x, \"y\"".into(), "line\r\nbreak".into()]];
        let parsed = parse_csv(&csv_text(&rows));
        assert_eq!(parsed, rows);
    }

    #[test]
    fn conflict_names() {
        for n in ["index (1).csv", "index-DESKTOP-4F2.csv", "Index (Sam's conflicted copy 2026-10-20).csv", "index_2.csv"] {
            assert!(is_conflict_copy(n, "index"), "{n}");
        }
        for n in ["index.csv", "indexes.csv", "index.csv.part", "index.xlsx", "views (1).csv"] {
            assert!(!is_conflict_copy(n, "index"), "{n}");
        }
    }

    #[test]
    fn ranges() {
        assert_eq!(parse_range("bytes=0-99", 1000), Some((0, 100)));
        assert_eq!(parse_range("bytes=900-", 1000), Some((900, 1000)));
        assert_eq!(parse_range("bytes=-10", 1000), Some((990, 1000)));
        assert_eq!(parse_range("bytes=990-5000", 1000), Some((990, 1000)));
        assert_eq!(parse_range("bytes=1000-", 1000), None);
    }

    // an encrypted (untraceable) vault: nothing readable on disk, everything readable through the app
    #[test]
    fn encrypted_vault() {
        let dir = std::env::temp_dir().join(format!("lv-enc-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        crypt::set_key(Some(crypt::derive("correct horse")));
        let vault = Vault::new(dir.clone(), false);
        let index = fs::read(dir.join(INDEX_NAME)).unwrap();
        assert!(index.starts_with(b"LVX1") && !index.windows(4).any(|w| w == b"uid,"));
        let picture: Vec<u8> = b"RIFF....WEBP".iter().copied().chain((0..50_000u32).map(|i| (i % 253) as u8)).collect();
        let id = vault.save_upload("000001_x.webp", &vault.category_folder("Photos").unwrap(), &picture).unwrap();
        let on_disk = fs::read(dir.join("Photos").join("000001_x.webp")).unwrap();
        assert!(on_disk.starts_with(b"LVX1") && !on_disk.windows(4).any(|w| w == b"WEBP"));
        // the page reads it decrypted, byte ranges too
        let req = tauri::http::Request::builder().uri(format!("http://vault.localhost/{}", utf8_percent_encode(&id, NON_ALPHANUMERIC)))
            .header("range", "bytes=8-40007").body(Vec::new()).unwrap();
        let res = serve(&vault, &req);
        assert_eq!(res.status(), 206);
        assert_eq!(res.body().as_slice(), &picture[8..40008]);
        // uploads in pieces, settings, the index
        write_piece(&vault, "tok12345", true, None, &mut &picture[..30_000]).unwrap();
        write_piece(&vault, "tok12345", false, None, &mut &picture[30_000..]).unwrap();
        let id2 = finish_piece(&vault, "tok12345", "000002_y.webp", &vault.category_folder("Photos").unwrap()).unwrap();
        assert_eq!(crypt::read(&vault.resolve(&id2).unwrap()).unwrap(), picture);
        vault.save_settings(&json!({ "theme": "berry" })).unwrap();
        assert_eq!(vault.load_settings()["theme"], "berry");
        assert!(fs::read(vault.app_dir("settings.json")).unwrap().starts_with(b"LVX1"));
        crypt::set_key(None);
        assert!(crypt::read(&dir.join(INDEX_NAME)).is_err());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn names() {
        assert_eq!(safe_name("a:b/c. "), "a-b-c");
        assert_eq!(ext_of("Clip.MP4"), ".mp4");
        assert_eq!(ext_of("noext"), "");
        let n = file_name("000123", "2026-10-16T12:00:00.000Z", "video", "Street Style", 7, 3, ".mp4");
        assert!(n.starts_with("000123_2610") && n.ends_with("_v073_Street Style.mp4"), "{n}");
        assert_eq!(collection_name("  a   b "), "a b");
        assert_eq!(thumb_size("320x240"), "320x240");
        assert_eq!(thumb_size("320x"), "");
        assert_eq!(clean_tags("Blue_Hair, smile  -bad* Copyright:Foo artist:Bob smile"), "blue_hair smile bad series:foo artist:bob");
    }
}
