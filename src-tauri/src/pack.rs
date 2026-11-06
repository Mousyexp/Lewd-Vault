// Share packs (You → Share): a few items as one .zip to give a friend, and a friend's pack opened
// into your vault. A pack holds:
//   lewd-vault-pack.json   what's in it: each item's name, category, NSFW, rating, tags, collection,
//                          source, kind, size, length and its file's name in the pack
//   files/<n>.<ext>        the items' files, as they are in the vault
//   card.png               a picture of the pack (made by the page)
// The files are stored as they are (they're compressed already), streamed from disk.
// Opening a pack unpacks its files into _app/streams (named pack-<token>-<n>.<ext>) for the page to
// add like any dropped file (thumbnails, previews and all); then they're removed.

use super::{err, Res, Vault};
use serde_json::{json, Value};
use std::fs;
use std::io::{Read, Write};
use std::path::Path;

const MANIFEST: &str = "lewd-vault-pack.json";
const MAX_ITEMS: usize = 2000;

pub fn export(vault: &Vault, ids: &[String], card_png: &[u8], title: &str, path: &Path) -> Res<Value> {
    let (items, _) = vault.load_index()?;
    let chosen: Vec<_> = ids.iter().filter_map(|id| items.iter().find(|i| &i.id == id)).take(MAX_ITEMS).collect();
    if chosen.is_empty() {
        return Err("Nothing to put in the pack".into());
    }
    let file = fs::File::create(path).map_err(|e| format!("Could not create {}: {e}", path.display()))?;
    let mut zip = zip::ZipWriter::new(file);
    let opts = zip::write::SimpleFileOptions::default().compression_method(zip::CompressionMethod::Stored).large_file(true);
    let mut list = Vec::new();
    for (k, i) in chosen.iter().enumerate() {
        let src = vault.resolve(&i.id)?;
        let ext = src.extension().map(|e| e.to_string_lossy().to_lowercase()).unwrap_or_else(|| "bin".into());
        let name = format!("files/{:04}.{ext}", k + 1);
        zip.start_file(name.as_str(), opts).map_err(err)?;
        let mut f = super::crypt::Reader::open(&src).map_err(err)?; // (an untraceable vault's files go out plain: the pack is for sharing)
        std::io::copy(&mut f, &mut zip).map_err(err)?;
        list.push(json!({
            "file": name, "name": i.name, "category": i.category, "nsfw": i.nsfw, "quality": i.quality, "tags": i.tags,
            "collection": i.collection, "source": i.source, "kind": i.kind, "width": i.width, "height": i.height, "duration": i.duration
        }));
    }
    if !card_png.is_empty() {
        zip.start_file("card.png", opts).map_err(err)?;
        zip.write_all(card_png).map_err(err)?;
    }
    let manifest = json!({ "app": "Lewd Vault", "version": 1, "title": title, "made": chrono::Utc::now().to_rfc3339(), "items": list });
    zip.start_file(MANIFEST, opts).map_err(err)?;
    zip.write_all(serde_json::to_string_pretty(&manifest).map_err(err)?.as_bytes()).map_err(err)?;
    zip.finish().map_err(err)?;
    Ok(json!({ "count": chosen.len(), "path": path.display().to_string() }))
}

