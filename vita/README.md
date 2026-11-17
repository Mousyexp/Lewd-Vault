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
| Library | tap a card; drag to scroll; hold a card for its quick actions; drag the right edge to fly through the months; tap a category chip | D-pad/stick; ✕ open (hold: quick actions); L/R category; △ sort; □ filters; START slideshow; ○ next tab |
| Full view | swipe for the next; pinch to zoom; double-tap zoom (or heart: Settings); the filmstrip at the bottom; pull up for details | ←/→ or L/R next; ✕ zoom; △ details (↑↓ choose, ←→ change: stars, level, category, tags, delete); □ heart; START slideshow; ○ back |
| A video | tap the bar to jump | ✕ play/pause; ←/→ 10 s; the stick scrubs; ↑ 2× / 4× (no sound); ↓ A-B loop (A, B, off); L/R next |
| Browser | tap the site button (the tabs), Filter, the sort, the search box; tap a card | L/R previous/next site; △ sort; □ filter; START the site list; ✕ open; ○ next tab |
| Browser's full view | swipe; pinch; tap Save; pull up for details, tap a tag to search it | ←/→ next; □ save; △ details; ✕ zoom or play/pause; ○ back |
| Anywhere | | SELECT covers the screen with a plain clock (again to come back); ○ in the Library, Browser and Settings goes round those three |

Settings: search the Library, video volume, interface sounds, what a double tap does, the glow behind
pictures, Google Drive, another vault, clearing the thumbnail cache.

