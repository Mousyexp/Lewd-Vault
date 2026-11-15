# AGENTS.md

Notes for anyone (person or AI) working in this repository. Keep it short and true.

## What this is

Lewd Vault: a Tauri 2 app for collecting, tagging and viewing NSFW media, for Windows (a portable
`.exe` or an MSI), iOS (a sideloaded IPA) and Android (an APK). One crate, one page. `docs/GUIDE.md`
describes the features, the vault folder and the code layout; `mobile/README.md` covers the phones.

## Rules

- **Never edit `src/`.** It is built from `ui/` and `mobile/ui/` by `npm run sync` (`sync-ui.mjs`).
  Every patch in `sync-ui.mjs` must match exactly once, or the build stops.
- **The vault format must not change.** `index.csv`/`views.csv` (BOM, CRLF), the `_app/` layout, the
  file names and the `LVX1` encryption are shared by Windows and the phones; the tests compare bytes.
  `src-tauri/tests/fixtures/**` is `-text` in `.gitattributes` for that reason.
- **Do not rename** the bundle identifier (`local.imagevault.desktop`), the data folder names
  ("Image Vault data", "Image Vault Desktop") or the config file: existing installs would lose
  their vault.
- **`mobile/ui/phone.css`: every rule starts with `html.phone`** (`html.phone.ios` / `html.phone.android`
  for one phone only). The file ships in the Windows build.
- Platform splits in Rust: `cfg(desktop)` / `cfg(windows)` for the PC, `cfg(mobile)` for both phones,
  `cfg(target_os = "ios")` / `cfg(target_os = "android")` for one.
- No secrets in the repository. The Google key is a build-time secret
  (`LEWD_VAULT_GOOGLE_CLIENT_ID` / `_SECRET`).

## Checking your work

- `npm test`: the page builds, every script parses, the phone pieces are in place.
- `cd src-tauri && cargo test`: the backend (on Linux this needs the GTK/WebKit dev packages; CI
  runs it on Windows and Ubuntu).
- `node tools/screenshots.mjs <5 pictures>` drives the real page against the real backend.
- iOS can only be built by `.github/workflows/ios.yml` (a macOS runner), Android by
  `.github/workflows/android.yml` (or locally with the Android SDK and NDK). The Windows release is a
  version tag; see `docs/GUIDE.md`, "Building it".
