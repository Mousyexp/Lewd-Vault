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

Two user priorities outrank the rest:

- **Vault format compatibility.** The iOS app must read/write the *same* vault folder as Windows
  (same `index.csv`/`views.csv` with BOM + CRLF, `_app/` layout, `file_name` naming, base64url ids,
  `LVX1` ChaCha20+PBKDF2 encryption with the fixed salt, `pack-*.bin`). A no-op round-trip must be
  byte-identical. The full contract and a compatibility test matrix are in `docs/IOS-PHASE-1.md`.
- **Liquid Glass.** No free adoption in a webview. WebKit exposes the real system material as
  `-apple-visual-effect: -apple-system-glass-material` (and `-clear`/`-subdued`/`-media-controls`),
  gated on `HAVE_MATERIAL_HOSTING` which `wtf/PlatformHave.h` defines for `PLATFORM(IOS)`; also
  `HAVE_CORE_MATERIAL` = `PLATFORM(COCOA)`. Non-standard, must be progressive enhancement. Portable
  fallback: `backdrop-filter` (needs the `-webkit-` prefix on older iOS; the CSS currently has 27
  `backdrop-filter` uses and **zero** prefixed ones). Honour `prefers-reduced-transparency` and
  `prefers-reduced-motion`.