**The Library's tools** (`src/lib.c`), as on the phone: Filters (□) for the kind, NSFW up to, stars,
smart albums (Never opened, Forgotten, Long videos, Most loved, 1 star, Continue watching), a
collection, the card size, the tags panel (✕ adds a tag, □ excludes it) and saved filters. Hold a card
for its quick actions: stars, the level dial, category, select (then △ for many at once) and delete
(into the vault's `_trash`, as the PC does for an Untraceable vault). Edits go into `index.csv` a
moment after the last one, after the day's backup in `_app/backups`.

**The slideshow** (the Slideshow tab, `src/show.c`) is the PC's: a session climbs the NSFW levels, the
first stage up to a level, then one level at a time, each for some minutes, with slides from slow to
faster and faster (Gentle, Standard and Fast are the PC's presets), and what's above the level reached
is locked in the Library until the session gets there. Only time with the app open counts; a session
left for six hours ends. Simple is the old one (a few seconds each, shuffled or not). Both have motion
(slow pan, slow zoom, breathing), a fade from one to the next and a sleep timer.

**PIN lock** (Settings, `src/pin.c`): a PIN pad when the app starts, and, if chosen, when the Vita
wakes. A decoy PIN opens another vault on the card instead, and changes nothing about the lock or which
vault opens next time. Wrong tries are told after the next right one; after five it waits 30 s between
tries. It locks the app; an Untraceable vault is what keeps the files themselves safe. **Your stats**
shows what's in the vault and how it's been looked at. The **rear touchpad** scrolls the Library and
the Browser, and a swipe across it moves through the full view.

**Hide it** (SELECT, anywhere): a plain clock, or (Settings) a calculator that really calculates;
the PIN and = gets back in, or with no PIN, holding the display for a second. **Touch lock**: hold L
and R for a second and touch and buttons do nothing (for the time chosen, or until L and R are held
again) while a picture, video or slideshow carries on (`src/privacy.c`). **Clean up** (Settings) shows
the Library one at a time, the least looked-at first: swipe away (or ←) what you don't want, keep (→),
undo (△); the ones let go go into the vault's `_trash` at the end, after asking. **Health check** finds
items whose file is missing and counts for items that are gone, and puts them right (`src/clean.c`).

**Continue watching**: a video left part-way starts where it stopped. The places are kept in the
vault's `_app/kv/resume.json`, the same file the PC's Videos tab keeps, so it carries on across the two.

## The Browser

Danbooru, Yande.re, Konachan, e621, Xbooru, TBIB, Hypnohub, AIBooru, Safebooru and e926, read through
their JSON APIs (the same four kinds of site as the desktop's Discover: `src/booru.c`), so no web page
loads. The grid is the Library's; the site button is the tab bar (L/R step through it), Filter picks
the ratings (General, Sensitive, Questionable, Explicit), the search and which category saves go into,
and the sort is Latest, Hot (where the site has it), Top or Random. More results load as you scroll.
The site button opens the sites window (`tools/make-site-icons.mjs` draws its icons). It has two
tabs (L / R): **Pictures**, the sites, ending in **+** to add any booru by its address (the Vita works
out which of the four kinds it is; □ removes one you added), and **Videos**, the video sources, which
open the **video browser**: a banner with what's trending, then rows (Trending, New, Top this month,
Top of all time, Something random) of cards, as the TV apps have them; a video plays in the
Browser's own full view. hanime.tv and MissAV are listed but locked: both only answer after a
Cloudflare browser check, which an app can't pass. Sakugabooru brings anime animation clips. Gelbooru and Rule34 are there too, with your account's API key (Filter → API key: the user ID and the
key from the site's My Account → Options). As on the desktop, posts tagged as sexual content involving
minors are never shown or searched for.

The search box opens the search sheet: type a search, bookmark it, the tags most found in the results
so far (✕ adds one, □ leaves it out), recent searches and bookmarks (on any site). Filter also has
**Top of** (today, this week, this month: Danbooru's `age:`, e621's `date:`, Moebooru's `date:>=`),
**Hidden tags** (posts with any of them never show), and **Saves**: Vita size, or the original at full
size.

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

## Wi-Fi transfer

Settings → Wi-Fi transfer: while that screen is open, the Vita serves a small page at
`http://<its address>:8080` (shown, with a QR code for a phone). Pick or drop pictures and videos on it
from a phone or PC on the same Wi-Fi; each goes onto the card and into the vault (the Browser's save
category, or "Wi-Fi"), with a card-sized thumbnail for pictures, encrypted in an Untraceable vault.
Only that page is served: nothing in the vault can be read from outside, and the server stops when
you leave the screen (`src/wifi.c`).

## The LiveArea

The page behind the bubble (`sce_sys/livearea/contents`, drawn by `tools/make-livearea.mjs`, style
a5): the heart is the gate that starts the app as usual; two slim launchers beside it open the
**Google Drive vault** and the **vault on the card** straight away (past the PIN lock, if there is
one); the panel under the heart shows the vault's numbers (items, hearts, time spent looking),
written by the app whenever a vault opens (`src/livearea.c`). The Google Drive vault is the one
"Get it from Drive" made (a vault of its own, its folder ending in "(Drive)") or one that has been
synced; with none on the card yet, its frame opens Google Drive's screen to get it.

## Google Drive

Settings → Google Drive keeps the vault in step with the **Lewd Vault** folder the PC syncs to Google
Drive (Settings → Storage → Google Drive over the web): both ways, with `src-tauri/src/gdrive.rs`'s
rules (`src/drive.c`). Changes go to the other side; a file is only deleted on one side when the other
deleted it and it hadn't changed since, never on a first sync, and a sync that would delete more than
half of a side stops. When both sides changed a file, the Vita's copy wins and Drive's is kept beside
it; for `index.csv` and `views.csv` that copy is folded in straight after (new rows, the larger counts),
as the PC does. An Untraceable vault syncs with **Lewd Vault (encrypted)** and must have the same
password. On a Vita with no vault yet, △ in the picker gets one from Drive.

**Signing in** is Google's sign-in for TVs: the Vita shows a QR code and a short code; scan it with a
phone, type the code, allow it. **The key**: the `drive.file` scope only shows a key the files made by
its own Google Cloud project, so the Vita needs a client in the PC key's project, and Google's device
sign-in only takes the "TVs and Limited Input devices" type:

1. [console.cloud.google.com](https://console.cloud.google.com/apis/credentials), the same project as
   the PC's key: **Clients → Create client → TVs and Limited Input devices**.
2. Type its Client ID and secret on the Vita once, or put them in `ux0:data/LWDV00001/google.txt`
   (the ID on the first line, the secret on the second). A build can carry one instead
   (`LEWD_VAULT_GOOGLE_TV_CLIENT_ID` / `_SECRET` when it's built; never in the repository).

The key and the sign-in stay in `ux0:data/LWDV00001/drive/`, with each vault's manifest (what each file
was at the last sync); files a sync deletes on the Vita go to `ux0:data/LWDV00001/drive/trash/`. It
can be tried without Google: `tests/drive_mock.py` is a stand-in for the parts of Google it uses, and a
debug build talks to it when `ux0:data/LWDV00001/debug_drive.txt` holds its address.

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

The tab bar is a pill floating over the bottom, with icons in the manner of iOS's symbols
(`tools/make-ui-icons.mjs`, drawn at exactly their size); buttons, chips and rows are at least 36 px
high and every touch target has a few pixels of slack.

Nothing slow happens on the drawing thread: `index.csv` and `views.csv` are written by a writer
thread (the drawing thread only makes the text), videos are opened and closed on a thread of their
own, textures are freed three frames after they're let go instead of waiting for the GPU, and at
most six thumbnails a frame become textures. Every picture is decoded to exactly the pixels it's
shown at: a card's thumbnail to the card's size (covered and cropped, cached at that size), the full
view's picture to the screen (copied into one of three buffers made once), and to 1920 px only when
zoomed in. Gradients are one stretched ramp, not a rectangle per line. Debug builds log each
screen's frame times (`perf ...` in `log.txt`); they were checked in Vita3K pinned to one core and
throttled to a quarter of it with a cgroup.

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
| `src/main.c` | The screens: picker, password, library, full view, settings, cover |
| `src/lib.c` | The Library's filters, tags panel, saved filters, quick actions, selecting many, editing |
| `src/show.c` | The slideshow's sessions and plan; Continue watching |
| `src/pin.c` | The PIN lock (and decoy PIN), and the stats page |
| `src/wifi.c` | Wi-Fi transfer: the upload page and its server |
| `src/privacy.c`, `src/clean.c` | The calculator disguise and touch lock; clean up and the health check |
| `src/livearea.c` | The LiveArea's frames (which vault they open) and its numbers |
| `src/vbrowse.c` | The video browser |
| `tools/make-ui-icons.mjs` | The tab bar's and buttons' icons (`ui/`) |
| `src/drive.c`, `src/drive_ui.c` | Google Drive: the sign-in, the sync, its screen |
| `src/third_party/qrcodegen.*` | Project Nayuki's QR code generator (MIT, its notice in the file) |
| `src/vault.c` | `index.csv` / `views.csv`, finding files, thumbnails (packs too) |
| `src/lvcrypt.c` | The Untraceable format: SHA-256, PBKDF2, ChaCha20, "LVX1" files |
| `src/image.c` | WebP/JPEG/PNG to pixels at the size they're shown |
| `src/video.c` | sceAvPlayer for MP4; FFmpeg + libvpx for WebM/MKV |
| `src/web.c` | The Browser's screens and threads (compiled inside `main.c`) |
| `src/booru.c`, `src/json.c` | The sites' searches and answers; a small JSON reader |
| `src/net.c` | libcurl over the Vita's network |
| `tests/drive_mock.py` | A stand-in for Google's sign-in and Drive, for trying the sync in Vita3K |
| `tests/` | The core against the golden vaults and the site readers against saved answers (`tests/booru/`, from the sites' all-ages halves), on a PC |
| `tools/make-livearea.py`, `tools/make-livearea.mjs` | The bubble, cover art and splash; the LiveArea page (Chromium via Playwright) |
| `tools/make-site-icons.mjs` | The sites window's icons (`sites/`) |
| `font.ttf` | DejaVu Sans (see `FONT-LICENSE.txt`); the Vita's own font is used for Japanese |
| `cacert.pem` | Mozilla's certificate authorities (certifi, MPL-2.0) |

The Title ID `LWDV00001` is where the Vita keeps the app and its data: don't change it.
