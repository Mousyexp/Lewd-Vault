# AGENTS.md

Repository-specific knowledge for future sessions. Keep this short and factual.

## What this is

Lewd Vault (formerly "Image Vault"): a Windows-only Tauri 2 desktop app for collecting, tagging and
viewing NSFW media, shipped as a portable `~6 MB` `Lewd Vault.exe` (or an MSI). The frontend is a
shared page (`ui/Index.html` + `ui/*.js` + `ui/desktop.css`) that is also the Google Apps Script web
version; `sync-ui.mjs` patches it into `src/`, which Tauri serves.

## Build

- `npm install`, then `npm run dist` (needs Rust + VS C++ Build Tools + Node).
- `npm run sync` regenerates `src/` from `ui/`. Never edit `src/` by hand.
- Every patch in `sync-ui.mjs` must match exactly once, or the build stops.
- Edit the shared page in `ui/Index.html`; edit desktop-only additions in `ui/*.js`.
- Tests: `npm test` runs `tools/check.mjs`; `cargo test` covers the Rust pieces (`src-tauri`).
- Build output (`src/`, `src-tauri/target/`, `src-tauri/gen/`, `dist/`) is git-ignored.

## Architecture notes

- **One Rust binary today**: `src-tauri/src/main.rs` has `fn main()`. Tauri **mobile** needs a
  `[lib] crate-type = ["staticlib", "cdylib", "rlib"]` plus `lib.rs::run()` and a thin `main.rs`.
- **The page talks to Rust through one command**: `api(fn, ...args)` → `tauri::generate_handler![api,
  ...]` in `main.rs`, which mirrors the Apps Script `Code.gs` calls.
- **Custom protocol**: files are served by a `vault` protocol registered with
  `register_asynchronous_uri_scheme_protocol`; `serve()` in `main.rs` handles byte ranges (`206`,
  `Content-Range`, a 4 MB `RANGE_PIECE`). On Windows/Android the origin is
  `http://vault.localhost`, **elsewhere `vault://localhost`** (see
  `crates/tauri/src/protocol/mod.rs`). The frontend currently hardcodes the Windows form in
  `sync-ui.mjs` (`vaultUrl`) and the Rust side emits it in `getThumb`/the item list.
- **Loopback server**: `fastlane.rs` binds `127.0.0.1:0`, picks a free port, requires a random
  `x-key` on every request, and checks `Origin`. Reusable pattern for any local HTTP need.
- **Windows-only paths**: `explorer` (open links/folders), `trash` crate (no iOS backend),
  `%LOCALAPPDATA%`/`%APPDATA%`/`USERPROFILE` paths, `attrib +h +s`, and the
  `tauri-plugin-global-shortcut` emergency key (`Ctrl+Shift+Space`).
- **Plugins**: `single-instance` (desktop-only), `dialog` (iOS ok), `global-shortcut` (desktop-only).
- **Crypto**: `crypt.rs` (ChaCha20 + PBKDF2) is pure Rust and portable. `getrandom` 0.2 uses
  `CCRandomGenerateBytes` on iOS. `ureq` + `rustls` + `ring` build for `aarch64-apple-ios`.
- `tauri.conf.json` sets `withGlobalTauri: true` and `csp: null`.

## iOS port research

See `docs/IOS-PORT.md` (research) and `docs/IOS-PHASE-1.md` (detailed Phase 1 plan). Key
conclusions: feasible as a sideloaded IPA; serve media over a loopback HTTP server (custom-scheme
`<video>`/`<audio>` is fragile on iOS — WebKit bugs 146351 and 324711); compute the origin instead
of hardcoding `http://vault.localhost/`; the crate must be restructured for mobile; iOS builds need
macOS + Xcode.

### Audit of `docs/IOS-PORT.md` (13 Nov 2026, against the code)

Verified claims that matter, and two fixes made while checking:

- **JS dialogs are silently dropped on iOS (confirmed).** wry 0.57.0's iOS `WKUIDelegate`
  (`src/wkwebview/class/wry_web_view_ui_delegate.rs`) implements only media-capture permission,
  file-upload (macOS) and navigation actions — none of `runJavaScriptAlert`/`Confirm`/`TextInputPanel`.
  WebKit's `UIDelegate::UIClient` (`Source/WebKit/UIProcess/Cocoa/UIDelegate.mm`) then falls back to
  `alert` = no-op, `confirm` = `false`, `prompt` = `""`. So any flow gated on `confirm` **runs without
  asking** on iOS. The audit's count of **24** `confirm`/`prompt`/`alert` call sites (16 in `ui/*.js`,
  8 in `src/index.html`) is exact. Fix in Phase 4: in-page modal.
- **`fastlane.rs` Origin allowlist (fixed).** It held only the two Windows forms; the iOS page origin
  is `tauri://localhost`, so iOS would get no CORS header and a `403` (`!cors.is_empty()` is in the
  gate). `ORIGINS` now also carries `tauri://localhost` and `"null"` (opaque origin). The real guard
  is unchanged: the per-launch `x-key`.
