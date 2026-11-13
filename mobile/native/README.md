# Native iOS pieces (that stay in `src-tauri`)

There is deliberately no code in this directory. This file exists to explain **why**: the parts of the
port that feel "iOS-native" cannot move here without breaking the build, because Tauri resolves them by
fixed path. They are grouped and documented instead, so a reader looking for "the iOS bits" finds them.

## What lives where, and why it cannot move

| Piece | Path | Why it is fixed |
| --- | --- | --- |
| `Info.plist` | `src-tauri/Info.plist` | Tauri auto-discovers `Info.plist` **next to `tauri.conf.json`**. There is an explicit `bundle.infoPlist` override in the config schema, but the auto-discovered file is the current, working setup and the file-sharing + ATS keys are what make the vault openable in the Files app. Moving it means also editing `tauri.conf.json`; not worth the risk for a cosmetic move. |
| iOS app-icon catalog | `src-tauri/icons/ios/` | `tauri icon` writes into `gen/apple/Assets.xcassets/AppIcon.appiconset` when it exists, and falls back to `src-tauri/icons/ios`. That fallback path is Tauri's, not ours. |
| iOS bundle settings | `src-tauri/tauri.conf.json` → `bundle.iOS` | Tauri reads this key (capital I/O/S). `minimumSystemVersion: "26.0"` is the iOS-26 decision. |
| Crate shape for iOS | `src-tauri/Cargo.toml` → `[lib] crate-type` | The crate must be a `staticlib` for iOS; the linker looks for it by name. |
| Rust iOS branches | `src-tauri/src/*.rs` under `#[cfg(target_os = "ios")]` | These are the same modules the Windows build compiles — storage roots (`storage.rs`), the loopback media server (`media.rs`), the recycle-bin shim (`recycle.rs`), and the `ios` class injection (`lib.rs`). Splitting them out would fork shared code. |

## The Rust iOS surface, at a glance

- `src-tauri/src/lib.rs`
  - the `ios` class initialization script (before the page's scripts) — what turns the whole UI layer on
  - `#[cfg_attr(mobile, tauri::mobile_entry_point)] pub fn run()` — the exported `start_app` symbol
- `src-tauri/src/storage.rs` — `ios_home()` / `ios_documents()` / `ios_support()`; the vault lives in
  `Documents` so the Files app can see it (that is how a Windows vault gets onto the phone).
- `src-tauri/src/media.rs` — the loopback HTTP media server (`127.0.0.1`, per-launch token in the first
  path segment), because a Tauri custom scheme cannot stream media to `<video>`/`<audio>` on iOS.
- `src-tauri/src/recycle.rs` — a no-op shim on iOS (no trash crate there).

`tools/check.mjs` pins the load-bearing ones (the `ios` class, the entry-point attribute, the staticlib,
the `Info.plist` keys) so a rename cannot silently break the IPA.
