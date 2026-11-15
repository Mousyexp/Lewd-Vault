# Lewd Vault on iOS and Android

The iPhone/iPad app and the Android app are the same Tauri crate as the Windows app, built for more
targets: an IPA you sideload (iOS 26+) and an APK (arm64 Android). Both phones use one front-end
layer, in this folder; everything else is shared and branches in place.

**The rule that outranks the rest:** a vault folder stays interchangeable between Windows and the phones,
byte for byte (`index.csv`, `views.csv`, `_app/`, the Untraceable encryption). The tests in
`src-tauri/src/lib.rs` hold it against the fixtures in `src-tauri/tests/fixtures/`.

## What is where

| Piece | Where | What it does |
| --- | --- | --- |
| The phone layer | `mobile/ui/phone.js`, `mobile/ui/phone.css` | The tab bar and More tiles, the Photos-style Library, the long-press preview sheet, the full-screen viewer and its gestures, iOS controls, confirm/prompt, Hide now (lock screen, calculator), the background cover, Android's back button, and the phone-only extras: Stories, Sort and the level dial. |
| Turning it on | `src-tauri/src/lib.rs` | An initialization script adds `phone` and `ios` or `android` to `<html>` before the page runs. Every rule in `phone.css` starts with `html.phone`, so Windows never sees it (`npm test` enforces that). |
| Media | `src-tauri/src/media.rs` | A loopback HTTP server (`127.0.0.1`, a random port and token per launch). WKWebView doesn't feed `<img>`/`<video>` from a custom scheme reliably (and Android's request interception can't stream a seekable video), so the page's files come from here; the page gets the address as `window.__VAULT_ORIGIN`. |
| Storage | `src-tauri/src/storage.rs` | iOS: the vault lives in the app's **Documents** (visible in the Files app, which is how a vault from a PC gets onto the phone); the app's own things, and an Untraceable vault, live in Application Support, which Files doesn't show. Android: all of it in the app's private data folder (`files/` and `app_support/`). |
| No Recycle Bin | `src-tauri/src/recycle.rs` | Deleted items go to the vault's own `_trash`. |
| Bundle settings | `src-tauri/tauri.conf.json` (`bundle.iOS`), `src-tauri/Info.plist` | The minimum iOS version; file sharing and the loopback exception. Tauri finds both by path, so they stay in `src-tauri`. |

`sync-ui.mjs` appends `phone.css` to the desktop stylesheet (it must come last, to override it) and
copies `phone.js`; both ship in the Windows build too and do nothing there.

## What a phone can't do, and what the app does about it

| On Windows | On the phones |
| --- | --- |
| `confirm()` / `prompt()` | WKWebView drops them (confirm is always "no"). `phone.js` shows a sheet, and replays the tap with your answer. |
| Right-click menu | Long-press a picture: it lifts into a preview, with its actions in a sheet (stars, the level and categories as tap strips). The sheet presses the desktop menu's own buttons, so it never falls behind it. |
| The emergency key (a global shortcut) | **More → Hide now**, or a three-finger tap anywhere. It shows a lock screen (swipe up, then your PIN) or a working calculator (your PIN, then `=`); with no PIN, hold the clock or the display for a second. The app also covers itself in the background, so the app switcher shows nothing. |
| Open with another program, Show in folder | Hidden. |
| Cloud-synced folders, Another folder… | Hidden (there is one container). |
| Google Drive in your browser | iOS: hidden (iOS suspends the app while Safari is in front, so the sign-in could never return). Android: works, and is the way to bring a vault over. |
| In-app updates | Install a newer IPA or APK. |
| Esc / the window's close | Android: the back button closes what's on top (a sheet, the viewer, the slideshow), then goes back to the Library. |

## Building the IPA

There is no local build without a Mac. **Actions → Build iOS → Run workflow**
(`.github/workflows/ios.yml`) runs the Rust tests, then `tauri ios init` and `tauri ios build` on a
macOS runner, and uploads the **LewdVault-ipa** artifact.

- Unsigned by default (`--no-sign`): sign it on the device with AltStore or SideStore.
- Fill in **release** (a tag, such as `v1.2.0`) and the IPA is also attached to that release as
  `Lewd.Vault.iOS.ipa`, replacing the one already there.
- With the repository secrets `APPLE_API_KEY`, `APPLE_API_ISSUER`, `APPLE_API_KEY_P8` and
  `APPLE_DEVELOPMENT_TEAM` (a paid Apple Developer account, with the App ID
  `local.imagevault.desktop` registered), tick **signed** for an IPA that installs directly.

Things the workflow already handles, in case it ever needs changing: the unsigned path sets a
placeholder development team and unsets the (empty) Apple secrets, because `tauri ios build` checks
both before it looks at `--no-sign`; `package.json` needs the `tauri` script the generated Xcode
project calls; and the runner and Xcode version are pinned, because the iOS 26 SDK is required.

## Building the APK

**Actions → Build Android → Run workflow** (`.github/workflows/android.yml`) runs `npm test`, then
`tauri android init` and `tauri android build --apk --target aarch64` on a Linux runner with its
Android SDK and NDK, signs the APK and uploads the **LewdVault-apk** artifact. Fill in **release**
(a tag) to attach it to that release as `Lewd.Vault.Android.apk`, replacing the one there.

- **Signing.** Set the repository secrets `ANDROID_KEYSTORE_B64` (the `.jks` file, base64),
  `ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS` and `ANDROID_KEY_PASSWORD` and every APK is signed
  with your key, so a newer one installs over the old one and keeps the vault. Without them each run
  signs with a throwaway key: it installs, but the next build won't update it, and uninstalling
  first deletes the app's data. Make a key once with
  `keytool -genkeypair -keystore release.jks -alias lewdvault -keyalg RSA -keysize 2048 -validity 10000`
  and keep it safe (`base64 -w0 release.jks` gives the secret's value).
- **Cleartext to 127.0.0.1.** Android blocks plain http in release builds; the workflow turns
  `usesCleartextTraffic` on in the generated project, because the media server is plain http on
  loopback (only reachable on the phone, and it wants the launch's token).
- Locally: the Android SDK, an NDK (`NDK_HOME`), JDK 17 and `rustup target add aarch64-linux-android`,
  then the same commands.

## Trying the layout without a device

`npm run sync`, serve `src/`, and add the `phone` and `ios` (or `android`) classes to `<html>` before the page's scripts run (the
app does this itself). Outside Tauri the page runs in its demo mode; `tools/screenshots.mjs` shows
how to point it at the real backend instead. Use a phone-sized window (393 × 852) for the phone
layout; 820px and wider gets the iPad layout, which keeps the sidebar.

This checks layout and behaviour, not WebKit itself: test on a real device before relying on it.