- **`maint.rs` self-update (fixed).** `install_update` writes a `.cmd` and calls `app.exit(0)`; on iOS
  a stray invoke would quit the app. It is now `#[cfg(windows)]` with a `#[cfg(not(windows))]` stub
  that returns an error. Also unreachable on iOS anyway: `exe_dir_writable()` is `false` there, so
  `update_check` never reports `newer`.
- **`serve()` CORS (confirmed).** `serve()` returns `Access-Control-Allow-Origin: *` and does not
  inspect `Origin` (the loopback token in the first path segment is the guard). `media.rs` forwards
  those headers, so the iOS page's `fetch` reads work. Do not describe it as an `Origin` check.
- **Autoplay.** The WebView2 `--autoplay-policy=no-user-gesture-required` flag is Windows-only; wry's
  iOS default `autoplay: true` maps to `setMediaTypesRequiringUserActionForPlayback(None)`, but
  Safari's "Auto-Play" setting and Low Power Mode can still block unmuted autoplay, so keep the cover
  video `muted` + `playsinline` + a `play().catch(...)` retry.
- **Counts confirmed by grep**: `contextmenu` = 3 (`library.js:427`, `privacy.js:136/159`);
  `dblclick` = 3 (`library.js:982`, `look.js:304`, `player.js:206`); mouse handlers = 8
  (`desktop.js` ×6, `discover.js:1450`, `src/index.html:3695`); 20 `.js`/`.mjs` under `src/`.
- **`requestFullscreen`** appears once (`src/index.html:576`) but `ui/slideshow.js` reassigns
  `showFullscreen` at runtime to the backend `window_action` version, so the element call is dead
  code (and a no-op on iPhone).
- **Not worth doing (skipped):** capability-gating `VideoEncoder`/`AudioEncoder`/`setSinkId`. The
  target is iOS 26, which has all three natively; gating would add UI risk for no reachable benefit.

### Building the iOS IPA in CI

There is no local Mac, so the IPA is built by GitHub Actions (`.github/workflows/ios.yml`):
`macos-26` (macOS 26 Tahoe, **Xcode 26.6 + the iOS 26 SDK** — required because
`bundle.iOS.minimumSystemVersion` is `26.0`; `macos-latest` currently aliases to it). `brew install
xcodegen`, then `npx tauri ios init --ci`, `npx tauri icon art/app-icon.png`, then `npx tauri ios
build`. No Apple secrets → `--no-sign` (unsigned IPA, signed on device); `APPLE_API_KEY` +
`APPLE_API_ISSUER` + `APPLE_API_KEY_P8` set → `--export-method debugging` (signed IPA). Facts worth
keeping:

- `bundle.active: false` does **not** block `tauri ios build`: that command uses cargo-mobile2's
  archive/export, not Tauri's desktop bundler.
- The IPA lands at `src-tauri/gen/apple/build/<arch>/<ProductName>.ipa` (cargo-mobile2 `export_dir` =
  `gen/apple/build`). The icon must be written *after* `ios init`, because `tauri icon` targets
  `gen/apple/Assets.xcassets/AppIcon.appiconset` only once that exists.
- Signing reads `APPLE_API_KEY` + `APPLE_API_ISSUER` + `APPLE_API_KEY_PATH` (a *file path* to the
  `.p8`); the workflow writes the secret text to a temp file. Signing also needs
  `APPLE_DEVELOPMENT_TEAM` or `bundle.iOS.developmentTeam`; a partial API-key set is a hard error.
- **The test-vault fixtures must stay byte-exact.** `src-tauri/tests/fixtures/**` is marked `-text` in
  `.gitattributes` because the repo-wide `* text=auto eol=lf` rewrote the plain vault's CRLF (the
  format the app writes, `lib.rs`) to LF on checkout, so on a fresh clone the encrypted fixture no
  longer decrypted back to it. Do not remove that rule.


Two user priorities outrank the rest:

- **Vault format compatibility.** The iOS app must read/write the *same* vault folder as Windows
  (same `index.csv`/`views.csv` with BOM + CRLF, `_app/` layout, `file_name` naming, base64url ids,
  `LVX1` ChaCha20+PBKDF2 encryption with the fixed salt, `pack-*.bin`). A no-op round-trip must be
  byte-identical. The full contract and a compatibility test matrix are in `docs/IOS-PHASE-1.md`.
- **Liquid Glass.** No free adoption in a webview. WebKit exposes the real system material as
  `-apple-visual-effect: -apple-system-glass-material` (and `-clear`/`-subdued`/`-media-controls`),
  gated on `HAVE_MATERIAL_HOSTING` which `wtf/PlatformHave.h` defines for `PLATFORM(IOS)`; also
  `HAVE_CORE_MATERIAL` = `PLATFORM(COCOA)`. Non-standard, must be progressive enhancement. Portable
  fallback: `backdrop-filter` (needs the `-webkit-` prefix on older iOS). **Done**: every standard
  `backdrop-filter` declaration now has its `-webkit-backdrop-filter` pair first, and `tools/check.mjs`
  fails the build if any future one is unpaired. Honour `prefers-reduced-transparency` and
  `prefers-reduced-motion`.
