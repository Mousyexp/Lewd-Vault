# Image Vault — desktop

Image Vault as a portable Windows app: one 6.0 MB `Image Vault.exe`, built with Tauri 2 (a small
Rust program plus the WebView2 that Windows already has). Your files live in a plain folder on your
disk instead of Google Drive. It works offline, and it adds booru-style tags and a desktop layout.
The page is shared with the Google Apps Script web version: `ui/Index.html` is that page, and the
build applies the desktop changes to it (see Build it).

## Repository layout

| Path | What it is |
| --- | --- |
| `ui/Index.html` | The shared page (same file as the web version's `Index.html`) |
| `ui/*.js`, `ui/desktop.css` | The desktop layer added on top of the page |
| `sync-ui.mjs` | Builds `src/` (the page Tauri serves) from `ui/` — `npm run sync` |
| `src-tauri/` | The Rust backend (Tauri 2) |
| `art/` | Icon, decor and sound sources copied into the build |
| `tools/` | Icon/decor/theme generators, the dist packager and the shortcut script |
| `docs/PLAN.md` | The development plan the "Phase N" notes refer to |

`src/`, `dist/`, `node_modules/` and `src-tauri/target/` are build output and are not committed.
Build: `npm install`, then `npm run dist` (needs Rust and the Tauri prerequisites).

## Run it

Double-click the **Image Vault** shortcut on your desktop, or `dist\Image Vault\Image Vault.exe`.
The first start asks where your library should live (see below).
The `Image Vault` folder is the whole app: move or copy it anywhere (a USB stick works too). Next
to the exe it creates

```
Image Vault data/
  index.csv, views.csv                 the list and the view counts (Drive's columns, plus "tags")
  <category>/000123_261016_v073_<category>.mp4
  _app/thumbs/  _app/clips/  _app/streams/
  _app/settings.json, _app/state.json  settings and the next unique number
```

This is the same layout and the same file names as the Drive folder, so it stays usable without the
app, and a copy of your Drive folder opens as it is (see below). To keep the data somewhere else, set
`IMAGE_VAULT_DATA=D:\path\to\folder` before starting. Delete moves files to the Recycle Bin.
Starting the app a second time brings the open window forward.

You need the WebView2 runtime. Windows 10 and 11 normally have it.

## Where the vault lives (welcome screen)

On first start the app asks where your library should live:

- **Google Drive, OneDrive, Dropbox or iCloud Drive.** Image Vault makes its own folder,
  **Image Vault Desktop**, inside that service's synced folder, and the service's app uploads it and
  keeps it in sync. It's found automatically:
  - Google Drive for desktop's `X:\My Drive`, or its mirrored folder;
  - the OneDrive folder;
  - Dropbox's `info.json`;
  - `iCloudDrive`.

  A service that isn't installed shows **Get the app** (its download page) and **Check again**.
- **This PC:** `Image Vault data` next to the app.
- **Another folder…:** a folder picker, for a USB drive, a NAS or any other synced folder.

If you already have a library, you can **copy it** to the new place (the old folder stays as a
backup) or start empty. If the chosen folder already holds a vault, the app opens that one. The app
then restarts on the new place.

To change it later, go to **Settings → Storage → Change where it lives…**.

- **Why the services' own apps, not a sign-in inside Image Vault:** everything still reads from the
  local disk, so the app stays as fast as before and works offline. There's no Google developer
  project or sign-in code to maintain, and the service handles uploading, conflicts and your
  other devices.
- **Where the choice is kept:** `Image Vault.config.json` next to the exe, or
  `%APPDATA%\Image Vault\config.json` if that folder can't be written.
- **What never syncs:** the card-size thumbnail cache and unfinished uploads stay on this PC in
  `%LOCALAPPDATA%\Image Vault`.
- **Deleting:** if the cloud drive has no Recycle Bin, deleted items go to the vault's `_trash`
  folder.
- **The web version:** the desktop vault is a separate folder from the web app's `Image Vault`
  folder on Drive, because the two name files differently.
- **Two computers:** use the vault on one computer at a time. The cloud service would make conflict
  copies of `index.csv` if two wrote it at once.

### Untraceable (encrypted, no traces on the PC)

For a PC that isn't only yours, such as a work laptop. Pick **Untraceable** on the welcome screen
(or **Settings → Storage → Change where it lives…**), choose a password, and the app keeps the vault
in a hidden `data.temp` folder (`src-tauri/src/crypt.rs`, `storage.rs`): next to `Lewd Vault.exe` for
the portable copy, in **Documents** for the installed one (it can't write into Program Files), or any
place picked with **Another place…**. A folder a cloud service uploads (OneDrive often backs up
Documents on work PCs) gets a warning: the files stay encrypted, but they'd be in that cloud too.

- **Every file is encrypted** (ChaCha20, with a key made from your password by PBKDF2-SHA256): the
  pictures, videos, thumbnails, `index.csv`, settings, everything. Without the password they're
  noise: Explorer, Windows Search and photo apps can't show, preview or index them. Files are
  decrypted only in memory, as the app reads them (seeking in videos included).
- **Nothing goes anywhere else on the PC:** the thumbnail and web caches, unfinished uploads,
  Discover's sources and the Google Drive sign-in stay in `data.temp` (encrypted), and the built-in
  browser engine runs in private mode with its data folder in `data.temp` too. Deleted items go to
  the vault's own `_trash`, not the Recycle Bin. The choice itself is kept next to the app
  (portable) or in `%APPDATA%\Image Vault\config.json` (installed), hidden.
  Discreet (number-only) file names are switched on, the window is called "Notes" on the taskbar,
  and "Open in another app" is turned off (it would need a decrypted copy on disk).
- **The password** is asked on a plain grey screen every time the app starts. Forget it and the vault
  can't be recovered.
- **Moving in:** your current library can be copied in, encrypted on the way. Optionally the old,
  unencrypted folder is then deleted for good, and what the app kept on this PC before (caches, the
  browser engine's data in `%LOCALAPPDATA%`, the `%APPDATA%` config when the app keeps its own next to itself) is cleared at the next start.
- **Installer or portable:** both can do it. The portable zip leaves less behind: the installer shows
  in the Start menu and in Settings → Apps, and keeps its config in `%APPDATA%`.
- **With Google Drive in your browser**, an untraceable vault syncs to its own Drive folder,
  "Lewd Vault (encrypted)", and stays encrypted there. Use the same password on every PC (the app
  checks, and stops syncing on a mismatch).
- **What it can't hide:** that a program ran (Windows keeps the names of programs that ran, so rename
  `Lewd Vault.exe` and its folder to something dull), the size of `data.temp`, and anything the
  Windows account's administrators or monitoring software record (screenshots, network logs). It
  hides the files from anyone looking at the disk; it isn't protection against your employer's IT.

### Google Drive over the web (no Drive app needed)

Google Drive for desktop needs an administrator to install. On a PC without it, pick **Google Drive
in your browser** on the welcome screen (the vault itself on **This PC** or **Untraceable**, then you
sign in), or turn it on later in **Settings → Storage → Google Drive over the web**
(`ui/drivesync.js`, `src-tauri/src/gdrive.rs`):

1. **A Google key.** Google only lets an app into Drive with an "OAuth client". A release build can
   carry one (set `LEWD_VAULT_GOOGLE_CLIENT_ID` and `LEWD_VAULT_GOOGLE_CLIENT_SECRET` as repository
   secrets; the release workflow builds them in). Otherwise press **Add your key…** and make your own,
   free, in about five minutes:
   1. [console.cloud.google.com](https://console.cloud.google.com/apis/credentials): make a project.
   2. **APIs & Services → Library → Google Drive API → Enable.**
   3. **Google Auth Platform** (OAuth consent screen): any app name, audience **External**. Add your
      Google address as a **test user**, or press **Publish app** (in "Testing" Google signs you out
      after 7 days; published, it may show an "unverified app" warning you can click past).
   4. **Clients → Create client → Desktop app.** Paste its Client ID and Client secret into the app.
2. **Sign in with Google.** Your web browser opens Google's own sign-in page; the app gets a token
   that can only reach the files it made itself (the `drive.file` scope), never the rest of your
   Drive.
3. **Syncing.** The vault syncs both ways with a **Lewd Vault** folder in My Drive: on sign-in,
   every 10 minutes while the app is open (can be turned off) and on **Sync now**. Large videos go up
   in 8 MB resumable pieces.

- **Deletions** go across only for files that were synced before and didn't change since, into the
  Recycle Bin here or Drive's trash there (restorable for 30 days). Never on a first sync, and a
  sync that would delete more than half of one side stops and says so.
- **Both changed:** this PC's copy wins and Drive's is saved beside it as `<name> (from Drive)<ext>`.
  For `index.csv` and `views.csv` that's a conflict copy the app merges by itself.
- **Not synced:** `_app/cache`, `_app/backups`, unfinished uploads.
- **On this PC only:** the key and the sign-in, in `%LOCALAPPDATA%\Image Vault\drive\<vault>\`.
- **Several PCs:** use the **same key** on each (Drive only shows a key the files made with that
  key). The Google Drive for desktop option is separate and unchanged.

## Discover (built-in image browser)

The **Discover** tile browses image sites inside the app and saves straight into the vault.

**Sources:**

| Site | Account needed |
|---|---|
| Danbooru | no |
| Safebooru | no |
| Yande.re | no |
| Konachan | no |
| Gelbooru | yes: API key |
| Rule34 | yes: API key |

Add any other site of the same three kinds (Danbooru-, Gelbooru- or Moebooru-style) with its
address. Keys stay on this PC, in `%LOCALAPPDATA%\Image Vault\sources.json`.

**Not a web browser.** No site page is ever loaded. Image Vault reads each site's API (its data
feed) and fetches only the pictures, through the app (`src-tauri/src/web.rs`). That's stronger than
an ad blocker:
- no ads, popups, redirects or trackers;
- the sites only ever see this app;
- only the sites you added can be reached.

Pictures are cached on this PC (at most about 1.5 GB).

**What it does:**
- **Search:** with the site's own tag suggestions. Tab or the arrows pick one.
- **Filters:** rating chips (General, Sensitive, Questionable, Explicit) and Newest, Top score or
  Random.
- **History:** ‹ › go back and forward through your searches, instantly, scroll position included.
- **Results:** edge to edge, whole pictures in rows, more as you scroll. Hover shows the glow and a
  ♥ to save.
- **Details:** the picture large with its colours glowing behind it, ← → for the next one.
  - Size, type, rating, score, date, the original source.
  - Every tag in its type colour: click one to search for it, + to add it to the search.
  - "More like this" for the artist and characters.
  - **Open on site** opens the post in your normal browser.
- **Save (♥ or S):** downloads the full picture and runs it through the normal conversion.
  - It goes into a category named after the site.
  - It keeps the site's tags, with artist, character and series types where the site gives them.
  - The NSFW level comes from the site's rating, and it's rated 2 stars.
  - The post's link goes in the new `source` column of index.csv, so saved posts show
    "✓ In your vault".
  - **Save as…** lets you set the category, NSFW level, stars, collection and tags first.
- **Hiding tags:** **Sources → Never show posts with these tags** hides whatever you list.
- **Built-in filter:** posts tagged as sexual content involving minors (`loli`, `shota` and similar)
  are never shown and can't be searched for. This can't be turned off.

Dropdown menus (their open lists too) use the theme's colours.

## Speed and look (the desktop layer)

1. **Thumbnails and preview clips load straight from disk.** `<img>` and `<video>` point at the
   app's file server, so nothing is copied into memory as a Blob. The web version keeps up to 800
   thumbnail and 150 clip copies.
2. **Card-sized thumbnails.** The backend makes 240–960 px JPEGs of the 900 × 1200 thumbnails, to
   fit the card size and screen scaling, and keeps them in `_app/cache/` (safe to delete). Each card
   decodes about 6× fewer pixels.
3. **Instant full view.** Pictures open by URL, the grid thumbnail shows until the full picture is
   decoded, and the pictures either side are decoded ahead, so ← → is instant.
4. **Faster start.** No loading screen (the list is local), and the app opens on the section you
   used last.
5. **No white flash.** The window starts hidden in your theme's colour and appears once the page
   has drawn itself.
6. **Title bar in the theme.** It replaces the white Windows frame. Drag it, double-click it to
   maximize, or use its minimize, maximize and close buttons.
7. **One-line Library toolbar.** Filters are pills, with the item count and a **zoom slider** for
   picture size. "Whole pictures" (justified rows) is the default layout.
8. **Ambient detail view.** A blurred glow of the picture's colours sits behind it, the panel
   slides in, and pictures sharpen in as they load.
9. **Hover** shows a faint, short glow around the picture, and nothing over it.
10. **Keyboard and polish.**
    - `/` or Ctrl+F searches, Esc clears the search, and Ctrl+1…6 switch sections.
    - Sections fade in.
    - "Nothing matches" offers to clear the search.
    - Reduced-motion settings are respected.

Before and after, measured on a 1,500-item library with real 900 × 1200 thumbnails:

| | before | after |
|---|---|---|
| launch → Library on screen with thumbnails | 1.96–2.04 s | 1.35 s |
| first screen of thumbnails after opening the Library | 123 ms | 77 ms |
| 30-screen scroll settled | 5.7 s | 3.2 s (thumbnails not resized yet) |
| renderer memory after that scroll | 184 MB | 124 MB |

The WebView2 GPU process holds 400–750 MB in both versions. It drops to about 410 MB as soon as
the Library is off screen: that's Chromium's image and tile cache, which it frees when needed.

**No wasted space:**
- **Library, collections and Clean up:** previews sit edge to edge, with no gaps and no rounded
  corners. Hovering shows a thin accent outline inside the tile instead of lifting it.
- **Library edges:** the grid runs from the sidebar to the tag panel, and the panel is docked flush
  on the right.
- **Viewer:** the picture or video fills its whole area, scaled to fit and never cropped.
  "Hide details" (or **I**) removes the side panel so it gets the whole window; the choice is
  remembered.

## Time spent

How long each item is actually on screen counts for more than how often it was opened.

- **When it counts:** every second an item is in the full view, or on screen in the slideshow
  (not paused), while the window is in front and visible.
- **Walking away:** a still picture stops counting after 3 minutes with no mouse or keyboard
  activity. Playing videos and the slideshow keep counting.
- **Where it's kept:** the `seconds_viewed` column in views.csv, saved every 20 s. Seconds not saved
  yet wait in the browser storage, so closing the app loses nothing.
- **Where it shows:**
  - the details line ("· 4 min looked at");
  - Stats: a "Time looking" total and a "Most time spent" list;
  - the Library sort "Most time spent".

**Hover in the Library:** no tags or details over the picture, just a faint, short glow around it.

## Tags

After the category, add as many tags as you like. They work like on Gelbooru:

- **Format:** lowercase, words joined with `_`, separated by spaces: `blue_hair smile outdoors`.
- **Types:** a prefix gives a tag a type and a colour: `artist:` (red), `character:` (green),
  `series:` (purple), `meta:` (orange). The shortcuts `a:` `c:` `s:` `m:` work too. Tags without a
  prefix are general (blue).
- **Typing:** Space, comma or Enter finishes a tag, and Backspace removes the last one. Suggestions
  show tags you already use, most used first: ↑ ↓ pick one and Tab takes the first.
- **Hints:** under the box are the tags most used in the chosen category and your recent tags.
  Tags carry over to the next file, and a new batch starts empty.
- **Library search:**

  | Query | Matches |
  |---|---|
  | `blue_hair smile` | both tags |
  | `-sketch` | without this tag |
  | `~cat ~dog` | either tag |
  | `blue_*`, `artist:*` | wildcards |
  | `category:art`, `collection:x`, `name:x` | category, collection, file name |
  | `kind:video` | type of item |
  | `tagcount:0`, `tagcount:>5` | number of tags |

  A tag without a type also finds the same name with a type (`zoe` finds `character:zoe`).
- **Tag panel:** on the right of the Library, it lists the tags in the current results, grouped by
  type, with counts. Click a name to search for it, `+` to add it to the search, `–` to exclude it.
- **Bulk tagging:** "Tag all shown" adds or removes tags on every item the current search shows.
- **Detail view:** edit the tags; click one to search for it.
- **Slideshow:** a tag search limits it to matching items.
- **Stats:** shows the most used tags.

Tags are stored in the `tags` column of index.csv. Only the desktop app knows about them; the web
version ignores the column (and would drop it if it rewrote that same file).

## Icon and loading screen

- **Icon:** your picture (`art/icon-source.webp`) recoloured with the rose palette
  `#F9CDD4 #ECA1AC #E27589 #B25B6E #7C3042` (each pixel's brightness picks a colour from dark wine
  to blush) and cut out along its rounded shape. It's the exe, window, taskbar and shortcut icon,
  and the mark in the sidebar. To change it, replace `art/icon-source.webp` and run
  `npm run icon`, then `npm run dist`.
- **Heart loading screen:** at start-up a glass heart fills with rose liquid as the app really loads
  (setup, the list, the first screen of pictures). It beats and glows, little hearts float up, and a
  heart rides the progress bar. It shows for at least 1.5 s, then the app fades in.
  Settings → Look → "Heart loading screen at start-up" turns it off.
- **Settings → Look → Animations:**
  - Always: the default.
  - Follow Windows: no animations when Windows has "Show animations" turned off. This PC has it
    off ("Adjust for best performance").
  - Off.

  The loading screen always animates.

## Round of refinements (20 Oct 2026)

**Her voice:** Appreciate and the coach moan with real recordings by **MagicalMysticVA**
(https://freesound.org/people/magicalmysticva/, CC BY 4.0). They're short clips cut from her "Cute Girl Voice
Reactions", "Cute Anime Girl Voice Sigh", "Cute Hentai Girl Voice Orgasm", "Female Cute/Kawaii Semi Intense
Hentai Moan", "High Pitch Cute Female NSFW Lewd Moan", "Very Soft Low Female Voice Breathy Moan" and "Raspy
Tomboy Voice Female Moan", trimmed, faded and evened out (ffmpeg). The originals are in `art/sounds/src`, the
clips in `art/sounds`. Discover's videos now stream through the app, so they can be jumped and fast-forwarded
(Danbooru's and others' used to come whole through the picture proxy). A finished save bursts hearts and
sparkles; chips and tags ripple and squish when clicked.

**Later the same day:** the Library's sidebar tile shows how much space the vault takes on disk (`vaultSize`)
under its item count. Discover's top (tabs, header, search, chips) stays at the top while you scroll: it
shrinks to the tabs and search going down and opens fully on any scroll up.


**Saving from any browser asks first.** Discover, Videos and Audio → Find all open the same question
(`vaultDiscover.ask`). It asks for the main category, the NSFW level (ten faces) and the stars; Audio asks
for its category only. Suggestions show but nothing is chosen for you:
- categories your items with the same tags are in, plus one from the source's kind (Anime, Real, Furry...),
  never a site's or community's name;
- the level the site's rating hints at, with a dotted ring.

The tags are filled in and the source is shown. Keys work too: 1–0 for the level, Q W E for the stars,
Enter, Esc. Then the button says "Added to vault" at once with a small spinner that becomes a tick. If you've
moved on by the time it lands, "Imported!" shows at the bottom right. The Library's full view shows
"Source: …" under the name.

**Discover.**
- **Sharper tiles:** Danbooru's 720 px pictures, Gelbooru's and e621's samples, Moebooru's samples,
  Philomena's medium size, Reddit's 960 px, and Bing thumbnails at 720 px (same shape).
- **Sharp icons:** the sites' own SVG or 128–512 px icons. Sites with only a 16–32 px favicon get a badge
  drawn in their colour.
- **Catalogue:** Reddit's communities sit in one group that opens when clicked or searched.
- **RedGifs is a Discover source** (moved from Videos and added once):
  - its 60 most-followed niches are chips with their pictures;
  - Top (this week, this month, all time) and Latest;
  - clips play on their tile under the mouse;
  - the post view uses the app's own player.
- **A portrait video now fits the player** (it used to grow to its own height and get cut off).

**Videos.**
- **Eporner and Rule34Video** (new: fan-made 3D and 2D hentai) play in the app's own player and can be
  saved:
  - playing: the file streams through `vault://stream` 4 MB at a time, so seeking works;
  - saving: `downloadStart` writes it to `%LOCALAPPDATA%\Image Vault\downloads` with its progress on the button.
- **Pornhub** only hands its files to its own player, so it stays embedded and can't be saved.
- **Left out:** hanime.tv and MissAV, which stream commercial videos without the rights holders'
  permission. Eporner's "hentai" search was mostly unrelated videos, so that row went too.
- **Long videos (10 minutes or more)** show in Videos only, not in the Library (`libraryOk`).

**Audio → Find has many sources**, Reddit's communities in one tab:
- soundgasm performers (pin them; Reddit brings up more);
- Audio Desires (voice actors' stories; free previews, the whole story on their site);
- Classic erotica (public-domain books read by LibriVox volunteers, through the Internet Archive:
  Fanny Hill, Venus in Furs, The Kama Sutra, Lady Chatterley's Lover...; chapters listed);
- Freesound (sounds, with your free key);
- or any link you paste.

Big files go to disk first (`downloadBlob`).

**Library.**
- **Category:** a big square tile with a 2×2 picture of what's in it; its menu shows every category with
  pictures.
- **Tags panel:** drag its edge (200–620 px, double-click resets) or fold it away; both are remembered.
- **Hover:** a lift, a shadow and a glow.
- **Hold:** the tile grows 1.55× over its neighbours in Rows. Each row's `content-visibility` used to clip it;
  the held row now lets it out.
- **★★★:** pulses pink (a corner mark on great items, the details' rating, the Add page).
- **Full view:** Appreciate's heart sits centred and beats; close, delete and edit moved to the top left.

**You.**
- **Stopwatch:** opens a sessions popup (this session, today, the streak, the last 7 days as bars,
  sessions by day). A 🔥 streak counter sits beside it.
- **Level road:** a Duolingo-style row of stops, with what each level brings: titles, banner colours,
  your own picture at 2, banners from the vault at 5, moving pictures at 8. Earlier choices stay.
- **Your picture:** placed in its circle by dragging and zooming. 30 badges have their own drawn art
  instead of emoji.

**The coach.** Her lines show in her bubble (your PC's voice can read them: "PC voice", off by default).
When a session starts she moans: one of your recordings from Audio → Moans (chosen at random, or the one
you pick under "Start"), or a soft built-in one made in the app; or no sound. (The recorded AI voices were
taken out on 20 Oct.) Her picture: her face down to her cleavage, centred.

**The NSFW faces:** ten original anime-style faces of a grown woman, one per level: sweet, shy,
blushing, flirty, flustered, biting her lip, heart eyes, panting, lewd, ahegao.

**Never shown:** `child`, `young`, `cub`, `foal`, `filly`, `foalcon`, `aged_down`, `underage` and
`preteen` were added to the built-in list.

**Look and motion.**
- chunky buttons that press down; menus with icon tiles; glossy progress bars;
- pages fade up; sidebar tiles float, and their pictures wobble;
- a shine sweeps across primary buttons; chips bob and pop; sheets puff in; errors shake;
- an earned badge does a "tada"; Discover's tiles rise in one after another.

These follow Hover.css, micron and Infinite, plus Animate.css and Magic.css, rewritten here. Nothing
moves when Windows asks for less motion.

**Add → paste links** (beside the drop zone, or paste into it): direct links, RedGifs, Imgur gifv,
Danbooru, e621, Yande.re and Konachan posts, and Eporner and Rule34Video pages. They go through the usual
questions and keep their source.

## Discover: each source its own browser (19 Oct 2026, `ui/discover.js` → LENS)

Every source gets controls made for what it offers. They are kept in one table (`LEVELS`, `VIEWS`, `SITE`
between `// LENS-START` and `// LENS-END`), and every chip's tag was checked on its site (thousands to
millions of posts each).
- **Its own header**: the site's icon and colour (which also tints its tab, its view and its chips), one line
  on what it is, and what to know: "2 tags a search without an account", "All-ages site", "Everything here
  is explicit", "One page a minute without an app id", "Your key".
- **Its own levels**, with the site's own rating names: General/Sensitive/Questionable/Explicit (Danbooru,
  Gelbooru); Safe/Questionable/Explicit (Yande.re, Konachan, e621, and older Gelbooru sites like Rule34, TBIB,
  Xbooru and Hypnohub, which say `safe`, not `general`); Safe/Suggestive/Questionable/Explicit (Derpibooru,
  Furbooru). All-ages and all-explicit sites show that instead of buttons. Bing has SafeSearch
  (Strict/Moderate/Off), and Reddit has Pictures / GIFs & videos (its feeds don't say what's NSFW). Each
  source remembers its levels, view and span.
- **Its own views**: Latest, Hot (Danbooru `order:rank`, e621 `order:hot`), Popular today / this week /
  this month (`age:` on Danbooru, `date:` on e621 and Moebooru, which also has "this year", a time window on
  Philomena), Trending (Philomena's Wilson score over 3 days), Top, Random. Reddit: Hot, New, Rising, Top
  (today to all time). Without a Danbooru account, Hot/Popular/Top/Random use one of the 2 tags (`rating:`
  and `age:` don't), and the page says so.
- **Its own categories**, a rail of chips under the search. Chips in a group replace each other, and
  groups combine (e621: a species, a body, a pairing).
  - Danbooru and its kin: **Trending today**, from the site's own most-searched list
    (`/explore/posts/searches.json`), plus Series and Themes.
  - Konachan: Scenes and Resolution (Full HD+, QHD+, 4K+ through `width:>=`).
  - Hypnohub: Eyes (spirals, rings, empty...), Roles, Themes.
  - e621/e926: Species, Bodies, Pairings, Media.
  - Derpibooru: Characters and Styles.
  - Paheal: Series and Kinds.
  - Yande.re: Themes.
  - Bing: Size, Type, Shape, Time, and Colour as swatches.
- **Reddit is one tab with a community picker**: the button by the search (or clicking the tab again)
  opens every catalogue community by category, a Popular entry, and any `r/name` you type. ★ pins one.
  Under the search: your pinned communities, recent ones, and more like the one you're in. Communities you had
  as separate sources became pinned ones; in Sources, catalogue communities say "Pin". A Reddit tab is added
  once for everyone (remove it in Sources).
- **Tiles say what suits the site** on hover: a Reddit post's title and community; a web picture's site and
  size; a booru post's rating dot, score and size. Wallpaper sites get bigger rows, web results smaller ones.
- **Bookmarks** keep all of it (levels, view, span, chips, community). Older bookmarks still open.
- **Never shown, on every site**: added `child`, `young` and `cub` (e621's word), `foal`/`filly`/`foalcon`
  (Derpibooru's), `aged_down`, `underage` and `preteen` to the built-in list.
- **Profile picture**: it is cut to a circle by a mask as well as rounded corners, so a moving picture stays
  round even when the graphics card draws the video itself. The level badge sits outside the circle.

*Tested 19 Oct 2026 on a test copy: every source tab (18) loaded with its own controls; chips, views, spans
and levels on Danbooru, e621, Konachan (4K+ gave only 3840-wide and larger), Yande.re, Derpibooru, Paheal,
Hypnohub and Bing; Danbooru's third tag explains the limit; Reddit's picker, pins, recent, media filter and
community switching (with the one-a-minute countdown).*

## Small touches (Phase 1 of docs/PLAN.md, 19 Oct 2026)

- **The viewer** has a big round close button at the top right and a bin under it (or press Del). The bin's lid opens and it gulps, then it asks before deleting. Scrolling over the picture no longer scrolls the page behind it, here and in Discover's post view.
- **Category** fields (details, tagging form, Discover's "Save as") have a themed dropdown of your categories. A new one is still typed. NSFW level and rating stay manual.
- **More than 10 tags** fold behind "+N more" / "Show less" (in the details and in Discover).
- **Settings** is wider, and the explanations under each option are larger and easier to read.
- **Hover**: a short soft glow that settles to a faint one.
- **Videos** have a pink see-through badge with the duration, and a LONG mark at 10 minutes or more.
- **Discover** shows "Added" with a spinner as soon as you click Save, then a check once it's in the vault. It fetches nothing while another tab is open. When a site times out on a search (Danbooru's `500 QueryCanceled`), it tries once more after 3 s, then shows a calm note.
- **Decorations** fade in instead of jumping into place (the page's slide-in moved them). The Stats column no longer has the badly cut arm. Click a girl and she bounces with a little heart.

## Share packs (Phase 9 of PLAN.md, a first step; `ui/share.js`, `src-tauri/src/pack.rs`)

- **You → Share → Make a share pack**: pick a collection, a category, your original content, or your 50 most
  appreciated items. Give it a name and save one .zip: the files as they are, a `lewd-vault-pack.json` listing each item's
  name, category, NSFW, rating, tags, collection, source and size, and a **vault card** (a 1200×630 picture: a fan of
  its pictures, its name, what's in it, its top tags, your vault's name and level). Files are streamed from disk, so
  big videos never sit in memory.
- **Open a friend's pack**: see its card, how many items and which categories, then add them all into their own
  categories or one of yours. Tags, scores, collections and sources come along. The unpacked files are removed afterwards.
- Tested by a Rust round trip (`cargo test pack`): export, open, same bytes, same tags and scores, clean-up.
- A shared online space (accounts, a feed, likes) would need a server, sign-ups and age checks, so it isn't part of this.
  Packs can be shared any way you like.

## Coach, discreet names, emergency key, Excel (Phase 8 of PLAN.md)

- **The coach** (the heart-pulse button at the top right; `ui/coach.js`): a small companion you can drag around, shrink
  to her face, or close.
  - **Kegel** follows the Mayo Clinic's guide: squeeze, hold and relax, starting at 3 s each, 10 in a row, 3 sets a day, and working up to 10 s.
    The levels are Gentle, Steady, Strong and Quick flicks. A ring fills while you squeeze and empties while you relax, she counts
    reps and sets with a rest in between, and finished days build a streak. Her safety note: don't practise while peeing,
    stop if it hurts, and ask a doctor if you're unsure.
  - **JOI**: pick a length (5–30 minutes), a pace (gentle, normal or intense) and an ending (you may finish, a ruined one,
    denied, or let fate decide). She sets a rhythm with a beating heart, a soft click and her voice: slow, faster, fast,
    edges ("Stop. Hands off.", then a pause before she lets you carry on), and a ten-second countdown at the end. "I'm close"
    makes her stop you. It runs over anything, the slideshow too.

  Her voice is the PC's own text-to-speech (nothing leaves the PC), and the voice and the click can be turned off.
  Streaks and finished sessions count towards the Pelvic royalty and On the edge badges (`_app/kv/coach.json`).
- **Discreet names** (Settings → Storage; on the welcome screen, ticked by default for a new vault): every file goes
  into one neutral folder, `Media`, named by its number only (`000123_file.webp`). Categories, scores and dates stay
  inside the app, and the category list comes from the index. Switching moves the files once on its own thread with
  a progress bar, and can be undone. A move cut short is harmless, because items are found by their number in any
  folder. Your existing library is never renamed unless you switch it on. Tested: 12 test files there and back.
- **The emergency key** (Settings → Privacy; also shown on the welcome screen): Ctrl+Shift+Space by default, and it can
  be changed by pressing a new combination. It works even when another app is in front: every video, the slideshow,
  the audio player and the coach stop, and **The emergency key shows**:
  - **A screensaver** (the default): soft drifting colours behind a big clock and the date, full screen, like a
    lock screen.
  - **A cat video**: Keyboard Cat from YouTube, looping, with sound; or **Pick a video…** (a file, copied to
    `_app/cover.<ext>`, works offline) or **YouTube link…**. The screensaver shows behind it if it can't load.
  - **Nothing**: the window vanishes, from the taskbar too (the old behaviour). Starting the app again now brings
    it back as well (before, the second start only "unminimized" the hidden window, so it could only be closed from
    Task Manager).

  Clicks and keys don't take the cover away (nobody else can click their way in); press the key again to come back,
  optionally behind a plain "Notes" cover that you click away.
- **Export to Excel** (Settings → Storage): your library as a workbook (`src-tauri/src/excel.rs`). The Library sheet has one row
  per item (number, name, category, kind, NSFW, rating, date added, size, length, collection, tags, views,
  appreciations, minutes looked, last viewed, source, file), with a pink header, filters, a frozen top row, banded rows,
  real dates and sensible widths. A Categories sheet has the totals. `index.csv` and `views.csv` stay plain CSV,
  because the app needs them that way and a .csv can't hold colour.

## You: profile, levels, badges, your time (Phase 7 of PLAN.md, `ui/profile.js`, `ui/lust.js`)

- **The stopwatch** at the top right shows this session's active time. It stops while you're away (no mouse or keys
  for 3 minutes and nothing playing, or the window behind others). Click it for your sessions.
- **Your picture** (top right, with your level on it) opens **You**: a banner (a colour, more come with levels, or
  any picture or moving clip from your vault), your picture (one of yours, still or moving), your vault's name,
  your title (Curious → Admirer → Collector → Connoisseur → ... → Vault deity, unlocked by level), level and XP, stats,
  badges and your sessions.
- **XP** comes from time looking (1 a minute), items (2 each), tagged items, appreciations (3), active days (20),
  slideshow minutes, badges (50) and time in the app. Each level takes 100 · level^1.5 XP.
- **30 badges**: First love, Smitten, Collector, Hoarder, Archivist, Curator, Tag wizard, Organised, Hours in, Marathon,
  **Disciplined** (a session over 5 hours), **Night owl** (a session started after midnight), Early bird, Devoted
  (7 days in a row), Obsessed (30), Slideshow star, Full throttle, Explorer, Film buff, Original, Audiophile, Hypnotised,
  Pelvic royalty and On the edge (with Phase 8's coach), and more. Locked ones show their progress, and a pink
  note pops up when one is earned.
- **Stats → Your time**: minutes per day (30 days), hours per week (12 weeks), a calendar of the last half year
  (darker = more), and your sessions (collapsed). Hover a bar or a day for its value. The log is kept with the
  vault (`_app/kv/daily.json`, `sessions.json`, `profile.json`) and starts now: earlier days weren't recorded.
- **The NSFW faces**: five faces, drawn here as simple vector art (expressions only: calm, blushing, flustered,
  lewd, ahegao), one per two levels. In the details the NSFW number is now a face and ten level buttons. The
  Library's detailed layouts show the face too.
- **Appreciate** is a round heart button that pops with a burst of hearts and makes a sound: a soft built-in
  "mmh" (made by the app, no recording), nothing, or any recording of yours from Audio (Settings → Look →
  Appreciate sound). No voice clip is bundled, since there's none whose licence could be checked.

## The slideshow (Phase 6 of PLAN.md, `ui/slideshow.js`)

- **No zoom by default.** Options → Look → **Motion**: None (still), Slow pan, Hypnotic bounce (slow or fast),
  Breathing, Drifting sideways, or the old Slow zoom. It applies to videos too.
- **Full screen covers the whole monitor**, taskbar included (the window's own full screen; the page's full screen
  left the taskbar showing at the bottom). The bar at the bottom is in the theme, with drawn icons instead of emoji.
- **Pauses when you switch to another window**, and carries on when you come back (Options → Playback).
- **Category chips** at the top: click one to show only it, Shift+click to add or remove one, All for
  everything. This works mid-session; what's queued is replaced at once.
- **A succubus tail** along the bottom, always visible, with a beating heart at its tip showing how far the session has come.
- **A pink ripple** where you click.
- **Earlier slideshows** under the plan: when, how long, how many slides and the level reached, plus this week's
  total (kept with the vault in `_app/kv/showhistory.json`).

## Video player, Videos and Audio (Phase 5 of PLAN.md)

- **The video player** (`ui/player.js`) replaces the browser's controls on every video: the full view, the
  Videos theater and anything new. It has a pink seek bar (with what's loaded, a time bubble and a little picture of the
  moment under the mouse), play, ±10 s, loop A–B, speed, picture fit (fit / fill / actual size, remembered),
  volume and full screen (the whole monitor; Esc leaves). Drag up and down on the left edge for brightness and on
  the right edge for volume (or scroll there). Keys: Space/K, J/L ±10 s, Shift+←/→ ±5 s, ,/. a frame at
  a time, ↑/↓ volume, M, F, [ ]. The controls fade while it plays.
- **Seeking, fixed.** Three causes:
  1. On the desktop, uploaded videos still played through the web version's own streaming player
     (MSE over a seek table). Now everything plays straight from disk.
  2. Uploaded videos were *fragmented* MP4s with no index, so the player had to read the whole file to learn its
     length (8 s+ before the first jump on a 118 MB test file). New uploads are saved as ordinary MP4s, and older ones are
     rewritten in the background while you're away: copied, never re-encoded, then checked (same
     length, seeks) before replacing the original (`replaceItemFile`).
  3. The file server answered "from byte X to the end" with the *whole* rest of the file (hundreds of MB per
     jump). Answers are now 4 MB pieces.
  On the 10-minute test video, jumps went from 0.6–2.6 s (and 8 s+ before the rewrite) to 0.1–0.4 s.
- **Videos** (sidebar):
  - **Yours** shows your library's long videos (10 minutes or more) in streaming-service rows: Continue
    watching, Newest, Most watched, one row per category, and a hero with the clip playing. They open in the full
    view at the spot you stopped (kept in `_app/kv/resume.json`).
  - **Browse sites**:
    - **RedGifs** clips play in the app's own player and save into the vault in one click.
    - **Eporner** and **Pornhub** play in their own embedded players, sandboxed: no popups, no new windows,
      no leaving the app. "Watch later" keeps them.
    - **hanime.tv** has no API, so its button opens the site in your browser.

    Search searches the site. Hovering a card plays a preview. Long ones (10+ min) get a LONG badge. Every title
    and tag goes through the never-show filter. The app sends no referrer with its requests (RedGifs refuses a foreign one).
  - **Watch later** lists what you kept (`_app/kv/watchlater.json`).
- **Audio** (sidebar):
  - **Yours** holds your audio (JOI, hypno, ASMR, erotic stories, moans, music...) in categories. Drop files on
    the page or use "Add audio files". The files are kept in `_app/audio` and the list in `_app/kv/audio.json`.
    Rename, recategorise, or delete (to the Recycle Bin).
  - **Find** lists audio posted to Reddit's audio communities (gonewildaudio, GWASapphic and others) that link to
    soundgasm, through Reddit's public feeds like Discover (no app id needed, one look a minute). Or paste any
    soundgasm link. Listen at once, or save in one click (sorted into JOI / Hypno / ASMR by the title; the
    [bracketed] parts of the title become tags).
  - **The player** sits at the bottom and keeps playing on every page. It has ±15 s, a seek bar, speed, a sleep timer
    (15–60 min) and volume, and media keys work. It remembers where each recording was left. *Tested 19 Oct 2026:
    r/gonewildaudio listed 24 posts; one played and saved (22:13, with its tags).*
- The vault keeps small JSON documents in `_app/kv/` (`kvGet` / `kvSet`), so these travel with the library.

## Discover's sources (Phase 4 of PLAN.md)

- **About a hundred sources** in a catalogue (Discover → Sources → Add more sources), in 16 categories,
  each with its own icon: anime art, hentai and rule 34, real people, cosplay, femboys, trans, men and gay,
  women and yuri, furry, pony, feet, kink and BDSM, hypno, monster girls, art and wallpapers, and the whole web.
  Search the list, click Add. Nothing aimed at anyone under 18 is listed, and the never-show filter covers every
  source: tags on boorus, and words in titles on Reddit and the web (loli, shota, underage, minor, child, ...).
- **New kinds of site**: e621/e926; Philomena (Derpibooru, Furbooru); Shimmie (Rule34 Paheal); older
  Gelbooru (TBIB); plus Danbooru, Gelbooru and Moebooru as before. All checked on 19 Oct 2026: Danbooru,
  Safebooru, Yande.re, Konachan, AIBooru, Xbooru, TBIB, Hypnohub, Paheal, e621, Derpibooru and Furbooru answer.
  Realbooru's API is offline, so it isn't listed.
- **Reddit**: ~90 communities in the catalogue, all in one Reddit tab with a community picker (see "each source
  its own browser" above). Type `r/name` to open any community, or words to search. It works in two ways:
  - **Without an app id** (the default): Reddit's public feeds (RSS), since Reddit's JSON refuses apps
    without one. About 30 pictures and videos a page (pictures, GIFs, Imgur gifv, RedGifs, Reddit videos
    without sound; galleries and text posts aren't in the feeds). Reddit allows **one feed a minute per
    internet connection**, so Discover and Audio share one queue, a minute apart, with a countdown; a page read
    in the last 5 minutes comes from memory, and one you've moved on from is dropped unread. Ratings: posts
    count as explicit, except in sources of the Art category. Posts save into a category named after the
    community, "r-gonewild" for r/gonewild (categories are folders, so they can't hold a slash).
    *Tested 19 Oct 2026: r/gonewild showed 32 posts; full-size pictures, GIFs and saving work.*
  - **With your own app id**: faster pages, galleries (saved as a collection) and sound on Reddit videos,
    and **Log in** for your home feed (the picker's first entry; `web.rs` `reddit_login` opens Reddit's Allow
    page in your browser and comes back to the app). Create the app at reddit.com/prefs/apps → create
    another app → *installed app*, redirect uri `http://127.0.0.1:65010/reddit`. Since November 2025 Reddit
    approves new app ids by request (its Developer Platform form), so the id may only work once Reddit says
    yes; until then it falls back to the public feeds. Paste it in Sources → Reddit. *Untested: no approved
    app id here.*
- **Web images**: the whole web through Bing (Google's and DuckDuckGo's searches no longer let apps in,
  so buttons open them in your browser instead). Filters: size, type (photo, clip art, drawing, GIF,
  transparent), shape, colour, time, as chips; SafeSearch Strict, Moderate or Off.
- **Tabs** show each site's icon. Drag a tab onto another to reorder them. The sidebar's Discover tile
  shows the active source's icon, big and tilted.
- **Bookmarks**: ☆ Bookmark keeps the current source, search, ratings and order as a chip under the search bar.
- **Find the source** (right-click a Library picture): its thumbnail goes to SauceNAO when you've
  added your free key (Sources → Find the source; 200 searches a day), otherwise to IQDB. Matches show with
  their similarity. **Open** opens the page, and **Add its tags** adds the match's artist, characters and series
  (for booru matches, all the post's tags) to your item. Tested with IQDB, which is often busy and
  can take a minute.
- Pictures from search results and Reddit come from anywhere on the web, so the picture proxy now
  fetches from any public address. Addresses on this PC or the local network are refused, and API
  calls still only go to your sources and the services above. Connections try IPv4 first: IQDB's IPv6
  address doesn't answer here, which used to make requests hang.
- Fixed: switching source or search while a page was still loading could show the old results and
  never load the new ones.

## The Library (Phase 3 of PLAN.md, `ui/library.js`)

- **Sharper previews for landscape videos.** Preview clips were capped at 320 px *wide* in every
  orientation, so a landscape clip was 320×180 next to a portrait one's 320×570. Now the *short* side is capped
  at 320 px: portrait clips are exactly as before, and landscape ones get the same number of pixels (568×320). So no clip
  costs more to play than a portrait one did. (A 400 px try made scrolling stutter.) The clip starts 2/5 of the way in (past intros). Older
  clips are remade in the background, landscape first, one at a time while nothing else is busy
  ("Sharpening previews" in the sidebar status). They're read straight from disk in pieces, so long
  videos aren't loaded whole, named `<uid>.c2.mp4`, and the old clip is deleted. Grid thumbnails were
  already sized per tile from 900×1200+ originals, so landscape *pictures* were fine.
- **Smooth scrolling, previews still playing** (19 Oct 2026). The page used to start a video player
  the moment a tile came within 400 px of the screen and destroy it the moment it left, several per frame
  while scrolling. Making a media player is heavy work, and that was the stutter. Now tiles near the screen
  get their clip loaded ahead of time (at most 2 new players a frame, the nearest first), tiles on screen
  play, and players that leave are kept until there are more than (playing + 24), so scrolling back reuses
  them. Nothing over the grid blurs what scrolls under it anymore (the video badges, titlebar and tag
  panel had `backdrop-filter`, redrawn every frame), tiles have `contain: layout style`, and tiles can't
  hover mid-scroll. The clip remake runs only while you're away. The moving background is off behind the
  Library and Discover. Measured on 600 moving previews with real-sized clips (Intel Iris Plus at 3000×2000,
  200%): scrolling holds 57 fps with ~11 playing; the main thread is ~35% busy.
  Findings along the way: Chrome decodes clips under 360 px on the CPU and from 360 px on the GPU, but
  both scrolled the same. The NVIDIA card made no difference ("Use the faster graphics card" in
  Settings → Library, off by default).
- **Look time from previews**: a moving preview playing on screen adds 0.1 s a second to its item.
- **Hold a tile** for a moment: it grows 30% (away from the window's edges). Let go on it to open it,
  or move away to put it back.
- **Right-click a tile**: Open, Open with its program, Show in folder, Edit picture, Copy picture,
  Category ▸, NSFW level ▸, Rating ▸, Collection ▸ (or a new one, or take it out), Mark as original
  content, Delete.
- **Layouts**: Rows, Even grid, Masonry (whole pictures in columns), Big cards with details, and List
  with details. The zoom slider still sets the size.
- **For you** (Sort): a fresh mix each session: about 60% what you love most (views, appreciations, time),
  25% what you haven't opened in 30 days or ever, and 15% anything.
- **Original content**: Type → Original content (newest first). Mark items from the right-click
  menu or with the switch in the details (it's the tag `meta:oc`). Tags like selfie, mirror, trib,
  couple, solo, toy, lingerie, cosplay, outdoor, shower and pov show as stickers on its tiles.
- **Manage tags** (button in the Library's tag panel): every tag with its count. Rename it (into an
  existing name merges them), change its type, or delete it from every item.
- **Edit picture** (the pencil in the full view, or right-click): rotate, flip, skew, brightness,
  contrast, saturation, hue, warmth, soften, vignette, and presets (Soft, Vivid, Warm, Mono, Dreamy, Noir). Saved as a
  new item with the same category, tags and collection. *Replace* also sends the original to the Recycle Bin.
- **Clean up → Find duplicates**: every thumbnail becomes a 64-bit difference hash (src-tauri/src/dupes.rs,
  on all cores, remembered in the local cache). Pictures 5 bits or less apart, of the same shape and family,
  are grouped. A small window (bottom right, can be made small) shows progress. Review shows each group
  with the biggest marked Keep; bin the others per group or all at once.
- **Clean up → Compact videos**: videos stored far above about 0.08 bits per pixel per frame are listed with
  the space they'd free. Each is re-encoded (H.264) at the same resolution with the sound copied, optionally
  brought down to 1080p. The result is used only if it's at least 15% smaller and as long as the
  original. A copy of the original goes to the Recycle Bin first, and the file is replaced in place, so
  views, time and tags stay. Tested: a 20 s 1080p 20 Mbit/s video went from 51 MB to 13 MB.
  WebCodecs' per-frame quantizer mode isn't reachable through Mediabunny, so this targets a bitrate
  instead of a fixed quality.

## Lewd Vault: name, heart, fonts, cursor, Custom theme, sidebar (Phase 2 of PLAN.md)

- **The name** is Lewd Vault: window, titlebar, loading screen, welcome screen, and the exe
  (`dist/Image Vault/Lewd Vault.exe`) with its desktop shortcut "Lewd Vault". The folder keeps its old name, so
  a library kept next to the app ("Image Vault data") and `Image Vault.config.json` stay where they were.
  `npm run dist` removes the old `Image Vault.exe`, and `npm run shortcut` removes the old shortcut.
- **The logo** is the loading screen's heart, drawn by `tools/make-icon.mjs` from the same path (exe, window,
  taskbar). In the sidebar it's live: slow, still water moves inside it.
- **Fonts** (bundled, SIL OFL, from @fontsource-variable): only the name "Lewd Vault" is in *Dancing Script*. Everything
  else is *Nunito* by default. Settings → Look → Font offers Quicksand, Comfortaa, Fredoka, Inter or the
  theme's own.
- **Mouse pointer**: Windows' own by default. A pink heart or a paw (drawn in the theme's colour) can be picked on the
  welcome screen at first start, or in Settings → Look.
- **Settings** explain every option in plain words: what it does and when to change it.
- **Custom theme**: under the theme swatches, pick a main colour and a background (or one of the
  starting backgrounds) and a font. Changing one switches to Custom. The decorations take the theme's
  colour on every theme.
- **Moving background** (Settings → Look): faint hearts and sparkles drift up slowly, with a soft glow now
  and then. It's one small canvas, it pauses while the window is in the background or a picture or the slideshow is open, and it's off with
  Animations off.
- **Sidebar**: drag its right edge to resize it (200–400 px). Double-click the edge (or drag it far left)
  for icons only. When the tiles don't fit, the list scrolls with a faint arrow instead of squashing
  them. The tiles have small animations: Library's cards fan out, Slideshow turns into
  pause, Add and Settings turn, Discover spins, Stats bars grow, and Clean up wiggles.

## Sidebar

The sections are big tiles that fill the sidebar, after Spotify's category cards. Each has a bold
title, a live line under it and a big tilted icon:

- **Library:** a wide tile at the top with the item count, and your three newest pictures tilted
  like album covers.
- **Add** and **Slideshow** side by side; the Slideshow line says when a session is running.
- **Stats** ("8 min looked at") and **Clean up** side by side.
- **Settings:** a wide tile at the bottom.

On Rose the tiles use the palette itself; other themes take their colours from the theme's accent.
The active tile has a white outline. In narrow windows the sidebar becomes a column of icons.

## Rose theme and decorations

- **Rose** is the default theme: a deep rose-grey with the palette's pinks. Any theme you pick in
  Settings is kept.
- **Decorations:** seven of your pictures in rose tints, with their backgrounds cut out:
  - a column of three standing beside the page on Add, Stats and Settings, when the window is wide
    enough (hidden while you tag);
  - one under the slideshow plan;
  - tiny ones peeking over the slideshow plan, the Tags panel, the tagging form and the clean-up bar;
  - two faint ones in the corners of the loading screen.

  They're all decoded before the loading screen closes, so they appear with everything else. On dark
  themes other than Rose they're toned down. Settings → Look → Decorations hides them.
- **Making them:** `npm run decor` remakes them from `art/decor-source/`.
  - It fills each background in from the edges, so whites inside the picture stay.
  - A fake transparency checkerboard drawn into a picture is recognised and removed, including
    enclosed pockets.
  - Pieces not joined to the figure, like a watermark or logo, are dropped.
  - Then it applies the icon's gradient map.

  Pictures that didn't cut out cleanly are left out (listed in `tools/make-decor.mjs`).

## Build it

You need Rust (stable MSVC), the Visual Studio C++ Build Tools and Node.

```
npm install
npm run dist       # builds, then puts it in dist/Image Vault/Image Vault.exe (no installer)
npm run shortcut   # an "Image Vault" shortcut on your desktop, pointing at that exe
npm run icon       # remakes the icon from art/icon-source.webp
npm run decor      # remakes the decorations from art/decor-source/
npm run dev        # runs it with live reload
```

`npm run sync` builds `src/`; `build` and `dev` run it first. It copies `ui/Index.html` and applies
the desktop changes listed in `sync-ui.mjs`. It adds `ui/desktop.css` (the look) and
`ui/desktop.js` (tags, search, desktop settings). It also copies Mediabunny, ThumbHash and the
Inter and Nunito fonts from `node_modules`, so the app works offline.

Edit the shared UI in `ui/Index.html` and the desktop additions in `ui/`, never in `src/`. Every
patch has to match exactly once, so if a change to `Index.html` breaks one, the build stops instead
of shipping a half-patched page.

## How it works

- **Calls:** `api(fn, ...args)` becomes a single Tauri command, `api`. In
  `src-tauri/src/main.rs`, it implements the same calls as Code.gs with the same validation,
  file-name rule (`fileName_`), checksums and return values. It adds `setTags`, `openFile` and
  `openDataFolder`.
- **Reading files:** files come from `http://vault.localhost/<id>`, a custom protocol with `Range`
  support (206) for the video streamer and the thumbnail packs. The full view and the slideshow
  point `<video>` straight at it, so even huge videos never sit in memory.
- **Saving files:** they go to the backend in 8 MB pieces, with real progress. Unfinished pieces
  are cleared at start-up.
- **File ids:**
  - `item-000123`: found through index.csv, so a rename or move keeps the id.
  - `thumbs-` / `clips-` / `streams-` + the base64url file name.
  - `file-` + a base64url path.
  - Drive ids, from a copied Drive folder: resolved through the index rows that name them.
    Drive thumbnail packs are matched to their local `pack-*.bin` by size and by checking that
    every slice is a WebP.
- **No browser cache:** the files are already on disk, so the preloading settings are hidden.
- **The desktop layer's files** (loaded in this order after the shared page, each patching or wrapping the page's own
  functions; `sync-ui.mjs` copies them and adds the few hooks they need):

  | File | What it does |
  | --- | --- |
  | `desktop.js` | Tags, splash, titlebar, tiles, welcome screen, time tracking |
  | `discover.js` | Discover, the source catalogue, Reddit, Find the source |
  | `look.js` | Fonts, pointer, Custom theme, background, sidebar |
  | `library.js` | Clips, smooth scrolling, menus, layouts, tag manager, editor, duplicates, compacting, seekable videos |
  | `player.js` | The video player |
  | `videos.js` | Videos |
  | `audio.js` | Audio |
  | `slideshow.js` | The slideshow |
  | `profile.js` | You, XP, badges, the stopwatch, Stats charts |
  | `lust.js` | Faces and the heart |
  | `privacy.js` | Discreet names, the emergency key (screensaver, cat video), Excel |
  | `drivesync.js` | Google Drive over the web (Settings → Storage) |
  | `coach.js` | The coach |
  | `share.js` | Share packs |
- **The Rust side:**

  | File | What it does |
  | --- | --- |
  | `main.rs` | The calls, the file server, the window, the emergency key |
  | `storage.rs` | Where the vault lives |
  | `gdrive.rs` | Google Drive over the web: browser sign-in and two-way sync |
  | `crypt.rs` | Untraceable mode: the files encrypted, the password |
  | `web.rs` | Discover's requests and picture cache, the Reddit login |
  | `dupes.rs` | Duplicates |
  | `discreet.rs` | Discreet names |
  | `excel.rs` | The Excel export |
  | `pack.rs` | Share packs |

  `cargo test` runs 8 tests (CSV, ranges, names, domains, private addresses, duplicates, Excel, packs).
- **Small documents with the vault:** `_app/kv/*.json` (`kvGet` / `kvSet`) holds resume positions, watch later,
  audio, slideshow history, the activity log, your profile, the coach and the storage choice.
- **Debugging:** `IMAGE_VAULT_DEBUG_PORT=9222` opens WebView2's DevTools protocol on that port; `IMAGE_VAULT_GPU=low|high`
  picks the graphics card; setting `LOCALAPPDATA` to another folder gives a test copy its own browser storage and Discover sources.

## Tested (17 Oct 2026, Windows 10 LTSC 21H2, WebView2 155)

Driven in the release exe over the DevTools protocol.

**Conversion:** every path runs inside WebView2, with no warnings.

| Input | Result |
|---|---|
| JPEG 2400×1600, 1.16 MB | WebP, 558 KB |
| PNG screenshot, 168 KB | lossless WebP, 56 KB |
| static GIF | WebP |
| animated GIF | H.264 MP4 + preview clip |
| H.264+AAC MP4 | kept as is: fragmented MP4 + seek table + clip |
| VP9+Opus WebM | VP9 kept, Opus → AAC, fragmented MP4 |
| H.264 MKV, H.264 MOV | remuxed to fragmented MP4 |
| AV1+Opus MP4 | AV1 kept, Opus → AAC |

**What this WebView2 supports:**

| Feature | Result |
|---|---|
| VideoEncoder / VideoDecoder / AudioEncoder / ImageDecoder / MediaSource | all present |
| H.264 High (1080p), VP9, AV1 encode and decode | yes |
| AAC encode | yes |
| ImageDecoder: GIF, WebP, AVIF | yes |
| MSE `avc1` + `mp4a` | yes |
| HEVC | no, on this machine |

HEVC (iPhone video) needs Microsoft's HEVC Video Extensions. Without them, such a video is saved
unchanged and gets no thumbnail; with them, it is kept as HEVC.

**Features checked:**
- **Add:** several files with different tags through the UI. Tags were saved, including the `c:`
  and `s:` shortcuts.
- **Browse and open:** thumbnails (single files and packs) and moving previews. The full view
  opens images, GIFs, MSE-streamed video with seeking, and video straight from disk.
- **Edit:** category and scores (the file moves and is renamed), tags, collections, views,
  settings, and thumbnail packing. Delete goes to the Recycle Bin.
- **Search:** all the query forms above, the tag panel, and bulk add and remove.
- **Slideshow:** with and without a tag filter.
- **Other tabs:** Stats (with Most used tags), Clean up and Settings (with the data folder) render.
- **Uploads:** a 21 MB file in 3 pieces took 0.48 s, with progress 38 / 76 / 100 %, and its bytes
  were verified.
- **Drive copy:** simulated with Drive item ids and a Drive-id thumbnail pack. All thumbnails show
  and files open.
- **Single instance:** a second start leaves 1 process.

**Measurements:**

| | |
|---|---|
| exe size | 4.3 MB (including bundled libraries and fonts, and the thumbnail resizer) |
| launch → Library ready, 9 items | 1.2–1.8 s |
| launch → Library ready, 3,000 items | 1.8 s |
| 3,000 items: Library first render | 97 ms |
| 3,000 items: a tag search | 17–28 ms |
| 3,000 items: suggestions | 7 ms |
| memory, 3,000 items, Library open (app + 6 WebView2 processes) | ~440 MB private |
| memory, after scrolling all 1,800 visible cards into the page | ~930 MB private |

The web version could not be timed on this machine for comparison: it has no Edge or Chrome, and
no deployed web-app URL or Google sign-in. To compare, open the `/exec` URL in Edge and note when
the Library appears. That time includes Apps Script's `doGet` and the Drive round trips this
version skips.

## Not done yet

- **Window edge resizing:** the window has no Windows frame. Resizing by dragging its edges relies
  on Tauri's built-in support for frameless windows, and I didn't try it by hand (dragging can't be
  scripted here). Maximize and restore were tested; minimize and close go through the same call.

- **Opening files:** "Open" and "Show in folder" call Explorer. They weren't clicked in testing,
  so they wouldn't pop windows open on the test machine.
- **Memory after long scrolling:** memory grows with very long scrolling in the Library. The
  shared page keeps every loaded card in the page; it doesn't unload cards far off screen. Their video
  players are let go past a cap since Phase 3.
- **Untested without accounts:** Reddit with your own app id (faster pages, galleries, your home feed) needs an
  approved id; the public feeds are tested. SauceNAO needs your key. Both are written to their documented APIs
  but untried here.
- **Dialogs:** the save and open dialogs (Excel, share packs) can't be clicked by the test scripts. What they call is
  covered by `cargo test`.
- **Tags on the web:** the web version doesn't have tags.
- **HEVC:** see above.

## Safety for synced vaults (20 Oct 2026)

- **Daily backups.** At each start and before the first change of a day, `index.csv`, `views.csv` and `_app/settings.json` are copied into `_app/backups/<date>/`. The newest 14 days are kept. To undo a bad sync, close the app and copy them back.
- **Conflict copies are merged.** If two PCs change the vault at once and the cloud service keeps both (`index (1).csv`, `index-DESKTOP-4F2.csv`, `views (Sam's conflicted copy …).csv`), the app folds the copy in at start, every 3 minutes and when its window comes forward. Items only the copy lists are added if their file is in the vault, and each view count takes the higher number. If both list an item, this PC's details win. The copy moves to `_app/backups/conflicts`.
- **The web version keeps desktop columns.** `Code.gs` now keeps columns it doesn't use itself (`tags`, `source`) when it saves `index.csv`. Paste the new `Code.gs` into the Apps Script project and deploy it again for the fix to take effect.
- **Thumbnail packs are tidied.** A while after start, a pack where over a quarter (and over 256 KB) is thumbnails of deleted or re-thumbnailed items is written again with only what's used, and a pack nothing uses goes to the Recycle Bin. Packs changed in the last 10 minutes are left alone.
- **Saving is about 11× faster.** Pieces go through a private local connection (`fastlane.rs`: 127.0.0.1, a random port and a 128-bit key known only to the page, the app's origin only) instead of WebView2's channel. Measured: 458 MB/s against 42 MB/s. If the connection can't start or drops, saving falls back to the old way.

## Smoother Library (20 Oct 2026)

Measured on 3,000 items: switching a filter took 40–58 ms before anything showed and the pictures arrived ~100 ms later; now 15–18 ms, with pictures already in memory shown in the first frame. A fast fling went from about 22 to 35 fps.
- Blurred placeholders are made as cards come near the screen, not for a whole batch at once (150 took 65 ms).
- The Tags panel is drawn just after the grid's first frame, tags are split once per item, numbers are formatted with one shared formatter, and the decoration over the panel no longer forces a layout of the new grid.
- During a fast fling the moving previews hold still and no new players start; they pick up as the page slows.

## Bigger, rounder, easier to click (20 Oct 2026)

- **Size of everything** (Settings → Look): the whole window zoomed (Normal, Big, Bigger, Biggest; Big by default), through WebView2's own zoom (`window_action "zoom:1.1"`).
- Thick pill scrollbars in the theme's colour everywhere, and shown on the sideways rows (Discover's sources and categories, video strips, the level road).
- Chunky buttons, fields, chips and sliders with thick outlines; the sidebar tiles have a solid ledge underneath, lift on hover and sink in when pressed (after Duolingo). Thicker icon strokes, bolder rounder text. The jelly squeeze on clicking a tile is gone.
- The player's loop icon now turns looping of the whole video on and off (R); looping a part has its own A–B button (Shift+R).
- Erome: Hot/New also for searches, chips for type (videos, pictures only, big albums), popular topics, bodies and looks; an album's own tags; a strip of all its pictures and videos in the viewer. Saving an album makes one collection, each file with its own type.

## Your own sites, and every browser customisable (20 Oct 2026)

- **Build your own site** (`ui/sites.js`) in Discover (Sources → ✨ Build your own site), Videos (＋ Your own site) and Audio (＋ Your own site). A site is a recipe: its addresses (front page, search, a category) with `{query}` / `{query+}` / `{query-}`, `{page}`, `{sort}`, `{category}` in them and `[ … ]` for what only later pages have; its orders and categories as `Label = value` lines; and only when the guesses are wrong, CSS selectors for the items, their link, picture, title and length, and for the file and the tags (or `re:` a regular expression for the file). Items are found by themselves (links with a picture that repeat the same way, kept apart by the shape of their addresses), the file too (`<video>`, `<audio>`, og:video, an .mp4 / .mp3 address in the page), and the tags (the page's keywords and links to tag pages outside its menus). **Try it** reads the real site and shows the items, the first file (playable) and its tags before saving; **Find the items for me** fills "Each item"; recipes can be copied and pasted as text. Random picks a random page among the first few dozen. Streams in pieces (HLS, .m3u8) can't be played.
- They're kept with Discover's sources (`%LOCALAPPDATA%\Image Vault\sources.json`: Discover's in `sources` as kind `recipe`, Videos' and Audio's in `sites`), and only their addresses may be read (`web.rs learn`; `webAllow` while one is being tried).
- **⚙ Customise** in Discover (any source: your own chip groups `# Group` / `Label = words`, the site's own chips on or off, the header on or off, picture size; "Back to how it was"), Videos (sites shown or hidden and in what order, your own front-page rows `Title = words | sort:… | category:…`, the site's own rows and the big video on or off, card width) and Audio (sources shown or hidden, their order).
- Fullscreen from the Videos theater now covers the sidebar too.

## Screenshots

`node tools/screenshots.mjs <folder with the 5 demo pictures>` retakes every picture in `docs/screenshots/` from the
real page and the real Rust backend: it starts the backend as a small local HTTP server over a fresh demo vault
(`main.rs` test `screenshot_server`, opt-in with `--ignored`), opens the page in Chromium with a stand-in for
Tauri's bridge, adds the pictures through the Add screen, looks at a few, then visits each screen. The demo pictures
aren't in the repository.
