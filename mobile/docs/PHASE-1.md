# iOS port — Phase 1: foundation, format compatibility, Liquid Glass

Phase 1 is the foundation every later phase sits on. It has one rule that outranks everything else:

> **A vault folder must round-trip between Windows and iOS with no loss and no format change.**
> The iOS app reads and writes the *same* folder the Windows app does: same `index.csv`,
> `views.csv`, `_app/` layout, file names, ids and encryption. Nothing in Phase 1 may invent a new
> on-disk shape.

Phase 0 (the throwaway spike: crate restructuring, `tauri ios init`, one video over loopback HTTP)
comes before this. Phase 1 assumes that spike proved media playback works.

The other Phase 1 goal is the **look**: adopt Liquid Glass so the iOS app reads as native rather
than as a Windows app in a phone frame, while keeping the eleven themes the desktop already has
(nine named in `THEME_LIST`, plus `rose` — the default — and `custom`).

Read `docs/PORT.md` first for the research behind these decisions.

---

## What "compatible" concretely means

The format is defined in `src-tauri/src/lib.rs` (the header comment) and produced by
`file_name`, `to_csv`, `views_csv`, `csv_text`, `write_atomic`, `app_dir`, and `crypt.rs`. Phase 1
must reproduce all of it byte-for-byte. The contract:

### 1. Folder layout

```
<vault root>/
  index.csv                      (items)
  views.csv                      (view counts, appreciations, seconds)
  <category>/000123_261016_v073_<category>.mp4     item files, named per file_name()
  _app/thumbs/000123.webp        per-item thumbnails
  _app/thumbs/pack-<stamp>-<n>.bin                  packed thumbnails
  _app/clips/000123.mp4          preview clips
  _app/streams/000123.json       stream metadata
  _app/settings.json             settings (starts with "LVX1" when untraceable)
  _app/state.json                { "nextUid": N }
  _app/backups/<date>/           rolling backups of the three files above
  _app/kv/<name>.json            small documents (resume positions, watch later, profile…)
  _app/key.check                 untraceable password check
  _trash/                        in-vault trash (at the vault ROOT, not under _app)
```

`_app` is `APP_NAME`. Categories are user folders. Nothing here is Windows-specific except the
*parent* choice of vault root (section "Storage roots" below).

### 2. `index.csv`

UTF-8 **with a BOM** (`\u{feff}`), **CRLF** line endings, RFC 4180 quoting (a cell is quoted only
when it contains `"`, `,`, `\r` or `\n`; `"` doubled). Header order is fixed:

```
id,uid,file,original_name,category,kind,nsfw,quality,added,width,height,
duration,preview_id,thumb,thumbhash,stream_id,thumb_size,collection,collection_cover,tags,source
```

Rules that must match exactly:

- Numbers are written the way JavaScript prints them: `0` → empty, integer → no `.0`
  (`num_or_empty`).
- `collection_cover` is `1` or empty.
- `kind` is one of `image`, `gif`, `video`.
- Rows are read by **column name**, not position, so older files with fewer columns still load
  (`parse_rows` / `Row::get`). Keep this tolerance; a newer iOS build may add a column and Windows
  must still read the file, and vice versa. **Do not add or reorder columns in Phase 1.**
- Timestamps in `added` are RFC 3339; the file-name date is that timestamp in **local time**
  (`%y%m%d`). Timezone handling must be identical or file names diverge.

### 3. `views.csv`

```
id,uid,views,last_viewed,appreciations,last_appreciated,seconds_viewed
```

`seconds_viewed` is rounded to one decimal (`(s*10).round()/10`, then `num_or_empty`).

### 4. Item file names (`file_name`)

- Normal: `{uid}_{YYMMDD}_{code}{nsfw:02}{quality}_{category}{ext}`, where `code` is `i`/`g`/`v`
  and `category` goes through `safe_name`.
- Discreet mode: `{uid}_file{ext}`.
- `ext_of` only accepts an extension of 1–5 ASCII alphanumerics, lowercased.

### 5. Ids (the page-facing layer)

