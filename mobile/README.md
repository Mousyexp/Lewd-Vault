# Lewd Vault on iOS

The iPhone and iPad app is the same Tauri crate as the Windows app, built for a second target and
sideloaded as an IPA (iOS 26+). This folder holds the only iOS-specific front-end code; everything
else is shared and branches in place.

**The rule that outranks the rest:** a vault folder stays interchangeable between Windows and iOS,
byte for byte (`index.csv`, `views.csv`, `_app/`, the Untraceable encryption). The tests in
`src-tauri/src/lib.rs` hold it against the fixtures in `src-tauri/tests/fixtures/`.

## What is where

| Piece | Where | What it does |
| --- | --- | --- |
| The iOS layer | `mobile/ui/ios.js`, `mobile/ui/ios.css` | The tab bar and More tiles, the Photos-style Library, the long-press preview sheet, the full-screen viewer and its gestures, iOS controls, confirm/prompt, Hide now (lock screen, calculator) and the background cover. |
| Turning it on | `src-tauri/src/lib.rs` | An initialization script adds `ios` to `<html>` before the page runs. Every rule in `ios.css` starts with `html.ios`, so Windows never sees it (`npm test` enforces that). |
| Media | `src-tauri/src/media.rs` | A loopback HTTP server (`127.0.0.1`, a random port and token per launch). WKWebView doesn't feed `<img>`/`<video>` from a custom scheme reliably, so the page's files come from here; the page gets the address as `window.__VAULT_ORIGIN`. |
| Storage | `src-tauri/src/storage.rs` | The vault lives in the app's **Documents** (visible in the Files app, which is how a vault from a PC gets onto the phone). The app's own things, and an Untraceable vault, live in Application Support, which Files doesn't show. |
| No Recycle Bin | `src-tauri/src/recycle.rs` | Deleted items go to the vault's own `_trash`. |
| Bundle settings | `src-tauri/tauri.conf.json` (`bundle.iOS`), `src-tauri/Info.plist` | The minimum iOS version; file sharing and the loopback exception. Tauri finds both by path, so they stay in `src-tauri`. |

`sync-ui.mjs` appends `ios.css` to the desktop stylesheet (it must come last, to override it) and
copies `ios.js`; both ship in the Windows build too and do nothing there.

## What iOS can't do, and what the app does about it

| On Windows | On iOS |
| --- | --- |
| `confirm()` / `prompt()` | WKWebView drops them (confirm is always "no"). `ios.js` shows a sheet, and replays the tap with your answer. |
| Right-click menu | Long-press a picture: it lifts into a preview, with its actions in a sheet (stars, the level and categories as tap strips). The sheet presses the desktop menu's own buttons, so it never falls behind it. |
| The emergency key (a global shortcut) | **More → Hide now**, or a three-finger tap anywhere. It shows a lock screen (swipe up, then your PIN) or a working calculator (your PIN, then `=`); with no PIN, hold the clock or the display for a second. The app also covers itself in the background, so the app switcher shows nothing. |
| Open with another program, Show in folder | Hidden. |
| Cloud-synced folders, Another folder… | Hidden (there is one container). |
| Google Drive in your browser | Hidden: iOS suspends the app while Safari is in front, so the sign-in could never return. |
| In-app updates | Install a newer IPA. |

## Building the IPA

There is no local build without a Mac. **Actions → Build iOS → Run workflow**
(`.github/workflows/ios.yml`) runs the Rust tests, then `tauri ios init` and `tauri ios build` on a
macOS runner, and uploads the **LewdVault-ipa** artifact.

- Unsigned by default (`--no-sign`): sign it on the device with AltStore or SideStore.
- With the repository secrets `APPLE_API_KEY`, `APPLE_API_ISSUER`, `APPLE_API_KEY_P8` and
  `APPLE_DEVELOPMENT_TEAM` (a paid Apple Developer account, with the App ID
  `local.imagevault.desktop` registered), tick **signed** for an IPA that installs directly.

Things the workflow already handles, in case it ever needs changing: the unsigned path sets a
placeholder development team and unsets the (empty) Apple secrets, because `tauri ios build` checks
both before it looks at `--no-sign`; `package.json` needs the `tauri` script the generated Xcode
project calls; and the runner and Xcode version are pinned, because the iOS 26 SDK is required.

## Trying the layout without a device

`npm run sync`, serve `src/`, and add the `ios` class to `<html>` before the page's scripts run (the
app does this itself). Outside Tauri the page runs in its demo mode; `tools/screenshots.mjs` shows
how to point it at the real backend instead. Use a phone-sized window (393 × 852) for the phone
layout; 820px and wider gets the iPad layout, which keeps the sidebar.

This checks layout and behaviour, not WebKit itself: test on a real device before relying on it.
