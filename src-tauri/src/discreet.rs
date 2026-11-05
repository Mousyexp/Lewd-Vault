// Discreet names (Settings → Storage, and on by default for a new vault from the welcome screen):
// every item's file goes into one neutral folder, Media, named by its number only
// (000123_file.webp). Categories, scores and dates stay in index.csv, where the app reads them.
// Off puts everything back into its category's folder with the descriptive name
// (000123_261016_v073_Street Style.mp4).
//
// Switching moves the files one by one on its own thread (the page shows the progress), then writes
// the new names into index.csv. A move cut short is harmless: items are found by their number in any
// folder, and the next switch finishes the job. The choice is kept in _app/kv/storage.json.

use super::{err, ext_of, file_name, safe_name, Res, Vault, DISCREET, MEDIA_FOLDER};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::fs;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

#[derive(Default)]
pub struct Moving {
    done: AtomicU64,
    total: AtomicU64,
    state: Mutex<Value>, // {status: "", "running", "done" or the error}
}

pub fn load(vault: &Vault) {
    let on = fs::read_to_string(vault.app_dir("kv").join("storage.json")).ok()
        .and_then(|t| serde_json::from_str::<Value>(&t).ok()).and_then(|v| v["discreet"].as_bool()).unwrap_or(false);
    DISCREET.store(on, Ordering::Relaxed);
}

pub fn chosen(vault: &Vault) -> bool {
    vault.app_dir("kv").join("storage.json").exists()
}

pub fn start(vault: Arc<Vault>, on: bool) -> Res<Value> {
    {
        let mut st = vault.moving.state.lock().unwrap();
        if st["status"] == "running" {
            return Err("Already moving the files".into());
        }
        *st = json!({ "status": "running" });
    }
    std::thread::spawn(move || {
        let result = run(&vault, on);
        *vault.moving.state.lock().unwrap() = match result {
            Ok(n) => json!({ "status": "done", "moved": n }),
            Err(e) => json!({ "status": e }),
        };
    });
    Ok(Value::Bool(true))
}

pub fn progress(vault: &Vault) -> Value {
    let mut v = vault.moving.state.lock().unwrap().clone();
    v["done"] = json!(vault.moving.done.load(Ordering::Relaxed));
    v["total"] = json!(vault.moving.total.load(Ordering::Relaxed));
    v["discreet"] = json!(DISCREET.load(Ordering::Relaxed));
    v
}

fn run(vault: &Vault, on: bool) -> Res<u64> {
    let was = DISCREET.swap(on, Ordering::Relaxed);
    let kv = vault.app_dir("kv");
    fs::create_dir_all(&kv).map_err(err)?;
    fs::write(kv.join("storage.json"), json!({ "discreet": on }).to_string()).map_err(err)?;
    let (items, _) = vault.load_index()?;
    let items = (*items).clone();
    vault.moving.total.store(items.len() as u64, Ordering::Relaxed);
    vault.moving.done.store(0, Ordering::Relaxed);
    let mut names: HashMap<String, String> = HashMap::new();
    let mut moved = 0u64;
    for i in &items {
        vault.moving.done.fetch_add(1, Ordering::Relaxed);
        if i.uid.is_empty() {
            continue; // (an item from before numbered names: it keeps its place)
        }
        let name = file_name(&i.uid, &i.added, &i.kind, &i.category, i.nsfw, i.quality, &ext_of(&i.file));
        let to = if on { vault.root.join(MEDIA_FOLDER).join(&name) } else { vault.root.join(safe_name(&i.category)).join(&name) };
        let guess = if was { vault.root.join(MEDIA_FOLDER).join(&i.file) } else { vault.root.join(safe_name(&i.category)).join(&i.file) };
        let from = if guess.exists() { guess } else {
            match vault.item_path(&i.uid) { Ok(p) => p, Err(_) => continue }
        };
        if from != to {
            if to.exists() {
                continue; // something already has that name: left alone
            }
            fs::create_dir_all(to.parent().unwrap()).map_err(err)?;
            if fs::rename(&from, &to).is_err() {
                fs::copy(&from, &to).map_err(|e| format!("Could not move {}: {e}", i.file))?;
                let _ = fs::remove_file(&from);
            }
            moved += 1;
        }
        names.insert(i.id.clone(), name);
    }
    vault.change_index(|items| {
        for i in items.iter_mut() {
            if let Some(n) = names.get(&i.id) {
                i.file = n.clone();
            }
        }
    })?;
    // the folders this emptied go: the category folders when on, Media when off
    let empty = |p: &std::path::Path| fs::read_dir(p).map(|mut r| r.next().is_none()).unwrap_or(false);
    if on {
        let emptied: std::collections::HashSet<String> = items.iter().map(|i| safe_name(&i.category)).collect();
        for c in emptied {
            let p = vault.root.join(c);
            if p.is_dir() && empty(&p) {
                let _ = fs::remove_dir(&p);
            }
        }
    } else {
        let p = vault.root.join(MEDIA_FOLDER);
        if p.is_dir() && empty(&p) {
            let _ = fs::remove_dir(&p);
        }
    }
    Ok(moved)
}
