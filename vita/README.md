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
| Library | tap a mode (All, Pictures, Videos, Comics) or a category chip; tap a card; drag to scroll; hold a card for its quick actions; drag the right edge to fly through the months | D-pad/stick; ✕ open (hold: quick actions); L/R the mode; △ sort; □ filters; START slideshow; ○ next tab (in a book: back to the shelf) |
| Full view | swipe for the next; pinch to zoom; double-tap zoom (or heart: Settings); the filmstrip at the bottom; pull up for details | ←/→ or L/R next; ✕ zoom; △ details (↑↓ choose, ←→ change: stars, level, category, tags, delete); □ heart; START slideshow; ○ back |
| A video | tap the bar to jump | ✕ play/pause; ←/→ 10 s; the stick scrubs; ↑ 2× / 4× (no sound); ↓ A-B loop (A, B, off); L/R next |
| Browser | tap the site button (the tabs), Filter, the sort, the search box; tap a card | L/R previous/next site; △ sort; □ filter; START the site list; ✕ open; ○ next tab |
| Browser's full view | swipe; pinch; tap Save; pull up for details, tap a tag to search it | ←/→ next; □ save; △ details; ✕ zoom or play/pause; ○ back |
| Anywhere | | SELECT covers the screen with a plain clock (again to come back); ○ in the Library, Browser and Settings goes round those three |

Settings, in sections (Appearance, Playback, Privacy, Sync and sources, Library): search the Library, video
volume, interface sounds, **Theme** (the accent: the vault's own, any of ten for the Vita, or **Custom**:
✕ opens a colour wheel, touch it or ←→ hue, ↑↓ colourfulness, L/R brightness), **Background** (dark,
black for OLED screens, midnight, plum), **Sound pack** (the Vita's own, or Soft, Retro (8-bit) and
Bubbly, made by `tools/make-sound-packs.py`), what a double tap does, the glow behind pictures, Google
Drive, another vault, clearing the thumbnail cache. While Theme or Background is chosen, a small
Library at the right shows it in those colours.

Videos, in both full views: a play/pause button in the middle with ten seconds back and on either side,
the bar at the foot (what has downloaded so far as a lighter part, the time left in a pill); a double
tap at the left or right third skips ten seconds back or on, with a ripple there. Swipe a picture or
video down to close it: it follows the finger, smaller, and flies back into its card. An episode
(a long video) shows its card while it loads or is paused: its name, length and site, and ♥ Favorite
/ ↓ Offline as big buttons.

In **All**, a row over the grid: **Continue watching** (videos stopped part way, with how far) and
**Recently appreciated** (the ♥ given in the full view, the newest first); it scrolls away with the page, ↑ from the first row goes into it. Pinch the
Library with two fingers to make the cards a size bigger or smaller (the three sizes of Filters' Card
size). On the Comics shelf each book has its spine, its pages' edges and a bar for how far it's been read.

The tab bar: its capsule stretches from tab to tab; the Library shows how many came in from the
Browser since it was looked at; the Browser has a ring while a save, an offline copy or a conversion
is going; hold Library for the Comics shelf, hold Browser for Favourites. A new tab slides in from its
side; sheets spring open. The video browser's banner drifts slowly; each source is left where it was;
**Up next** has your favourite videos. Search: your recent searches on the site as chips; after typing,
"Did you mean" offers the tags the last word could be (from the results and past searches);
**≈ Similar** in either full view's details (✕ there) searches for what that one is most about.
**Three fingers tapped together** (all at once, kept still, lifted quickly) bring up the calculator
cover at once, as SELECT does. The very first start shows three cards: the vault, the gestures and
Wi-Fi transfer.

On every screen the chosen card or row has the same ring, gliding from one to the next; a card
still loading shows its shape with a light passing over it and a small loading ring.

