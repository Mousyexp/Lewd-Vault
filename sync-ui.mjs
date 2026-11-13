// Builds the desktop app's page (src/) from the shared page (ui/Index.html, the web app's page):
//   - applies the few changes the desktop needs to Index.html (the Drive / Apps Script calls go to
//     the Rust backend and the local files instead, plus hooks for the tags feature),
//   - adds the desktop layer: ui/desktop.css (the desktop look) and ui/desktop.js (tags, search,
//     desktop-only settings),
//   - copies the libraries and fonts the page loads from the internet in the web version, so the
//     app works offline.
// The web version stays the one source of the shared UI: run this (npm run sync) after changing
// Index.html. Every patch must match exactly once, so a change in Index.html that breaks one stops
// the build instead of quietly shipping a half-patched page.
import fs from 'node:fs';

const here = new URL('.', import.meta.url);
const out = new URL('src/', here);
let page = fs.readFileSync(new URL('ui/Index.html', here), 'utf8').replace(/\r\n/g, '\n'); // (a Windows checkout may have CRLF line ends: the patches below use LF)

function patch(find, replace) {
  const n = page.split(find).length - 1;
  if (n !== 1) throw new Error(`sync-ui: expected 1 match, found ${n}, for:\n${find}`);
  page = page.replace(find, () => replace);
}

// ---------- 1. the bridge: inside Tauri everything goes to the Rust side ----------
patch(`const inAppsScript = !!(window.google && google.script && google.script.run);`,
`// desktop app (Tauri): local files instead of Drive, see desktop/README.md
const inTauri = !!(window.__TAURI__ && window.__TAURI__.core);
const inAppsScript = inTauri || !!(window.google && google.script && google.script.run);
const tauriInvoke = inTauri ? window.__TAURI__.core.invoke : null;
// Untraceable mode (src-tauri/src/crypt.rs): the vault is encrypted, and opens with its password
// before anything is read. Resolves once it's open (at once in the other modes).
function vaultUnlock() {
  return tauriInvoke('api', { name: 'lockState', args: [] }).then(st => {
    if (st && st.untraceable) document.title = 'Notes'; // (the page's title can show as the window's)
    if (!st || !st.locked) return;
    return new Promise(done => {
      const show = () => {
        const box = document.createElement('div');
        box.className = 'ul-lock';
        box.innerHTML = '<form class="ul-card"><span class="ul-icon"></span><b>Enter your password</b>' +
          '<input type="password" autocomplete="off" spellcheck="false" aria-label="Password">' +
          '<button class="btn primary" type="submit">Open</button><span class="ul-err"></span></form>';
        const form = box.querySelector('form'), input = box.querySelector('input'), go = box.querySelector('button'), msg = box.querySelector('.ul-err');
        form.onsubmit = async e => {
          e.preventDefault();
          if (!input.value) return;
          go.disabled = input.disabled = true; go.textContent = 'Opening…'; msg.textContent = '';
          try { await tauriInvoke('unlock', { password: input.value }); box.remove(); done(); }
          catch (err) {
            msg.textContent = String(err && err.message || err);
            go.disabled = input.disabled = false; go.textContent = 'Open'; input.value = ''; input.focus();
          }
        };
        document.body.append(box);
        setTimeout(() => input.focus(), 60);
      };
      if (document.body) show(); else addEventListener('DOMContentLoaded', show);
    });
  }).catch(() => {});
}
// files are read through the vault's own origin, which the app sets on the window before the page
// runs: the vault:// protocol on the desktop (WebView2 spells it http://vault.localhost/), the
// loopback media server on iOS (custom schemes don't feed <img>/<video> on WKWebView)
const vaultOrigin = () => (window.__VAULT_ORIGIN || 'http://vault.localhost').replace(/\\/$/, '');
const vaultUrl = id => vaultOrigin() + '/' + encodeURIComponent(id);
async function localFetch(id, init) {
  const res = await fetch(vaultUrl(id), init);
  if (!res.ok) throw new Error(\`File not found (\${res.status})\`);
  return res;
}
// hooks the desktop layer (desktop.js) fills in: tags when saving, tag search in the Library and slideshow
var tagsForSave = () => '', tagFilter = () => true, showTagOk = () => true, afterRenderLibrary = () => {}, timeOf = () => 0;
// and library.js: preview clips, the "For you" order, Original content
var makeClip = null, forYouKey = () => 0, isOriginal = () => false;
// and videos.js: which items the Library shows (long videos live in Videos)
var libraryOk = () => true;
// and slideshow.js: the slides' motion, and full screen for the whole monitor
var slideMotion = null, showFullscreen = on => { if (on === false || (on === undefined && document.fullscreenElement)) { if (document.fullscreenElement) document.exitFullscreen().catch(() => {}); } else $('show').requestFullscreen().catch(() => {}); };`);

