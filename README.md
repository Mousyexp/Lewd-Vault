# Lewd Vault

A private, offline Windows app for collecting and enjoying your NSFW pictures, GIFs, videos and audio. 18+ only.

## Features

- **Your files, your disk.** One portable `.exe` (≈6 MB). Everything lives in a plain folder on your PC, a USB stick, or a synced Google Drive / OneDrive / Dropbox / iCloud folder.
- **Library.** Fast grid with preview clips, categories, ratings, collections and a "For you" order.
- **Tags.** Booru-style tags (`artist:`, `character:`, `series:`) with suggestions and tag search.
- **Discover.** Browse image, video and audio sites inside the app and save to your vault in one click. Add your own sites too.
- **Slideshow.** Timed NSFW-level stages, motion effects and true full screen.
- **Video player.** Smooth seeking, A–B loop, speed and frame stepping, plus keyboard shortcuts.
- **Privacy.** Discreet file names and an emergency hide key. Nothing leaves your PC unless you choose to.
- **You.** Profile, levels, badges and time-spent stats.
- **Coach.** Guided Kegel and JOI sessions.
- **Share packs.** Bundle a collection into one `.zip` with a preview card. Open a friend's pack to add it to your vault.
- **Themes.** Built-in themes, plus a Custom theme you design yourself.

## Run

Build it (below), open `distImage Vaultewd vault.exe`, and pick where your vault should live. You need WebView2, which Windows 10 and 11 already have.

## Build

```bash
npm install
npm run dist
```

You need Node.js, Rust and the [Tauri prerequisites](https://tauri.app/start/prerequisites/). The full guide is in [docs/GUIDE.md](docs/GUIDE.md).
