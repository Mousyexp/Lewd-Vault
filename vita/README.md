# Lewd Vault for PS Vita

The phone app's library, full view and slideshow, made native for the Vita (C, vita2d). It opens the
same vault as the PC and the phones, Untraceable ones included, and writes views and hearts back into
its `views.csv` in the PC's format, so copying the folder home carries them along.

It is a separate program from the Tauri app: the Vita has no web engine an app can use, so the page
can't run there. `vita/src/vault.c` reads the vault the way `src-tauri/src/lib.rs` does, and
`vita/src/lvcrypt.c` is the Untraceable format of `crypt.rs`, written again in C. Both are tested
against `src-tauri/tests/fixtures` (`sh vita/tests/run.sh`).

## Installing

Needs a Vita (or PS TV) with HENkaku/Ensō, and VitaShell.

1. Get `Lewd.Vault.Vita.vpk` (Actions → Build PS Vita → the run's **LewdVault-vpk** artifact, or a
   release).
2. Copy it to the Vita (VitaShell: SELECT starts its FTP server; or USB) and install it from VitaShell.
3. Copy your vault folder into **`ux0:data/LewdVault/`**. That's the `Image Vault data` folder from
   the PC (or `data.temp` for an Untraceable vault), so it ends up as, for example,
   `ux0:data/LewdVault/Image Vault data/index.csv`. `uma0:` and `imc0:` work too. Several vaults side
   by side give a picker.

The app keeps its own settings and a thumbnail cache in `ux0:data/LWDV00001/` (the Title ID). An
Untraceable vault's cached thumbnails are encrypted with its key, like everything else of it.

## Using it

| | Touch | Buttons |
|---|---|---|
| Library | tap a card; drag to scroll; tap a category chip | D-pad/stick; ✕ open; L/R category; △ sort; □ pictures/videos; START slideshow; ○ settings |
| Full view | swipe for the next; pinch to zoom; double-tap zoom; pull up for details; tap a tag to search it | ←/→ or L/R next; ✕ play/pause (videos) or zoom; ↑/↓ ±10 s; △ details; □ heart; START slideshow; ○ back |
| Anywhere | | SELECT covers the screen with a plain clock (again to come back) |

Settings (○ in the library, or the bottom bar): search, the slideshow's time per picture and
shuffle, video volume, another vault, clearing the thumbnail cache.

## What plays

- Pictures: WebP, JPEG, PNG (decoded at screen size; zoom goes to 1920 px).
- Videos: **MP4/M4V/MOV** (H.264) play in the Vita's hardware decoder, through sceAvPlayer.
  **WebM/MKV** (VP8/VP9 with Opus/Vorbis/AAC sound) play in software: up to about 480p smoothly;
  bigger VP9 is more than one Cortex-A9 core can decode in time. GIFs and videos the Vita can't play
  show their preview clip (`_app/clips`).
- An Untraceable vault's files, videos included, are decrypted as they're read and never written
  out plain.

Not on the Vita: adding, editing or deleting items, Discover, Chat, Google Drive sync (copy the
folder instead).

## Building it

With VitaSDK (`$VITASDK` set) and its packages (vita2d, libwebp, libjpeg-turbo, libpng, FFmpeg,
libvpx; the `vitasdk/vitasdk` Docker image has them all):

```bash
cmake -S vita -B vita/build -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
```

```bash
cmake --build vita/build
```

which makes `vita/build/LewdVault.vpk`. `.github/workflows/vita.yml` does the same in CI. Add
`-DCMAKE_C_FLAGS=-DLV_DEBUG` for a build that logs to `ux0:data/LWDV00001/log.txt` (and reads a
password from `debug_password.txt` there, for testing in an emulator).

Testing in Vita3K works for the library, pictures, the Untraceable unlock and WebM. Vita3K's
sceAvPlayer stand-in can't play MP4 (it hangs or crashes the emulator), and its NEON emulation
garbles WebP *encoding*, which the app notices and stops caching thumbnails for; neither happens on
a Vita. The thumbnail cache checks every thumbnail it writes by reading it back.

## Speed

Tuned for the Cortex-A9 (and checked against the optimisation guide the port was given):
`-O3 -mcpu=cortex-a9 -mfpu=neon`, unused code dropped at link time; sorting and filtering read a
16-byte record per item instead of its strings; the index's strings live in a few large blocks, not
thousands of small allocations; thumbnails decode on a worker thread at card size and are cached
small; circles and round corners are one textured quad each instead of 100-triangle fans; no
per-frame work grows with the size of the vault.

## Files

| File | What it is |
|---|---|
| `src/main.c` | The screens: picker, password, library, full view, slideshow, settings, cover |
| `src/vault.c` | `index.csv` / `views.csv`, finding files, thumbnails (packs too) |
| `src/lvcrypt.c` | The Untraceable format: SHA-256, PBKDF2, ChaCha20, "LVX1" files |
| `src/image.c` | WebP/JPEG/PNG to pixels at the size they're shown |
| `src/video.c` | sceAvPlayer for MP4; FFmpeg + libvpx for WebM/MKV |
| `tests/` | The core against the golden vaults, on a PC |
| `tools/make-livearea.py` | The home-screen icon and LiveArea pictures, from the app's icon |
| `font.ttf` | DejaVu Sans (see `FONT-LICENSE.txt`); the Vita's own font is used for Japanese |

The Title ID `LWDV00001` is where the Vita keeps the app and its data: don't change it.