`item-000123`, `clips-<base64url>`, `thumbs-<base64url>`, `streams-<base64url>`,
`file-<base64url path>`. Base64 is URL-safe, **no padding** (`URL_SAFE_NO_PAD`). Any iOS code that
mints ids must produce the same strings, because the index stores them (`thumb`, `preview_id`,
`stream_id`).

### 6. `state.json`

`{ "nextUid": N }`, allocated under a lock so two writers cannot collide. On iOS, allocation must
still read-modify-write the same file (see "Concurrency" below).

### 7. Untraceable encryption (`crypt.rs`)

`"LVX1"` + 12-byte nonce + bytes XORed with the ChaCha20 keystream from that nonce. Key is
PBKDF2-HMAC-SHA256(password, `b"lewd-vault/untraceable/v1"`, 310000 rounds). **Salt is fixed**, so
the same password gives the same key on any device — that is what lets an encrypted vault sync
between Windows and iOS. A plain file (no `LVX1` prefix) is still read as plain, even in an
untraceable vault. Reproduce this exactly; a different salt, nonce size or round count makes the
two apps mutually unreadable.

### 8. Thumbnail packs

`pack-<stamp>-<n>.bin` is a concatenation of slices; the index points at one with
`thumbs-<b64>:<offset>:<length>`. Keep the same packing so Windows can read an iOS-made pack and
vice versa. Phase 1 only has to *read* packs correctly; writing them is Phase 3.

### Compatibility test matrix (the acceptance gate)

Add these as an iOS-side test target and, where possible, a shared fixture set:

| Case | Expected |
| --- | --- |
| Windows-made vault (plain) opened on iOS | Every item lists, thumbnails and media play, tags/collections intact. |
| iOS-made vault opened on Windows | Same, with no `git diff`-style change to the CSV shape. |
| Same item added on both, then folder copied | Ids stay stable; no duplicate rows; no column reorder. |
| Untraceable vault made on Windows | Opens on iOS with the same password. |
| Untraceable vault made on iOS | Opens on Windows with the same password. |
| Untraceable vault made by an independent implementation | Opens with the same password (the fixture test). |
| Vault with an older, shorter CSV header | Loads on iOS (by-name columns). |
| Vault with a packed-thumbnail `thumb` value | Thumbnails resolve on iOS. |
| `index (1).csv` / `index-DESKTOP-4F2.csv` conflict copies | Folded the same way as Windows. |
| Non-ASCII category names | `safe_name` output identical on both. |
| `\r\n` inside a tag/category cell | Quoted identically on both. |

The cleanest way to hold this honest is a **golden-vault fixture**: a small folder checked in (or
generated by `tools/`) with a known `index.csv`/`views.csv`, and a test on each platform that reads
it and writes it back unchanged. `npm test` (`tools/check.mjs`) can gain a step that runs the
fixture through the JS side; a Rust test in `src-tauri` covers the backend side.

---

## Storage roots on iOS (the one place the layout legitimately differs)

The *format* is identical; only the *root* differs.

- Windows picks the root next to the `.exe` or in `Documents`/`%LOCALAPPDATA%` (`storage.rs`).
- iOS cannot write next to itself. Root candidates: the app's **Documents** directory (visible in
  the Files app with `UIFileSharingEnabled` + `LSSupportsOpeningDocumentsInPlace`) or
  **Application Support** (hidden). Recommend Documents for the normal vault, Application Support
  for the untraceable hidden folder — mirroring the Windows split between the visible folder and
  `data.temp`.
- Replace `exe_dir`, `local_home`, `local_base`, `appdata_config` and the cloud-folder probes
  (`google_drive`, `onedrive`, `dropbox`) with a `#[cfg(target_os = "ios")]` implementation that
  returns sandbox paths. Keep the Windows branch untouched.
- "Point the vault at a synced folder" (Drive/OneDrive/Dropbox/iCloud) does not exist on iOS. Use
  the **Drive API sync** already in `gdrive.rs` instead; that keeps the folder format identical,
  which is the whole point.

---

## Concurrency (this is where round-trip compatibility actually breaks)