patch(`const setupReady = (bootSetup ? Promise.resolve(bootSetup) : api('getSetup')).then(`,
      `const setupReady = (bootSetup ? Promise.resolve(bootSetup) : inTauri ? vaultUnlock().then(() => api('getSetup')) : api('getSetup')).then(`);

patch(`function api(fn, ...args) {
`, `function api(fn, ...args) {
  if (inTauri) return tauriInvoke('api', { name: fn, args });
`);

// ---------- 2. reading files and byte ranges ----------
patch("  const res = await drive(`${DRIVE_API}${encodeURIComponent(id)}?alt=media`);",
      "  const res = inTauri ? await localFetch(id) : await drive(`${DRIVE_API}${encodeURIComponent(id)}?alt=media`);");
patch("  const res = await drive(`${DRIVE_API}${encodeURIComponent(id)}?alt=media`, { headers: { Range: `bytes=${start}-${end - 1}` } });",
      "  const res = await (inTauri ? localFetch(id, { headers: { Range: `bytes=${start}-${end - 1}` } })\n" +
      "    : drive(`${DRIVE_API}${encodeURIComponent(id)}?alt=media`, { headers: { Range: `bytes=${start}-${end - 1}` } }));");

// ---------- 3. saving files: sent to the Rust side in pieces, with progress ----------
// Through the fast lane (fastlane.rs: 127.0.0.1 with a key, as fast as the disk) when it runs, else
// through the upload command (WebView2's channel, about 40 MB/s). A piece the lane drops is sent
// again the slow way when that can't double it (the first piece, or one with its place in the file).
patch(`async function driveUpload(blob, name, parentId, { properties, onProgress } = {}) {
`, `var fastLane;
async function sendPiece(body, headers) {
  if (fastLane === undefined) fastLane = await api('fastLane').catch(() => null);
  if (fastLane) {
    let r;
    try {
      r = await fetch(\`http://127.0.0.1:\${fastLane.port}/upload\`, { method: 'POST', body, headers: { ...headers, 'x-key': fastLane.key } });
    } catch (e) {
      fastLane = null;
      console.warn('Fast lane lost, saving the slow way: ' + errMsg(e));
      if (!(headers['x-pos'] || headers['x-first'] === '1')) throw new Error('The connection to the save dropped. Try again.');
    }
    if (r) {
      const answer = await r.json().catch(() => 'Save failed');
      if (!r.ok) throw new Error(answer);
      return answer;
    }
  }
  return tauriInvoke('upload', body instanceof Blob ? new Uint8Array(await body.arrayBuffer()) : body, { headers });
}
async function driveUpload(blob, name, parentId, { properties, onProgress } = {}) {
  if (inTauri) {
    const token = crypto.randomUUID().replace(/-/g, '');
    for (let at = 0, piece = 8 * 1024 * 1024; ; at += piece) {
      if (fastLane === undefined) fastLane = await api('fastLane').catch(() => null);
      piece = fastLane ? 64 * 1024 * 1024 : 8 * 1024 * 1024;
      const done = at + piece >= blob.size;
      const id = await sendPiece(blob.slice(at, at + piece), {
        'x-token': token, 'x-first': at === 0 ? '1' : '', 'x-done': done ? '1' : '',
        'x-name': encodeURIComponent(name), 'x-parent': parentId });
      if (onProgress) onProgress(Math.min(1, (at + piece) / Math.max(1, blob.size)));
      if (done) return id;
    }
  }
`);
// the scores are in the file name already; there are no Drive properties to keep
patch(`async function patchProps(id, properties) {
  if (!inAppsScript) return;`, `async function patchProps(id, properties) {
  if (!inAppsScript || inTauri) return;`);

// ---------- 4. index.csv / views.csv: read straight from disk ----------
patch(`async function fetchTracked(fileId, key, knownMd5, parse) {
`, `async function fetchTracked(fileId, key, knownMd5, parse) {
  if (inTauri) {
    const r = await tauriInvoke('api', { name: 'readTracked', args: [fileId] });
    return knownMd5 && r.md5 === knownMd5 ? null : { md5: r.md5, data: parse(r.text) };
  }
`);
patch(`      collection: get(r, 'collection'), collectionCover: get(r, 'collection_cover') === '1',`,
      `      collection: get(r, 'collection'), collectionCover: get(r, 'collection_cover') === '1', tags: get(r, 'tags'), source: get(r, 'source'),`);

