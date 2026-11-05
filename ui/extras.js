// Lewd Vault desktop: the idea-board batch (November 2026). Each part stands on its own:
//   1. Library toolbar in one row: Sort, a Filters button (Type, NSFW, Rating, Layout), active filters as chips
//   2. Top bar: streak, coach, stopwatch and profile as one cluster
//   3. Settings you can search, with an index of its sections; a live preview of the themes
//   4. Friendly empty Library screen
//   5. Full view: a filmstrip of the neighbours; ? shows every keyboard shortcut
//   6. Library: select many (Ctrl/Shift+click) and change them all at once
//   7. Clean up: Find look-alikes (resized / re-saved copies), worked out in a Web Worker
//   8. Downloads panel for everything being saved from Images, Videos and Audio
//   9. PIN lock (on start, after being away, Ctrl+L)
//  10. Backup, health check and updates (Settings → Maintenance)
//  11. Adapt to the PC: fewer moving previews when scrolling is slow
//  12. Ctrl+Shift+P: a small performance overlay
(() => {
  'use strict';
  if (!window.vaultUI || !inTauri) return;
  const { make, thumbSrc } = window.vaultUI;
  const html = document.documentElement;
  const typing = e => e.target.closest && e.target.closest('input, textarea, select, [contenteditable]');
  const button = (cls, text, run) => { const b = make('button', 'btn' + (cls ? ' ' + cls : ''), text); b.type = 'button'; if (run) b.onclick = run; return b; };
  const gb = n => n >= 1e9 ? (n / 1e9).toFixed(1) + ' GB' : (n / 1e6).toFixed(n < 1e7 ? 1 : 0) + ' MB';
  // renderLibrary, renderSettings and openDetail are the page's own top-level functions, so they live on
  // window and every call by name goes through it: a wrapper here is seen everywhere. The extra work runs
  // right after the call starts (openDetail sets what it shows before its first await).
  const wrapGlobal = (name, fn) => {
    const orig = window[name];
    if (typeof orig !== 'function') return console.warn('extras: no ' + name);
    window[name] = function () { const r = orig.apply(this, arguments); try { fn.apply(this, arguments); } catch (e) { console.warn(e); } return r; };
  };

  // =====================================================================================
  // 1. The Library toolbar in one row
  // =====================================================================================
  const filters = document.querySelector('#tab-library .filters');
  if (filters) {
    const field = id => $(id) && $(id).closest('div');
    const pop = make('div', 'lf-pop hidden');
    for (const id of ['fKind', 'fNsfw', 'fQ', 'fLayout']) { const f = field(id); if (f) pop.append(f); }
    document.body.append(pop);
    const fbtn = make('button', 'lf-btn'); fbtn.type = 'button'; fbtn.title = 'Type, NSFW level, rating and layout';
    const chips = make('div', 'lf-chips');
    const sortField = field('fSort');
    (sortField || filters.firstChild).after(fbtn, chips);
    const DEF = { fKind: '', fNsfw: '10', fQ: '0' };
    const fire = id => $(id).dispatchEvent(new Event('change', { bubbles: true }));
    function drawChips() {
      const on = Object.keys(DEF).filter(id => $(id) && $(id).value !== DEF[id]);
      fbtn.innerHTML = `<span>Filters</span>${on.length ? `<b>${on.length}</b>` : ''}<i>▾</i>`;
      chips.replaceChildren(...on.map(id => {
        const sel = $(id), text = sel.options[sel.selectedIndex] ? sel.options[sel.selectedIndex].text : sel.value;
        const c = make('button', 'lf-chip'); c.type = 'button'; c.title = 'Remove this filter';
        c.append(make('span', '', id === 'fNsfw' ? `NSFW up to ${sel.value}` : text), make('i', '', '×'));
        c.onclick = () => { sel.value = DEF[id]; fire(id); drawChips(); };
        return c;
      }));
    }
    const closePop = () => pop.classList.add('hidden');
    fbtn.onclick = () => {
      if (!pop.classList.contains('hidden')) return closePop();
      const r = fbtn.getBoundingClientRect();
      Object.assign(pop.style, { left: r.left + 'px', top: r.bottom + 8 + 'px' });
      pop.classList.remove('hidden');
    };
    addEventListener('pointerdown', e => { if (!pop.contains(e.target) && !fbtn.contains(e.target)) closePop(); }, true);
    addEventListener('keydown', e => { if (e.key === 'Escape') closePop(); });
    addEventListener('scroll', closePop, { passive: true });
    for (const id of Object.keys(DEF)) if ($(id)) $(id).addEventListener('change', drawChips);
    drawChips();
  }

  // =====================================================================================
  // 2. The top bar's little buttons as one cluster
  // =====================================================================================
  {
    const bar = document.querySelector('.titlebar');
    const parts = ['.tb-streak', '.tb-coach', '.tb-clock', '.tb-me'].map(s => bar && bar.querySelector(s)).filter(Boolean);
    if (parts.length) {
      const cluster = make('div', 'tb-cluster');
      parts[0].before(cluster);
      cluster.append(...parts);
    }
  }

  // =====================================================================================
  // 3. Settings: search, an index of the sections, and a live preview of the themes
  // =====================================================================================
  const settingsTab = $('tab-settings');
  const search = make('div', 'st-search');
  search.innerHTML = '<span class="search-icon"></span><input type="search" id="stFind" placeholder="Find a setting: preview, theme, key, backup…" spellcheck="false" autocomplete="off">';
  const index = make('div', 'st-index');
  settingsTab.prepend(search, index);
  const findInput = search.querySelector('input');
  // the page's blocks, in order: headings start a section, the rest are its settings
  function blocks() {
    const out = [];
    const walk = parent => {
      for (const n of parent.children) {
        if (n === search || n === index || n.classList.contains('decor-rail')) continue;
        if (n.id === 'settingsForm' || n.classList.contains('st-group') || n.querySelector(':scope > .setting, :scope > h3')) { walk(n); continue; }
        out.push(n);
      }
    };
    walk(settingsTab);
    return out;
  }
  function applyFind() {
    const q = findInput.value.trim().toLowerCase();
    let head = null, headText = '', shownInSection = 0;
    const heads = [];
    const finish = () => { if (head) head.classList.toggle('st-hide', !!q && !shownInSection); };
    for (const n of blocks()) {
      if (n.tagName === 'H3') { finish(); head = n; headText = n.textContent.toLowerCase(); shownInSection = 0; heads.push(n); continue; }
      const hit = !q || headText.includes(q) || n.textContent.toLowerCase().includes(q);
      n.classList.toggle('st-hide', !hit);
      if (hit) shownInSection++;
    }
    finish();
    index.replaceChildren(...heads.filter(h => !h.classList.contains('st-hide')).map(h => {
      const b = make('button', 'st-jump', h.textContent); b.type = 'button';
      b.onclick = () => h.scrollIntoView({ behavior: 'smooth', block: 'start' });
      return b;
    }));
  }
  findInput.addEventListener('input', applyFind);
  findInput.addEventListener('keydown', e => { if (e.key === 'Escape') { findInput.value = ''; applyFind(); } });
  function themePreview() {
    const sw = document.querySelector('#settingsForm .swatches');
    if (!sw || sw.nextElementSibling && sw.nextElementSibling.classList.contains('theme-preview')) return;
    const pv = make('div', 'theme-preview');
    pv.innerHTML = `<div class="tp-side"><b>Library</b><span>124 items</span></div>
      <div class="tp-main"><div class="tp-row"><span class="tp-chip on">Newest</span><span class="tp-chip">Filters</span></div>
      <div class="tp-cards"><span class="tp-card a"><i>★★★</i></span><span class="tp-card b"></span><span class="tp-card c"><i>▶ 0:20</i></span></div>
      <div class="tp-row"><span class="tp-btn">Save</span><span class="tp-tag">blue hair</span><span class="tp-dim">How it looks</span></div></div>`;
    const show = key => { pv.dataset.theme = key; };
    show(S('theme'));
    sw.after(pv);
    for (const b of sw.querySelectorAll('.swatch')) {
      b.addEventListener('mouseenter', () => show(b.dataset.theme));
      b.addEventListener('focus', () => show(b.dataset.theme));
      b.addEventListener('mouseleave', () => show(S('theme')));
    }
  }
  wrapGlobal('renderSettings', () => { themePreview(); applyFind(); });
  themePreview(); applyFind();

  // =====================================================================================
  // 4. A friendly empty Library
  // =====================================================================================
  // (the grid is watched rather than renderLibrary wrapped: the page's own filter menus call the
  // original function directly)
  const onGrid = fn => new MutationObserver(() => { try { fn(); } catch (e) { console.warn(e); } }).observe($('grid'), { childList: true });
  onGrid(() => {
    const empty = document.querySelector('#grid > .empty');
    if (!empty || empty.classList.contains('lv-empty')) return;
    const any = library && library.length;
    empty.classList.add('lv-empty');
    const art = make('img', 'lv-empty-art'); art.src = '/decor/corset.webp'; art.alt = '';
    const text = make('div', 'lv-empty-text');
    text.append(make('b', '', any ? 'Nothing matches these filters' : 'Your vault is empty'),
      make('span', '', any ? 'Loosen a filter, or clear them all to see everything again.' : 'Add pictures and videos from your PC, or find some in the Browser.'));
    const acts = make('div', 'lv-empty-acts');
    if (any) acts.append(button('primary', 'Clear all filters', () => {
      for (const [id, v] of [['fCat', ''], ['fKind', ''], ['fNsfw', '10'], ['fQ', '0']]) if ($(id)) $(id).value = v;
      const s = document.querySelector('#tab-library .lib-search input, #tab-library input[type=search]');
      if (s && s.value) { s.value = ''; s.dispatchEvent(new Event('input', { bubbles: true })); }
      $('fCat').dispatchEvent(new Event('change', { bubbles: true }));
      renderLibrary();
    }));
    else acts.append(button('primary', 'Add files', () => document.querySelector('nav [data-tab="add"]').click()),
      button('', 'Open the Browser', () => document.querySelector('nav [data-tab="discover"]').dispatchEvent(new MouseEvent('click', { bubbles: true }))));
    text.append(acts);
    empty.replaceChildren(art, text);
  });

  // =====================================================================================
  // 5. Full view: filmstrip, and ? for the keyboard shortcuts
  // =====================================================================================
  const strip = make('div', 'filmstrip hidden');
  document.querySelector('#detail .big').append(strip);
  function drawStrip() {
    // opened from the Library's grid (which gives no list): the tiles shown there, in their order
    let list = detailList;
    if (!list && !$('tab-library').classList.contains('hidden')) {
      const shown = [...$('grid').querySelectorAll('.card')].map(c => c._item).filter(Boolean);
      if (shown.length > 1 && shown.includes(current)) list = shown;
    }
    const at = list ? list.indexOf(current) : -1;
    if (!list || at < 0) return strip.classList.add('hidden');
    const from = Math.max(0, Math.min(at - 7, list.length - 15)), shown = list.slice(from, from + 15);
    strip.replaceChildren(...shown.map(it => {
      const b = make('button', 'fs-item' + (it === current ? ' on' : '')); b.type = 'button';
      b.title = `${it.category} · ${'★'.repeat(it.quality || 1)}`;
      const im = make('img'); im.alt = ''; im.loading = 'lazy'; im.src = thumbSrc(it, 160);
      b.append(im);
      if (it.kind !== 'image' && !/\.gif$/i.test(it.file)) b.append(make('i', '', '▶'));
      b.onclick = e => { e.stopPropagation(); if (it !== current) openDetail(it, list); };
      return b;
    }));
    strip.append(make('span', 'fs-count', `${at + 1} / ${list.length}`));
    strip.classList.remove('hidden');
    const on = strip.querySelector('.on');
    if (on) on.scrollIntoView({ block: 'nearest', inline: 'center' });
  }
  wrapGlobal('openDetail', drawStrip);
  const KEYS = [
    ['Anywhere', [['Ctrl+1 … 6', 'Go to a section'], ['/', 'Search the Library'], ['Ctrl+L', 'Lock with your PIN'], ['Ctrl+Shift+P', 'Performance overlay'], ['?', 'This list']]],
    ['Library', [['Ctrl+click', 'Select tiles'], ['Shift+click', 'Select a range'], ['Esc', 'Clear the selection'], ['Hold a tile', 'Make it bigger']]],
    ['Full view', [['← →', 'Previous / next'], ['A', 'Appreciate'], ['Esc', 'Close']]],
    ['Videos', [['Space or K', 'Play / pause'], ['J  L', 'Back / forward 10 s'], ['Shift+← →', '5 s'], [', .', 'A frame at a time'], ['[ ]', 'Slower / faster'], ['M  F', 'Mute / full screen']]],
    ['Slideshow', [['Space', 'Pause'], ['← →', 'Skip'], ['↑ ↓', 'Speed'], ['Esc', 'Leave']]]
  ];
  const keysBox = make('div', 'keys-sheet hidden');
  keysBox.innerHTML = '<div class="keys-card"><div class="keys-head"><b>Keyboard shortcuts</b><button class="btn" type="button">Close</button></div><div class="keys-grid"></div></div>';
  keysBox.querySelector('.keys-grid').append(...KEYS.map(([group, list]) => {
    const g = make('div', 'keys-group'); g.append(make('h4', '', group));
    for (const [k, what] of list) { const r = make('div', 'keys-row'); r.append(make('kbd', '', k), make('span', '', what)); g.append(r); }
    return g;
  }));
  document.body.append(keysBox);
  const toggleKeys = on => keysBox.classList.toggle('hidden', on === undefined ? !keysBox.classList.contains('hidden') : !on);
  keysBox.querySelector('button').onclick = () => toggleKeys(false);
  keysBox.addEventListener('pointerdown', e => { if (e.target === keysBox) toggleKeys(false); });
  addEventListener('keydown', e => {
    if (typing(e)) return;
    if (e.key === '?') { e.preventDefault(); toggleKeys(); }
    else if (e.key === 'Escape' && !keysBox.classList.contains('hidden')) { e.stopPropagation(); toggleKeys(false); }
  }, true);

  // =====================================================================================
  // 6. Select many, change many
  // =====================================================================================
  const grid = $('grid');
  const picked = new Set();
  let anchor = null;
  const msBar = make('div', 'ms-bar hidden');
  document.body.append(msBar);
  const cardsShown = () => [...grid.querySelectorAll('.card')].filter(c => c._item);
  function markCards() { for (const c of grid.querySelectorAll('.card')) c.classList.toggle('picked-many', !!c._item && picked.has(c._item)); }
  function drawBar(text) {
    msBar.classList.toggle('hidden', !picked.size);
    if (!picked.size) return;
    const n = picked.size;
    msBar.replaceChildren();
    msBar.append(make('b', 'ms-count', text || `${n} selected`));
    const group = (label, ...kids) => { const g = make('div', 'ms-group'); g.append(make('span', 'ms-label', label), ...kids); msBar.append(g); return g; };
    group('Rating', ...[1, 2, 3].map(q => button('ms-star', '★'.repeat(q), () => change({ quality: q }))));
    const nsfw = make('select'); nsfw.append(new Option('NSFW…', ''), ...Array.from({ length: 10 }, (_, k) => new Option(String(k + 1), String(k + 1))));
    nsfw.onchange = () => nsfw.value && change({ nsfw: +nsfw.value });
    group('NSFW', nsfw);
    const cat = make('input'); Object.assign(cat, { placeholder: 'Category', spellcheck: false }); cat.setAttribute('list', 'catList');
    group('Move to', cat, button('', 'Set', () => cat.value.trim() && change({ category: cat.value.trim() })));
    const col = make('input'); Object.assign(col, { placeholder: 'Collection (empty: none)', spellcheck: false }); col.setAttribute('list', 'colList');
    group('Collection', col, button('', 'Set', () => setCol(col.value.trim())));
    const tags = make('input'); Object.assign(tags, { placeholder: 'tags like blue_hair', spellcheck: false });
    group('Tags', tags, button('', 'Add', () => tagChange(tags.value, 'add')), button('', 'Remove', () => tagChange(tags.value, 'remove')));
    const end = make('div', 'ms-group ms-end');
    end.append(button('', 'Select all shown', () => { for (const c of cardsShown()) picked.add(c._item); markCards(); drawBar(); }),
      button('danger', 'Bin', () => binPicked()), button('', 'Done', () => clearPick()));
    msBar.append(end);
  }
  function clearPick() { picked.clear(); anchor = null; markCards(); drawBar(); }
  grid.addEventListener('click', e => {
    const card = e.target.closest('.card');
    if (!card || !card._item || !(e.ctrlKey || e.metaKey || e.shiftKey || picked.size)) return;
    e.stopImmediatePropagation(); e.preventDefault();
    if (e.shiftKey && anchor && anchor.isConnected) {
      const all = cardsShown(), a = all.indexOf(anchor), b = all.indexOf(card);
      for (const c of all.slice(Math.min(a, b), Math.max(a, b) + 1)) picked.add(c._item);
    } else {
      picked.has(card._item) ? picked.delete(card._item) : picked.add(card._item);
      anchor = card;
    }
    markCards(); drawBar();
  }, true);
  addEventListener('keydown', e => { if (e.key === 'Escape' && picked.size && !typing(e) && $('detail').classList.contains('hidden')) clearPick(); });
  onGrid(() => { for (const it of [...picked]) if (!library.includes(it)) picked.delete(it); markCards(); if (!picked.size) drawBar(); });
  async function change(f) {
    const items = [...picked];
    let done = 0, failedN = 0;
    for (const it of items) {
      drawBar(`Changing ${++done} of ${items.length}…`);
      try {
        const fields = { category: it.category, nsfw: it.nsfw, quality: it.quality, ...f };
        const r = await api('updateImage', it.id, fields);
        Object.assign(it, fields, { uid: r.item.uid, file: r.item.file });
        libraryWritten(r);
      } catch (e) { failedN++; console.warn(errMsg(e)); }
    }
    if (f.category) addCategory(f.category);
    toast(failedN ? `${items.length - failedN} changed, ${failedN} not` : `${items.length} changed`, failedN ? 'err' : 'ok');
    renderLibrary(); drawBar();
  }
  async function setCol(name) {
    try { await setCollectionOf([...picked].map(i => i.id), name); toast(name ? `In ${name}` : 'Out of their collections', 'ok'); renderLibrary(); }
    catch (e) { toast(errMsg(e), 'err'); }
  }
  async function tagChange(text, mode) {
    const tags = window.vaultUI.cleanTags(String(text).split(/[\s,]+/).filter(Boolean));
    if (!tags.length) return;
    try { libraryWritten(await api('setTags', [...picked].map(i => i.id), tags.join(' '), mode)); toast(mode === 'add' ? 'Tags added' : 'Tags removed', 'ok'); renderLibrary(); }
    catch (e) { toast(errMsg(e), 'err'); }
  }
  async function binPicked() {
    const ids = [...picked].map(i => i.id);
    if (!confirm(`Send ${ids.length} ${ids.length === 1 ? 'item' : 'items'} to the Recycle Bin?`)) return;
    try { trashedHere(await api('trashImages', ids), ids); toast(`${ids.length} in the Recycle Bin`, 'ok'); clearPick(); renderLibrary(); }
    catch (e) { toast(errMsg(e), 'err'); }
  }

  // =====================================================================================
  // 7. Clean up: Find look-alikes (resized, re-saved or slightly cropped copies), in a Web Worker
  // =====================================================================================
  // Each item's thumbhash (already in index.csv) is decoded to a tiny picture, made grey and 9×8,
  // and turned into a 64-bit "difference hash"; two items whose hashes differ in at most 5 of 64
  // bits, with nearly the same shape and the same colours in the same places, are look-alikes. All of it runs off the page's main thread.
  const WORKER = `
    import { thumbHashToRGBA } from '${location.origin}/vendor/thumbhash.js';
    const b64 = s => Uint8Array.from(atob(s.replace(/-/g, '+').replace(/_/g, '/') + '='.repeat((4 - s.length % 4) % 4)), c => c.charCodeAt(0));
    // a 4x4 grid of average colours: copies match it closely, a recoloured picture doesn't
    function colours(w, h, rgba) {
      const out = new Float32Array(48);
      for (let gy = 0; gy < 4; gy++) for (let gx = 0; gx < 4; gx++) {
        let r = 0, g = 0, b = 0, n = 0;
        for (let y = Math.floor(gy * h / 4); y < Math.floor((gy + 1) * h / 4); y++) for (let x = Math.floor(gx * w / 4); x < Math.floor((gx + 1) * w / 4); x++) {
          const k = (y * w + x) * 4; r += rgba[k]; g += rgba[k + 1]; b += rgba[k + 2]; n++;
        }
        const c = (gy * 4 + gx) * 3; n = n || 1;
        out[c] = r / n / 255; out[c + 1] = g / n / 255; out[c + 2] = b / n / 255;
      }
      return out;
    }
    function dhash(hash) {
      const { w, h, rgba } = thumbHashToRGBA(b64(hash));
      const g = new Float32Array(72);
      for (let y = 0; y < 8; y++) for (let x = 0; x < 9; x++) {
        const sx = Math.min(w - 1, Math.floor((x + 0.5) * w / 9)), sy = Math.min(h - 1, Math.floor((y + 0.5) * h / 8)), k = (sy * w + sx) * 4;
        g[y * 9 + x] = rgba[k] * 0.299 + rgba[k + 1] * 0.587 + rgba[k + 2] * 0.114;
      }
      let hi = 0, lo = 0;
      for (let y = 0; y < 8; y++) for (let x = 0; x < 8; x++) {
        const bit = g[y * 9 + x] > g[y * 9 + x + 1] ? 1 : 0, n = y * 8 + x;
        if (n < 32) lo |= bit << n; else hi |= bit << (n - 32);
      }
      return { bits: [hi >>> 0, lo >>> 0], col: colours(w, h, rgba) };
    }
    const pop = v => { v -= (v >>> 1) & 0x55555555; v = (v & 0x33333333) + ((v >>> 2) & 0x33333333); return (((v + (v >>> 4)) & 0x0f0f0f0f) * 0x01010101) >>> 24; };
    onmessage = ({ data }) => {
      const items = [];
      for (const it of data) { try { const d = dhash(it.hash); items.push({ id: it.id, ar: it.ar, h: d.bits, c: d.col }); } catch (e) {} }
      const parent = items.map((_, k) => k);
      const root = k => { while (parent[k] !== k) k = parent[k] = parent[parent[k]]; return k; };
      for (let a = 0; a < items.length; a++) {
        if (a % 500 === 0) postMessage({ progress: a / items.length });
        const A = items[a];
        for (let b = a + 1; b < items.length; b++) {
          const B = items[b];
          if (Math.abs(A.ar - B.ar) > 0.06 * Math.max(A.ar, B.ar)) continue;
          // (the same light-and-dark pattern in other colours is a different picture)
          let diff = 0; for (let i = 0; i < 48; i++) diff += Math.abs(A.c[i] - B.c[i]);
          if (diff / 48 > 0.06) continue;
          if (pop(A.h[0] ^ B.h[0]) + pop(A.h[1] ^ B.h[1]) <= 5) parent[root(a)] = root(b);
        }
      }
      const groups = new Map();
      items.forEach((it, k) => { const r = root(k); if (!groups.has(r)) groups.set(r, []); groups.get(r).push(it.id); });
      postMessage({ groups: [...groups.values()].filter(g => g.length > 1) });
    };`;
  function lookAlikes() {
    const items = (library || []).filter(i => i.thumbhash && i.width && i.height).map(i => ({ id: i.id, hash: i.thumbhash, ar: i.width / i.height }));
    if (items.length < 2) return toast('Not enough pictures to compare yet', 'err');
    const note = make('div', 'lv-look-note', 'Comparing every picture… 0%');
    const tools = document.querySelector('.lv-cleanup-tools');
    tools.append(note);
    const url = URL.createObjectURL(new Blob([WORKER], { type: 'text/javascript' }));
    const w = new Worker(url, { type: 'module' });
    const stop = () => { w.terminate(); URL.revokeObjectURL(url); note.remove(); };
    w.onerror = e => { stop(); toast('Look-alikes: ' + (e.message || 'could not compare'), 'err'); };
    w.onmessage = ({ data }) => {
      if (data.progress !== undefined) { note.textContent = `Comparing every picture… ${Math.round(data.progress * 100)}%`; return; }
      stop();
      const byId = new Map(library.map(i => [i.id, i]));
      // biggest first, so "Keep" starts on the sharpest copy
      const groups = data.groups.map(g => g.map(id => byId.get(id)).filter(Boolean).sort((a, b) => b.width * b.height - a.width * a.height)).filter(g => g.length > 1);
      if (!groups.length) return toast('No look-alikes: every picture looks different', 'ok');
      window.vaultReviewDuplicates(groups, 'Look-alikes', 'Each row looks like the same picture saved more than once (resized, re-saved or slightly cropped). Check them: some may only be similar. Keep starts on the biggest; "Bin the others" sends the rest to the Recycle Bin.');
    };
    w.postMessage(items);
  }
  {
    const tools = document.querySelector('.lv-cleanup-tools');
    if (tools) tools.append(button('', '◐ Find look-alikes', lookAlikes),
      button('', '🩺 Health check', () => healthCheck()));
  }

  // =====================================================================================
  // 8. The downloads panel
  // =====================================================================================
  {
    const list = [];
    const fab = make('button', 'dl-fab hidden'); fab.type = 'button'; fab.title = 'Downloads';
    const panel = make('div', 'dl-panel hidden');
    document.body.append(fab, panel);
    const active = () => list.filter(d => d.state === 'running');
    function draw() {
      const run = active().length, bad = list.filter(d => d.state === 'failed').length;
      fab.classList.toggle('hidden', !list.length);
      fab.innerHTML = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><path d="M12 4v11M7 10l5 5 5-5M5 20h14"/></svg>${run ? `<b>${run}</b>` : bad ? '<b class="bad">!</b>' : ''}`;
      fab.classList.toggle('busy', !!run);
      if (panel.classList.contains('hidden')) return;
      const head = make('div', 'dl-head');
      head.append(make('b', '', 'Downloads'), make('span', '', run ? `${run} going` : 'All done'),
        button('', 'Clear finished', () => { for (let k = list.length - 1; k >= 0; k--) if (list[k].state !== 'running') list.splice(k, 1); draw(); }));
      const rows = list.slice().reverse().map(d => {
        const r = make('div', 'dl-row ' + d.state);
        const im = make('img'); im.alt = ''; if (d.thumb) im.src = d.thumb;
        const mid = make('div', 'dl-mid');
        mid.append(make('b', '', d.title || 'Untitled'), make('span', 'dl-from', d.from + (d.text ? ' · ' + d.text : '')));
        const bar = make('div', 'dl-bar'); const fill = make('i');
        if (d.state === 'running') { if (d.frac == null) bar.classList.add('busy'); else fill.style.width = Math.round(d.frac * 100) + '%'; }
        else fill.style.width = '100%';
        bar.append(fill); mid.append(bar);
        const acts = make('div', 'dl-acts');
        if (d.state === 'failed' && d.retry) acts.append(button('primary', 'Retry', () => { list.splice(list.indexOf(d), 1); draw(); d.retry(); }));
        if (d.state !== 'running') acts.append(button('dl-x', '×', () => { list.splice(list.indexOf(d), 1); draw(); }));
        r.append(im, mid, acts);
        return r;
      });
      panel.replaceChildren(head, ...(rows.length ? rows : [make('p', 'dl-empty', 'Nothing being saved.')]));
    }
    fab.onclick = () => { panel.classList.toggle('hidden'); draw(); };
    addEventListener('pointerdown', e => { if (!panel.contains(e.target) && !fab.contains(e.target)) panel.classList.add('hidden'); }, true);
    window.vaultDownloads = {
      add({ title, thumb, from, retry }) {
        const d = { title, thumb, from, retry, state: 'running', frac: null, text: 'Starting' };
        list.push(d); if (list.length > 60) list.shift();
        draw();
        return {
          progress(frac, text) { if (frac != null) d.frac = Math.max(0, Math.min(1, frac)); if (text) d.text = text; draw(); },
          done() { d.state = 'done'; d.text = 'Saved'; draw(); },
          fail(msg) { d.state = 'failed'; d.text = msg || 'Failed'; draw(); }
        };
      }
    };
  }

  // =====================================================================================
  // 9. PIN lock
  // =====================================================================================
  // A privacy screen, not encryption: the PIN keeps the app covered on start and after you've been
  // away; the files on disk stay as they are. Only a salted PBKDF2 hash of the PIN is kept.
  const lockScreen = make('div', 'pin-lock hidden');
  lockScreen.innerHTML = `<div class="pin-card"><span class="pin-heart"></span><b>Lewd Vault is locked</b><span class="pin-sub">Enter your PIN</span>
    <input type="password" id="pinInput" inputmode="numeric" autocomplete="off" maxlength="12"><span class="pin-err"></span>
    <div class="pin-pad"></div></div>`;
  document.body.append(lockScreen);
  const pinInput = lockScreen.querySelector('#pinInput'), pinErr = lockScreen.querySelector('.pin-err');
  lockScreen.querySelector('.pin-pad').append(...['1', '2', '3', '4', '5', '6', '7', '8', '9', '⌫', '0', 'OK'].map(k => {
    const b = make('button', 'pin-key' + (k === 'OK' ? ' ok' : ''), k); b.type = 'button';
    b.onclick = () => { if (k === '⌫') pinInput.value = pinInput.value.slice(0, -1); else if (k === 'OK') tryUnlock(); else pinInput.value += k; pinInput.focus(); };
    return b;
  }));
  async function pinHash(pin, salt) {
    const key = await crypto.subtle.importKey('raw', new TextEncoder().encode(pin), 'PBKDF2', false, ['deriveBits']);
    const bits = await crypto.subtle.deriveBits({ name: 'PBKDF2', hash: 'SHA-256', salt: new TextEncoder().encode(salt), iterations: 150000 }, key, 256);
    return [...new Uint8Array(bits)].map(b => b.toString(16).padStart(2, '0')).join('');
  }
  const pinSet = () => !!(settings.pin && settings.pin.hash);
  let locked = false;
  function lock() {
    if (!pinSet() || locked) return;
    locked = true;
    html.classList.add('pin-locked');
    lockScreen.classList.remove('hidden');
    pinInput.value = ''; pinErr.textContent = '';
    document.querySelectorAll('video, audio').forEach(m => { try { m.pause(); } catch (e) {} });
    setTimeout(() => pinInput.focus(), 50);
  }
  let tries = 0;
  async function tryUnlock() {
    const v = pinInput.value;
    if (!v) return;
    if (await pinHash(v, settings.pin.salt) === settings.pin.hash) {
      locked = false; tries = 0;
      html.classList.remove('pin-locked');
      lockScreen.classList.add('hidden');
      lastSeen = Date.now();
    } else {
      tries++;
      pinInput.value = '';
      pinErr.textContent = tries >= 5 ? 'Wrong PIN. Wait a moment before trying again.' : 'Wrong PIN';
      if (tries >= 5) { pinInput.disabled = true; setTimeout(() => { pinInput.disabled = false; pinInput.focus(); pinErr.textContent = ''; }, 30000); }
    }
  }
  pinInput.addEventListener('keydown', e => { e.stopPropagation(); if (e.key === 'Enter') tryUnlock(); });
  // away: no mouse or keys for the chosen minutes, or the window was in the background that long
  let lastSeen = Date.now();
  for (const ev of ['pointermove', 'pointerdown', 'keydown', 'wheel']) addEventListener(ev, () => { if (!locked) lastSeen = Date.now(); }, { passive: true, capture: true });
  const awayMinutes = () => (settings.pin && settings.pin.idle) || 0;
  setInterval(() => { if (awayMinutes() && !locked && Date.now() - lastSeen > awayMinutes() * 60e3) lock(); }, 15000);
  document.addEventListener('visibilitychange', () => { if (document.visibilityState === 'visible' && awayMinutes() && Date.now() - lastSeen > awayMinutes() * 60e3) lock(); });
  addEventListener('keydown', e => { if (e.ctrlKey && !e.shiftKey && (e.key === 'l' || e.key === 'L') && pinSet()) { e.preventDefault(); lock(); } });
  setupReady.then(() => { if (pinSet()) lock(); });
  async function askNewPin() {
    return window.vaultSites.sheet(pinSet() ? 'Change your PIN' : 'Set a PIN', (body, close) => {
      body.append(make('p', 'dc-sheet-lead', 'Four to twelve digits. The app asks for it when it starts, after you\'ve been away (if you choose), and on Ctrl+L. It covers the app; it doesn\'t encrypt your files.'));
      const a = make('input'); Object.assign(a, { type: 'password', inputMode: 'numeric', placeholder: 'New PIN', maxLength: 12 });
      const b = make('input'); Object.assign(b, { type: 'password', inputMode: 'numeric', placeholder: 'The same again', maxLength: 12 });
      const idle = make('select');
      for (const [v, l] of [[0, 'Only when the app starts'], [5, 'After 5 minutes away'], [15, 'After 15 minutes away'], [30, 'After 30 minutes away'], [60, 'After an hour away']]) idle.add(new Option(l, v));
      idle.value = String(awayMinutes());
      const msg = make('p', 'pin-msg');
      const save = button('primary', 'Save PIN', async () => {
        if (!/^\d{4,12}$/.test(a.value)) { msg.textContent = 'Use 4 to 12 digits.'; return; }
        if (a.value !== b.value) { msg.textContent = 'The two PINs are different.'; return; }
        const salt = [...crypto.getRandomValues(new Uint8Array(16))].map(x => x.toString(16).padStart(2, '0')).join('');
        settings.pin = { salt, hash: await pinHash(a.value, salt), idle: +idle.value };
        saveSettingsSoon(); close(true); drawMaint(); toast('PIN set', 'ok');
      });
      const f = make('div', 'pin-form'); f.append(a, b, idle, msg);
      const acts = make('div', 'dc-actions st-actions'); acts.append(save);
      body.append(f, acts);
      setTimeout(() => a.focus(), 30);
    });
  }

  // =====================================================================================
  // 10. Maintenance in Settings: updates, backup, health check, PIN
  // =====================================================================================
  const maint = make('div', 'st-group maint');
  const storageHead = [...settingsTab.querySelectorAll(':scope > h3')].find(h => /storage/i.test(h.textContent));
  (storageHead || settingsTab.lastChild).before(maint);
  let updateInfo = null;
  function drawMaint() {
    maint.replaceChildren();
    // the PIN, with the other privacy settings
    maint.append(make('h3', '', 'PIN lock'));
    const pinRow = make('div', 'setting mt-row');
    pinRow.append(make('span', 's-label', pinSet() ? `On · ${awayMinutes() ? `locks after ${awayMinutes()} minutes away, and on start` : 'locks when the app starts'} · Ctrl+L locks now` : 'Off. A PIN covers the app on start and after you\'ve been away.'));
    const pinActs = make('div', 'mt-acts');
    pinActs.append(button(pinSet() ? '' : 'primary', pinSet() ? 'Change PIN…' : 'Set a PIN…', askNewPin));
    if (pinSet()) pinActs.append(button('danger', 'Turn off', async () => {
      if (!confirm('Turn the PIN lock off?')) return;
      delete settings.pin; saveSettingsSoon(); drawMaint(); toast('PIN lock off', 'ok');
    }));
    pinRow.append(pinActs);
    maint.append(pinRow);
    // backup and health
    maint.append(make('h3', '', 'Backup and health'));
    const bk = make('div', 'setting mt-row');
    bk.append(make('span', 's-label', settings.backupFolder ? `Backs up to ${settings.backupFolder}. Only what changed is copied.` : 'Copy the whole vault to a drive or folder. After the first time, only what changed is copied.'));
    const bkActs = make('div', 'mt-acts');
    bkActs.append(button('primary', settings.backupFolder ? 'Back up now' : 'Back up to…', () => backup(false)));
    if (settings.backupFolder) bkActs.append(button('', 'Change folder…', () => backup(true)));
    bk.append(bkActs);
    const hc = make('div', 'setting mt-row');
    hc.append(make('span', 's-label', 'Check for missing files, broken previews and leftover preview clips, and fix them.'));
    const hcActs = make('div', 'mt-acts'); hcActs.append(button('', 'Health check', () => healthCheck()));
    hc.append(hcActs);
    maint.append(bk, hc);
    // updates
    maint.append(make('h3', '', 'Updates'));
    const up = make('div', 'setting mt-row');
    const cur = updateInfo ? updateInfo.current : '';
    up.append(make('span', 's-label', updateInfo ? (updateInfo.newer ? `Version ${updateInfo.latest} is out (you have ${cur}).` : `You have the newest version (${cur}).`) : 'Check GitHub for a newer version of the app.'));
    const upActs = make('div', 'mt-acts');
    if (updateInfo && updateInfo.newer) upActs.append(button('primary', 'Update and restart', () => runUpdate()));
    upActs.append(button('', 'Check for updates', () => checkUpdate(true)));
    up.append(upActs);
    if (updateInfo && updateInfo.newer && updateInfo.notes) { const n = make('p', 'mt-notes'); n.textContent = String(updateInfo.notes).slice(0, 600); up.append(n); }
    maint.append(up);
    applyFind();
  }
  async function backup(choose) {
    let folder = settings.backupFolder;
    if (choose || !folder) {
      folder = await tauriInvoke('pick_folder').catch(() => null);
      if (!folder) return;
      settings.backupFolder = folder; saveSettingsSoon(); drawMaint();
    }
    try {
      await api('backupStart', folder);
      const t = toastLive('Backing up…');
      let p;
      do {
        await new Promise(r => setTimeout(r, 700));
        p = await api('backupProgress');
        if (p.total) t(`Backing up: ${fmtN(p.done)} of ${fmtN(p.total)} files looked at, ${fmtN(p.copied)} copied`);
      } while (p.status === 'running');
      t(null);
      if (p.status !== 'done') throw new Error(p.status);
      settings.lastBackup = new Date().toISOString(); saveSettingsSoon();
      toast(`Backed up: ${fmtN(p.copied)} files copied (${gb(p.bytes || 0)}), the rest were already there`, 'ok');
    } catch (e) { toast('Backup: ' + errMsg(e), 'err'); }
  }
  // a status line that stays while something runs (the page's toasts go away by themselves)
  function toastLive(text) {
    const n = make('div', 'live-note'); n.textContent = text; document.body.append(n);
    return t => { if (t === null) n.remove(); else n.textContent = t; };
  }
  async function healthCheck() {
    let r;
    try { r = await api('healthCheck'); } catch (e) { return toast(errMsg(e), 'err'); }
    window.vaultSites.sheet('Health check', (body, close) => {
      const ok = !r.missing.length && !r.noPreview.length && !r.orphanClips.length;
      body.append(make('p', 'dc-sheet-lead', ok ? `All ${fmtN(r.items)} items are fine: every file is there, every preview works, nothing is left over.` : `Looked at ${fmtN(r.items)} items.`));
      const row = (n, title, text, fixText, what) => {
        if (!n) return;
        const d = make('div', 'hc-row');
        d.append(make('b', '', `${fmtN(n)} ${title}`), make('span', '', text), button('primary', fixText, async () => {
          try { const out = await api('healthFix', what); if (out && out.md5 !== undefined) libraryWritten(out); toast('Fixed', 'ok'); close(); location.reload(); }
          catch (e) { toast(errMsg(e), 'err'); }
        }));
        body.append(d);
      };
      row(r.missing.length, r.missing.length === 1 ? 'item without its file' : 'items without their file', 'Their file was moved or deleted outside the app. Removing them only tidies the list.', 'Remove them from the list', 'missing');
      row(r.noPreview.length, r.noPreview.length === 1 ? 'video without its preview' : 'videos without their preview', 'Their moving preview is missing; tiles will show the still picture instead.', 'Fix the tiles', 'noPreview');
      const bytes = r.orphanClips.reduce((s, o) => s + (o.bytes || 0), 0);
      row(r.orphanClips.length, 'leftover preview clips', `Clips no item uses any more (${gb(bytes)}).`, 'Send to the Recycle Bin', 'orphanClips');
    });
  }
  async function checkUpdate(loud) {
    try {
      updateInfo = await api('updateCheck');
      drawMaint();
      if (updateInfo.newer && !loud) toast(`Lewd Vault ${updateInfo.latest} is out: Settings → Updates`, 'ok');
      else if (loud) toast(updateInfo.newer ? `Version ${updateInfo.latest} is ready to install` : 'You have the newest version', 'ok');
    } catch (e) { if (loud) toast(errMsg(e), 'err'); }
  }
  async function runUpdate() {
    if (!updateInfo || !updateInfo.url) return;
    try {
      await api('updateDownload', updateInfo.url);
      const t = toastLive('Downloading the update…');
      let p;
      do {
        await new Promise(r => setTimeout(r, 500));
        p = await api('updateProgress');
        if (p.total) t(`Downloading the update: ${Math.floor(p.done / p.total * 100)}%`);
      } while (p.status === 'running');
      t(null);
      if (p.status !== 'ready') throw new Error(p.status);
      toast('Restarting into the new version…', 'ok');
      await new Promise(r => setTimeout(r, 600));
      await tauriInvoke('install_update');
    } catch (e) { toast('Update: ' + errMsg(e), 'err'); }
  }
  setupReady.then(() => {
    drawMaint();
    // once a day, quietly
    const last = +(localStorage.getItem('imageVault.updateCheck') || 0);
    if (Date.now() - last > 864e5) { try { localStorage.setItem('imageVault.updateCheck', String(Date.now())); } catch (e) {} setTimeout(() => checkUpdate(false), 20000); }
  });

  // =====================================================================================
  // 11. Adapt to the PC: when scrolling the Library runs slow, fewer previews play at once
  // =====================================================================================
  {
    let frames = 0, scrollMs = 0, last = 0, tuned = false;
    const tick = now => {
      if (!html.classList.contains('is-scrolling') || $('tab-library').classList.contains('hidden')) { last = 0; return; }
      if (last) { scrollMs += now - last; frames++; }
      last = now;
      if (scrollMs > 4000) {
        const fps = frames / (scrollMs / 1000);
        frames = 0; scrollMs = 0;
        const now2 = S('maxPlaying');
        if (fps < 40 && now2 > 6 && !tuned) {
          tuned = true;
          settings.maxPlaying = Math.max(6, Math.round(now2 * 0.66));
          saveSettingsSoon();
          toast(`Scrolling ran at ${Math.round(fps)} frames a second, so at most ${settings.maxPlaying} previews now play at once (Settings → Library)`, 'ok');
        }
      }
      requestAnimationFrame(tick);
    };
    new MutationObserver(() => { if (html.classList.contains('is-scrolling') && !last) requestAnimationFrame(tick); })
      .observe(html, { attributes: true, attributeFilter: ['class'] });
  }

  // =====================================================================================
  // 12. Ctrl+Shift+P: the performance overlay
  // =====================================================================================
  {
    const hud = make('div', 'perf-hud hidden');
    document.body.append(hud);
    let on = false, frames = 0, longTasks = 0, t0 = performance.now(), timer = null;
    try { new PerformanceObserver(l => { longTasks += l.getEntries().length; }).observe({ entryTypes: ['longtask'] }); } catch (e) {}
    const count = () => { frames++; if (on) requestAnimationFrame(count); };
    function draw() {
      const now = performance.now(), fps = frames / ((now - t0) / 1000);
      frames = 0; t0 = now;
      const playing = [...document.querySelectorAll('video')].filter(v => !v.paused).length;
      const mem = performance.memory ? (performance.memory.usedJSHeapSize / 1e6).toFixed(0) + ' MB' : '–';
      hud.innerHTML = `<b>${Math.round(fps)}</b> fps<br>${playing} playing<br>${longTasks} slow tasks<br>${document.getElementsByTagName('*').length} elements<br>${mem} script memory`;
      longTasks = 0;
    }
    addEventListener('keydown', e => {
      if (!(e.ctrlKey && e.shiftKey && (e.key === 'P' || e.key === 'p'))) return;
      e.preventDefault();
      on = !on;
      hud.classList.toggle('hidden', !on);
      clearInterval(timer);
      if (on) { frames = 0; t0 = performance.now(); requestAnimationFrame(count); timer = setInterval(draw, 1000); }
    });
  }
})();
