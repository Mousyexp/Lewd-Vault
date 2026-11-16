# Lewd Vault: the guide

Everything the app does, how your files are kept, and how the code is put together.
The short version is in the [README](../README.md).

- [Where your vault lives](#where-your-vault-lives)
- [Adding and tagging](#adding-and-tagging)
- [The Library](#the-library)
- [The Browser](#the-browser)
- [Slideshow, Videos and Audio](#slideshow-videos-and-audio)
- [Privacy](#privacy)
- [You, Stats and the coach](#you-stats-and-the-coach)
- [Backups, health and updates](#backups-health-and-updates)
- [iPhone and iPad](#iphone-and-ipad)
- [The vault folder](#the-vault-folder)
- [Building it](#building-it)
- [How the code is organised](#how-the-code-is-organised)
- [Credits](#credits)

## Where your vault lives

On first start the app asks where your library should live. Change it later in
**Settings → Storage → Change where it lives…**; your library can be copied to the new place.

| Choice | What it means |
| --- | --- |
| **This PC** | A plain folder: next to the app (portable) or `Documents\Image Vault data` (installed). |
| **Google Drive, OneDrive, Dropbox, iCloud Drive** | The app makes its own folder, `Image Vault Desktop`, inside that service's synced folder; the service's own app uploads it. Everything still opens from your disk, so it is fast and works offline. |
| **Google Drive in your browser** | For PCs where Google Drive's app can't be installed (it needs an administrator). The vault lives on this PC, or Untraceable, and the app syncs it with Drive itself. See below. |
| **Untraceable** | The vault encrypted in a hidden folder, with nothing left anywhere else on the PC. See [Privacy](#privacy). |
| **Another folder…** | A USB stick, a NAS, any folder. |

Use the vault on one computer at a time. If two change it at once and a cloud service keeps both
copies of `index.csv` (`index (1).csv`), the app merges the copy back in by itself.

### Google Drive in your browser

**Settings → Storage → Google Drive over the web**, or the welcome screen's card.

1. **A Google key.** Google only lets an app into Drive with an "OAuth client". A release build can
   carry one (the repository secrets `LEWD_VAULT_GOOGLE_CLIENT_ID` and
   `LEWD_VAULT_GOOGLE_CLIENT_SECRET`). Otherwise press **Add your key…** and make your own, free:
   1. Open [console.cloud.google.com](https://console.cloud.google.com/apis/credentials) and make a project.
   2. **APIs & Services → Library → Google Drive API → Enable.**
   3. **Google Auth Platform**: any app name, audience **External**. Add your Google address as a
      test user, or press **Publish app** (in "Testing", Google signs you out after 7 days).
   4. **Clients → Create client → Desktop app.** Paste its Client ID and secret into the app.
2. **Sign in with Google.** Your browser opens Google's own page. The app can only reach the files
   it made itself (the `drive.file` scope), never the rest of your Drive.
3. The vault then syncs both ways with a **Lewd Vault** folder in your Drive: on sign-in, every
   10 minutes, and on **Sync now**.

Safety: a file is deleted on one side only if it was synced before and hasn't changed since, and it
goes to the Recycle Bin or Drive's trash. Nothing is deleted on a first sync, and a sync that would
delete more than half of either side stops. If both sides changed a file, this PC's copy wins and
Drive's is kept beside it as `<name> (from Drive)`. Use the same key on every PC.

## Adding and tagging

Drop files on **Add** (or paste links, one per line). Each one gets a category, an NSFW level
(1–10), a rating (1–3 stars), and as many tags as you like. Pictures are stored as WebP; videos keep
their codec when it plays everywhere, and get a short preview clip.

Tags are booru-style: `blue_hair`, and typed ones with a prefix, `artist:`, `character:`,
`series:`, `meta:` (or `a:`, `c:`, `s:`, `m:`). The app suggests tags you used before and the popular
ones in the chosen category.

## The Library

- **Search** by tags: `blue_hair -sketch ~cat ~dog artist:*` (all of, none of, any of, wildcard).
- **Filters**: category, type, NSFW up to, rating, sort (including a "For you" order), and five
  layouts (rows, even grid, masonry, big cards, list).
- **The Tags panel** lists the tags of what is shown; `+` and `−` add or exclude one.
- **Many at once**: Ctrl/Shift-click to select, then re-rate, re-tag, move or bin them together.
- **Right-click** a tile for everything else: edit the picture, change category or level, find the
  source (SauceNAO, IQDB), collections.
- **Clean up** finds what you never open, exact duplicates and look-alikes, and can compact videos.

## The Browser

Browse boorus, Reddit, and video and audio sites in tabs inside the app, and save with one click:
the post's tags and source come along. **Sources** adds sites (most Danbooru-, Gelbooru-, Moebooru-,
e621-, Philomena- and Shimmie-style sites work) and holds the keys some of them need. A filter list
that can't be turned off keeps out content involving minors on every source.

## Slideshow, Videos and Audio

- **Slideshow**: timed stages that climb through your NSFW levels (how long each lasts, how fast the
  slides go, what happens at the end), with motion effects, sound and true full screen.
- **Videos**: long videos get their own section, with Continue watching, your own sites and
  Watch later. The player has A–B loop, speed, frame stepping and keyboard shortcuts.
- **Audio**: your own files and audio sites (JOI, hypno, ASMR, stories), with a queue.

## Privacy

- **Discreet names** (Settings → Storage): every file in one neutral folder, `Media`, named by number
  only. Categories, scores and dates stay inside the app.
- **PIN lock**: covers the app on start, after you've been away, and on Ctrl+L. It is a screen, not
  encryption.
- **The emergency key** (Ctrl+Shift+Space by default, works from any app): everything pauses and the
  app becomes a screensaver (a clock on soft colours), a cat video (or a video you pick), or
  disappears from the screen and taskbar. Press it again to come back.
- **Untraceable**: for a PC that isn't only yours. The vault lives in a hidden `data.temp` folder
  (next to the portable app, in Documents for the installed one, or a place you pick) and **every
  file in it is encrypted** with a key made from your password. Caches, unfinished uploads and the
  built-in browser's own data go in there too, deleted items go to the vault's own `_trash`, and the
  window is called "Notes" on the taskbar. The password is asked on a plain screen at every start;
  **if you forget it, nothing can bring the vault back.**

  What it can't hide: that a program ran (Windows remembers program names, so the renamed portable
  app leaves the least behind), the size of `data.temp`, and anything monitoring software records. It
  hides the files from someone looking at the disk; it is not protection from your employer's IT.
  The encryption is ChaCha20 with a PBKDF2-SHA256 key (`src-tauri/src/crypt.rs`); it keeps files
  unreadable, it does not detect tampering.

## You, Stats and the coach

**You** is your profile: XP, levels with unlocks, badges, a streak and your time. **Stats** charts
your viewing. **Share packs** bundle a collection into one `.zip` with a preview card, and open
packs from friends. **The coach** is a small companion for Kegel workouts and JOI sessions.

## Backups, health and updates

In Settings: **Back up** copies the vault to a drive or folder (after the first time, only what
changed), **Health check** finds missing files and broken previews and fixes them, and **Updates**
installs a newer release in one click. The app also keeps 14 days of `index.csv`, `views.csv` and
settings in `_app/backups`. **Export to Excel** writes the library as a spreadsheet.

## Fun features

Appreciating (the ♥ button and its counts), star ratings and NSFW levels are the "fun features". The
welcome screen asks about them once, on a page of its own with a moving preview of each; keeping them
is recommended. Without them the app is plainer: no hearts, stars or levels anywhere, a slideshow
that simply plays, and new things saved with neutral values (level 5, two stars) so the vault is
the same either way. **Settings → Look → Fun features** turns them on or off at any time.

## iPhone, iPad and Android

The same app runs on iOS 26 as a sideloaded IPA; see the [README](../README.md#-on-iphone--ipad-sideloaded)
for installing it and [mobile/README.md](../mobile/README.md) for how it is built. A vault is the
same folder on both: put one in the app's **Documents** folder with the Files app.

Android phones get the same phone app as an APK (**Actions → Build Android**, or the release's
`Lewd.Vault.Android.apk`), with the system back button closing whatever is on top. There the vault
lives in the app's private storage; bring one over with Google Drive over the web (Settings →
Storage), which works on Android.

On a phone the app is laid out like Photos:

- **Library**: an edge-to-edge grid, the title over it, **Select**, and a switcher (All / Pictures /
  GIFs / Videos) above the floating tab bar. Search, filters and tags are sheets from the bottom.
- **Long-press a picture** and it lifts into a preview with its actions under it: stars, the NSFW
  level and categories as strips you tap.
- **The viewer**: the picture full screen with a glass toolbar (appreciate, details, edit, delete).
  Tap to hide the controls, double-tap to appreciate, swipe sideways for the next one, up for the
  details, down to close. The slideshow takes the same swipes and double-tap.
- **Hide now** (in More, or tap anywhere with three fingers) is the emergency key. It shows a lock
  screen, a cat video, or a working calculator (Settings → Privacy). To come back: swipe up on the
  lock screen and enter your PIN, or type your PIN into the calculator and press `=`. With no PIN
  set, hold the clock (or the calculator's display) for a second.
- **Stories**: rings over the Library (New, Favourites, then your biggest categories). Each plays full
  screen, one after another: tap for the next, hold to pause, swipe sideways for another ring, up to
  open it, down to close.
- **Sort** (the first ring, or More → Sort): the pictures you haven't sorted yet, as a stack of cards.
  Swipe right for 2 stars, left for 1, up for a favourite (3 stars and a heart); Undo takes one back.
  What's sorted is kept in the vault (`_app/kv/phone-sort.json`), so every device agrees.
- **The level dial**: tap the level in the viewer's toolbar (or in Stories and Sort) and slide your
  thumb along the half circle; it saves when you let go.
- **The Browser** works like Safari: the address bar sits at the bottom (back, the site and your
  search, reload, tabs, the page menu) and shrinks to a small pill while you scroll. Tap the address
  for the search page (favourite sites, bookmarks, recent searches, the site's tag suggestions), the
  tabs button for every source as a card, and pull down to reload. Swipe from the left edge to go
  back. A post opens large with its details underneath: swipe for the next, down to close.
  **⋯ → Customise the Browser**: the bar at the bottom or the top; pictures whole, 2 or 3 across, or
  as a feed; filters in a sheet or at the top; hide the bars while scrolling; colour the bar like the
  site; edge swipes; pull to reload; blur the previews.
- **The first start** opens on an animated welcome (the heart fills, the name rises), then asks about
  the **fun features**.
- **The grid**: pinch it for 2, 3, 5 or 7 across. After Select, slide a finger across pictures to
  pick a run of them, and a handle on the right edge flings through the whole library, showing the
  month. Over the grid: smart albums (Never opened, Forgotten, Long videos, Most loved, 1 star) and
  your own saved filters (+ Save filters; hold one to remove it). A long-press preview can be
  **pinned** to float over the Library while you scroll.
- **The viewer** uses the whole screen, with the picture's colours glowing behind it. Pinch or
  double-tap to zoom, then pan (it glides). The filmstrip works like Photos: slide it and the picture
  follows. On videos: drag sideways in the lower half to scrub, double-tap a side to skip 10 seconds,
  hold for 2×, and the little-window button keeps a video playing while you browse. Turn the phone
  for landscape.
- **The level dial** also sets the level when adding, for a whole selection at once, and caps the
  slideshow (the round button bottom left, live). New uploads come as cards: swipe right to save,
  left to skip.
- **Sharing to the app**: share pictures or videos from Photos, Safari or any app to Lewd Vault
  (or "Open in"); they're waiting in Add next time you open it.
- **Settings → Phone** (also in More) gathers the phone's own settings: animated themes (Sakura,
  Midnight, Bubblegum, Aurora, Kitten, Neon, Lavender, Mono) and your own accent; seasonal touches and
  milestone celebrations; haptics; text size and density; the tab bar (drag to choose which four sit
  in it); what double-tap, two-finger tap and long-press do in the viewer; Face ID / fingerprint
  unlock; a decoy PIN that opens an empty vault; locking again after leaving the app; shake to hide;
  hiding from screen recordings (Android also blocks screenshots); a break-in photo after three wrong
  PINs; the app icon (the heart in other colours, or a disguise: Calculator, Notes, Weather, Clock,
  Files, Compass; on Android the name changes too); a reminder to delete what you added from Photos;
  and low-data mode.
- The app covers itself while it is in the background, so the app switcher shows nothing.

Not on the phones: the global emergency shortcut, opening files in other apps, cloud-folder choices,
Google Drive's browser sign-in (iOS only; Android has it), and in-app updates (install a newer IPA
or APK instead).

## The vault folder

A vault is a plain folder you can read without the app:

```
Image Vault data/
  index.csv            one row per item: id, file, category, kind, levels, tags, source...
  views.csv            views, appreciations and time per item
  <Category>/          the files: 000123_261016_i073_<Category>.webp
  Media/               ...or all of them here, named by number (Discreet names)
  _app/
    thumbs/ clips/ streams/ audio/     thumbnails (packed), preview clips, seek tables, audio
    settings.json, state.json, kv/     settings, the next item number, small documents
    backups/                           daily copies of the two lists and the settings
```

Both lists are UTF-8 CSV with a BOM and CRLF line ends. The format is the same on Windows and iOS,
and tests hold it byte for byte (`src-tauri/tests/fixtures`). In an Untraceable vault each file
starts with `LVX1`, then a 12-byte nonce, then the encrypted bytes.

## Building it

You need Node.js, Rust and the [Tauri prerequisites](https://tauri.app/start/prerequisites/).

```bash
npm install
npm run dev          # run it with live reload
npm run dist         # build, into dist/Image Vault/Lewd Vault.exe
npm run installer    # build the .msi
npm test             # checks the page builds and hangs together
cd src-tauri && cargo test    # the backend's tests
```

A release is a version tag: bump the version in `package.json`, `src-tauri/Cargo.toml` and
`src-tauri/tauri.conf.json`, then push a tag such as `v1.2.0` (or create the release on GitHub and
run **Actions → Build → Run workflow** with the tag). The workflow builds the installer and the
portable zip and attaches them. The iOS IPA is built by **Actions → Build iOS**, the Android APK by **Actions → Build Android**
(both take the release's tag to attach to it).

`node tools/screenshots.mjs <folder with 5 pictures>` retakes the README's screenshots from the
real page and the real backend (an opt-in test, `screenshot_server`, serves the backend locally).

## How the code is organised

- **`ui/`** is the page. `Index.html` is the shared core (it began as a Google Apps Script web
  app); the other files are the desktop layer, each adding one area:

  | File | Area |
  | --- | --- |
  | `desktop.js`, `desktop.css` | tags, search, the sidebar, the welcome screen, the desktop look |
  | `library.js`, `extras.js` | the Library: layouts, menus, editor, duplicates, selection, PIN, maintenance |
  | `discover.js`, `sites.js` | the Browser and its sources |
  | `player.js`, `videos.js`, `audio.js`, `slideshow.js` | playing things |
  | `profile.js`, `lust.js`, `coach.js`, `share.js` | You, levels, the coach, share packs |
  | `privacy.js`, `drivesync.js`, `look.js` | discreet names, the emergency key, Drive sync, themes |
- **`mobile/ui/`** is the phone layer for iOS and Android (`phone.js`, `phone.css`), applied only
  under `html.phone`.
- **`sync-ui.mjs`** builds `src/` from those: it patches `Index.html` for the app (every patch must
  match exactly once, or the build stops) and copies the fonts and libraries so the app works
  offline. Never edit `src/`.
- **`src-tauri/src/`** is the Rust backend. The page calls it through one command,
  `api(name, ...args)`, and reads files from the `vault` protocol (`http://vault.localhost/<id>`,
  with byte ranges for video).

  | File | Area |
  | --- | --- |
  | `lib.rs` | the calls, the file server, the window, the emergency key |
  | `storage.rs` | where the vault lives |
  | `crypt.rs` | Untraceable: encryption and the password |
  | `gdrive.rs` | Google Drive in your browser |
  | `web.rs` | the Browser's requests and cache |
  | `fastlane.rs`, `media.rs` | fast local uploads; the phones' media server |
  | `discreet.rs`, `dupes.rs`, `excel.rs`, `pack.rs`, `maint.rs` | discreet names, duplicates, Excel, share packs, backup/health/updates |

## Credits

- Voice clips: **MagicalMysticVA** on [Freesound](https://freesound.org/people/magicalmysticva/),
  CC BY 4.0, cut into short clips.
- Fonts: Inter, Nunito, Dancing Script, Quicksand, Comfortaa and Fredoka (SIL Open Font License).
- Libraries: [Tauri](https://tauri.app), [Mediabunny](https://mediabunny.dev), ThumbHash.
- Artwork in the app and the screenshots is used with permission of its creators and is not covered
  by the code's licence.