Two writers on the same `index.csv` is the classic corruption case: the desktop app is
single-instance and takes a `Mutex` (`Vault.lock`), but a synced folder means *two devices* can
edit at once. Windows already handles this with `write_atomic` plus conflict-copy folding
(`is_conflict_copy`). iOS must do the same, and additionally:

- **Write the whole file atomically**: temp file in the same directory, `fsync`, then rename. Never
  truncate-in-place.
- **Preserve the BOM and CRLF** on every rewrite (a rewrite that drops the BOM makes Windows
  re-add it, producing a spurious diff and, on some sync tools, a conflict copy).
- **Refuse to write if the file changed under us**: keep the md5 the app read (`IndexCache` already
  does this) and re-read+re-merge if it no longer matches, rather than clobbering.
- **Allocate `nextUid` from `state.json` under a lock** and treat a collision as "re-read and
  retry", so an iOS and a Windows device cannot both mint `000123`.
- **Never reorder columns or rows** on a plain read/write cycle. A round-trip of an untouched vault
  must be byte-identical.

Add a test: read a fixture, write it back with no changes, assert the bytes are identical.

---

## Liquid Glass

### What it is, and what the webview can and cannot do

Liquid Glass is Apple's iOS 26 / macOS 26 material (WWDC25). It is adopted automatically by
**SwiftUI/UIKit/AppKit standard components** — bars, sheets, popovers, toolbars, tab bars, controls.
A Tauri app draws **one full-screen WKWebView**; it has no UIKit chrome for the system to restyle,
so there is no "free" adoption. The glass has to be *drawn by the web content*, or *hosted by
native views the app adds around/over the webview*.

Two real levers, in order of fidelity:

**Lever A — the native glass material (`-apple-visual-effect`).** WebKit exposes Apple's system
materials to CSS. From WebKit's `CSSProperties.json` and `CSSValueKeywords.in`, the values include:

```
-apple-system-glass-material
-apple-system-glass-material-clear
-apple-system-glass-material-subdued
-apple-system-glass-material-media-controls
-apple-system-glass-material-media-controls-subdued
```

plus blur materials and vibrancy values. These are **real system materials** (the same ones UIKit
uses), so they refract and adapt like the system does. Caveats, all verified in WebKit source:

- The property is `-apple-visual-effect`, and the glass values are gated on
  `HAVE_MATERIAL_HOSTING`, which `wtf/PlatformHave.h` defines for **`PLATFORM(IOS)`**, macOS 16+,
  Mac Catalyst and tvOS. So the values are compiled in for iOS.
- `CSSProperties.json` also gates the whole property on `HAVE_CORE_MATERIAL`, which is
  `PLATFORM(COCOA)`. iOS qualifies.
- It is **non-standard** and undocumented by Apple for web authors, and its codegen is gated behind
  WebKit's internal `useSystemAppearance` setting (`CSSProperties.json`), so it is only honoured
  when the system appearance is in play. Treat it as progressive enhancement, never as the only
  path.
- To be safe, test it early on the real target (Phase 1 exit check) and keep Lever B as the
  fallback. If the property is ignored, the UI must still look right.

**Lever B — the web approximation (`backdrop-filter`).** The portable path, and what the app should
rely on by default:

- `backdrop-filter` is supported on iOS Safari/WebKit (MDN BCD: Safari 18 unprefixed; the
  `-webkit-` prefix goes back to iOS 9). **The pre-Phase-1 `ui/desktop.css` had 27 `backdrop-filter`
  uses and zero `-webkit-backdrop-filter`; the prefix has since been added next to every use there
  (now 31 of each), and `tools/check.mjs` fails the build if a new unprefixed use appears — it scans
  both `ui/desktop.css` and the page's own inline `<style>` in `ui/Index.html` (which had one
  unpaired `.show-bar` use that the desktop-only scan used to miss)** — the prefix is the safe bet
  on older iOS and is harmless on new.
- The app already uses the pattern: the sidebar is
  `background: color-mix(in srgb, var(--panel) 62%, transparent); backdrop-filter: blur(18px) saturate(1.3);`.
  Liquid Glass is a *refinement* of exactly this: more blur, a saturation/contrast lift, a subtle
  inner highlight (top edge), a hairline border, and a soft specular sheen.
