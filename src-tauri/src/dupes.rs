// Duplicates (Clean up → Find duplicates): every item's thumbnail is shrunk to 9 x 8 grey pixels and
// turned into a 64-bit difference hash (each bit: is this pixel darker than the one to its right).
// Pictures that look the same, even resized, re-saved or slightly recoloured, get hashes that differ
// in only a few bits. It runs on its own threads; the page asks for progress and gets the groups
// (best first: most pixels, then oldest). Hashes are kept on this PC (cache/dupes.json), so the
// next search only hashes what's new.

use super::{storage, Res, Vault};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::fs;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

const NEAR: u32 = 5; // at most this many bits apart

#[derive(Default)]
pub struct Dupes {
    done: AtomicU64,
    total: AtomicU64,
    state: Mutex<Value>, // {status: "", "running", "done" or the error, groups}
}

fn dhash(bytes: &[u8]) -> Option<u64> {
    let img = image::load_from_memory(bytes).ok()?;
    let g = img.resize_exact(9, 8, image::imageops::FilterType::Triangle).to_luma8();
    let (lo, hi) = g.pixels().fold((255u8, 0u8), |(lo, hi), p| (lo.min(p[0]), hi.max(p[0])));
    if hi - lo < 12 {
        return None; // flat (a black first frame, a blank page): would match every other flat one
    }
    let mut h = 0u64;
    for y in 0..8 {
        for x in 0..8 {
            h = (h << 1) | (g.get_pixel(x, y)[0] < g.get_pixel(x + 1, y)[0]) as u64;
        }
    }
    Some(h)
}

pub fn start(vault: Arc<Vault>) -> Res<Value> {
    {
        let mut st = vault.dupes.state.lock().unwrap();
        if st["status"] == "running" {
            return Ok(Value::Bool(true));
        }
        *st = json!({ "status": "running" });
    }
    std::thread::spawn(move || {
        let result = find(&vault);
        let mut st = vault.dupes.state.lock().unwrap();
        *st = match result {
            Ok(groups) => json!({ "status": "done", "groups": groups }),
            Err(e) => json!({ "status": e }),
        };
    });
    Ok(Value::Bool(true))
}

pub fn progress(vault: &Vault) -> Value {
    let mut v = vault.dupes.state.lock().unwrap().clone();
    v["done"] = json!(vault.dupes.done.load(Ordering::Relaxed));
    v["total"] = json!(vault.dupes.total.load(Ordering::Relaxed));
    v
}

fn find(vault: &Vault) -> Res<Vec<Vec<String>>> {
    let (items, _) = vault.load_index()?;
    let items: Vec<_> = items.iter().filter(|i| !i.thumb.is_empty()).cloned().collect();
    let cache_path = storage::local_dir(&vault.root, "cache").join("dupes.json");
    let known: HashMap<String, u64> = fs::read_to_string(&cache_path).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default();
    vault.dupes.total.store(items.len() as u64, Ordering::Relaxed);
    vault.dupes.done.store(0, Ordering::Relaxed);
    let hashes: Mutex<HashMap<String, u64>> = Mutex::new(HashMap::new());
    let next = AtomicU64::new(0);
    let workers = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4).clamp(1, 8);
    std::thread::scope(|s| {
        for _ in 0..workers {
            s.spawn(|| loop {
                let k = next.fetch_add(1, Ordering::Relaxed) as usize;
                let Some(i) = items.get(k) else { break };
                let h = known.get(&i.thumb).copied().or_else(|| vault.thumb_bytes(&i.thumb).ok().and_then(|b| dhash(&b)));
                if let Some(h) = h {
                    hashes.lock().unwrap().insert(i.thumb.clone(), h);
                }
                vault.dupes.done.fetch_add(1, Ordering::Relaxed);
            });
        }
    });
    let hashes = hashes.into_inner().unwrap();
    if let Some(dir) = cache_path.parent() {
        let _ = fs::create_dir_all(dir);
        let _ = fs::write(&cache_path, serde_json::to_string(&hashes).unwrap_or_default());
    }
    // pairs close enough, in the same family (pictures with pictures, moving with moving) and of
    // about the same shape, joined into groups
    let list: Vec<(usize, u64)> = items.iter().enumerate().filter_map(|(k, i)| hashes.get(&i.thumb).map(|h| (k, *h))).collect();
    let mut parent: Vec<usize> = (0..items.len()).collect();
    fn root(p: &mut Vec<usize>, mut x: usize) -> usize {
        while p[x] != x {
            p[x] = p[p[x]];
            x = p[x];
        }
        x
    }
    let moving = |k: usize| items[k].kind != "image";
    let shape = |k: usize| if items[k].width > 0 && items[k].height > 0 { items[k].width as f64 / items[k].height as f64 } else { 0.0 };
    for a in 0..list.len() {
        for b in a + 1..list.len() {
            let ((ka, ha), (kb, hb)) = (list[a], list[b]);
            if (ha ^ hb).count_ones() > NEAR || moving(ka) != moving(kb) {
                continue;
            }
            let (sa, sb) = (shape(ka), shape(kb));
            if sa > 0.0 && sb > 0.0 && (sa - sb).abs() / sa.max(sb) > 0.08 {
                continue;
            }
            let (ra, rb) = (root(&mut parent, ka), root(&mut parent, kb));
            if ra != rb {
                parent[rb] = ra;
            }
        }
    }
    let mut groups: HashMap<usize, Vec<usize>> = HashMap::new();
    for &(k, _) in &list {
        let r = root(&mut parent, k);
        groups.entry(r).or_default().push(k);
    }
    let mut out: Vec<Vec<String>> = groups
        .into_values()
        .filter(|g| g.len() > 1)
        .map(|mut g| {
            g.sort_by(|&x, &y| (items[y].width * items[y].height).cmp(&(items[x].width * items[x].height)).then(items[x].added.cmp(&items[y].added)));
            g.into_iter().map(|k| items[k].id.clone()).collect()
        })
        .collect();
    out.sort_by(|a, b| b.len().cmp(&a.len()));
    Ok(out)
}

#[cfg(test)]
mod tests {
    #[test]
    fn same_picture_resized_is_near() {
        let img = image::RgbImage::from_fn(120, 90, |x, y| image::Rgb([(x * 2) as u8, (y * 2) as u8, ((x + y) % 255) as u8]));
        let enc = |w: u32, h: u32| {
            let small = image::imageops::resize(&img, w, h, image::imageops::FilterType::Triangle);
            let mut out = std::io::Cursor::new(Vec::new());
            image::DynamicImage::ImageRgb8(small).write_to(&mut out, image::ImageFormat::Png).unwrap();
            out.into_inner()
        };
        let (a, b) = (super::dhash(&enc(120, 90)).unwrap(), super::dhash(&enc(60, 45)).unwrap());
        assert!((a ^ b).count_ones() <= super::NEAR);
    }
}