**Modes** (the switch beside the title, or L / R): **All** (everything, a comic once: its cover, marked COMIC with its pages, opening the book, ○ back to All), **Pictures** (pictures and GIFs, not
comics' pages), **Videos**, and **Comics**: a shelf of books, one card each (its first page, its pages
counted). A book is a collection whose pages are marked as a comic: the category Comics, Manga,
Doujinshi, Webtoon or Manhwa, or one of those words as a tag, so comics made on the PC or synced
from Drive show up as they are. A book opens at its first page, or at the page after the one last
looked at; its pages run in order ("page 2" before "page 10"); ○ goes back to the shelf. **Tall pages**
(a webtoon's strips, taller than 2.2 times their width) are shown at the screen's width from their top
and scroll down: drag, flick, the stick, or ↑↓ a screenful at a time (↓ at the foot: the next page).
The Manga reader does the same.

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
(slow pan, slow zoom, breathing), a fade from one to the next and a sleep timer. Its page is one
screen: a card on the left (a few of the pictures, the kind as a four-way switch, what it will do, the
levels as a bar, Start), the settings as tiles on the right, two across, each value big with ‹ ›.
↑↓ go through them, ←→ change one (tap a tile's left or right half), ✕ or START starts.

**Reading comics** (`src/reader.c`): a book from the Library and a chapter from the Comics tab are read
the same way, as the reading options say. They're in Settings (Comics) and in the reader (△, or Options
at the top): **Reading** pages one at a time, or a continuous strip scrolled down; **Orientation**
landscape, or portrait with the Vita on its side (right edge up: the D-pad turns with it); **Page turn**
slide, fade, zoom or none; **Direction** left to right, or right to left for manga; **Taps** the sides
turn pages, anywhere turns to the next, or taps only show the controls; **Double tap** zoom or nothing;
**Rear touchpad** a swipe turns pages, or nothing. In the Comics tab, □ saves the chapter.

**Viewing pictures and videos** (`src/viewopt.c`): how the full views (the Library's and the Browser's)
show what's open, set apart for pictures and for videos. **Size**: fit to the screen, fill it (cropped),
its original size (each of its pixels a screen pixel; the sharper copy is loaded), or fit the width;
a video can also be stretched to fill. **Orientation**: landscape, or portrait with the Vita on its
side (as the comic reader turns it; swipes and drags turn with it). A picture bigger than the screen
is moved with a drag or the stick, and a swipe past its edge goes on to the next. They're in Settings
(Playback) and in the full view: the size button at the top, or ↑ on a picture.

The Filters and Order lists have an icon for each row.

**Order** (△ or its button: a list): newest, oldest, most viewed, least viewed, recently viewed, most
time spent, top rated (the stars), most appreciated (♥), shuffle, for you.

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

Anime and hentai boorus: Danbooru, Yande.re, Konachan, Xbooru, Hypnohub, Safebooru, Sakugabooru,
Gelbooru, and Xbooru's and Danbooru's comics, read through their JSON APIs; and **image sites** that
aren't boorus, marked IMAGE SITE in the sites window after the boorus: **E-Shuushuu** (anime art; its
own API, a search word looked up as its tag number first) and **booru.io** (anime and hentai art, one
page of 50 a search). Each is a kind of its own in `src/booru.c` (`BK_ESHUU`, `BK_BOORUIO`), with
tests against saved answers (the same four kinds of site as the desktop's Discover: `src/booru.c`), so no web page
loads. The grid is the Library's; the site button is the tab bar (L/R step through it), Filter picks
the ratings (General, Sensitive, Questionable, Explicit), the search and which category saves go into,
and the sort is Latest, Hot (where the site has it), Top or Random. More results load as you scroll.
The site button opens the sites window (`tools/make-site-icons.mjs` draws its icons). It has three
tabs (L / R), each in pages of eighteen tiles (the arrows and dots at the top, or a swipe sideways): **Comics** (MangaDex, E-Hentai and the sources below: covers, a book's chapters, and a reader that turns
pages with ←/→, a tap at either side or a swipe, L / R for the chapters, ✕ to zoom; `src/manga.c`,
`src/manga_ui.c`; △ or **Save chapter** puts the chapter, or the whole E-Hentai gallery, into the
vault as a comic: its pages as they are, the category Comics, the book's tags and `comic`, one
collection, so it's a book in the Library's Comics mode). Each comic source's tile says what it has:
**MANGA** (MangaDex: manga of all kinds), **H MANGA** (E-Hentai, HentaiFox, AsmHentai, HentaiEra,
nhentai.xxx, HentaiZap, HentaiEnvy, 3Hentai, HentaiHand, HentaiRox, Pururin, HentaiManga: doujinshi and hentai manga, one gallery a
book), **H WEBTOON** (Manhwa18, HentaiWebtoon, MangaDistrict, Manga18fx, ManhwaDen: adult webtoons, a series with its chapters, read as one continuous strip) or **SHORT
COMICS** (Multporn, FreeAdultComix: short adult comics on one page). They're read from the sites'
own pages as a browser would get them (`LV_MSRC` in `src/manga.c`), with the app's own User-Agent; a
gallery tagged as something never shown isn't opened (its own tags, not its "related" ones). A webtoon's chapter can come as a few strips, each 700 x 20,000 pixels or more: it's never decoded whole.
Where the page gives a strip's size, it's read as bands of about a page's shape (`#lvband=` in its
address): the strip is downloaded once and cut top to bottom in one pass (`lv_decode_bands` in
`src/image.c`), each band kept as a small JPEG for the last two strips, and the reader asks for one band
at a time; Save keeps the bands as the chapter's pages. Comics are always 18+ (there's no switch). Where each book was left (its chapter and page) is kept
(`comic_progress.tsv`): a book shows **Continue** (or R), and the top of a source's list has a
**Continue reading** row of the books left part way, from any source. **↓ All** on a book with
chapters saves every chapter not in the vault yet, one after another, each a comic of its own.
MangaDex
chapters only published officially elsewhere have no pages on MangaDex: when a book has no English
chapter to read there, its chapters in every language are listed instead. Sites behind a Cloudflare
check (nhentai.net, IMHentai, Hentai2Read, Toonily, ...), ones that are only a JavaScript app, and ones
of AI-made comics aren't offered. **Pictures**, the sites, and **Videos**. Each
tab ends in **+**, the **source finder**: it reads the lists other booru tools keep (gallery-dl's
supported sites, the `booru` library's sites.json, Grabber's site lists: `src/discover.c`), keeps the
sites of the kinds the Browser reads, leaves out what's never offered (sites for minors, furry,
pony and real-people boorus), then asks each one as the Browser would: does it answer, and how many of
its latest 40 are videos. One press adds a site, or a video source (□ in the switcher removes one
you added; "Type an address" adds any booru by hand). The lists know few sites the app doesn't have
already: most of what turns up are the video sources (AIBooru, Hypnohub, Safebooru, ...). The Manga
tab says why it can't offer more: each reader site is a program of its own. **Videos**, the video sources, which
open the **video browser**: a banner with what's trending, then rows (Trending, New, Top this month,
Top of all time, Something random) of cards, as the TV apps have them; a video plays in the
Browser's own full view. **Rule34Video** (its hentai, Genshin Impact and Blue Archive tags) is read as a
KVS site: its list pages as HTML, each video's MP4 (the 360p one, quickest to come) from its own page,
whose tags are checked again. **Check sources** in the sites window asks every source of the tab and
moves the ones that don't answer to the end, greyed. hanime.tv and MissAV are listed but locked: both only answer after a
Cloudflare browser check, which an app can't pass. Sakugabooru brings anime animation clips; Xbooru Comics and Danbooru Comics are those sites' comic pages. Gelbooru and Rule34 are there too, with your account's API key (Filter → API key: the user ID and the
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

