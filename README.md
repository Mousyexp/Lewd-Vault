# 💖 Lewd Vault

**Your spicy collection, beautifully organised, 100% private, and all yours.** 🔒✨

Lewd Vault is a cute, fast Windows app for collecting, tagging and enjoying your NSFW pictures, GIFs, videos and audio. No accounts, no cloud you didn't pick, no judgement. 😏 *18+ only.*

![The Library](docs/screenshots/library.webp)

## ✨ Why you'll love it

- 🧘 **Calm by default.** The everyday stuff is big and up front; extra options fold away next to where you use them and remember how you left them.
- 🗂️ **A Library that just works.** Big, buttery-smooth grid with preview clips, collections, ratings and a "For you" order.
- 🏷️ **Booru-style tags.** `artist:`, `character:`, `series:`… with suggestions and lightning-fast tag search.
- 🔍 **Browser.** One big tile for 🖼️ Images, 🎬 Videos and 🎧 Audio. Browse your favourite sites in browser-style tabs right inside the app, resize the tiles, and save with one click. You can even add your own sites!
- 🎞️ **Slideshow.** Timed NSFW-level stages that build up, with motion effects and true full screen. 🔥
- 🎬 **A real video player.** Smooth seeking, A–B loop, speed control, frame-by-frame stepping and keyboard shortcuts.
- 🕵️ **Privacy first.** Discreet file names, a **PIN lock**, plus an **emergency key** that instantly turns the app into a screensaver or a cat video. 🙈🐱
- 🥷 **Untraceable mode.** On a work laptop? Your whole vault stays encrypted in a hidden folder behind a password: next to the app, in Documents, or anywhere you pick. No thumbnails, caches or browser history anywhere else on the PC, and a plain "Notes" window on the taskbar. 🔐
- ☁️ **Google Drive, even without the Drive app.** No admin rights? Sign in once in your browser and your vault syncs both ways with your Drive (encrypted there too, in Untraceable mode). Google Drive for desktop, OneDrive, Dropbox and iCloud folders still work as before.
- ✅ **Tidy in bulk.** Ctrl/Shift-click to select many tiles and re-rate, re-tag or move them all at once; find duplicates *and* look-alikes (resized or re-saved copies).
- ⬇️ **One downloads panel** for everything you save, with retry, plus a status dot on every site tab.
- 🛟 **Backups, health checks and updates** from Settings: copy your vault to a drive (only what changed), fix broken previews, and update the app in one click.
- 🏆 **Level up!** Earn XP, unlock titles and badges, keep a streak and see your stats.
- 💪 **Your coach.** Guided Kegel workouts and JOI sessions with a cute companion.
- 🎁 **Share packs.** Bundle a collection into one `.zip` with a pretty preview card, and open packs from friends.
- 🎨 **Themes galore.** Rose, Berry night, Lavender, Mint… or design your own.
- 💾 **Your files, your disk.** One tiny ~6 MB portable `.exe` (or an installer). Keep your vault on your PC, a USB stick, or a synced Google Drive, OneDrive, Dropbox or iCloud folder. No Drive app (it needs admin)? Sign in to Google Drive in your browser and sync straight from the app.

## 📸 Take a peek

| ➕ Add & tag | 🎞️ Slideshow |
| --- | --- |
| ![Adding and tagging](docs/screenshots/add.webp) | ![Slideshow](docs/screenshots/slideshow.webp) |
| 🏆 **Your profile & levels** | 💪 **The coach** |
| ![Profile](docs/screenshots/you.webp) | ![Coach](docs/screenshots/coach.webp) |
| 🎨 **Themes & settings** | 📊 **Stats** |
| ![Settings](docs/screenshots/settings.webp) | ![Stats](docs/screenshots/stats.webp) |
| 🧭 **Pick where it lives** | 🥷 **Untraceable mode** |
| ![The welcome screen](docs/screenshots/welcome.webp) | ![Untraceable mode](docs/screenshots/untraceable.webp) |
| 🙈 **The emergency key** (a screensaver, or a cat video) | 🔒 **Untraceable's password screen** |
| ![The emergency key's screensaver](docs/screenshots/emergency.webp) | ![The password screen](docs/screenshots/password.webp) |

<sub>Screenshots show a demo vault. Artwork is used with permission of its creators.</sub>

## 🚀 Get started

Grab **[Lewd Vault 1.1](https://github.com/Mousyexp/Lewd-Vault/releases/latest)** from Releases:

- 💿 **Lewd.Vault.msi**: the installer. Start menu entry, uninstall from Windows Settings.
- 🎒 **Lewd.Vault.Portable.zip**: unzip anywhere (a USB stick works too) and run `Lewd Vault.exe`.

Then pick where your vault should live (your PC, a cloud folder, Google Drive in your browser, or 🥷 Untraceable), and start adding! 🎉 Both versions can do everything, Untraceable included, and both update themselves from Settings → Updates.

🥷 **Most discreet:** the portable zip, renamed to something dull (say `Notes.exe`). The installer shows up in the Start menu and in Settings → Apps; the portable app leaves no install behind and keeps the encrypted vault right next to itself.

You need WebView2, which Windows 10 and 11 already have. 👍

🛡️ **"Windows protected your PC"?** The app isn't code-signed (certificates cost money), so
SmartScreen warns the first time you run it. Click **More info → Run anyway**.

## 📱 On iPhone / iPad (sideloaded)

The same app, redesigned for a phone like Photos: an edge-to-edge grid, a floating tab bar, bottom
sheets, a long-press preview with tap strips, and a full-screen viewer you swipe through. There is no App Store build (the content would
never pass review), so it is an **IPA** you sideload. It needs **iOS 26**.

![Lewd Vault on iPhone: the Library, the long-press preview, the viewer, More, and the two Hide-now decoys (a lock screen and a calculator)](docs/screenshots/iphone.webp)

1. On GitHub: **Actions → Build iOS → Run workflow**. When it finishes, download the
   **LewdVault-ipa** artifact.
2. Install it with **AltStore** or **SideStore** (a free Apple ID; AltStore runs from your Windows
   PC, and re-signs the app every 7 days). With a paid Apple Developer account the workflow can
   build a signed IPA that installs directly; see [mobile/README.md](mobile/README.md).

Your vault is the *same folder* as on Windows: open the app's **Documents** folder in the Files app
and drop a vault in. Untraceable mode's encryption works the same on both. The emergency key is
**Hide now** (or a three-finger tap): the app turns into a lock screen or a working calculator that
only opens with your PIN, and it covers itself in the app switcher. A few PC-only things are left
out (cloud folders, Drive sign-in, in-app updates); the list is in [mobile/README.md](mobile/README.md).

## 🧑‍💻 Build it yourself

```bash
npm install
npm run dist
```

You need Node.js, Rust and the [Tauri prerequisites](https://tauri.app/start/prerequisites/). 📖 Want every detail? The full guide is in [docs/GUIDE.md](docs/GUIDE.md).

## 📜 Licence

The code is [MIT](LICENSE). The artwork (in the app and in the screenshots) is used with permission
of its creators and isn't covered by that licence. Credits are in the [guide](docs/GUIDE.md#credits).

---

Made with 💕 and a little bit of mischief.
