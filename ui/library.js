// Lewd Vault desktop: the Library (Phase 3 of PLAN.md). Loaded after desktop.js and look.js; uses the
// page's own functions (library, api, renderLibrary, openDetail, setCollectionOf, ...).
//   - preview clips: from 2/5 of the way in (past the intro), the short side capped at 320 px instead of
//     the width, so landscape clips are as sharp as portrait ones; older clips are remade in the background
//   - moving previews on screen add 0.1 s a second to an item's look time
//   - hold the mouse on a tile to see it bigger, over its neighbours (let go on it to open it)
//   - right-click a tile: open, category, NSFW level, rating, collection, Original content, copy, delete…
//   - layouts: Rows, Even grid, Masonry, Big cards, List; and the "For you" order
//   - Original content: a Type of its own, with badges
//   - Manage tags: rename, retype or delete a tag across the whole library
//   - a picture editor (rotate, flip, skew, colour sliders, presets)
//   - Clean up: Find duplicates, and Compact videos, in a small progress window

(() => {
  'use strict';
  const { make, tagChip, cleanTags, nameOf, typeOf, tagsChanged, tagsOf, addTime, thumbSrc } = window.vaultUI;
  const sleep = ms => new Promise(r => setTimeout(r, ms));
  const even = n => Math.max(2, Math.round(n / 2) * 2);
  const fmtBytes = n => n >= 1e9 ? (n / 1e9).toFixed(2) + ' GB' : n >= 1e6 ? Math.round(n / 1e6) + ' MB' : Math.round(n / 1e3) + ' KB';
  const hasTag = (item, t) => tagsOf(item).includes(t);

  // =====================================================================================
  // Preview clips
  // =====================================================================================
  // the short side is capped at 320 px: exactly the old portrait clips (320 x 568), and landscape ones
  // get the same number of pixels (568 x 320) instead of 320 x 180, so no clip costs more to play than before
  const CLIP_SHORT = 320, CLIP_LONG = 960, CLIP_MARK = '.c3.', CLIP_FPS = 24;
  const clipSize = (w, h) => { const s = Math.min(1, CLIP_SHORT / Math.min(w, h), CLIP_LONG / Math.max(w, h)); return [even(w * s), even(h * s)]; };
  async function clipFrom(source, info, stage, onConversion) {
    const mb = await loadMediabunny();
    const input = new mb.Input({ source, formats: mb.ALL_FORMATS });
    try {
      const track = await input.getPrimaryVideoTrack();
      if (!track) return null;
      const w = track.displayWidth || info.width, h = track.displayHeight || info.height;
      const dur = (await input.computeDuration()) || info.duration || 0;
      const len = S('previewSeconds');
      // from 2/5 of the way in, so it shows the video itself rather than its intro (short ones from the start)
      const start = dur > len * 1.6 ? Math.min(dur * 0.4, dur - len) : 0;
      const [cw, ch] = clipSize(w, h);
      const output = new mb.Output({ format: new mb.Mp4OutputFormat({ fastStart: 'in-memory' }), target: new mb.BufferTarget() });
      const conversion = await mb.Conversion.init({
        input, output, tracks: 'primary', showWarnings: false,
        // (at most 24 frames a second: previews play sped up, many at once, and a 60 fps source played at
        // 1.5x meant 90 frames a second to decode per tile; 24 fps at 1.5x is 36)
        video: { codec: 'avc', quality: mb.QUALITY_MEDIUM, width: cw, height: ch, fit: 'fill', frameRate: CLIP_FPS },
        audio: { discard: true },
        trim: { start, end: dur ? Math.min(dur, start + len) : start + len }
      });
      if (!conversion.isValid) throw new Error('this video cannot be read here');
      if (stage) conversion.onProgress = p => stage('Making preview', p);
      if (onConversion) onConversion(conversion);
      try { await conversion.execute(); } finally { if (onConversion) onConversion(null); }
      return new Blob([output.target.buffer], { type: 'video/mp4' });
    } finally {
      input.dispose();
    }
  }
  makeClip = async (blob, info, stage) => {
    try { return await clipFrom(new (await loadMediabunny()).BlobSource(blob), info, stage); }
    catch (e) { console.warn('No preview clip: ' + errMsg(e)); return null; }
  };

  // older clips (320 px wide, from the start) are remade in the background, landscape ones first,
  // read straight from disk in pieces, one at a time, and only while you're not using the app (no
  // mouse or keys for 20 s, or the window is in the background), so browsing never stutters. A remake
  // that's under way when you come back is stopped and done again later.
  const clipName = id => {
    try { const b = id.slice(id.indexOf('-') + 1).replace(/-/g, '+').replace(/_/g, '/'); return atob(b + '='.repeat((4 - b.length % 4) % 4)); }
    catch (e) { return ''; }
  };
  const CLIP_SKIP_KEY = 'imageVault.clipSkip2'; // (a first version failed every remake: that list is not used)
  const clipSkip = (() => { try { return new Set(JSON.parse(localStorage.getItem(CLIP_SKIP_KEY)) || []); } catch (e) { return new Set(); } })();
  let lastInput = Date.now(), activeClip = null;
  for (const ev of ['pointermove', 'pointerdown', 'keydown', 'wheel', 'scroll']) {
    addEventListener(ev, () => { lastInput = Date.now(); if (activeClip && document.hasFocus()) activeClip.cancel().catch(() => {}); }, { passive: true, capture: true });
  }
  const away = () => !document.hasFocus() || document.visibilityState !== 'visible' || Date.now() - lastInput > 20000;
  const busy = () => !away() || busyUploading() || !$('show').classList.contains('hidden') || compacting;
  async function sharpenClips() {
    await setupReady;
    while (!library) await sleep(500);
    await sleep(15000); // after start-up has settled
    const landscape = i => i.width > i.height * 1.1;
    const todo = library.filter(i => i.previewId && i.previewId.startsWith('clips-') && i.kind !== 'image' && i.uid
        && !clipName(i.previewId).includes(CLIP_MARK) && !clipSkip.has(i.id))
      .sort((a, b) => landscape(b) - landscape(a) || b.added.localeCompare(a.added));
    let done = 0;
    for (const item of todo) {
      while (busy()) await sleep(4000);
      if (!library.includes(item)) continue; // deleted meanwhile
      thumbText = `Sharpening previews ${fmtN(done)} / ${fmtN(todo.length)} (while you're away)`;
      setUploadStatus();
      try {
        let stopped = false;
        const blob = await clipFrom(new (await loadMediabunny()).UrlSource(vaultUrl(item.id)), item, null, c => { activeClip = c; })
          .catch(e => { if (/cancel/i.test(errMsg(e))) { stopped = true; return null; } throw e; });
        if (stopped) { todo.push(item); continue; } // you came back: later
        if (blob) {
          const s = await needSetup();
          const id = await uploadHelper(blob, item.uid + '.c3.mp4', s.folders.clips);
          const r = await api('updateThumbs', [{ id: item.id, previewId: id }]);
          item.previewId = id;
          libraryWritten(r);
        }
      } catch (e) {
        console.warn(`Preview for ${item.file} not remade: ${errMsg(e)}`);
        clipSkip.add(item.id);
        try { localStorage.setItem(CLIP_SKIP_KEY, JSON.stringify([...clipSkip])); } catch (e2) {}
      }
      done++;
      await sleep(300);
    }
    thumbText = ''; setUploadStatus();
    await makeSeekable();
  }
  sharpenClips();

  // =====================================================================================
  // Videos made seekable
  // =====================================================================================
  // Videos used to be saved as fragmented MP4s with a separate seek table (for the web version's
  // streaming). Played straight from disk they have no index: the player reads through the whole
  // file to learn its length, and every jump takes a moment. So, while you're away, each one is
  // rewritten as an ordinary MP4 with its index: copied, never re-encoded, so nothing is lost. The
  // copy is written to _app/streams, opened and checked (same length, seeking works), and only
  // then takes the original's place. New videos are saved that way from the start.
  const SEEK_SKIP_KEY = 'imageVault.seekSkip';
  const seekSkip = (() => { try { return new Set(JSON.parse(localStorage.getItem(SEEK_SKIP_KEY)) || []); } catch (e) { return new Set(); } })();
  async function remux(item, onConversion) {
    const mb = await loadMediabunny();
    const input = new mb.Input({ source: new mb.UrlSource(vaultUrl(item.id)), formats: mb.ALL_FORMATS });
    const token = crypto.randomUUID().replace(/-/g, ''), name = `seek-${item.uid}.mp4`;
    let first = true;
    const send = (data, position, done) => sendPiece(data, {
      'x-token': token, 'x-first': first ? '1' : '', 'x-pos': String(position), 'x-done': done ? '1' : '',
      'x-name': encodeURIComponent(name), 'x-parent': 'app-streams' });
    try {
      const writable = new WritableStream({ async write(chunk) { await send(chunk.data, chunk.position, false); first = false; } });
      const output = new mb.Output({ format: new mb.Mp4OutputFormat({ fastStart: false }), target: new mb.StreamTarget(writable, { chunked: true, chunkSize: 8 * 2 ** 20 }) });
      const conversion = await mb.Conversion.init({ input, output, showWarnings: false });
      const lost = conversion.discardedTracks.filter(d => d.reason !== 'discarded_by_user');
      if (!conversion.isValid || lost.length) throw new Error('cannot copy this one here');
      if (onConversion) onConversion(conversion);
      try { await conversion.execute(); } finally { if (onConversion) onConversion(null); }
    } finally { input.dispose(); }
    return send(new Uint8Array(0), 0, true); // the id of the finished copy
  }
  // the copy opens, is as long as the original, and jumps to its middle quickly
  async function checkCopy(id, item) {
    const v = document.createElement('video');
    v.muted = true; v.preload = 'metadata'; v.src = vaultUrl(id);
    try {
      await new Promise((ok, no) => { v.onloadedmetadata = ok; v.onerror = () => no(new Error('the copy doesn\'t open')); setTimeout(() => no(new Error('the copy took too long to open')), 15000); });
      if (!isFinite(v.duration) || (item.duration && Math.abs(v.duration - item.duration) > Math.max(1.5, item.duration * 0.01))) throw new Error('the copy is not as long as the original');
      const t0 = performance.now();
      v.currentTime = v.duration / 2;
      await new Promise((ok, no) => { v.onseeked = ok; setTimeout(() => no(new Error('the copy doesn\'t seek')), 5000); });
      return performance.now() - t0;
    } finally { v.removeAttribute('src'); v.load(); }
  }
  async function makeSeekable() {
    const todo = library.filter(i => i.kind === 'video' && i.streamId && i.uid && !seekSkip.has(i.id)).sort((a, b) => b.added.localeCompare(a.added));
    let done = 0;
    for (const item of todo) {
      while (busy()) await sleep(4000);
      if (!library.includes(item) || !item.streamId) continue;
      thumbText = `Making videos seekable ${fmtN(done)} / ${fmtN(todo.length)} (while you're away)`;
      setUploadStatus();
      try {
        let stopped = false;
        const id = await remux(item, c => { activeClip = c; }).catch(e => { if (/cancel/i.test(errMsg(e))) { stopped = true; return null; } throw e; });
        if (stopped) { todo.push(item); continue; }
        await checkCopy(id, item);
        if (current === item) { done++; continue; } // being watched right now: next time
        libraryWritten(await api('replaceItemFile', item.id, id));
        item.streamId = '';
      } catch (e) {
        console.warn(`${item.file} not made seekable: ${errMsg(e)}`);
        seekSkip.add(item.id);
        try { localStorage.setItem(SEEK_SKIP_KEY, JSON.stringify([...seekSkip])); } catch (e2) {}
      }
      done++;
      await sleep(300);
    }
    thumbText = ''; setUploadStatus();
  }

  // =====================================================================================
  // Look time: moving previews on screen count 0.1 s a second
  // =====================================================================================
  const grid = $('grid');
  setInterval(() => {
    if (document.visibilityState !== 'visible' || !document.hasFocus() || $('tab-library').classList.contains('hidden') || !$('detail').classList.contains('hidden')) return;
    for (const v of grid.querySelectorAll('video')) {
      if (v.paused || v.readyState < 3) continue;
      const card = v.closest('.card');
      if (!card || !card._item) continue;
      const r = card.getBoundingClientRect();
      if (r.bottom > 0 && r.top < innerHeight) addTime(card._item, 0.1);
    }
  }, 1000);

  // =====================================================================================
  // Smooth scrolling, with the previews playing
  // =====================================================================================
  // The page used to start a video player the moment a tile came within 400 px of the screen and
  // tear it down the moment it left, several per frame while scrolling. Making a media player is
  // heavy main-thread work, and that was the stutter. Now, as video feeds do it:
  //   - tiles near the screen get their clip loaded ahead of time (paused), at most 2 new players a
  //     frame, the ones on screen first, then the nearest
  //   - tiles on screen play (up to "Most moving previews playing at once"), the rest pause
  //   - players that leave are kept, and only past a cap (playing + 24) are the ones out of sight
  //     the longest let go, 2 a frame, so scrolling back and forth reuses them
  //   - while the page scrolls the mouse can't hover tiles (no hover restyling mid-scroll)
  const html = document.documentElement;
  const inView = new Set(), loaded = new Map(); // card -> when it was last near or on screen
  const viewObserver = new IntersectionObserver(es => {
    for (const e of es) e.isIntersecting ? inView.add(e.target) : inView.delete(e.target);
    schedule();
  }, { threshold: 0.2 });
  const usable = c => c._video && c.isConnected && !c.classList.contains('locked') && c._item && c._item.previewId;
  const docOrder = (a, b) => a.compareDocumentPosition(b) & Node.DOCUMENT_POSITION_FOLLOWING ? -1 : 1;
  let ticking = false;
  const schedule = () => { if (!ticking) { ticking = true; requestAnimationFrame(tick); } };
  function startClip(card) {
    const v = card._video;
    v.preload = 'auto';
    v.defaultPlaybackRate = v.playbackRate = S('previewSpeed');
    v.src = vaultUrl(card._item.previewId);
    v.addEventListener('canplay', schedule, { once: true });
    loaded.set(card, performance.now());
  }
  function stopClip(card) {
    const v = card._video;
    loaded.delete(card);
    if (!v) return;
    v.classList.remove('in');
    if (!v.getAttribute('src')) return;
    v.pause(); v.removeAttribute('src'); v.load(); // frees the decoder
  }
  function tick() {
    ticking = false;
    for (const c of inView) if (!c.isConnected) inView.delete(c);
    for (const c of [...loaded.keys()]) if (!c.isConnected) stopClip(c); // the grid was redrawn
    if (gridPaused || $('tab-library').classList.contains('hidden')) {
      for (const c of loaded.keys()) if (!c._video.paused) c._video.pause();
      return;
    }
    const now = performance.now();
    const play = new Set([...inView].filter(usable).sort(docOrder).slice(0, S('maxPlaying')));
    const near = [...visible].filter(usable);
    for (const c of play) loaded.has(c) && loaded.set(c, now);
    for (const c of near) loaded.has(c) && loaded.set(c, now);
    // new players: on screen first, then the nearest to the middle of the screen; 2 a frame
    let budget = flinging ? 0 : 2, more = false;
    const waiting = near.filter(c => !play.has(c) && !c._video.getAttribute('src'));
    if (waiting.length > 2) {
      const mid = innerHeight / 2;
      const dist = new Map(waiting.map(c => { const r = c.getBoundingClientRect(); return [c, Math.abs(r.top + r.height / 2 - mid)]; }));
      waiting.sort((a, b) => dist.get(a) - dist.get(b));
    }
    for (const c of [...play, ...waiting]) {
      if (c._video.getAttribute('src')) continue;
      if (budget-- <= 0) { more = true; break; }
      startClip(c);
    }
    for (const c of loaded.keys()) {
      const v = c._video;
      if (play.has(c) && !flinging) { if (v.paused) v.play().catch(() => {}); }
      else if (!v.paused) v.pause();
    }
    // past the cap, let go of the ones out of sight the longest
    const cap = S('maxPlaying') + 24;
    if (loaded.size > cap) {
      const gone = [...loaded].filter(([c]) => !play.has(c) && !visible.has(c)).sort((a, b) => a[1] - b[1]);
      gone.slice(0, Math.min(2, loaded.size - cap)).forEach(([c]) => stopClip(c));
      if (loaded.size > cap && gone.length > 2) more = true;
    }
    if (more) schedule();
  }
  // the page's own playback steps, replaced by the above
  reconcile = schedule;
  loadClip = () => schedule();
  unloadClip = card => { stopClip(card); schedule(); };
  hideCard = function (card) {
    card._thumbShown = false;
    card._img.removeAttribute('src');
    card._img.classList.remove('in');
    if (card._video && !card._video.paused) card._video.pause(); // kept loaded for when you scroll back
    schedule();
  };
  const makeCardView = makeCard;
  makeCard = function () { const card = makeCardView.apply(this, arguments); if (card._video) viewObserver.observe(card); return card; };
  // no hover restyling while the page scrolls; during a fling (over 3000 px a second) the previews
  // hold still and no new players start, so the video decoder isn't fighting the scroll, and it all
  // picks up again as the page slows down
  let scrollEnd = null, lastY = scrollY, lastT = 0;
  var flinging = false;
  addEventListener('scroll', () => {
    if (!html.classList.contains('is-scrolling')) html.classList.add('is-scrolling');
    const now = performance.now(), speed = Math.abs(scrollY - lastY) / Math.max(1, now - lastT);
    lastY = scrollY; lastT = now;
    const was = flinging;
    flinging = speed > 3;
    if (was && !flinging) schedule();
    clearTimeout(scrollEnd);
    scrollEnd = setTimeout(() => { html.classList.remove('is-scrolling'); if (flinging) { flinging = false; schedule(); } }, 140);
  }, { passive: true });

  // =====================================================================================
  // Hold a tile: 30% bigger (let go on it to open it, move away to put it back)
  // =====================================================================================
  let holdTimer = null, held = null;
  const unhold = () => {
    clearTimeout(holdTimer); holdTimer = null;
    if (held) { held.classList.remove('held'); const row = held.closest('.jrow'); if (row) row.classList.remove('lv-holding'); held = null; }
  };
  grid.addEventListener('pointerdown', e => {
    if (e.button !== 0) return;
    const card = e.target.closest('.card');
    if (!card) return;
    unhold();
    holdTimer = setTimeout(() => {
      const r = card.getBoundingClientRect(), main = document.querySelector('main').getBoundingClientRect();
      // grows away from the window's edges
      const x = r.left - main.left < r.width * 0.3 ? 'left' : main.right - r.right < r.width * 0.3 ? 'right' : 'center';
      const y = r.top < 120 ? 'top' : innerHeight - r.bottom < r.height * 0.3 ? 'bottom' : 'center';
      card.style.transformOrigin = `${x} ${y}`;
      // (in Rows, each row clips what's in it, for smooth scrolling: the held tile's row lets it out over the others)
      const row = card.closest('.jrow'); if (row) row.classList.add('lv-holding');
      card.classList.add('held');
      held = card;
    }, 260);
  });
  grid.addEventListener('pointerup', () => setTimeout(unhold, 0)); // after the click that opens it
  grid.addEventListener('pointerout', e => { if (held && !held.contains(e.relatedTarget)) unhold(); else if (holdTimer && !e.target.closest('.card')?.contains(e.relatedTarget)) unhold(); });
  grid.addEventListener('pointercancel', unhold);
  grid.addEventListener('dragstart', e => e.preventDefault());

  // =====================================================================================
  // Right-click menu
  // =====================================================================================
  const menu = make('div', 'lv-menu hidden');
  document.body.append(menu);
  const closeMenu = () => menu.classList.add('hidden');
  addEventListener('pointerdown', e => { if (!menu.contains(e.target)) closeMenu(); }, true);
  addEventListener('keydown', e => { if (e.key === 'Escape') closeMenu(); });
  addEventListener('scroll', closeMenu, true);
  function menuItems(list, items) {
    list.replaceChildren();
    for (const it of items) {
      if (it === '-') { list.append(make('div', 'lv-sep')); continue; }
      const b = make('button', 'lv-item' + (it.danger ? ' danger' : '') + (it.sub ? ' has-sub' : '') + (it.on ? ' on' : ''));
      b.type = 'button';
      b.append(make('span', 'lv-ico', it.icon || ''), make('span', 'lv-label', it.label));
      if (it.hint) b.append(make('span', 'lv-hint', it.hint));
      if (it.sub) {
        const sub = make('div', 'lv-sub');
        menuItems(sub, it.sub);
        b.append(sub);
      } else b.onclick = () => { closeMenu(); Promise.resolve(it.run()).catch(e => toast(errMsg(e), 'err')); };
      list.append(b);
    }
  }
  function openMenu(x, y, items) {
    menuItems(menu, items);
    menu.classList.remove('hidden');
    const r = menu.getBoundingClientRect();
    menu.style.left = Math.min(x, innerWidth - r.width - 8) + 'px';
    menu.style.top = Math.min(y, innerHeight - r.height - 8) + 'px';
    menu.classList.toggle('flip', x + r.width + 220 > innerWidth); // submenus open to the left
  }
  async function changeProps(item, f) {
    const want = { category: item.category, nsfw: item.nsfw, quality: item.quality, ...f };
    const r = await api('updateImage', item.id, want);
    Object.assign(item, want, { uid: r.item.uid, file: r.item.file });
    libraryWritten(r);
    addCategory(want.category);
    renderLibrary();
  }
  async function setTagOn(item, tag, on) {
    const r = await api('setTags', [item.id], tag, on ? 'add' : 'remove');
    item.tags = cleanTags(on ? [...tagsOf(item), tag] : tagsOf(item).filter(t => t !== tag)).join(' ');
    libraryWritten(r); tagsChanged();
  }
  async function copyPicture(item) {
    const blob = await (await fetch(vaultUrl(item.id))).blob();
    const bmp = await createImageBitmap(blob);
    const c = new OffscreenCanvas(bmp.width, bmp.height);
    c.getContext('2d').drawImage(bmp, 0, 0);
    bmp.close();
    await navigator.clipboard.write([new ClipboardItem({ 'image/png': await c.convertToBlob({ type: 'image/png' }) })]);
    toast('Copied. Paste it anywhere.', 'ok');
  }
  async function deleteItems(items) {
    if (!confirm(items.length === 1 ? 'Delete this? It goes to the Recycle Bin.' : `Delete these ${items.length}? They go to the Recycle Bin.`)) return;
    const ids = items.map(i => i.id);
    trashedHere(await api('trashImages', ids), ids);
    toast(items.length === 1 ? 'Moved to the Recycle Bin' : `${items.length} moved to the Recycle Bin`, 'ok');
    renderLibrary();
  }
  const ask = (q, def = '') => { const v = prompt(q, def); return v == null ? null : v.trim(); };
  function itemMenu(item, card) {
    const cats = [...new Set(library.map(i => i.category))].sort((a, b) => a.localeCompare(b));
    const cols = collectionNames();
    const oc = isOriginal(item);
    return [
      { icon: '↗', label: 'Open', hint: 'Click', run: () => card.click() },
      { icon: '▣', label: 'Open with its program', run: () => api('openFile', item.id) },
      { icon: '📁', label: 'Show in folder', run: () => api('showInFolder', item.id) },
      { icon: '🔎', label: 'Find the source…', run: () => window.vaultDiscover ? window.vaultDiscover.findSource(item) : toast('Discover is not ready yet', 'err') },
      ...(item.kind === 'image' ? [{ icon: '✎', label: 'Edit picture…', run: () => openEditor(item) }, { icon: '⧉', label: 'Copy picture', run: () => copyPicture(item) }] : []),
      '-',
      { icon: '▤', label: 'Category', sub: [...cats.map(c => ({ label: c, on: c === item.category, run: () => changeProps(item, { category: c }) })),
        '-', { label: 'New category…', run: () => { const c = ask('New category name'); if (c) return changeProps(item, { category: c }); } }] },
      { icon: '🔥', label: 'NSFW level', sub: Array.from({ length: 10 }, (_, k) => ({ label: String(k + 1), on: item.nsfw === k + 1, run: () => changeProps(item, { nsfw: k + 1 }) })) },
      { icon: '★', label: 'Rating', sub: [[1, '★ okay'], [2, '★★ good'], [3, '★★★ great']].map(([q, l]) => ({ label: l, on: item.quality === q, run: () => changeProps(item, { quality: q }) })) },
      { icon: '❒', label: 'Collection', sub: [...cols.map(c => ({ label: c, on: c === item.collection, run: async () => { await setCollectionOf([item.id], c); renderLibrary(); } })),
        '-', { label: 'New collection…', run: async () => { const c = ask('New collection name'); if (c) { await setCollectionOf([item.id], c); renderLibrary(); } } },
        ...(item.collection ? [{ label: 'Take out of "' + item.collection + '"', run: async () => { await setCollectionOf([item.id], ''); renderLibrary(); } }] : [])] },
      { icon: '♥', label: oc ? 'Not original content' : 'Mark as original content', run: async () => { await setTagOn(item, OC_TAG, !oc); renderLibrary(); } },
      '-',
      { icon: '🗑', label: 'Delete', hint: 'Del', danger: true, run: () => deleteItems([item]) }
    ];
  }
  grid.addEventListener('contextmenu', e => {
    const card = e.target.closest('.card');
    if (!card || !card._item) return;
    e.preventDefault();
    unhold();
    openMenu(e.clientX, e.clientY, itemMenu(card._item, card));
  });

  // =====================================================================================
  // Layouts, and the "For you" order
  // =====================================================================================
  const LAYOUTS = [['rows', 'Rows (whole pictures)'], ['grid', 'Even grid (cropped to fit)'], ['masonry', 'Masonry (whole pictures, in columns)'], ['big', 'Big cards with details'], ['list', 'List with details']];
  const layoutSpec = SETTINGS.find(s => s.key === 'libraryLayout');
  layoutSpec.options = LAYOUTS;
  $('fLayout').replaceChildren(...LAYOUTS.map(([v, l]) => new Option(l.replace(/ \(.*/, ''), v)));
  $('fLayout').value = S('libraryLayout');
  const MASONRY_ROW = 4;
  function placeMasonry() {
    if (grid.dataset.layout !== 'masonry') return;
    const cards = grid.querySelectorAll(':scope > .card, :scope > .ccard, :scope > button');
    const first = cards[0];
    if (!first) return;
    const w = first.getBoundingClientRect().width;
    for (const c of cards) {
      const it = c._item;
      const ar = it && it.width && it.height ? it.width / it.height : 0.75;
      c.style.gridRowEnd = `span ${Math.max(1, Math.round(w / ar / MASONRY_ROW))}`;
    }
  }
  new MutationObserver(placeMasonry).observe(grid, { childList: true });
  new ResizeObserver(placeMasonry).observe(grid);
  // the details under a big card or beside a list row
  const fmtSpent = s => s >= 3600 ? `${(s / 3600).toFixed(1)} h` : s >= 60 ? `${Math.round(s / 60)} min` : `${Math.round(s)} s`;
  const makeCardLib = makeCard;
  makeCard = function (item) {
    const card = makeCardLib.apply(this, arguments);
    if (item.width && item.height) card.style.setProperty('--ar', item.width / item.height);
    const layout = S('libraryLayout');
    if (layout === 'big' || layout === 'list') {
      const info = make('span', 'lv-info');
      const top = make('span', 'lv-info-top');
      top.append(make('b', '', item.category), make('span', 'lv-info-dim', [item.kind === 'image' ? '' : item.duration ? fmtTime(item.duration) : '', item.width ? `${item.width}×${item.height}` : ''].filter(Boolean).join(' · ')));
      const tags = make('span', 'lv-info-tags');
      for (const t of tagsOf(item).slice(0, layout === 'list' ? 8 : 5)) tags.append(tagChip(t));
      const stats = make('span', 'lv-info-dim', [`${fmtN(viewsOf(item.id))} ${viewsOf(item.id) === 1 ? 'view' : 'views'}`, timeOf(item.id) ? fmtSpent(timeOf(item.id)) : '', `NSFW ${item.nsfw}`, item.added.slice(0, 10)].filter(Boolean).join(' · ') + ' · ');
      stats.append(make('span', 'lv-stars q' + (item.quality || 1), '★'.repeat(item.quality || 1)));
      info.append(top, tags, stats);
      card.append(info);
    }
    // Original content: badges
    if (isOriginal(item)) {
      const stickers = make('span', 'lv-stickers');
      stickers.append(make('span', 'lv-sticker oc', 'OC'));
      for (const b of OC_BADGES) if (hasTag(item, b)) stickers.append(make('span', 'lv-sticker', b.replace(/_/g, ' ')));
      card.append(stickers);
    }
    return card;
  };
  // For you: a fresh mix each session of what you love most (60%), what you haven't seen in a month
  // or ever (25%) and anything at all (15%)
  let fyKeys = null, fyCount = 0;
  forYouKey = item => {
    if (!fyKeys || fyCount !== library.length) {
      fyCount = library.length;
      const now = Date.now();
      const love = i => Math.log1p(viewsOf(i.id) + likesOf(i.id) * 3 + timeOf(i.id) / 30);
      const most = Math.max(1, ...library.map(love));
      fyKeys = new Map(library.map(i => {
        const c = viewCounts.get(i.id), last = c && c.last ? Date.parse(c.last) : 0;
        const stale = !last || now - last > 30 * 864e5 ? 1 : 0;
        const weight = 0.6 * love(i) / most + 0.25 * stale + 0.15;
        return [i.id, Math.random() ** (1 / weight)];
      }));
    }
    return fyKeys.get(item.id) || 0;
  };
  $('fSort').prepend(new Option('For you', 'foryou'));

  // =====================================================================================
  // Original content
  // =====================================================================================
  const OC_TAG = 'meta:oc';
  const OC_BADGES = ['selfie', 'mirror', 'trib', 'couple', 'solo', 'toy', 'lingerie', 'cosplay', 'outdoor', 'shower', 'pov'];
  isOriginal = item => hasTag(item, OC_TAG);
  $('fKind').add(new Option('Original content', 'oc'));
  $('fKind').addEventListener('change', () => { if ($('fKind').value === 'oc') { $('fSort').value = 'new'; renderLibrary(); } });
  // in the details: a switch
  const ocBtn = make('button', 'btn lv-oc');
  ocBtn.type = 'button';
  const drawOc = () => { if (!current) return; const on = isOriginal(current); ocBtn.classList.toggle('on', on); ocBtn.textContent = on ? '♥ Original content' : '♡ Mark as original content'; };
  ocBtn.onclick = async () => {
    if (!current) return;
    try { await setTagOn(current, OC_TAG, !isOriginal(current)); drawOc(); openDetail(current, detailList); }
    catch (e) { toast(errMsg(e), 'err'); }
  };
  $('dLike').after(ocBtn);
  // where it came from (anything saved from Discover, Videos or a link): "Source: site/…", opens in the browser
  const srcLine = make('button', 'linkish lv-source');
  srcLine.type = 'button';
  srcLine.onclick = () => { if (current && current.source) api('openLink', current.source.replace(/#\d+$/, '')).catch(err => toast(errMsg(err), 'err')); };
  $('dName').after(srcLine);
  const drawSource = () => {
    const src = current && current.source;
    srcLine.classList.toggle('hidden', !src || !/^https?:\/\//.test(src));
    if (src) { srcLine.textContent = 'Source: ' + src.replace(/^https?:\/\/(www\.)?/, '').slice(0, 64) + (src.length > 72 ? '…' : '') + ' ↗'; srcLine.title = src; }
  };
  const openDetailLib = openDetail;
  openDetail = function () { const r = openDetailLib.apply(this, arguments); drawOc(); drawSource(); editBtn.classList.toggle('hidden', !current || current.kind !== 'image'); return r; };

  // =====================================================================================
  // Sheets (a dialog card) and the small progress window
  // =====================================================================================
  function sheet(title, lead) {
    const back = make('div', 'dc-sheet lv-sheet');
    const card = make('div', 'dc-sheet-card');
    const head = make('div', 'dc-sheet-head');
    const close = make('button', 'btn', 'Close');
    close.onclick = () => back.remove();
    head.append(make('h2', '', title), close);
    const body = make('div', 'dc-sheet-body');
    if (lead) body.append(make('p', 'dc-sheet-lead', lead));
    card.append(head, body);
    back.append(card);
    back.addEventListener('pointerdown', e => { if (e.target === back) back.remove(); });
    document.body.append(back);
    return { back, body, close: () => back.remove() };
  }
  // the progress window: bottom right, can be made small; one job at a time
  const tray = make('div', 'lv-tray hidden');
  tray.innerHTML = `<div class="lv-tray-head"><b class="lv-tray-title"></b><button class="lv-tray-min" type="button" title="Make small">–</button></div>
    <div class="lv-tray-bar"><div></div></div><div class="lv-tray-text"></div><div class="lv-tray-actions"></div>`;
  document.body.append(tray);
  const tq = sel => tray.querySelector(sel);
  tq('.lv-tray-min').onclick = () => tray.classList.toggle('small');
  tray.addEventListener('click', e => { if (tray.classList.contains('small') && !e.target.closest('.lv-tray-min')) tray.classList.remove('small'); });
  const trayShow = (title, text = '', part = 0) => {
    tray.classList.remove('hidden');
    tq('.lv-tray-title').textContent = title;
    tq('.lv-tray-text').textContent = text;
    tq('.lv-tray-bar > div').style.width = (part * 100).toFixed(1) + '%';
    tq('.lv-tray-bar').classList.toggle('hidden', part < 0);
    tq('.lv-tray-actions').replaceChildren();
  };
  const trayActions = (...btns) => tq('.lv-tray-actions').replaceChildren(...btns);
  const button = (cls, text, run) => { const b = make('button', 'btn ' + cls, text); b.type = 'button'; b.onclick = run; return b; };

  // =====================================================================================
  // Manage tags
  // =====================================================================================
  const TYPES = [['general', 'Tag'], ['artist', 'Artist'], ['character', 'Character'], ['series', 'Series'], ['meta', 'Meta']];
  const withType = (tag, type) => type === 'general' ? nameOf(tag) : `${type}:${nameOf(tag)}`;
  async function renameTag(from, to) {
    to = cleanTags([to])[0];
    if (!to || to === from) return;
    const items = library.filter(i => hasTag(i, from));
    const ids = items.map(i => i.id);
    libraryWritten(await api('setTags', ids, from, 'remove'));
    libraryWritten(await api('setTags', ids, to, 'add'));
    for (const i of items) i.tags = cleanTags([...tagsOf(i).filter(t => t !== from), to]).join(' ');
    tagsChanged();
    toast(`"${from}" is now "${to}" on ${fmtN(items.length)} items`, 'ok');
  }
  async function deleteTag(tag) {
    const items = library.filter(i => hasTag(i, tag));
    libraryWritten(await api('setTags', items.map(i => i.id), tag, 'remove'));
    for (const i of items) i.tags = tagsOf(i).filter(t => t !== tag).join(' ');
    tagsChanged();
    toast(`"${tag}" removed from ${fmtN(items.length)} items`, 'ok');
  }
  function openTagManager() {
    const { body } = sheet('Manage tags', 'Every tag in your library. Rename one (a new name that already exists merges them), change its type, or delete it from every item. Changes are saved at once.');
    const search = make('input', 'lv-tm-search');
    Object.assign(search, { type: 'search', placeholder: 'Find a tag…', spellcheck: false });
    const list = make('div', 'lv-tm-list');
    body.append(search, list);
    const draw = () => {
      const counts = new Map();
      for (const i of library) for (const t of tagsOf(i)) counts.set(t, (counts.get(t) || 0) + 1);
      const q = search.value.trim().toLowerCase().replace(/\s+/g, '_');
      const rows = [...counts].filter(([t]) => !q || t.includes(q)).sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0])).slice(0, 300);
      list.replaceChildren(...rows.map(([tag, n]) => {
        const row = make('div', 'lv-tm-row');
        const name = make('input', 'lv-tm-name');
        Object.assign(name, { type: 'text', value: nameOf(tag), spellcheck: false, title: 'Type a new name and press Enter' });
        name.addEventListener('keydown', async e => {
          if (e.key !== 'Enter') return;
          try { await renameTag(tag, withType(name.value, typeOf(tag))); draw(); renderLibrary(); } catch (err) { toast(errMsg(err), 'err'); }
        });
        const type = make('select', 'lv-tm-type');
        TYPES.forEach(([v, l]) => type.add(new Option(l, v)));
        type.value = typeOf(tag);
        type.onchange = async () => { try { await renameTag(tag, withType(tag, type.value)); draw(); renderLibrary(); } catch (err) { toast(errMsg(err), 'err'); } };
        const del = button('danger lv-tm-del', 'Delete', async () => {
          if (!confirm(`Remove "${tag}" from all ${n} items?`)) return;
          try { await deleteTag(tag); draw(); renderLibrary(); } catch (err) { toast(errMsg(err), 'err'); }
        });
        row.append(tagChip(tag), name, type, make('span', 'lv-tm-n', fmtN(n)), del);
        return row;
      }));
      if (!rows.length) list.append(make('p', 'dc-sheet-lead', counts.size ? 'No tag matches.' : 'No tags yet.'));
    };
    search.addEventListener('input', draw);
    draw();
    search.focus();
  }
  // a button in the Library's tag panel
  // (the panel is drawn a moment after the grid: desktop.js drawPanel says when)
  document.addEventListener('drawn', e => {
    const head = e.target.closest && e.target.closest('.tag-panel') && e.target.querySelector('.panel-head');
    if (head && !head.querySelector('.lv-manage')) head.append(button('lv-manage', 'Manage', openTagManager));
  });

  // =====================================================================================
  // The picture editor
  // =====================================================================================
  const editBtn = make('button', 'lv-edit-btn');
  editBtn.type = 'button'; editBtn.title = 'Edit picture';
  editBtn.innerHTML = '<svg viewBox="0 0 24 24"><path d="M4 20l4-1 11-11-3-3L5 16z"/><path d="M14 6l3 3"/></svg>';
  editBtn.onclick = () => current && openEditor(current);
  document.querySelector('#detail .big').append(editBtn);
  const SLIDERS = [['brightness', 'Brightness', 50, 150, 100], ['contrast', 'Contrast', 50, 150, 100], ['saturate', 'Saturation', 0, 200, 100],
    ['hue', 'Hue', -180, 180, 0], ['warmth', 'Warmth', 0, 100, 0], ['blur', 'Soften', 0, 10, 0], ['vignette', 'Vignette', 0, 100, 0],
    ['skewX', 'Skew sideways', -30, 30, 0], ['skewY', 'Skew up/down', -30, 30, 0]];
  const PRESETS = { Original: {}, Soft: { contrast: 90, saturate: 85, brightness: 106, blur: 1 }, Vivid: { contrast: 115, saturate: 150 },
    Warm: { warmth: 45, saturate: 115 }, Mono: { saturate: 0, contrast: 112 }, Dreamy: { brightness: 110, saturate: 120, blur: 2, vignette: 40 }, Noir: { saturate: 0, contrast: 140, vignette: 70 } };
  function openEditor(item) {
    const st = { rotate: 0, flipX: false, flipY: false, ...Object.fromEntries(SLIDERS.map(([k, , , , d]) => [k, d])) };
    const ed = make('div', 'lv-editor');
    ed.innerHTML = `<div class="lv-ed-stage"><img alt=""><div class="lv-ed-vig"></div></div>
      <aside class="lv-ed-side"><div class="lv-ed-head"><b>Edit picture</b><button class="btn lv-ed-close" type="button">Cancel</button></div>
      <div class="lv-ed-row lv-ed-turns"></div><div class="lv-ed-presets"></div><div class="lv-ed-sliders"></div>
      <div class="lv-ed-save"></div><p class="lv-ed-note">Saved as a new item with the same category, tags and collection. "Replace" also sends the original to the Recycle Bin (its views and time start again).</p></aside>`;
    document.body.append(ed);
    const q = s => ed.querySelector(s);
    const img = q('img');
    img.src = vaultUrl(item.id);
    const filterOf = () => `brightness(${st.brightness}%) contrast(${st.contrast}%) saturate(${st.saturate}%) hue-rotate(${st.hue}deg) sepia(${st.warmth * 0.6}%) blur(${st.blur * 0.4}px)`;
    const draw = () => {
      img.style.filter = filterOf();
      img.style.transform = `rotate(${st.rotate}deg) scale(${st.flipX ? -1 : 1}, ${st.flipY ? -1 : 1}) skew(${st.skewX}deg, ${st.skewY}deg)`;
      q('.lv-ed-vig').style.opacity = st.vignette / 100;
      ed.querySelectorAll('.lv-ed-slider input').forEach(i => { i.value = st[i.name]; i.nextSibling.textContent = st[i.name]; });
    };
    const turns = [['⟲ Left', () => { st.rotate = (st.rotate + 270) % 360; }], ['⟳ Right', () => { st.rotate = (st.rotate + 90) % 360; }],
      ['⇋ Flip', () => { st.flipX = !st.flipX; }], ['⇅ Flip', () => { st.flipY = !st.flipY; }]];
    q('.lv-ed-turns').append(...turns.map(([l, f]) => button('', l, () => { f(); draw(); })));
    q('.lv-ed-presets').append(...Object.entries(PRESETS).map(([name, p]) => button('lv-ed-preset', name, () => {
      for (const [k, , , , d] of SLIDERS) if (!k.startsWith('skew')) st[k] = p[k] ?? d;
      draw();
    })));
    for (const [k, label, min, max] of SLIDERS) {
      const row = make('label', 'lv-ed-slider');
      const input = make('input');
      Object.assign(input, { type: 'range', min, max, step: 1, name: k });
      input.oninput = () => { st[k] = +input.value; draw(); };
      row.append(make('span', '', label), input, make('i', '', ''));
      q('.lv-ed-sliders').append(row);
    }
    const close = () => ed.remove();
    q('.lv-ed-close').onclick = close;
    ed.addEventListener('keydown', e => { if (e.key === 'Escape') close(); });
    async function render() {
      const bmp = await createImageBitmap(await (await fetch(vaultUrl(item.id))).blob());
      const w = bmp.width, h = bmp.height, sx = Math.tan(st.skewX * Math.PI / 180), sy = Math.tan(st.skewY * Math.PI / 180);
      // the size after skewing and turning
      const sw = w + Math.abs(sx) * h, sh = h + Math.abs(sy) * w;
      const side = st.rotate % 180 !== 0;
      const c = new OffscreenCanvas(Math.round(side ? sh : sw), Math.round(side ? sw : sh));
      const ctx = c.getContext('2d');
      ctx.translate(c.width / 2, c.height / 2);
      ctx.rotate(st.rotate * Math.PI / 180);
      ctx.scale(st.flipX ? -1 : 1, st.flipY ? -1 : 1);
      ctx.transform(1, sy, sx, 1, 0, 0);
      ctx.filter = filterOf().replace(/blur\(([\d.]+)px\)/, (_, b) => `blur(${b * Math.max(w, h) / 1000}px)`); // softening scaled to the picture
      ctx.drawImage(bmp, -w / 2, -h / 2);
      ctx.setTransform(1, 0, 0, 1, 0, 0);
      ctx.filter = 'none';
      if (st.vignette) {
        const g = ctx.createRadialGradient(c.width / 2, c.height / 2, Math.min(c.width, c.height) * 0.35, c.width / 2, c.height / 2, Math.hypot(c.width, c.height) / 2);
        g.addColorStop(0, 'rgba(0,0,0,0)'); g.addColorStop(1, `rgba(0,0,0,${st.vignette / 100 * 0.75})`);
        ctx.fillStyle = g; ctx.fillRect(0, 0, c.width, c.height);
      }
      bmp.close();
      return c.convertToBlob({ type: 'image/webp', quality: S('webpQuality') / 100 });
    }
    async function save(replace) {
      ed.classList.add('busy');
      try {
        const blob = await render();
        const file = new File([blob], item.file.replace(/\.[^.]+$/, '') + '-edit.webp', { type: 'image/webp' });
        jobs.push({ file, category: item.category, nsfw: item.nsfw, quality: item.quality, collection: item.collection || '', tags: item.tags || '', source: item.source || '' });
        pumpUploads();
        if (replace) {
          const ids = [item.id];
          trashedHere(await api('trashImages', ids), ids);
          closeDetail();
        }
        toast(replace ? 'Saved; the original is in the Recycle Bin' : 'Saved as a new item', 'ok');
        close();
        renderLibrary();
      } catch (e) {
        ed.classList.remove('busy');
        toast(errMsg(e), 'err');
      }
    }
    q('.lv-ed-save').append(button('primary', 'Save as new', () => save(false)), button('', 'Replace', () => {
      if (confirm('Save the edited picture and send the original to the Recycle Bin?')) save(true);
    }));
    draw();
  }

  // =====================================================================================
  // Clean up: Find duplicates
  // =====================================================================================
  const cleanupBar = document.querySelector('#tab-cleanup .cleanup-bar');
  const toolRow = make('div', 'lv-cleanup-tools');
  cleanupBar.after(toolRow);
  toolRow.append(
    button('', '⧉ Find duplicates', () => findDuplicates()),
    button('', '🎞 Compact videos', () => compactVideos()));
  async function findDuplicates() {
    trayShow('Finding duplicates', 'Comparing every picture…', 0);
    trayActions();
    try {
      await api('findDuplicates');
      let p;
      do {
        await sleep(400);
        p = await api('duplicateProgress');
        if (p.total) trayShow('Finding duplicates', `${fmtN(p.done)} of ${fmtN(p.total)} looked at`, p.done / p.total);
      } while (p.status === 'running');
      if (p.status !== 'done') throw new Error(p.status || 'Stopped');
      const byId = new Map(library.map(i => [i.id, i]));
      const groups = (p.groups || []).map(g => g.map(id => byId.get(id)).filter(Boolean)).filter(g => g.length > 1);
      const extra = groups.reduce((n, g) => n + g.length - 1, 0);
      trayShow('Duplicates', groups.length ? `${fmtN(groups.length)} ${groups.length === 1 ? 'group' : 'groups'}, ${fmtN(extra)} extra ${extra === 1 ? 'copy' : 'copies'}` : 'No duplicates. Every item is one of a kind.', -1);
      trayActions(...(groups.length ? [button('primary', 'Review', () => reviewDuplicates(groups))] : []), button('', 'Close', () => tray.classList.add('hidden')));
      if (groups.length) reviewDuplicates(groups);
    } catch (e) {
      trayShow('Duplicates', errMsg(e), -1);
      trayActions(button('', 'Close', () => tray.classList.add('hidden')));
    }
  }
  window.vaultReviewDuplicates = (groups, title, lead) => reviewDuplicates(groups, title, lead); // (extras.js: look-alikes)
  function reviewDuplicates(groups, title, lead) {
    const { body, close } = sheet(title || 'Duplicates', lead || 'Each row is one picture or video saved more than once. The one marked Keep is the biggest (click another to keep that one instead). "Bin the others" sends the rest to the Recycle Bin.');
    const keep = new Map(groups.map(g => [g, g[0]]));
    const all = button('danger', 'Bin every extra copy', async () => {
      const gone = groups.flatMap(g => g.filter(i => i !== keep.get(g)));
      if (!gone.length || !confirm(`Send ${gone.length} extra copies to the Recycle Bin?`)) return;
      const ids = gone.map(i => i.id);
      trashedHere(await api('trashImages', ids), ids);
      toast(`${ids.length} copies in the Recycle Bin`, 'ok');
      close(); renderLibrary(); renderCleanup();
    });
    body.append(all);
    for (const g of groups) {
      const row = make('div', 'lv-dup');
      const draw = () => {
        row.replaceChildren();
        for (const it of g) {
          const b = make('button', 'lv-dup-item' + (keep.get(g) === it ? ' keep' : ''));
          b.type = 'button';
          const im = make('img'); im.src = thumbSrc(it, 240); im.alt = ''; im.loading = 'lazy';
          b.append(im, make('span', 'lv-dup-cap', `${it.width}×${it.height} · ${it.category}`), make('span', 'lv-dup-keep', 'Keep'));
          b.onclick = () => { keep.set(g, it); draw(); };
          row.append(b);
        }
        row.append(button('danger lv-dup-bin', 'Bin the others', async () => {
          const ids = g.filter(i => i !== keep.get(g)).map(i => i.id);
          try { trashedHere(await api('trashImages', ids), ids); row.remove(); groups.splice(groups.indexOf(g), 1); renderLibrary(); renderCleanup(); }
          catch (e) { toast(errMsg(e), 'err'); }
        }));
      };
      draw();
      body.append(row);
    }
  }

  // =====================================================================================
  // Clean up: Compact videos
  // =====================================================================================
  // Videos (phones especially) are often stored at a much higher bitrate than needed. Each is
  // re-encoded at the same resolution (H.264) to about 0.08 bits per pixel per frame, the sound kept as
  // it is. The new file is used only if it's at least 15% smaller and as long as the old one; a copy
  // of the original goes to the Recycle Bin first. It replaces the file in place, so views, time,
  // tags and everything else stay.
  const BPP = 0.08, FPS_GUESS = 30;
  let compacting = false;
  async function sizeOf(item) {
    const r = await fetch(vaultUrl(item.id), { headers: { Range: 'bytes=0-0' } });
    const m = (r.headers.get('Content-Range') || '').match(/\/(\d+)/);
    try { await r.body?.cancel(); } catch (e) {}
    return m ? +m[1] : +(r.headers.get('Content-Length') || 0);
  }
  const targetBitrate = (item, cap) => { const [w, h] = cap ? capped(item) : [item.width, item.height]; return Math.max(600e3, w * h * FPS_GUESS * BPP); };
  const capped = item => { const s = Math.min(1, 1080 / Math.min(item.width, item.height)); return [even(item.width * s), even(item.height * s)]; };
  async function compactVideos() {
    if (compacting) { tray.classList.remove('hidden', 'small'); return; }
    const videos = library.filter(i => i.kind === 'video' && i.width && i.height && i.duration > 2);
    trayShow('Compact videos', 'Measuring your videos…', 0);
    trayActions();
    const found = [];
    for (let k = 0; k < videos.length; k++) {
      const item = videos[k];
      try {
        const size = await sizeOf(item);
        const rate = size * 8 / item.duration;
        if (rate > targetBitrate(item) * 1.6) found.push({ item, size, saving: 1 - targetBitrate(item) / rate });
      } catch (e) {}
      if (k % 10 === 0) trayShow('Compact videos', `Measuring ${fmtN(k + 1)} of ${fmtN(videos.length)}`, (k + 1) / videos.length);
    }
    tray.classList.add('hidden');
    found.sort((a, b) => b.size * b.saving - a.size * a.saving);
    const { body, close } = sheet('Compact videos', found.length
      ? 'These videos are stored much bigger than they need to be. Each is re-encoded at the same resolution (and the same sound), like Google Photos does. A copy of every original goes to the Recycle Bin first, and a new file is used only if it is at least 15% smaller.'
      : 'Every video is already compact. Nothing to do.');
    if (!found.length) return;
    const total = found.reduce((n, f) => n + f.size, 0), save = found.reduce((n, f) => n + f.size * f.saving, 0);
    const cap = make('label', 'lv-cv-cap');
    const capBox = make('input'); capBox.type = 'checkbox';
    cap.append(capBox, make('span', '', 'Also bring anything bigger than 1080p down to 1080p (saves much more, like Google Photos\' Storage saver)'));
    const list = make('div', 'lv-cv-list');
    for (const f of found.slice(0, 200)) {
      const row = make('div', 'lv-cv-row');
      const im = make('img'); im.src = thumbSrc(f.item, 240); im.alt = '';
      row.append(im, make('span', '', `${f.item.file}`), make('span', 'lv-info-dim', `${f.item.width}×${f.item.height} · ${fmtTime(f.item.duration)}`), make('b', '', `${fmtBytes(f.size)} → about ${fmtBytes(f.size * (1 - f.saving))}`));
      list.append(row);
    }
    body.append(make('p', 'lv-cv-sum', `${fmtN(found.length)} videos, ${fmtBytes(total)}. About ${fmtBytes(save)} could be freed.`), cap,
      button('primary', `Compact ${fmtN(found.length)} videos`, () => { close(); runCompact(found, capBox.checked); }), list);
  }
  async function runCompact(found, cap1080) {
    compacting = true;
    let saved = 0, done = 0, stop = false;
    const stopBtn = button('', 'Stop after this one', () => { stop = true; stopBtn.disabled = true; });
    try {
      for (const f of found) {
        if (stop) break;
        const item = f.item;
        const label = `${fmtN(done + 1)} of ${fmtN(found.length)} · ${fmtBytes(saved)} freed`;
        trayShow('Compacting videos', label, 0);
        trayActions(stopBtn);
        try {
          const mb = await loadMediabunny();
          const input = new mb.Input({ source: new mb.UrlSource(vaultUrl(item.id)), formats: mb.ALL_FORMATS });
          let buf, dur;
          try {
            const output = new mb.Output({ format: new mb.Mp4OutputFormat({ fastStart: 'in-memory' }), target: new mb.BufferTarget() });
            const [w, h] = cap1080 ? capped(item) : [item.width, item.height];
            const conversion = await mb.Conversion.init({
              input, output, tracks: 'primary', showWarnings: false,
              video: { codec: 'avc', bitrate: Math.round(targetBitrate(item, cap1080)), ...(w !== item.width ? { width: w, height: h, fit: 'fill' } : {}), forceTranscode: true },
              audio: {}
            });
            const lost = conversion.discardedTracks.filter(d => d.reason !== 'discarded_by_user');
            if (!conversion.isValid || lost.length) throw new Error('cannot convert this one here');
            conversion.onProgress = p => trayShow('Compacting videos', `${label} · ${item.file}`, p);
            await conversion.execute();
            buf = output.target.buffer;
            dur = await input.computeDuration();
          } finally { input.dispose(); }
          if (buf.byteLength > f.size * 0.85) continue; // not worth it
          if (Math.abs(dur - item.duration) > Math.max(1, item.duration * 0.02)) throw new Error('the new copy is not as long as the original');
          await api('backupToBin', item.id);
          await driveUpload(new Blob([buf], { type: 'video/mp4' }), item.file, await categoryFolder(item.category));
          if (item.streamId) { libraryWritten(await api('clearStream', item.id)); item.streamId = ''; }
          saved += f.size - buf.byteLength;
        } catch (e) {
          console.warn(`${item.file} not compacted: ${errMsg(e)}`);
        } finally { done++; }
      }
      trayShow('Videos compacted', `${fmtN(done)} done · ${fmtBytes(saved)} freed. The originals are in the Recycle Bin.`, -1);
    } finally {
      compacting = false;
      trayActions(button('', 'Close', () => tray.classList.add('hidden')));
    }
  }
  // =====================================================================================
  // The category: the Library's main choice, a big square showing what's in it; its menu shows
  // every category with a picture
  // =====================================================================================
  {
    const fCat = $('fCat'), filters = document.querySelector('#tab-library .filters');
    const row = make('div', 'lib-filterrow');
    filters.before(row);
    const tile = make('button', 'cat-tile'); tile.type = 'button'; tile.title = 'Pick a category';
    row.append(tile, filters);
    fCat.closest('div').classList.add('cat-hidden');
    const pop = make('div', 'cat-pop hidden');
    document.body.append(pop);
    const shown = i => libraryOk(i);
    const coverOf = items => items.slice().sort((a, b) => (b.quality - a.quality) || b.added.localeCompare(a.added)).slice(0, 4);
    const mosaic = (items, cls) => {
      const m = make('span', cls);
      for (const it of items) { const im = make('img'); im.alt = ''; im.loading = 'lazy'; im.src = thumbSrc(it, 200); m.append(im); }
      m.dataset.n = items.length;
      return m;
    };
    function drawTile() {
      if (!library) return;
      const c = fCat.value, items = library.filter(i => shown(i) && (!c || i.category === c));
      tile.replaceChildren(mosaic(coverOf(items), 'cat-mosaic'), make('span', 'cat-shade'));
      const t = make('span', 'cat-text');
      t.append(make('small', '', 'Category'), make('b', '', c || 'All'), make('span', 'cat-count', `${fmtN(items.length)} ${items.length === 1 ? 'item' : 'items'}`));
      tile.append(t, make('span', 'cat-chev', '▾'));
    }
    function openPop() {
      const cats = [...new Set(library.filter(shown).map(i => i.category))].map(c => [c, library.filter(i => shown(i) && i.category === c)]).sort((a, b) => b[1].length - a[1].length);
      const card = (name, items, value) => {
        const b = make('button', 'cat-card' + (fCat.value === value ? ' on' : ''));
        b.type = 'button';
        b.append(mosaic(coverOf(items), 'cat-mosaic'), make('span', 'cat-shade'));
        const t = make('span', 'cat-text'); t.append(make('b', '', name), make('span', 'cat-count', fmtN(items.length)));
        b.append(t);
        b.onclick = () => { fCat.value = value; fCat.dispatchEvent(new Event('change', { bubbles: true })); closePop(); };
        return b;
      };
      pop.replaceChildren(card('All', library.filter(shown), ''), ...cats.map(([c, items]) => card(c, items, c)));
      const r = tile.getBoundingClientRect();
      pop.style.left = r.left + 'px'; pop.style.top = (r.bottom + 8) + 'px';
      pop.classList.remove('hidden');
    }
    const closePop = () => pop.classList.add('hidden');
    tile.onclick = () => pop.classList.contains('hidden') ? openPop() : closePop();
    addEventListener('pointerdown', e => { if (!pop.contains(e.target) && !tile.contains(e.target)) closePop(); }, true);
    addEventListener('keydown', e => { if (e.key === 'Escape') closePop(); });
    addEventListener('scroll', closePop, { passive: true });
    fCat.addEventListener('change', drawTile);
    const renderLibraryCat = renderLibrary;
    renderLibrary = function () { const r = renderLibraryCat.apply(this, arguments); drawTile(); return r; };
  }

  // =====================================================================================
  // The Tags panel on the right: drag its edge to make it wider or narrower, or fold it away
  // (both remembered)
  // =====================================================================================
  {
    const layout = document.querySelector('#tab-library .lib-layout'), panel = layout && layout.querySelector('.tag-panel');
    if (panel) {
      const KEY = 'imageVault.tagPanel';
      let pref = {};
      try { pref = JSON.parse(localStorage.getItem(KEY)) || {}; } catch (e) {}
      const keep = () => { try { localStorage.setItem(KEY, JSON.stringify(pref)); } catch (e) {} };
      const grip = make('div', 'tp-grip'); grip.title = 'Drag: wider or narrower · double-click: back to normal';
      const fold = make('button', 'tp-fold'); fold.type = 'button';
      layout.append(grip, fold); // (outside the panel, which redraws its insides)
      const relayout = () => dispatchEvent(new Event('resize'));
      const apply = () => {
        layout.style.setProperty('--tagw', (pref.w || 280) + 'px');
        layout.classList.toggle('tags-folded', !!pref.folded);
        fold.innerHTML = pref.folded ? '<span>‹</span><small>Tags</small>' : '<span>›</span>';
        fold.title = pref.folded ? 'Show the tags' : 'Fold the tags away';
      };
      fold.onclick = () => { pref.folded = !pref.folded; keep(); apply(); relayout(); };
      grip.addEventListener('dblclick', () => { pref.w = 280; keep(); apply(); relayout(); });
      grip.addEventListener('pointerdown', e => {
        grip.setPointerCapture(e.pointerId);
        const x0 = e.clientX, w0 = pref.w || 280;
        layout.classList.add('tp-resizing');
        const move = ev => { pref.w = Math.round(Math.max(200, Math.min(620, w0 + (x0 - ev.clientX)))); apply(); };
        const up = () => { grip.removeEventListener('pointermove', move); grip.removeEventListener('pointerup', up); layout.classList.remove('tp-resizing'); keep(); relayout(); };
        grip.addEventListener('pointermove', move); grip.addEventListener('pointerup', up);
      });
      apply();
    }
  }

  // =====================================================================================
  // ★★★ glows pink: a small mark on great items, the details' rating
  // =====================================================================================
  {
    const makeCardStars = makeCard;
    makeCard = function (item) {
      const card = makeCardStars.apply(this, arguments);
      if (item.quality === 3) { const b = make('span', 'q3-mark', '★★★'); b.title = '★★★ great'; card.append(b); }
      return card;
    };
    const dQ = $('dQ');
    const drawQ = () => dQ.classList.toggle('q3', dQ.value === '3');
    dQ.addEventListener('change', drawQ);
    const openDetailStars = openDetail;
    openDetail = function () { const r = openDetailStars.apply(this, arguments); drawQ(); return r; };
  }
})();