**Favourites and long videos.** A long video (8 minutes or more, or from a source of whole episodes:
**HentaiWorld**, **HentaiCloud** and **HentaiCity** (each episode's own page read before it's shown:
its title, description, categories or tags checked against the never-shown words, its MP4 found
there; the answers kept for the session), Hentai Episodes and Uncensored Episodes (xvideos' hentai
search of 10 minutes and longer), R34V Longest (Rule34Video's longest)) gets two buttons in the full view instead of Save: **♥ Favorite** keeps
only its link (□), **↓ Offline** downloads it into the vault (START; asked while it's still coming, the
save page opens once it's all here). A comic's page has the same two (□ and △). The **Favourites**
screen (the ♥ in the Library's Videos and Comics modes) lists them, a tab each: Play or Read, Make
available offline, Remove (`src/favs.c`, `ux0:data/LWDV00001/favorites.tsv`).

**Offline copies are 480p.** The long-form sources' files are 480p already. A booru keeps only its
original: when that's bigger and Settings › Video conversion is turned on (it starts off; with it
off, such a video stays a favourite), the Vita (which can't re-encode video) hands its address to a
**conversion service** with your own API key, CloudConvert, FreeConvert or Rendi, tried in that order,
which makes an 854 x 480 H.264/AAC MP4 the Vita then downloads into the vault (`src/conv.c`). Add keys
in Settings › Video conversion, or paste them on the Wi-Fi transfer page (they're long); they're kept
in `ux0:data/LWDV00001/convert_keys.txt` only, and the service sees the video's address. With no key,
such a video stays a favourite.

**Videos play as they download.** The MP4 goes straight to the card and the system player starts as
soon as the first 768 KB are there, its reads waiting for the bytes still coming (`lv_video_open_growing`
in `src/video.c`, `lv_http_stream_file` in `src/net.c`); a thin line under the controls shows how much
has come. (An MP4 whose index is at its end waits for the end.) It starts with a head start (2 MB, or
2% of the file), pauses with "Buffering" if it catches up with the download, and seeks (a tap on the
bar, the left stick, ↑ ↓ ten seconds) only as far as has come. Leaving the video stops its download;
the Vita stays awake while one runs; last time's videos are deleted from the card at the start.
AI-made posts are left out everywhere: by their tags on the boorus, by their titles on the video sites.

## Wi-Fi transfer

Settings → Wi-Fi transfer: while that screen is open, the Vita serves a small page at
`http://<its address>:8080` (shown, with a QR code for a phone). Pick or drop pictures and videos on it
from a phone or PC on the same Wi-Fi; each goes onto the card and into the vault (the Browser's save
category, or "Wi-Fi"), with a card-sized thumbnail for pictures, encrypted in an Untraceable vault.
Only that page is served: nothing in the vault can be read from outside, and the server stops when
you leave the screen (`src/wifi.c`).

## The LiveArea

The page behind the bubble (`sce_sys/livearea/contents`, drawn by `tools/make-livearea.mjs`): the
gate in the middle starts the app as usual; the frame on its left opens the **Google Drive vault**
and the one on its right the **vault on the card**, straight away (past the PIN lock, if there is
one); the strip along the bottom shows the vault's numbers (items, hearts, time spent looking),
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

**Layout and gestures.** The Library's and the Browser's headers are one row of large buttons (the
Library: the modes, a category list, Filters, the sort, a Wi-Fi light that is green online and red
not; the Browser: the site, the search, Filter, the sort, the light). Scrolling down tucks the header
away and shrinks the tab bar to its icons; scrolling up brings them back. A sideways flick across the
grid changes the Library's mode or the Browser's site. The **tags sidebar** opens from the small arrow
at the right edge or a swipe in from it (a tap on a tag searches for it, a swipe left on it leaves it
out, a tap on a search word takes it away); a drag down the right edge is still the month scrubber.
**Save** in the Browser's full view opens the **save page**: a step a screen (how explicit, 0-10;
the category, or a new one; stars and whether to keep the site's tags), then an overview whose rows
go back to their step, and Save (Save now on every step).

**Scrolling never waits.** Cards whose picture isn't here yet show a loading ring; pictures are
decoded on other cores and put on the GPU at most 3 ms a frame; while the grid is flung fast nothing
new is asked for, and the next screenful is fetched in the direction it's going.

**Memory, with a margin.** Never right up to a limit: the C heap is 128 MB (measured in use: a few MB,
~30 MB while a big picture is decoded) and a big decode that would take it past 95% is made at the
screen's size instead; textures keep 8 MB of video memory and 16 MB of main memory free, going to main
memory when video memory is short, and not made at all rather than crash (`tex_new` in `src/main.c`).

The highlight ring that follows the buttons hides once the screen is touched, and comes back with the
next button press or push of the stick.

The tab bar is a pill floating over the bottom, with icons in the manner of iOS's symbols
(`tools/make-ui-icons.mjs`, drawn at exactly their size); buttons, chips and rows are at least 36 px
high and every touch target has a few pixels of slack.

**Smart collections** (first in the categories' list): Unseen, Hearted this month, Most viewed (in
that order), Short clips (GIFs and videos of 30 s or less) and New this week, kept up to date as the
vault changes. **Find duplicates** (Settings, Library): each thumbnail made into a fingerprint (a
difference hash), the ones that all but match grouped; the sharpest is kept, ✕ sends the rest to the
vault's trash (`src/dupes.c`). **Choosing many** (hold a card): a sideways drag paints the choice over
the cards it crosses; △ gives stars, the level, a category, a tag, a heart, a collection, a slideshow
of just them, all or none, or the trash. The Slideshow page keeps **your own** settings under a name
(+ Save or R; a tap or L to use one, held to let it go).

**Each part on its own.** The app's parts (the Library, its full view, the Browser and its full view,
the video browser, Comics, Favourites, Settings) each hold what they show: textures, a screen buffer or
three, connections. Held and not drawn, that costs no time, so a part that's been left keeps it, and
coming back is instant; but they mustn't add up to too little room for what comes next. So when
memory gets short (checked six times a second), every part not on screen gives its memory back at
once (`janitor` and `release_hidden` in `src/main.c`), the Library's own thumbnails too: off screen for
three frames or more, the GPU is done with their textures, so they're freed there and then. Before
something big is made (a video's player, a picture at twice the screen) the room is made first
(`make_room`), and a texture that finds no room gets the others let go and is tried once more. Their
lists stay, so coming back only fetches what's on screen. A debug build's `debug_lowmem.txt` in the
data folder makes memory always short, to test it. Each
worker closes its connection after 20 s with nothing to do (`wait_work`), Google Drive after each job,
and the Wi-Fi server stops with its page. Icons and other small textures live in main memory, in 4 KiB
pages, not a 256 KiB granule of video memory each (fifty icons and forty site tiles were ~22 MB of
it). Debug builds log what's held at every change of screen (`res` lines in the log).

**Smooth, frame after frame.** Motion goes by the clock, not by frames: each frame knows how long the
last one took (`ft`), and the eases, the flings' slowing and the scrolls use it (`ease`, `decay`), so a
slow frame moves things as far as two quick ones would instead of the animation stalling. Nothing slow
is done on the drawing thread: the network's base starts on a thread of its own at launch (the online
light shows offline until it's up, never a wait), the Browser's four workers are made by a helper, the
font's letters at the sizes used most and the app's and sites' icons are read under the loading screen
(and the sites window makes at most three new icons a frame after that), and pictures that come in are
put in a few at a time, none once a frame has spent 5 ms (`frame_busy`), the rest the next frame.

**Webtoon strips in a small budget.** A strip (700 x 23,000 pixels) is cut into page-sized bands
without ever being decoded whole: a JPEG a row at a time (a progressive one's coefficients, which
libjpeg keeps for the whole picture, are held compressed in RAM by our own backing store, ~7 MB
instead of ~50), a WebP a few bands per pass, a PNG in one pass (`lv_decode_bands` in `src/image.c`).

The drawing thread has the first CPU core to itself (every worker is pinned to the other two), the
clocks are at their highest (444 / 222 / 222 / 166 MHz), and the network starts on a thread of its
own at launch. Anything that takes a while shows a loading circle. Nothing slow happens on the
drawing thread: `index.csv` and `views.csv` are written by a writer
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

## On a real Vita

- **Drawing:** vita2d starts each frame's vertices at the start of one pool while the GPU may still be
  drawing the frame before (two can be queued), which garbled shapes. Each frame now starts in its own
  third of the 4 MB pool, in turn. Textures are freed three frames after their last use (or after
  `vita2d_wait_rendering_done()`). The frame waits for vblank with `sceDisplayWaitVblankStartCB()`.
  Every drawing call refuses a coordinate that isn't a number or is absurdly far off.
- **Lean:** nothing runs that isn't needed. Network threads start when the Browser, Drive and so on
  are first used, not at launch or when a vault opens. The sound mixer sleeps until a sound plays.
  Each frame's hand-offs from the workers are skipped without locking when nothing is waiting. The
  AI-made check runs once per post as it's read (and once over favourites when they load), never
  per frame.
- **Network:** the Net module, `sceNetInit` (its 1 MB buffer static), then `sceNetCtlInit`, quickly
  and only when something needs it. libcurl and OpenSSL (seconds on a cold start) start on the first
  worker thread that fetches, never on the drawing thread. No SceSsl/SceHttp module is loaded. Each request first
  checks `sceNetCtlInetGetState`: with no Wi-Fi it fails at once ("Offline"). TLS is libcurl with
  OpenSSL and the bundled Mozilla CA list (`cacert.pem`), with verification always on. A certificate
  that looks expired or not yet valid gets a message to set the Vita's date and time. Connect times
  out at 15 s and a stalled transfer after 20 s. A connection that breaks before anything arrives is
  tried once more on a new connection (never the same socket). Downloads go to `.part` files, which
  are removed on failure.
- **Sleep:** the power callback, or the wall clock jumping more than 5 s between frames, starts every
  connection afresh, stops the videos and restarts Wi-Fi transfer.
- **Power:** `sceKernelPowerTick` keeps the Vita awake while a video plays, a slideshow runs or anything
  downloads. While an offline copy, a chapter or a conversion is being saved, the PS button is held
  (`sceShellUtilLock`) and comes back the moment it's done.
- **Memory:** a 128 MB heap, kept under 95%. Textures go in video memory while 8 MB of it stays free
  (counted in its 256 KiB granules), else in main memory while 16 MB stays free.
- **VPK:** TITLEID `LWDV00001`, VERSION `01.00`, an UNSAFE self; `icon0` 128×128, `pic0` 960×544,
  `bg` 840×500, `startup` 280×158 and the LiveArea's other pictures, all 8-bit indexed PNGs.

## When it crashes

The Vita writes a dump to `ux0:data/psp2core-*.psp2dmp`. The address it crashed at (its PC, shown by
the system's crash dialog or a dump parser such as vita-parse-core) turns into a line of the source
with the build's ELF, from the same build as the VPK:

```
arm-vita-eabi-addr2line -f -e vita/build/LewdVault <PC address>
```

A debug build (`-DLV_DEBUG`) also writes `ux0:data/LWDV00001/log.txt`, with each screen's frame
times (`perf ...`) and any step over 4 ms (`pt ...`).

## The test kit

A separate VPK for testing, **Lewd Vault Test** (`LWDT00001`, so it installs beside the app and
uses the same vault, settings and data folder). Built by CI as the `LewdVault-test-kit-vpk` artifact,
or by hand:

```
cmake -S vita -B vita/build-test -DLV_TESTKIT=ON -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake && cmake --build vita/build-test
```

It opens on a menu (`src/test.c`): **Open the app normally** (the app as usual, a line of live
numbers at the top: frames a second, each frame's work, the heap, free main and video memory; every
button press and tap logged), **Run every test**, or one of them: the Library scrolling, the full view
going through pictures (how long each takes to be sharp), the Browser (first page, first previews,
scrolling), the video browser, Comics (a MangaDex list, book, chapters, pages), webtoon strips (cut
times, memory held), memory (every part made to give its memory back, then visited again), the
network (every source asked once: answer, time, size), the memory card (16 MB written and read, the
vault's files read) and a stress run (five tours of every screen, memory after each). The app drives
itself through the real screens; ○ and △ together stop a test. The settings are put back after.

Each run writes a report to `ux0:data/LWDV00001/reports/`: the console (firmware, model, clocks,
battery), memory, the vault, each test's numbers (frames a second, frame length average, p95, p99,
worst, late frames, work per frame, the heap and memory at their lowest; the aim is 60 frames a
second, and each measure says whether it was HELD, 99% of frames on time, or MISSED, and whether the
work or the drawing was the cause) and the log's last lines. The live numbers go red under 58 fps.
Reports can be read from a PC on the same Wi-Fi: open the app's Wi-Fi transfer page and go to
`/reports`. With a web address in `ux0:data/LWDV00001/report_url.txt` (a Discord webhook, or anything
that takes a POST) each report is also sent there as it's written; the address stays on the card,
never in the repository.

Crashes: where the app is (the test, its step, the screen) is kept in `testkit_marker.txt` as it
goes, and the previous run's log is kept as `log-prev.txt`. A run that ended without saying it was put
away (the PS button) is a crash: at the next start a crash report is written (where it was, the
system's own dumps in `ux0:data`, the last 200 lines of its log), and sent if there's an address.

## Files

| File | What it is |
|---|---|
| `src/main.c` | The screens: picker, password, library, full view, settings, cover |
| `src/lib.c` | The Library's modes (All, Pictures, Videos, Comics: books), filters, tags panel, saved filters, quick actions, selecting many, editing |
| `src/show.c` | The slideshow's sessions and plan; Continue watching |
| `src/pin.c` | The PIN lock (and decoy PIN), and the stats page |
| `src/wifi.c` | Wi-Fi transfer: the upload page and its server |
| `src/privacy.c`, `src/clean.c` | The calculator disguise and touch lock; clean up and the health check |
| `src/livearea.c` | The LiveArea's frames (which vault they open) and its numbers |
| `src/vbrowse.c` | The video browser |
| `src/test.c` | The test kit (`-DLV_TESTKIT=ON`): its menu, the tests that drive the app, reports, crash reports |
| `src/viewopt.c` | Viewing: the full views' size (fit, fill, original, fit width, stretch) and orientation, and their panel |
| `src/manga.c`, `src/manga_ui.c` | Comics: MangaDex, E-Hentai and the other sources (`LV_MSRC`), the covers, chapters and the reader, saving a chapter as a comic |
| `src/discover.c`, `tests/discover/` | The source finder's readers of the booru lists, and saved copies of those lists |
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
| `tests/manga_test.c`, `tests/manga/` | The manga readers against saved answers (all-ages ones) |
| `tests/drive_mock.py` | A stand-in for Google's sign-in and Drive, for trying the sync in Vita3K |
| `tests/` | The core against the golden vaults and the site readers against saved answers (`tests/booru/`, from the sites' all-ages halves), on a PC |
| `tools/make-livearea.py`, `tools/make-livearea.mjs` | The bubble, cover art and splash; the LiveArea page (Chromium via Playwright) |
| `tools/make-site-icons.mjs` | The sites window's icons (`sites/`) |
| `font.ttf` | DejaVu Sans (see `FONT-LICENSE.txt`); the Vita's own font is used for Japanese |
| `cacert.pem` | Mozilla's certificate authorities (certifi, MPL-2.0) |

The Title ID `LWDV00001` is where the Vita keeps the app and its data: don't change it.
