// Settings → Storage → "Export to Excel": the library as a readable, styled workbook. index.csv and
// views.csv stay plain CSV (Code.gs and the app read them, and a .csv can't hold colours or
// widths); this is the pretty copy to look at, sort and filter.
//   Library: one row per item with its views and time, a pink header, filters, frozen top row,
//            banded rows, sensible widths, real dates and numbers
//   Categories: items, views, time and appreciations per category

use super::{err, Res, Vault};
use rust_xlsxwriter::{Color, Format, FormatAlign, FormatBorder, Workbook};
use std::collections::HashMap;
use std::path::Path;

const PINK: u32 = 0xE27589;
const BLUSH: u32 = 0xFDEFF2;
const WINE: u32 = 0x7C3042;

pub fn export(vault: &Vault, path: &Path) -> Res<()> {
    let (items, _) = vault.load_index()?;
    let (views, _) = vault.load_views()?;
    let by_id: HashMap<&str, _> = views.iter().map(|v| (v.id.as_str(), v)).collect();
    let mut book = Workbook::new();
    let head = Format::new().set_bold().set_font_color(Color::White).set_background_color(Color::RGB(PINK))
        .set_border_bottom(FormatBorder::Medium).set_border_bottom_color(Color::RGB(WINE)).set_align(FormatAlign::VerticalCenter);
    let band = Format::new().set_background_color(Color::RGB(BLUSH));
    let plain = Format::new();
    let date = |banded: bool| (if banded { Format::new().set_background_color(Color::RGB(BLUSH)) } else { Format::new() }).set_num_format("yyyy-mm-dd hh:mm");
    let mins = |banded: bool| (if banded { Format::new().set_background_color(Color::RGB(BLUSH)) } else { Format::new() }).set_num_format("0.0");

    // ---------- Library ----------
    let sheet = book.add_worksheet().set_name("Library").map_err(err)?;
    let cols: [(&str, f64); 18] = [("Number", 9.0), ("Name", 30.0), ("Category", 18.0), ("Kind", 8.0), ("NSFW", 7.0), ("Rating", 9.0), ("Added", 17.0),
        ("Width", 8.0), ("Height", 8.0), ("Length (min)", 11.0), ("Collection", 18.0), ("Tags", 50.0), ("Views", 8.0), ("Appreciations", 13.0),
        ("Minutes looked", 14.0), ("Last viewed", 17.0), ("Source", 40.0), ("File", 34.0)];
    for (c, (name, width)) in cols.iter().enumerate() {
        sheet.write_string_with_format(0, c as u16, *name, &head).map_err(err)?;
        sheet.set_column_width(c as u16, *width).map_err(err)?;
    }
    sheet.set_row_height(0, 22).map_err(err)?;
    let parse_date = |s: &str| chrono::DateTime::parse_from_rfc3339(s).ok().map(|d| {
        let l = d.with_timezone(&chrono::Local).naive_local();
        rust_xlsxwriter::ExcelDateTime::parse_from_str(&l.format("%Y-%m-%dT%H:%M:%S").to_string()).ok()
    }).flatten();
    let stars = |q: i64| "★".repeat(q.clamp(1, 3) as usize);
    for (k, i) in items.iter().enumerate() {
        let r = (k + 1) as u32;
        let banded = k % 2 == 1;
        let f = if banded { &band } else { &plain };
        let v = by_id.get(i.id.as_str());
        let uid: f64 = i.uid.parse().unwrap_or(0.0);
        sheet.write_number_with_format(r, 0, uid, f).map_err(err)?;
        sheet.write_string_with_format(r, 1, &i.name, f).map_err(err)?;
        sheet.write_string_with_format(r, 2, &i.category, f).map_err(err)?;
        sheet.write_string_with_format(r, 3, &i.kind, f).map_err(err)?;
        sheet.write_number_with_format(r, 4, i.nsfw as f64, f).map_err(err)?;
        sheet.write_string_with_format(r, 5, stars(i.quality), f).map_err(err)?;
        match parse_date(&i.added) {
            Some(d) => { sheet.write_datetime_with_format(r, 6, &d, &date(banded)).map_err(err)?; }
            None => { sheet.write_string_with_format(r, 6, &i.added, f).map_err(err)?; }
        }
        if i.width > 0 { sheet.write_number_with_format(r, 7, i.width as f64, f).map_err(err)?; } else { sheet.write_blank(r, 7, f).map_err(err)?; }
        if i.height > 0 { sheet.write_number_with_format(r, 8, i.height as f64, f).map_err(err)?; } else { sheet.write_blank(r, 8, f).map_err(err)?; }
        if i.duration > 0.0 { sheet.write_number_with_format(r, 9, i.duration / 60.0, &mins(banded)).map_err(err)?; } else { sheet.write_blank(r, 9, f).map_err(err)?; }
        sheet.write_string_with_format(r, 10, &i.collection, f).map_err(err)?;
        sheet.write_string_with_format(r, 11, i.tags.replace('_', " "), f).map_err(err)?;
        sheet.write_number_with_format(r, 12, v.map_or(0, |v| v.views) as f64, f).map_err(err)?;
        sheet.write_number_with_format(r, 13, v.map_or(0, |v| v.likes) as f64, f).map_err(err)?;
        sheet.write_number_with_format(r, 14, v.map_or(0.0, |v| v.seconds) / 60.0, &mins(banded)).map_err(err)?;
        match v.and_then(|v| parse_date(&v.last)) {
            Some(d) => { sheet.write_datetime_with_format(r, 15, &d, &date(banded)).map_err(err)?; }
            None => { sheet.write_blank(r, 15, f).map_err(err)?; }
        }
        sheet.write_string_with_format(r, 16, &i.source, f).map_err(err)?;
        sheet.write_string_with_format(r, 17, &i.file, f).map_err(err)?;
    }
    sheet.set_freeze_panes(1, 0).map_err(err)?;
    sheet.autofilter(0, 0, items.len().max(1) as u32, (cols.len() - 1) as u16).map_err(err)?;

    // ---------- Categories ----------
    let cats = book.add_worksheet().set_name("Categories").map_err(err)?;
    let mut sum: HashMap<&str, (f64, f64, f64, f64)> = HashMap::new();
    for i in items.iter() {
        let v = by_id.get(i.id.as_str());
        let e = sum.entry(i.category.as_str()).or_default();
        e.0 += 1.0;
        e.1 += v.map_or(0, |v| v.views) as f64;
        e.2 += v.map_or(0.0, |v| v.seconds) / 60.0;
        e.3 += v.map_or(0, |v| v.likes) as f64;
    }
    let mut rows: Vec<_> = sum.into_iter().collect();
    rows.sort_by(|a, b| b.1 .0.partial_cmp(&a.1 .0).unwrap_or(std::cmp::Ordering::Equal));
    for (c, (name, width)) in [("Category", 24.0), ("Items", 10.0), ("Views", 10.0), ("Minutes looked", 15.0), ("Appreciations", 14.0)].iter().enumerate() {
        cats.write_string_with_format(0, c as u16, *name, &head).map_err(err)?;
        cats.set_column_width(c as u16, *width).map_err(err)?;
    }
    for (k, (name, (n, v, m, l))) in rows.iter().enumerate() {
        let r = (k + 1) as u32;
        let f = if k % 2 == 1 { &band } else { &plain };
        cats.write_string_with_format(r, 0, *name, f).map_err(err)?;
        cats.write_number_with_format(r, 1, *n, f).map_err(err)?;
        cats.write_number_with_format(r, 2, *v, f).map_err(err)?;
        cats.write_number_with_format(r, 3, *m, &mins(k % 2 == 1)).map_err(err)?;
        cats.write_number_with_format(r, 4, *l, f).map_err(err)?;
    }
    cats.set_freeze_panes(1, 0).map_err(err)?;

    book.save(path).map_err(|e| format!("Could not save {}: {e}", path.display()))
}

#[cfg(test)]
mod tests {
    #[test]
    fn writes_a_workbook() {
        let dir = std::env::temp_dir().join(format!("lv-excel-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let vault = super::Vault::new(dir.clone(), false);
        let item = super::super::Item { id: "item-000001".into(), uid: "000001".into(), file: "000001_x.webp".into(), name: "pic.webp".into(),
            category: "Photos".into(), kind: "image".into(), nsfw: 3, quality: 2, added: "2026-10-19T10:00:00Z".into(), width: 800, height: 600,
            tags: "blue_hair artist:someone".into(), ..Default::default() };
        vault.write_index(&[item]).unwrap();
        let out = dir.join("out.xlsx");
        super::export(&vault, &out).unwrap();
        let bytes = std::fs::read(&out).unwrap();
        assert!(bytes.len() > 4000 && bytes.starts_with(b"PK"), "an xlsx is a zip");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