pub fn open(vault: &Vault, path: &Path) -> Res<Value> {
    let file = fs::File::open(path).map_err(|e| format!("Could not open {}: {e}", path.display()))?;
    let mut zip = zip::ZipArchive::new(file).map_err(|_| "That isn't a share pack (not a .zip)".to_string())?;
    let manifest: Value = {
        let m = zip.by_name(MANIFEST).map_err(|_| "That .zip isn't a Lewd Vault share pack".to_string())?;
        let mut text = String::new();
        m.take(10_000_000).read_to_string(&mut text).map_err(err)?;
        serde_json::from_str(&text).map_err(|e| format!("The pack's list is damaged: {e}"))?
    };
    let token = super::md5_hex(format!("{}{:?}", path.display(), std::time::SystemTime::now()).as_bytes())[..10].to_string();
    let dir = vault.app_dir("streams");
    fs::create_dir_all(&dir).map_err(err)?;
    let mut out = Vec::new();
    for (k, it) in manifest["items"].as_array().cloned().unwrap_or_default().into_iter().take(MAX_ITEMS).enumerate() {
        let inner = it["file"].as_str().unwrap_or("");
        if !inner.starts_with("files/") || inner.contains("..") {
            continue;
        }
        let ext: String = Path::new(inner).extension().map(|e| e.to_string_lossy().to_lowercase()).unwrap_or_default().chars().filter(|c| c.is_ascii_alphanumeric()).take(5).collect();
        let name = format!("pack-{token}-{k:04}.{ext}");
        let mut entry = match zip.by_name(inner) { Ok(e) => e, Err(_) => continue };
        let mut f = super::crypt::Writer::create(&dir.join(&name)).map_err(err)?;
        std::io::copy(&mut entry, &mut f).map_err(err)?;
        let mut v = it.clone();
        v["id"] = json!(format!("streams-{}", super::b64(&name)));
        out.push(v);
    }
    let card = match zip.by_name("card.png") {
        Ok(mut c) => {
            let name = format!("pack-{token}-card.png");
            let mut f = super::crypt::Writer::create(&dir.join(&name)).map_err(err)?;
            std::io::copy(&mut c, &mut f).map_err(err)?;
            json!(format!("streams-{}", super::b64(&name)))
        }
        Err(_) => Value::Null,
    };
    Ok(json!({ "token": token, "title": manifest["title"], "made": manifest["made"], "items": out, "card": card }))
}

// the unpacked files go once they're added
pub fn tidy(vault: &Vault, token: &str) -> Res<Value> {
    if token.is_empty() || !token.chars().all(|c| c.is_ascii_hexdigit()) {
        return Err("Bad pack".into());
    }
    let prefix = format!("pack-{token}-");
    let mut n = 0;
    for f in fs::read_dir(vault.app_dir("streams")).map_err(err)?.flatten() {
        if f.file_name().to_string_lossy().starts_with(&prefix) && fs::remove_file(f.path()).is_ok() {
            n += 1;
        }
    }
    Ok(json!(n))
}

#[cfg(test)]
mod tests {
    #[test]
    fn round_trip() {
        let dir = std::env::temp_dir().join(format!("lv-pack-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(dir.join("Photos")).unwrap();
        let vault = super::Vault::new(dir.clone(), false);
        std::fs::write(dir.join("Photos").join("000001_x.webp"), b"RIFF....WEBPfake picture bytes").unwrap();
        let item = super::super::Item { id: "item-000001".into(), uid: "000001".into(), file: "000001_x.webp".into(), name: "pic.webp".into(),
            category: "Photos".into(), kind: "image".into(), nsfw: 4, quality: 3, tags: "blue_hair".into(), source: "https://example.com/1".into(), ..Default::default() };
        vault.write_index(&[item]).unwrap();
        let out = dir.join("pack.zip");
        let r = super::export(&vault, &["item-000001".into()], b"\x89PNG fake", "Test", &out).unwrap();
        assert_eq!(r["count"], 1);
        let opened = super::open(&vault, &out).unwrap();
        let items = opened["items"].as_array().unwrap();
        assert_eq!(items.len(), 1);
        assert_eq!(items[0]["tags"], "blue_hair");
        assert_eq!(items[0]["nsfw"], 4);
        let id = items[0]["id"].as_str().unwrap();
        let back = std::fs::read(vault.resolve(id).unwrap()).unwrap();
        assert_eq!(back, b"RIFF....WEBPfake picture bytes");
        assert!(opened["card"].is_string());
        assert_eq!(super::tidy(&vault, opened["token"].as_str().unwrap()).unwrap(), 2);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
