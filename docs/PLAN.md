# Lewd Vault: the next big round, the plan

This plan covers the long request from 18 Oct 2026. Nothing in it is built yet. It is split into
phases so each one can ship, be tried and be installed before the next starts. Work on the app's
own speed first, then the look, then each new section. Every phase ends with `npm run dist`, the
CDP screen check, and an update to README.md.

**Rules that hold for every phase**
- Only files under `desktop/` change. Top-level Code.gs, Index.html, README.md and NOTES.md stay as
  they are.
- The NEVER filter (loli, lolicon, shota, shotacon, toddlercon, toddler, child_on_child) covers every
  source: boorus, Reddit, image search, video and audio sites, and reverse search results. Every
  drawn character (cursors, emoji girls, decorations, the profile art) is a clear adult.
- The app never types passwords or keys. The user pastes their own API keys and app credentials.
- Before installing, check that the user's own running "Image Vault.exe" is closed. Never close it.
- Keep it fast: no new framework. Plain JS modules next to desktop.js and discover.js. Anything slow
  shows the round heart spinner.

---

## Research findings (Oct 2026)

| Topic | What was found | What it means for us |
|---|---|---|
| Reddit API | The free tier is for non-commercial use, at 100 queries per minute with OAuth. New app registration at reddit.com/prefs/apps may need manual approval. | The user registers a "script" or "installed app" and pastes the client id. OAuth runs in a loopback window (http://127.0.0.1:port). Without a login, use the public `.json` listings (`/r/x/hot.json`), rate-limited and without the home feed. |
| Google Images | The Custom Search JSON API is closed to new customers and ends on 1 Jan 2027. | There is no stable Google Images API. Offer "Open in Google Images" (a lens/search link in the system browser) and a reverse-search link instead. In-app image search uses DuckDuckGo plus Bing (see below). |
| DuckDuckGo images | Unofficial: get a `vqd` token from the search page, then call `i.js?q=..&vqd=..&f=size:,type:,layout:&p=1` (with safe search off). It can return a CAPTCHA or 403 when used too fast. | Works through the Rust proxy at about 1 request every 2 s, with a clear "DuckDuckGo is resting, try again in a minute" note. Options: size, type (photo, gif, clipart), layout, colour, time. |
| Reverse image search | SauceNAO has a free API key (8 searches per 30 s, 200 per day) with JSON output and booru links. IQDB has no API but simple HTML. | Right-click → "Find the source" sends the thumbnail to SauceNAO (with the user's key) and falls back to IQDB (parsed HTML). Results open in Discover with their tags. Buttons also open Google Lens, Yandex and TinEye in the system browser. |
| More boorus | Imgbrd-Grabber's source list confirms the API shapes: Danbooru, Gelbooru 0.2 (dapi), Moebooru, Shimmie, e621/e926, Sankaku, Philomena (Derpibooru), Szurubooru. e621 needs a descriptive User-Agent and about 1–2 requests per second. | Add the `e621`, `philomena`, `shimmie` and `szurubooru` adapters to KINDS. With the existing four kinds that covers hundreds of sites (most Gelbooru/Moebooru clones). Ship a catalogue of about 150 presets (see Phase 4). |
| RedGifs | v2 API: `GET /v2/auth/temporary` gives a token, then `/v2/gifs/search?search_text=&order=` | A video source, without a login. |
| Eporner | An official JSON API: `https://www.eporner.com/api/v2/video/search/?query=&per_page=&thumbsize=big&order=` with length, embed and thumbs. | A long-form video source with a real API. |
| Pornhub / hanime.tv | Pornhub had a public "webmasters" search API (`/webmasters/search?search=`, thumbnails, duration, embed). hanime.tv has no public API (its internal API changes and is behind Cloudflare). | Pornhub: search plus thumbnails plus play through its embed player in a sandboxed iframe with the adblock list. hanime: a "link" source only (it opens in the system browser), unless its search endpoint answers through the proxy. Mark it experimental. |
| Video re-encode | WebCodecs `VideoEncoder` supports `bitrateMode: "quantizer"` with a per-frame `avc: { quantizer: 0–51 }` (Chrome/Edge ≥ 117; WebView2 155 has it). HEVC/AV1 encode depends on the GPU. | A CRF-like re-encode at the same resolution: H.264 High at quantizer ≈ 26–28 for phone video, or AV1 when `VideoEncoder.isConfigSupported` says yes. Mediabunny (already vendored) does the demux, encode and mux. Audio is copied without change (AAC passthrough). |
| Google Photos "Storage saver" | It shrinks anything above 1080p and re-encodes lossy, saving about 50–70%. | Our "Compact videos" keeps the resolution (as asked) and saves space through the quantizer. Expect 30–60% on iPhone HEVC/H.264 with a high bitrate. An option offers "≤1080p like Google Photos" for more savings. |
| Duplicates | The Rust `image_hasher` crate does dHash/pHash. Near duplicates have a Hamming distance ≤ 6–10. | Hash the cached grid thumbnails (already 1 file per item), and keep the hashes in `_app/hashes.json`. For videos, hash the frame at 2/5. Compare with BK-tree buckets, which stays fast for 50k items. |
| Kegels (Mayo Clinic) | Hold 3 s, relax 3 s, 10 times, at least 3 sets a day. Work up to 10 s hold and 10 s relax. | The coach's levels: Beginner 3/3, Mid 5/5, Advanced 10/10, plus "flicks" (1 s pulses) and an edging/JOI rhythm mode. |
| Global hotkey | `tauri-plugin-global-shortcut` works across the system. | The emergency key works even when another app is focused. |

---

## Phase 1: quick fixes and the small things (1 session): DONE 19 Oct 2026

1. **The fullscreen close button** is round, 44 px, top right, with a darker ring that is easy to hit.
   **Delete** becomes a large bin button next to it with a lid-open → drop → close animation.
2. **No page scroll behind a fullscreen image** in Discover (`overflow: hidden` on main while `.dc-view` is open).
3. **The decoration girls stop "flicking"** into place: they render only once their `decorReady`
   image has decoded (`img.decode()`), with the rail positions computed before the first paint (no transition on first show).
4. **The middle girl's arm in the Stats column**: re-cut or swap that picture in `tools/make-decor.mjs`.
5. **Tags beyond 10** collapse into "+N more" with expand/collapse (detail, Discover side, tag box).
6. **Settings** become wider (max 1100 px, two columns where it fits). Option subtitles grow from
   12 px to 13.5 px with better contrast.
7. **A faint short hover shadow** animation on tiles (120 ms, fades in and out).
8. **A new video indicator**: a pink translucent pill with a play triangle and the duration, plus a
   **long-form badge** for videos of 10 minutes or more.
9. **Browser Save** shows the "Added" tag at once with a round spinner, then a check when the upload finishes.
10. **Discover pauses** when its tab isn't selected: no infinite-scroll fetches, no prefetch, and
    stale-request handling. A Danbooru `500 QueryCanceled` (search timeout) retries once after 3 s
    with the result count off, then shows a calm note instead of a red error.
11. **Main category, NSFW level and rating stay manual.** The main category field becomes a combo box: a
    dropdown of existing categories plus "New…".
12. **Decoration girls bounce** when clicked (a spring keyframe, with a short pink sparkle).

## Phase 2: name, logo, fonts, cursor, themes (1 session): DONE 19 Oct 2026

1. **The name is "Lewd Vault"**: window title, titlebar, splash, welcome, shortcut, exe (`productName`),
   README. Keep the identifier and data paths (`Image Vault` folders) so nothing breaks, and add a
   migration note. Discreet storage (below) keeps the folder names neutral.
2. **The logo is the splash heart** (the same SVG path) for the app icon, sidebar and titlebar: `tools/make-icon.mjs` draws it from the SVG.
3. **Fonts**: titles use a cursive face (vendored, OFL: *Pacifico* or *Dancing Script*). The body
   moves to a round font: *Nunito* (already vendored) becomes the default, and Inter stays as an option.
4. **A custom cursor** setting with about 10 cursors as 32 px PNGs, hotspot set, in `art/cursors/`. The default is a pink heart. Others:
   heart outline, lips, paw, wand, crescent, lollipop-free candy heart, cat ears, a tail arrow, and an
   adult anime girl peeking over the cursor (a clearly adult, NSFW-tier variant with a bare chest,
   drawn by us as simple vector art). "System cursor" stays available.
5. **A custom theme builder**: pick the main and secondary colours (colour inputs) and a font from the list,
   with a live preview. It is saved as `theme: "custom"` plus `customTheme {accent, accent2, font}` in settings.
   The decoration girls follow the theme: their pink is recoloured in CSS with
   `filter: hue-rotate()` computed from the accent hue, which is cheaper than re-cutting the webps.
6. **Animated theme backgrounds**: a fixed canvas behind main with very faint slow-drifting icons
   (hearts, stars, petals) and an occasional soft glow, at 30 fps, paused when the window is blurred or the
   Animations setting is off. A toggle sits in Settings.
7. **Sidebar micro-animations**: Library shuffles its cards on hover/click, Slideshow morphs between pause and play,
   Add's plus rotates, Stats bars grow. They are CSS-only, and "Animations off" turns them off.
8. **A resizable, compactable sidebar**: drag its edge (between 72 px icon-only and 320 px). Double-click
   switches to compact mode (icons only), and the width is remembered. When the tiles don't fit, the sidebar **scrolls** with a
   faint fade and an arrow at the bottom/top edge. The buttons keep their size and don't shrink.

## Phase 3: the Library (1–2 sessions): DONE 19 Oct 2026 (see README; the editor saves a new item instead of keeping _app/originals)

1. **Previews start at the 2/5 point** (seek before play) and use **higher-res previews for landscape videos**
   (clip width rises from 320 to 640 when aspect > 1.2). The clip generator honours this. Old clips regenerate
   lazily, one at a time.
2. **Look time for previews**: each item whose preview is on screen and playing gains 0.1 s per second
   (IntersectionObserver plus the existing pendingTime flush).
3. **Hold to enlarge**: holding the left mouse for 250 ms scales the tile by 1.3× in place (above the others).
   Releasing over it opens it, and moving away cancels.
4. **Right-click menus**: Open, Open fullscreen, Slideshow from here, Find the source (reverse search),
   Edit image, Tags…, Move to category ▸, NSFW level ▸, Add to collection ▸, Show in folder, Copy
   file, Bin. On several selected items, it acts on all of them.
5. **More layouts**: Justified (now), Square grid, Masonry (Pinterest-like), Big cards with info, List
   (table with sortable columns), Film strip. There is also a density slider.
6. **The library algorithm "For you"** (a new sort): a weighted shuffle that mixes 60% favourites/most viewed,
   25% unseen or not seen in 30+ days, and 15% random. It is reseeded per session so it feels fresh.
7. **An "Original content" subsection**: a library filter for items marked `oc` (a toggle in detail), sorted
   by date added, with badge tags (selfie, trib, couple, toy…) shown as pink stickers.
8. **A Tag manager panel**: all tags with counts, search, rename (merge if the target exists), delete across
   the library, and change type (artist/character/…). One `setTags` batch call.
9. **The image editor**: a floating pencil button in detail opens an edit layer with rotate 90°, flip,
   crop, skew X/Y, brightness, contrast, saturation, hue, warmth, blur and vignette sliders, and presets
   (Soft, Vivid, Mono, Warm). A canvas preview is saved as a new version (it keeps the original in `_app/originals`,
   with Undo).
10. **Duplicates**: "Find duplicates" in Clean up opens a small minimizable progress window (bottom right,
    like a download tray). Rust hashes the thumbnails in the background with `image_hasher`. The results are groups,
    keep the best (largest), and bin the rest with one click.
11. **Re-encode videos ("Compact videos")** in Clean up: a list of videos with an estimated saving
    (bitrate per pixel per second over a threshold catches iPhone files). It runs one by one in the same tray, using WebCodecs
    quantizer mode at the same resolution (or ≤1080p optional). The re-encoded file is kept only if it is at least 15%
    smaller, and the original goes to the Recycle Bin after a check (same duration, plays).

## Phase 4: Discover grows into a source browser (2 sessions): DONE 19 Oct 2026 (DuckDuckGo turned out to block apps: Bing instead; Reddit untested without an app id)

1. **A source catalogue** of about 150 presets in `ui/sources-catalog.js`, grouped into source categories with
   icons: Anime/hentai boorus, Furry, Realistic/3D, Cosplay, Femboy/trap (a small cute stylised
   icon), Gay, Lesbian/yuri, Feet, Monster girls, Pony, SFW art, Wallpapers, Video, Audio. It is searchable,
   and adding is one click. The sites come from the Imgbrd-Grabber list and public booru directories. Each is
   tested before shipping, and ones that fail or allow minors are dropped.
2. **New adapters**: e621/e926, Philomena, Shimmie2, Szurubooru, Reddit, DuckDuckGo images, Bing
   images, RedGifs, Eporner, Pornhub (webmasters search).
3. **Source tabs** get **favicons** (fetched once via the proxy `/favicon.ico` → cached), drag to
   **reorder**, and overflow scroll. The **sidebar Discover tile shows the active source's logo**.
4. **Bookmarks**: save the current source plus search plus rating as a bookmark chip under the search bar.
   They are reorderable and kept in sources.json.
5. **Reddit**: subreddits as searches (`r/name`). The home feed appears when the user has logged in (OAuth installed-app
   flow with their own client id; the token is kept in sources.json on this PC). Galleries, i.redd.it,
   v.redd.it (DASH → play via MSE, or the fallback mp4) and redgifs links are all handled.
6. **Image search** (DuckDuckGo plus Bing as "Web images"): size, type, layout, colour, time and safe-search options.
   Google Images is only an "Open in Google" link (the API is closed, see the findings).
7. **Reverse search from the library**: SauceNAO (the user's key) → IQDB fallback. The results show in
   Discover with "Save tags to this item".

## Phase 5: Videos and Long-form, Audio (2 sessions): DONE 19 Oct 2026 (Long-form and the video sites share one "Videos" section)

1. **The themed video player** replaces the native controls everywhere: a pink scrub bar with a
   hover thumbnail, **fixed seeking** (seek with `fastSeek` while dragging and exact on release, keyframe-aware
   for MSE streams), speed, loop A–B, fit/fill/original **resize preference** (remembered), and **vertical
   brightness/volume sliders** on the left/right edges (drag or wheel, like mobile players), plus keyboard
   shortcuts.
2. **Video sources** (RedGifs, Eporner, Pornhub, hanime as a link) in a **Netflix-like layout**: a
   hero row, horizontal rows per category/tag ("Trending", "Long", "Because you saved X"), and hover
   previews. Long-form videos (10 minutes or more) get the badge.
3. **A Long-form section**: a new sidebar entry with its own library view (videos ≥ 10 min), its own browser
   (the long video sources), and "continue watching" (the resume position is kept in views.csv as a new
   column `resume_at`).
4. **An Audio section**: its own sidebar entry, library and player. Audio files (mp3, m4a, ogg, opus,
   flac, wav) are stored under `Audio/` with categories (JOI, Hypno, ASMR, Erotic stories…). A mini
   player docks in the titlebar and **keeps playing in the background** across tabs (one `<audio>` element
   outside the tab DOM, with the Media Session API for media keys). The audio browser covers Reddit
   (r/gonewildaudio and similar subs via soundgasm.net links; soundgasm pages contain a direct
   m4a link) as its first source. Others can be added once they are checked.

## Phase 6: the slideshow (1 session): DONE 19 Oct 2026

1. **No zoom by default.** Motion effects are selectable: None, Ken Burns pan (slow), Hypnotic bounce slow,
   Hypnotic bounce fast, Breathe (scale 1 → 1.04), and Drift.
2. **Fix the bottom bar in fullscreen** (it is a leftover control strip: hidden in fullscreen, shown on mouse move).
3. **Pause when the app is unfocused** (window blur/visibilitychange, with an option).
4. **Category switch** chips at the top: All plus each main category, with no restart needed.
5. **A succubus-tail progress bar** with a heart tip: always visible, an SVG path whose dash offset follows the
   slide time.
6. **A click ripple** (a pink ring at the click point).
7. **History with durations** under the plan box: the last sessions, their item counts and their length.

## Phase 7: you, your stats, your progress (1–2 sessions): DONE 19 Oct 2026

1. **The profile page** (an avatar button at the top right): an animated profile picture picked from the library (a
   video clip loops), the vault name, a banner (from the library or a gradient), a title ("Connoisseur", "Night
   owl"…), level/XP, and stats.
2. **Level/XP**: XP comes from look time (1/min), saves, tagging and streak days. The level curve is
   `100 · level^1.5`. Titles and banners unlock with levels.
3. **Achievement badges** (about 30), e.g. "Disciplined" (a session over 5 h), "Night owl" (sessions after 3 am),
   "Curator" (1,000 tagged), "Archivist" (10k items), "Explorer" (10 sources), "Kegel king/queen" (a 30-day
   streak). They are computed from views.csv, state.json and the session log, with a pink toast when one is unlocked.
4. **The Stats page**: graphs (time per day and per week, top tags and categories over time, as small SVG charts), a
   **calendar heatmap** (GitHub-like, of the minutes viewed), and **session logs** (collapsed, one line each:
   start, length, items, top tags).
5. **The session stopwatch** at the top right in the titlebar, which pauses while idle or unfocused. Click it for the session log.
6. **Lust emoji girls per NSFW tier**: 5 small adult chibi faces (calm → blushing → flustered → lewd →
   ahegao-style), used as the NSFW level picker and badges. They are drawn as simple vector art by us.
7. **The Appreciate button** becomes a round heart button with a pop animation and an optional sound. The default is a soft
   CC0 "mm" sound. The user can pick their own sound file (any moan they like) in Settings, because a bundled
   voice clip needs a licence we can verify.

## Phase 8: coach, privacy, safety (1 session): DONE 19 Oct 2026 (an existing library is only renamed when you switch Discreet names on)

1. **The JOI and Kegel coach overlay**: a small floating assistant (an adult anime girl avatar plus a speech
   bubble) over any page or slideshow. Kegel mode follows the Mayo levels above, with a ring that
   fills and empties (hold/relax), set counts and a daily streak. JOI mode has a rhythm voice/visual (a beat
   metronome with speed changes, edge/hold/stop cues), slideshow sync and a session length. There are
   optional TTS cues (the Web Speech API, on-device voices). It is clearly a toy, and there is a short safety note on Kegel limits.
2. **Discreet storage** (on by default, a toggle in welcome): folders and files get neutral names
   (`Media Library`, `img_000123.jpg`). Category names live only in index.csv. This needs a migration that
   renames the existing files once, with a progress bar and resumable steps, and Code.gs compatibility is kept by an index-only mapping.
3. **The emergency key** (set in welcome and settings, default `Ctrl+Shift+Space`, global): it hides the window
   at once, mutes everything, pauses all media, and optionally shows a decoy (minimise to tray or switch to a
   "Calculator-like" blank). Pressing it again or clicking the tray icon restores the window. It uses `tauri-plugin-global-shortcut`.
4. **Pretty CSV**: index.csv and views.csv stay plain CSV (Excel/Code.gs need that, and CSV can't hold colour).
   The app adds readable column order, ISO dates and human headers in an exported "Vault.xlsx" with a pink
   header style (rust_xlsxwriter), as a button "Export to Excel". Honest note: true colour inside a .csv is impossible.

## Phase 9: sharing (start only): DONE 19 Oct 2026 (share packs; no online space)

1. **A Community tab, a first step**: export a "share pack" (a chosen collection → a zip with a manifest plus
   thumbnails plus a vault card image), and import a pack from a friend (zip → Add with tags and sources kept).
   There is no server yet. A real community (accounts, feeds) needs hosting, moderation and age checks. It is listed
   as a later decision for the user.

---

## Order and size

| Phase | Size | Depends on |
|---|---|---|
| 1 Quick fixes | S | — |
| 2 Name, logo, fonts, cursor, themes | M | — |
| 3 Library | L | 1 |
| 4 Discover sources | L | 1 |
| 5 Video/Long-form/Audio | XL | 4 (adapters), 3 (player in detail) |
| 6 Slideshow | M | — |
| 7 Profile, Stats, XP | L | time tracking (done) |
| 8 Coach, discreet, emergency, xlsx | M–L | — |
| 9 Sharing | S (begin) | 3 |

Phases 1, 2, 6 and 8 are independent and can run in any order. Phase 1 goes first because it is the cheapest
visible win.

## Things the user needs to supply or decide
- Their own API keys: SauceNAO (free), Reddit app client id, and optionally Gelbooru/Rule34.
- Whether discreet storage should rename the existing library (a one-time migration) or apply only to new files.
- Whether the profile and sharing should ever go online (needs a server; not planned now).
- Their own moan sound file, if the CC0 default isn't wanted.