- Respect accessibility: `@media (prefers-reduced-transparency: reduce)` → solid backgrounds;
  `@media (prefers-reduced-motion: reduce)` → no morph animation. Apple's own guidance is explicit
  that transparency and motion must adapt to these settings.

### The "glass" design tokens

Add a small set of tokens (in `ui/desktop.css`, alongside the existing `--bg/--panel/--line/--text/
--dim/--accent` tokens) so all eleven themes (nine named + `rose` + `custom`) keep working. Sketch:

```css
:root {
  --glass-blur: 24px;
  --glass-saturate: 1.8;
  --glass-tint: color-mix(in srgb, var(--panel) 55%, transparent);
  --glass-edge: color-mix(in srgb, var(--text) 14%, transparent);   /* hairline */
  --glass-sheen: rgba(255,255,255,.10);                              /* top highlight */
  --glass-radius: 22px;                                              /* iOS corner feel */
}

@supports ((backdrop-filter: blur(1px)) or (-webkit-backdrop-filter: blur(1px))) {
  .glass {
    background: var(--glass-tint);
    -webkit-backdrop-filter: blur(var(--glass-blur)) saturate(var(--glass-saturate));
            backdrop-filter: blur(var(--glass-blur)) saturate(var(--glass-saturate));
    border: 1px solid var(--glass-edge);
    border-radius: var(--glass-radius);
    box-shadow: inset 0 1px 0 var(--glass-sheen), var(--shadow);
  }
}

/* the native material, when the platform offers it */
@supports (-apple-visual-effect: -apple-system-glass-material) {
  html.ios .glass { -apple-visual-effect: -apple-system-glass-material; }
}

@media (prefers-reduced-transparency: reduce) {
  .glass { background: var(--panel); -webkit-backdrop-filter: none; backdrop-filter: none; }
}
```

Elements to glass, chosen to match where iOS puts glass (functional layer, not content):

- the **sidebar / nav** (already translucent — the easiest win),
- **top bars / toolbars**, the search field,
- **sheets, popovers, toasts, context menus, the player controls**,
- **bottom tab bar** on phone widths,
- **floating action buttons** (the "add" affordance).

Do **not** glass the content grid, tiles or thumbnails — Apple's guidance is to keep glass to the
functional layer so content stays the focus.

### Liquid Glass in the layout

Liquid Glass is as much a *shape and motion* system as a material. Phase 1 should:

- Increase corner radii toward the iOS look (`--radius`, `--btn-radius`, `--glass-radius`); controls
  "nestle into the corners".
- Add the **morph** on press: a control that fluidly becomes a menu/popover. On the web this is a
  scale/opacity transition plus a rounded-rect→sheet morph; keep it subtle and disable it under
  `prefers-reduced-motion`.
- Add the **scroll edge effect**: content scrolling under a bar fades/hardens at the edge instead of
  a hard line. On the web, a `mask-image` gradient at the top/bottom of the scroll container, or a
  gradient overlay under the bar.
- Respect **safe areas**: `viewport-fit=cover` and `env(safe-area-inset-*)` padding, so glass bars
  sit correctly under the notch and home indicator.

### App icon