// ---------- 5. no browser cache: the files are on the local disk already ----------
// (a second copy would only double the space used). Preloading only decodes the first screenful.
patch(`(cacheOpen = ('caches' in window ?`, `(cacheOpen = (!inTauri && 'caches' in window ?`);
patch(`  const c = await openCache();
  if (!c) return;
  preloading = true;`, `  const c = await openCache();
  if (!c) { if (inTauri) await warmFirstScreen(); return; }
  preloading = true;`);

// ---------- 6. videos play straight from disk ----------
// The <video> element asks for byte ranges itself, so even a huge video never sits in memory.
patch(`  try {
    const blob = (await cacheGet('full/' + item.id)) || await readFile(item.id, p => {`, `  if (inTauri) { vid.src = vaultUrl(item.id); setSpeed(1); vid.play().catch(() => {}); return; }
  try {
    const blob = (await cacheGet('full/' + item.id)) || await readFile(item.id, p => {`);
patch(`  const asVideo = item.kind === 'video' || (item.kind === 'gif' && /\\.mp4$/i.test(item.file));
  let blob = await cacheGet('full/' + item.id);`, `  const asVideo = item.kind === 'video' || (item.kind === 'gif' && /\\.mp4$/i.test(item.file));
  if (inTauri) return { url: vaultUrl(item.id), video: asVideo };
  let blob = await cacheGet('full/' + item.id);`);

// full-size pictures too: the grid thumbnail stays until the picture is decoded, then it swaps in
patch(`    if (quick) img.src = quick;
    try {`, `    if (quick) img.src = quick;
    if (inTauri) {
      const full = new Image();
      full.src = vaultUrl(item.id);
      full.decode().catch(() => {}).then(() => { if (ticket === detailOpen) img.src = full.src; });
      return;
    }
    try {`);

// ---------- 7. links, thumbnails and words that pointed at Drive ----------
patch("      url: `https://drive.google.com/file/d/${id}/view`",
      "      url: inTauri ? vaultUrl(id) : `https://drive.google.com/file/d/${id}/view`");
patch(`function legacyThumb(img, id, full) {
`, `function legacyThumb(img, id, full) {
  if (inTauri) { img.src = vaultUrl(id); return; }
`);
patch(`  $('dOpen').href = item.url;`, `  $('dOpen').href = item.url;
  if (inTauri) $('dOpen').onclick = e => { e.preventDefault(); api('showInFolder', item.id).catch(err => toast(errMsg(err), 'err')); };`);
patch(`  if (!confirm('Move this to Drive trash? (You can restore it from Drive for 30 days.)')) return;`,
      `  if (!confirm(inTauri ? 'Move this to the Recycle Bin?' : 'Move this to Drive trash? (You can restore it from Drive for 30 days.)')) return;`);
patch("  if (!ids.length || !confirm(`Move ${ids.length} ${ids.length === 1 ? 'item' : 'items'} to Drive trash? (You can restore them from Drive for 30 days.)`)) return;",
      "  if (!ids.length || !confirm(`Move ${ids.length} ${ids.length === 1 ? 'item' : 'items'} to ${inTauri ? 'the Recycle Bin' : 'Drive trash'}?${inTauri ? '' : ' (You can restore them from Drive for 30 days.)'}`)) return;");
patch(`const needSetup = async () => setup || await setupReady || Promise.reject(new Error('Not connected to Drive'));`,
      `const needSetup = async () => setup || await setupReady || Promise.reject(new Error(inTauri ? 'The data folder could not be opened' : 'Not connected to Drive'));`);

// ---------- 8. tags (desktop.js does the rest) ----------
patch(`  for (; index < until; index++) jobs.push({ file: queue[index], category, nsfw, quality, collection });`,
      `  const tags = tagsForSave();
  for (; index < until; index++) jobs.push({ file: queue[index], category, nsfw, quality, collection, tags, source: queue[index]._source || '' });`);
patch(`category: j.category, nsfw: j.nsfw, quality: j.quality, collection: j.collection })));`,
      `category: j.category, nsfw: j.nsfw, quality: j.quality, collection: j.collection, tags: j.tags || '', source: j.source || '' })));`);
