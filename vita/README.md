# Lewd Vault for PS Vita

The phone app's library, full view and slideshow, and a Browser for booru sites, made native for the
Vita (C, vita2d). It opens the same vault as the PC and the phones, Untraceable ones included, writes
views and hearts back into its `views.csv`, and saves what you find in the Browser into it, sized
for the Vita.

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
| Browser | tap the site button (the tabs), Filter, the sort, the search box; tap a card | L/R previous/next site; △ sort; □ filter; START the site list; ✕ open; ○ next tab |
| Browser's full view | swipe; pinch; tap Save; pull up for details, tap a tag to search it | ←/→ next; □ save; △ details; ✕ zoom or play/pause; ○ back |
| Anywhere | | SELECT covers the screen with a plain clock (again to come back); ○ in the Library, Browser and Settings goes round those three |

Settings (the bottom bar, or ○ from the Browser): search the Library, the slideshow's time per picture
and shuffle, video volume, another vault, clearing the thumbnail cache.

## The Browser

Danbooru, Yande.re, Konachan, e621, Xbooru, TBIB, Hypnohub, AIBooru, Safebooru and e926, read through
their JSON APIs (the same four kinds of site as the desktop's Discover: `src/booru.c`), so no web page
loads. The grid is the Library's; the site button is the tab bar (L/R step through it), Filter picks
the ratings (General, Sensitive, Questionable, Explicit), the search and which category saves go into,
and the sort is Latest, Hot (where the site has it), Top or Random. More results load as you scroll.
Gelbooru and Rule34 aren't there: they need an API key. As on the desktop, posts tagged as sexual
content involving minors are never shown or searched for.

**Light on the network and the card.** The grid loads each site's small preview (150-360 px), the full
view its sample (~850-1500 px, made for screens), never the original. Saving keeps a JPEG sample as it
is when it fits 1920 x 1088 (a Vita screen at 2x zoom) and otherwise makes one at that size; PNG and
WebP become JPEG (the format the Vita decodes fastest). A Konachan wallpaper saved this way is 758 KB
where the original is 22 MB. Its thumbnail is a JPEG just big enough to cover a card (~6-15 KB), its
row is *appended* to `index.csv` (the file isn't rewritten), and in an Untraceable vault all of it is
encrypted. GIFs and videos are saved as they are. Saved posts carry a ✓ in the grid and aren't saved
twice. The items are ordinary vault items (the PC opens them too), just Vita-sized.

HTTPS is checked against Mozilla's list of authorities (`cacert.pem`, from certifi). The app has its
own `inet_pton` (`src/net.c`): the Vita C library's answers -1 for a host name, which libcurl takes
for "an IP address" and then sends no server name (SNI) in its TLS hello, which most sites refuse.

## What plays

- Pictures: WebP, JPEG, PNG (decoded at screen size; zoom goes to 1920 px).
- Videos: **MP4/M4V/MOV** (H.264) play in the Vita's hardware decoder, through sceAvPlayer.
  **WebM/MKV** (VP8/VP9 with Opus/Vorbis/AAC sound) play in software: up to about 480p smoothly;
  bigger VP9 is more than one Cortex-A9 core can decode in time. GIFs and videos the Vita can't play
  show their preview clip (`_app/clips`).
- An Untraceable vault's files, videos included, are decrypted as they're read and never written
  out plain.

Not on the Vita: editing or deleting items, adding your own files, Reddit/Bing/video sites, Chat,
Google Drive sync (copy the folder instead).

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
`-DCMAKE_C_FLAGS=-DLV_DEBUG` for a build that logs to `ux0:data/LWDV00001/log.txt` and, for testing
in an emulator, reads a password from `debug_password.txt` there and an HTTP proxy from
`debug_proxy.txt` (with its authority in `debug_ca.pem`).

Testing in Vita3K works for the library, pictures, the Untraceable unlock, WebM and the Browser
(searching, previews, the full view, saving). Vita3K's
sceAvPlayer stand-in can't play MP4 (it hangs or crashes the emulator), and its NEON emulation
garbles WebP *encoding* and zlib's unpacking: the thumbnail cache checks every thumbnail it writes by
reading it back, and if a compressed download ever fails to unpack the app asks for plain ones from
then on. Neither should happen on a Vita.

## Speed

Tuned for the Cortex-A9 (and checked against the optimisation guide the port was given):
`-O3 -mcpu=cortex-a9 -mfpu=neon`, unused code dropped at link time; sorting and filtering read a
16-byte record per item instead of its strings; the index's strings live in a few large blocks, not
thousands of small allocations; thumbnails decode on a worker thread at card size and are cached
as small JPEGs; circles and round corners are one textured quad each instead of 100-triangle fans; no
per-frame work grows with the size of the vault; `web.c` is compiled inside `main.c` (one unit, so
the compiler inlines across them); textures are only freed after the frame that may still draw them.

## Files

| File | What it is |
|---|---|
| `src/main.c` | The screens: picker, password, library, full view, slideshow, settings, cover |
| `src/vault.c` | `index.csv` / `views.csv`, finding files, thumbnails (packs too) |
| `src/lvcrypt.c` | The Untraceable format: SHA-256, PBKDF2, ChaCha20, "LVX1" files |
| `src/image.c` | WebP/JPEG/PNG to pixels at the size they're shown |
| `src/video.c` | sceAvPlayer for MP4; FFmpeg + libvpx for WebM/MKV |
| `src/web.c` | The Browser's screens and threads (compiled inside `main.c`) |
| `src/booru.c`, `src/json.c` | The sites' searches and answers; a small JSON reader |
| `src/net.c` | libcurl over the Vita's network |
| `tests/` | The core against the golden vaults and the site readers against saved answers (`tests/booru/`, from the sites' all-ages halves), on a PC |
| `tools/make-livearea.py` | The home-screen icon and LiveArea pictures, from the app's icon |
| `font.ttf` | DejaVu Sans (see `FONT-LICENSE.txt`); the Vita's own font is used for Japanese |
| `cacert.pem` | Mozilla's certificate authorities (certifi, MPL-2.0) |

The Title ID `LWDV00001` is where the Vita keeps the app and its data: don't change it.