The desktop ships `icon.ico`/PNGs. iOS needs an **asset catalog** with Liquid Glass icon layers
(Xcode's Icon Composer; light/dark/clear/tinted variants). `tauri icon` can generate the catalog
from a source PNG, but the *layered* Liquid Glass icon needs Icon Composer on a Mac. Phase 1
delivers a plain generated catalog so the app builds; a layered icon is a Phase 5 polish item.

### Honesty about fidelity

A WKWebView app cannot get *pixel-identical* Liquid Glass: the real material refracts the content
behind it and reacts to device motion/lighting, which only native `UIGlassEffect` views do. The
web approximation gets close in still screenshots and very close in feel if the blur, saturation,
edge highlight and radii are tuned. If full fidelity is required later, the path is **native UIKit
bars/glass layered around the webview** (a Tauri plugin exposing `UIGlassEffect` views) — note it
as a possible Phase 6, not a Phase 1 requirement.

---

## Phase 1 work breakdown

Each item names the files it touches and a done-check.

### 1.0 — Crate restructure and iOS target (from Phase 0, hardened)

- `src-tauri/Cargo.toml`: add `[lib] name = "image_vault_lib" crate-type = ["staticlib","cdylib","rlib"]`.
- `src-tauri/src/lib.rs`: `#[cfg_attr(mobile, tauri::mobile_entry_point)] pub fn run()`.
- `src-tauri/src/main.rs`: becomes `fn main() { image_vault_lib::run() }`.
- `src-tauri/tauri.conf.json`: add `bundle.iOS` (minimum version, team placeholder, Info.plist path).
- **Done:** `cargo check` passes on the host; `tauri ios build` produces an app bundle (signed or
  `--no-sign`) in CI.

### 1.1 — Platform layer for storage roots

- `src-tauri/src/storage.rs`: add `#[cfg(target_os = "ios")]` paths (Documents / Application
  Support), no-op `hide`, drop the cloud-folder probes on iOS, keep `local_base` semantics.
- **Done:** the app launches on iOS, creates `_app/`, writes `settings.json` and `state.json` in the
  sandbox, and the vault root is reported correctly through the existing `dataFolder` call.

### 1.2 — One computed origin (kills the hardcoded Windows URL)

- `sync-ui.mjs`: replace the hardcoded `http://vault.localhost/` in `vaultUrl` with a value derived
  from the platform. **Done** — the backend hands it over as `window.__VAULT_ORIGIN` (an
  initialization script), not via `api(...)` or `window.__TAURI__`.
- `src-tauri/src/lib.rs`: `for_page` and `getThumb` must emit the same base (`vault_url()`).
- **Done:** on iOS, images load; on Windows, nothing changes.

### 1.3 — Media over loopback HTTP

- Add a media server (reuse the `fastlane.rs` pattern: `127.0.0.1:0`, random token, `206`/`Content-Range`,
  4 MB `RANGE_PIECE`). Keep `vault://` for the page itself. **Done** (`media.rs`); the guard is the
  per-launch token in the first path segment, not an `Origin` check (see `docs/PORT.md` §3.3).
- Point `vaultUrl` at the loopback base on iOS. **Done** (via `window.__VAULT_ORIGIN`).
- Add the ATS exception in `Info.plist` (`NSAllowsLocalNetworking`, and/or an `127.0.0.1`
  `NSExceptionDomains` entry). **Done** (`src-tauri/Info.plist`).
- **Done:** video seeks, audio plays, thumbnails and grid images load on a real device.

### 1.4 — Vault format: read/write parity

- Port/verify the CSV writer (`csv_text`, `to_csv`, `views_csv`), `file_name`, `ext_of`,
  `safe_name`, id minting (`URL_SAFE_NO_PAD`), `state.json` allocation, and the `crypt.rs` format on
  iOS (it is already pure Rust — mostly a compile-and-test job).
- Add the **golden-vault fixture** and the round-trip byte-identity test.
- Add a **golden untraceable vault** and a test that opens it. It is generated by
  `tools/chacha20.py` — a second implementation of the encryption written from the spec, not shared
  with `crypt.rs`, and checked against the RFC 8439 keystream vector — so the test proves the two
  implementations interoperate rather than only agreeing with themselves. `npm test` re-checks the
  fixture through that same script.
- Add the compatibility matrix tests from above.
- **Done:** every row of the compatibility matrix passes.

### 1.5 — Concurrency hardening

- Atomic writes with `fsync` + rename on iOS; BOM/CRLF preserved; md5-guarded re-merge; locked
  `nextUid` with retry; conflict-copy folding reused.
- **Done:** the round-trip byte-identity test passes; a simulated concurrent-write test does not lose
  rows.

### 1.6 — Liquid Glass, stage 1 (the material)

- Add the glass tokens and the `.glass` class to `ui/desktop.css`; add `-webkit-backdrop-filter`
  next to all 31 `backdrop-filter` uses in `ui/desktop.css` (and the one in `ui/Index.html`'s own
  `<style>`).
- Apply to sidebar/nav, bars, search, sheets/popovers/toasts/menus, player controls, FABs.
- Add the `@supports (-apple-visual-effect: …)` native enhancement behind an `html.ios` class.
- Add the `prefers-reduced-transparency` / `prefers-reduced-motion` fallbacks.
- **Done:** looks glassy on iOS, unchanged-but-fine on Windows, and degrades cleanly with reduced
  transparency on.

### 1.7 — Liquid Glass, stage 2 (shape, motion, safe areas)

- Radii toward iOS; press-morph transitions; scroll edge effect; `viewport-fit=cover` + safe-area
  insets; a bottom tab bar for phone widths.
- **Done:** no control sits under the notch/home indicator; the bars show the scroll edge effect.

### 1.8 — iOS app icon catalog

- `tauri icon` to generate `Assets.xcassets`; wire it in `tauri.conf.json`.
- **Done:** the app installs with a real icon (layered Liquid Glass icon deferred).

### 1.8b — The CI job that builds the IPA

- `.github/workflows/ios.yml` on `macos-latest`: `tauri ios init --ci`, then `tauri icon` (so the
  real icon lands in the catalog), then `tauri ios build` (`--no-sign` unless Apple API key secrets
  are set) and upload the IPA. Manual dispatch plus a path-filtered pull request.
- **Done:** a green run produces a `LewdVault-ipa` artifact.

### 1.9 — Verify

- `npm test` (extended with the fixture step) passes.
- `cargo test` passes on the host.
- The app runs on a real iOS device: vault opens, media plays, and a vault made on Windows opens on
  iOS and back.
- **Done:** Phase 1 exit criteria met; write the results into `docs/GUIDE.md`.

---

## Phase 1 exit criteria

1. The app builds for iOS in CI (`tauri ios build`, signed or `--no-sign`).
2. It launches on a real device and opens a vault in the iOS sandbox.
3. **A Windows-made vault opens on iOS and an iOS-made vault opens on Windows, with no format
   change and a byte-identical round-trip when nothing is edited.**
4. Media (images, video with seeking, audio) plays on iOS.
5. The UI has Liquid Glass surfaces (with the native `-apple-visual-effect` where available and the
   `backdrop-filter` fallback everywhere), correct safe areas, and reduced-transparency support.
6. `npm test` and `cargo test` pass, with the golden-vault fixture included.

---

## Risks specific to Phase 1

| Risk | Mitigation |
| --- | --- |
| `-apple-visual-effect` is ignored in the Tauri webview | Lever B (`backdrop-filter`) is the default; the native value is pure enhancement. Confirm on device at 1.6. |
| A CSV rewrite drops the BOM or changes line endings, causing sync conflicts | Round-trip byte-identity test is a hard gate (1.5). |
| Two devices mint the same uid | Locked `state.json` allocation with retry (1.5). |
| `crypt.rs` diverges (salt/nonce/rounds) and the encrypted vaults stop matching | Keep the constants literal; test a Windows-made untraceable vault on iOS (1.4). |
| Local timezone differences change `file_name` dates | Test with a vault made in a different timezone; the date comes from the stored `added` timestamp. |
| Loopback media server reachable by other apps | Bind `127.0.0.1` only, per-launch token in the first path segment (the `fastlane.rs` pattern; not an `Origin` check — see `docs/PORT.md` §3.3). |
| CSP header over WebKit's 16 KiB limit | Measured: with `csp: null` no CSP header is emitted at all, so the limit cannot be hit; re-measure only if a CSP is added later. |

---

## Explicitly deferred (not Phase 1)

- Google Drive two-way sync polish, Discover/Reddit sign-in, Excel/share-pack export UX (Phase 3+).
- Discreet-mode folder renaming, Untraceable scrubbing of Windows traces (Windows-only concepts).
- The emergency key as a system shortcut (no global hotkeys on iOS — Phase 4 decision).
- Self-update (dropped on iOS).
- Packed-thumbnail *writing* and the cleanup/health-check jobs (Phase 3).
- A layered Liquid Glass app icon via Icon Composer (Phase 5).
- Native `UIGlassEffect` views around the webview, if the web approximation is not enough (Phase 6).