patch(`    .filter(i => (!cat || i.category === cat) && kindOk(i) && i.nsfw <= maxN && (q === 0 || (q > 0 ? i.quality === q : i.quality >= -q)))
    .sort(sorters[$('fSort').value]);
`, `    .filter(i => (!cat || i.category === cat) && kindOk(i) && i.nsfw <= maxN && (q === 0 || (q > 0 ? i.quality === q : i.quality >= -q)) && tagFilter(i) && libraryOk(i))
    .sort(sorters[$('fSort').value]);
  afterRenderLibrary(list);
`);
patch(`  && (!o.categories.length || o.categories.includes(i.category)) && i.quality >= o.minRating;`,
      `  && (!o.categories.length || o.categories.includes(i.category)) && i.quality >= o.minRating && showTagOk(i);`);
patch(`    const col = $('dCol').value.trim();
    if (col !== (current.collection || '')) await setCollectionOf([current.id], col);`,
      `    if (window.saveDetailTags) await saveDetailTags(current);
    const col = $('dCol').value.trim();
    if (col !== (current.collection || '')) await setCollectionOf([current.id], col);`);

// ---------- 8b. time spent looking at each item (views.csv's seconds_viewed; desktop.js tracks it) ----------
patch(`    likes: +get(r, 'appreciations') || 0, lastLike: get(r, 'last_appreciated')
  }]);`, `    likes: +get(r, 'appreciations') || 0, lastLike: get(r, 'last_appreciated'), seconds: +get(r, 'seconds_viewed') || 0
  }]);`);
patch(`    views: (a, b) => viewsOf(b.id) - viewsOf(a.id) || b.added.localeCompare(a.added)
`, `    views: (a, b) => viewsOf(b.id) - viewsOf(a.id) || b.added.localeCompare(a.added),
    time: (a, b) => timeOf(b.id) - timeOf(a.id) || b.added.localeCompare(a.added),
    foryou: (a, b) => forYouKey(b) - forYouKey(a)
`);

// ---------- 9. libraries and fonts from the app instead of the internet ----------
patch(`const THUMBHASH_URL = 'https://cdn.jsdelivr.net/npm/thumbhash@0.1.1/thumbhash.js';`,
      `const THUMBHASH_URL = inTauri ? '/vendor/thumbhash.js' : 'https://cdn.jsdelivr.net/npm/thumbhash@0.1.1/thumbhash.js';`);
patch(`const MEDIABUNNY_URL = 'https://cdn.jsdelivr.net/npm/mediabunny@1.61.0/dist/bundles/mediabunny.min.mjs';`,
      `const MEDIABUNNY_URL = inTauri ? '/vendor/mediabunny.min.mjs' : 'https://cdn.jsdelivr.net/npm/mediabunny@1.61.0/dist/bundles/mediabunny.min.mjs';`);
patch(`    link.href = 'https://fonts.googleapis.com/css2?family=Nunito:wght@400;600;700;800&display=swap';`,
      `    link.href = inTauri ? '/vendor/fonts.css' : 'https://fonts.googleapis.com/css2?family=Nunito:wght@400;600;700;800&display=swap';`);

// videos of 10 minutes or more get a "long" badge in the Library
patch(`if (item.kind !== 'image') tile.append(el('span', 'tag badge', item.kind === 'gif'`,
      `if (item.kind !== 'image') tile.append(el('span', 'tag badge' + (item.kind === 'video' ? ' vid' : '') + (item.duration >= 600 ? ' long' : ''), item.kind === 'gif'`);

patch(`const b = document.createElement('span'); b.className = 'tag badge';`,
      `const b = document.createElement('span'); b.className = 'tag badge' + (item.kind === 'video' ? ' vid' : '') + (item.duration >= 600 ? ' long' : '');`);


// ---------- Phase 3 (PLAN.md): the Library ----------
// preview clips are made by library.js (from 2/5 of the way in, short side capped, so landscape
// clips are as sharp as portrait ones), and named .c3.mp4 (24 fps at most) so older ones can be told apart and remade
patch(`async function makePreview(blob, info, stage) {
  try {`, `async function makePreview(blob, info, stage) {
  if (makeClip) return makeClip(blob, info, stage);
  try {`);
patch(`uploadHelper(out.preview, uid + '.mp4', s.folders.clips)`, `uploadHelper(out.preview, uid + '.c3.mp4', s.folders.clips)`);
// more layouts (library.js): the picker keeps whichever is chosen, and the grid knows it
patch(`  $('fLayout').value = asRows ? 'rows' : 'grid';
  grid.classList.toggle('rows', asRows);`, `  $('fLayout').value = S('libraryLayout');
  grid.classList.toggle('rows', asRows);
  grid.dataset.layout = S('libraryLayout');`);
// ---------- Phase 5: the full view plays every video straight from disk (with byte ranges); the
// web version's own streaming player (MSE over a seek table) made seeking unreliable here ----------
patch(`  if (item.streamId) {
    try {
      await streamVideo(item, vid, ticket);`, `  if (item.streamId && !inTauri) {
    try {
      await streamVideo(item, vid, ticket);`);

// and new videos are saved as ordinary MP4s with their index (no fragments, no seek table), so
// seeking is instant (library.js rewrites older ones)
patch(`audio: true, quality: S('videoQuality'), fragmented: true, keepCodec: S('keepVideoCodec'),`,
      `audio: true, quality: S('videoQuality'), fragmented: !inTauri, keepCodec: S('keepVideoCodec'),`);

// ---------- Phase 6: the slideshow (slideshow.js) ----------
// a motion of your choice (none by default) instead of the slow zoom, on videos too
patch(`  if (media.video) node.play().catch(() => {});
  else if (o.kenBurns) {`, `  if (media.video) node.play().catch(() => {});
  if (slideMotion) onScreen.anim = slideMotion(node, ms + o.transitionMs, o, !!media.video);
  else if (!media.video && o.kenBurns) {`);
// full screen is the window's own (covering the taskbar too), not the page's
patch(`  if (o.fullscreen && box.requestFullscreen) box.requestFullscreen().catch(() => {});`, `  if (o.fullscreen) showFullscreen(true);`);
patch(`  $('showNote').textContent = '';
  if (document.fullscreenElement) document.exitFullscreen().catch(() => {});`, `  $('showNote').textContent = '';
  showFullscreen(false);`);
patch(`const toggleFull = () => document.fullscreenElement ? document.exitFullscreen().catch(() => {}) : $('show').requestFullscreen().catch(() => {});`,
      `const toggleFull = () => showFullscreen();`);

// "Original content": a Type of its own
patch(`const kindOk = i => !kind || (kind === 'moving' ? i.kind !== 'image' : i.kind === kind);`,
      `const kindOk = i => !kind || (kind === 'oc' ? isOriginal(i) : kind === 'moving' ? i.kind !== 'image' : i.kind === kind);`);

// ---------- 10. the desktop layer ----------
patch(`</head>`, `<link rel="stylesheet" href="/vendor/fonts.css">
<link rel="stylesheet" href="/desktop.css">
</head>`);
patch(`</body>`, `<script src="/desktop.js"></script>
<script src="/discover.js"></script>
<script src="/sites.js"></script>
<script src="/look.js"></script>
<script src="/library.js"></script>
<script src="/player.js"></script>
<script src="/videos.js"></script>
<script src="/audio.js"></script>
<script src="/slideshow.js"></script>
<script src="/profile.js"></script>
<script src="/lust.js"></script>
<script src="/privacy.js"></script>
<script src="/drivesync.js"></script>
<script src="/coach.js"></script>
<script src="/share.js"></script>
<script src="/extras.js"></script>
<script src="/streamline.js"></script>
<script src="/ios.js"></script>
</body>`);
patch(`<head>`, `<head>\n<title>Lewd Vault</title>
<meta name="referrer" content="no-referrer">`); // (no page address goes out with requests: RedGifs refuses a foreign one)
// fill the whole screen on iOS so the Liquid Glass bars sit under the notch and home indicator
// (desktop ignores the safe-area insets, so this is harmless there)
patch(`<meta name="viewport" content="width=device-width, initial-scale=1">`,
      `<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">`);

// ---------- write src/ ----------
const copy = (from, to) => { fs.mkdirSync(new URL('.', to), { recursive: true }); fs.copyFileSync(from, to); };
const mod = p => new URL(`node_modules/${p}`, here);
fs.rmSync(out, { recursive: true, force: true });
fs.mkdirSync(new URL('vendor/', out), { recursive: true });
fs.writeFileSync(new URL('index.html', out), page);
copy(new URL('ui/desktop.css', here), new URL('desktop.css', out));
copy(new URL('ui/desktop.js', here), new URL('desktop.js', out));
copy(new URL('ui/discover.js', here), new URL('discover.js', out));
copy(new URL('ui/sites.js', here), new URL('sites.js', out));
copy(new URL('ui/look.js', here), new URL('look.js', out));
copy(new URL('ui/library.js', here), new URL('library.js', out));
copy(new URL('ui/player.js', here), new URL('player.js', out));
copy(new URL('ui/videos.js', here), new URL('videos.js', out));
copy(new URL('ui/audio.js', here), new URL('audio.js', out));
copy(new URL('ui/slideshow.js', here), new URL('slideshow.js', out));
copy(new URL('ui/profile.js', here), new URL('profile.js', out));
copy(new URL('ui/lust.js', here), new URL('lust.js', out));
copy(new URL('ui/privacy.js', here), new URL('privacy.js', out));
copy(new URL('ui/drivesync.js', here), new URL('drivesync.js', out));
copy(new URL('ui/coach.js', here), new URL('coach.js', out));
copy(new URL('ui/share.js', here), new URL('share.js', out));
copy(new URL('ui/extras.js', here), new URL('extras.js', out));
copy(new URL('ui/streamline.js', here), new URL('streamline.js', out));
copy(new URL('ui/ios.js', here), new URL('ios.js', out));
copy(new URL('src-tauri/icons/128x128@2x.png', here), new URL('app-icon.png', out)); // the brand mark in the sidebar
for (const f of fs.readdirSync(new URL('art/decor/', here))) copy(new URL(`art/decor/${f}`, here), new URL(`decor/${f}`, out)); // see ui/desktop.js
// her moans (MagicalMysticVA on Freesound, CC BY 4.0): Appreciate's like-*.ogg, the coach's coach-*.ogg
for (const f of fs.readdirSync(new URL('art/sounds/', here)).filter(f => f.endsWith('.ogg'))) copy(new URL(`art/sounds/${f}`, here), new URL(`sounds/${f}`, out));
copy(mod('thumbhash/thumbhash.js'), new URL('vendor/thumbhash.js', out));
copy(mod('mediabunny/dist/bundles/mediabunny.min.mjs'), new URL('vendor/mediabunny.min.mjs', out));
// Inter (the desktop UI font) and Nunito (the cute themes), Latin and Latin Extended, variable weight
const fonts = [['Inter Variable', 'inter', '@fontsource-variable/inter/files/inter-%s-wght-normal.woff2'],
               ['Nunito', 'nunito', '@fontsource-variable/nunito/files/nunito-%s-wght-normal.woff2'],
               // the cursive titles, and the round fonts the theme builder offers
               ['Dancing Script', 'dancing', '@fontsource-variable/dancing-script/files/dancing-script-%s-wght-normal.woff2'],
               ['Quicksand', 'quicksand', '@fontsource-variable/quicksand/files/quicksand-%s-wght-normal.woff2'],
               ['Comfortaa', 'comfortaa', '@fontsource-variable/comfortaa/files/comfortaa-%s-wght-normal.woff2'],
               ['Fredoka', 'fredoka', '@fontsource-variable/fredoka/files/fredoka-%s-wght-normal.woff2']];
const ranges = { latin: 'U+0000-00FF, U+0131, U+0152-0153, U+02BB-02BC, U+02C6, U+02DA, U+02DC, U+0304, U+0308, U+0329, U+2000-206F, U+20AC, U+2122, U+2191, U+2193, U+2212, U+2215, U+FEFF, U+FFFD',
                 'latin-ext': 'U+0100-02BA, U+02BD-02C5, U+02C7-02CC, U+02CE-02D7, U+02DD-02FF, U+0304, U+0308, U+0329, U+1D00-1DBF, U+1E00-1E9F, U+1EF2-1EFF, U+2020, U+20A0-20AB, U+20AD-20C0, U+2113, U+2C60-2C7F, U+A720-A7FF' };
let css = '/* bundled fonts (SIL Open Font Licence), from @fontsource-variable */\n';
for (const [family, short, path] of fonts) {
  for (const [subset, range] of Object.entries(ranges)) {
    const file = `${short}-${subset}.woff2`;
    copy(mod(path.replace('%s', subset)), new URL(`vendor/${file}`, out));
    css += `@font-face { font-family: '${family}'; font-style: normal; font-display: swap; font-weight: 100 1000; src: url(${file}) format('woff2'); unicode-range: ${range}; }\n`;
  }
}
fs.writeFileSync(new URL('vendor/fonts.css', out), css);
console.log('sync-ui: wrote src/');
