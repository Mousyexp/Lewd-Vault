// Image Vault desktop layer: what the desktop app adds on top of the shared page (Index.html).
// Loaded after the page's own script, so it can use its functions and variables (library, api,
// renderLibrary, ...). sync-ui.mjs adds the hooks it fills in: tagsForSave, tagFilter, showTagOk,
// afterRenderLibrary and saveDetailTags.
//
// Tags work like on booru sites: each item gets as many as you like, written in lowercase with "_"
// between words and separated by spaces. A prefix gives a tag a type (and colour):
//   artist:name  character:name  series:name  meta:name   (shortcuts a: c: s: m:)
// Search:  blue_hair smile         both tags
//          -sketch                 not this tag
//          ~cat ~dog               at least one of these
//          blue_*  *hair           wildcards
//          category:x collection:x name:x kind:video tagcount:0 tagcount:>5

(() => {
  'use strict';
  document.documentElement.classList.add('desktop');

  // ---------- the heart loading screen ----------
  // A heart fills with rose "liquid" as the app really loads (setup, the list, the first screen of
  // pictures), with a heartbeat glow and a bar with a heart riding on it. Shown for at least
  // SPLASH_MIN_MS so it doesn't just blink; Settings → Look turns it off.
  const SPLASH_MIN_MS = 1500, SPLASH_MAX_MS = 6000;
  const splashOn = (() => { try { return (JSON.parse(localStorage.getItem('imageVault.settings')) || {}).splash !== false; } catch (e) { return true; } })();
  const HEART = 'M50 90C22 68 3 50 3 28 3 13 14 3 28 3c9 0 17 5 22 13C55 8 63 3 72 3c14 0 25 10 25 25 0 22-19 40-47 62z';
  const splash = splashOn ? (() => {
    const el = document.createElement('div');
    el.id = 'splash';
    const hearts = Array.from({ length: 16 }, (_, k) =>
      `<span style="left:${(k * 61) % 100}%;animation-delay:${(-k * 0.83).toFixed(2)}s;font-size:${10 + (k * 7) % 16}px;--o:${0.22 + (k % 4) * 0.1}">♥</span>`).join('');
    el.innerHTML = `
      <div class="sp-float">${hearts}</div>
      <div class="sp-center">
        <svg class="sp-heart" viewBox="0 0 100 94" aria-hidden="true">
          <defs>
            <clipPath id="spClip"><path d="${HEART}"/></clipPath>
            <linearGradient id="spFill" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="0" y2="96">
              <stop offset="0" stop-color="#ECA1AC"/><stop offset=".45" stop-color="#E27589"/><stop offset="1" stop-color="#7C3042"/>
            </linearGradient>
            <radialGradient id="spShine" cx=".32" cy=".26" r=".3"><stop offset="0" stop-color="#fff" stop-opacity=".55"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></radialGradient>
          </defs>
          <path class="sp-shell" d="${HEART}"/>
          <g clip-path="url(#spClip)">
            <g class="sp-level" transform="translate(0 94)">
              <path class="sp-wave back" d="M0 5Q12.5-1 25 5T50 5T75 5T100 5T125 5T150 5T175 5T200 5V200H0Z"/>
              <path class="sp-wave front" d="M0 5Q12.5-1 25 5T50 5T75 5T100 5T125 5T150 5T175 5T200 5V200H0Z"/>
            </g>
          </g>
          <path d="${HEART}" fill="url(#spShine)"/>
          <path class="sp-outline" d="${HEART}"/>
        </svg>
        <div class="sp-title">Lewd Vault</div>
        <div class="sp-bar"><div class="sp-bar-fill"></div></div>
        <div class="sp-status">Unlocking your vault…</div>
      </div>`;
    document.body.append(el);
    const level = el.querySelector('.sp-level'), bar = el.querySelector('.sp-bar-fill'), status = el.querySelector('.sp-status');
    const LINES = ['Unlocking your vault…', 'Setting the mood…', 'Warming up your favourites…', 'Almost ready for you…'];
    const born = performance.now();
    let target = 0.08, shown = 0, finished = false, line = 0;
    // the fill eases towards what's loaded, but never faster than the minimum time allows
    const tick = now => {
      const cap = Math.min(1, (now - born) / SPLASH_MIN_MS * 1.02);
      shown += (Math.min(target, cap) - shown) * 0.12;
      if (target >= 1 && cap >= 1 && shown > 0.985) shown = 1;
      level.setAttribute('transform', `translate(0 ${(94 - shown * 100).toFixed(2)})`);
      bar.style.width = (shown * 100).toFixed(1) + '%';
      const want = Math.min(LINES.length - 1, Math.floor(shown * LINES.length));
      if (want !== line) { line = want; status.textContent = LINES[line]; }
      if (shown >= 1 && !finished) { finished = true; el.classList.add('done'); setTimeout(() => el.remove(), 650); return; }
      requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
    setTimeout(() => { target = 1; }, SPLASH_MAX_MS);
    return { reach(p) { target = Math.max(target, p); } };
  })() : null;

  // ---------- tags: cleaning (same rules as clean_tag in main.rs) ----------
  const TYPES = ['artist', 'character', 'series', 'meta'];
  const SHORT = { a: 'artist', c: 'character', s: 'series', m: 'meta', copyright: 'series' };
  function cleanTag(raw) {
    let t = String(raw || '').trim().toLowerCase().replace(/^[-~]+/, '').replace(/\*/g, '');
    let kind = '';
    const k = t.indexOf(':');
    if (k > 0) {
      const p = SHORT[t.slice(0, k)] || t.slice(0, k);
      if (TYPES.includes(p)) { kind = p; t = t.slice(k + 1); }
    }
    t = t.replace(/^_+|_+$/g, '').replace(/[\u0000-\u001f]/g, '').slice(0, 64);
    return !t ? '' : kind ? `${kind}:${t}` : t;
  }
  const splitTags = s => String(s || '').split(/[\s,]+/).filter(Boolean);
  function cleanTags(list) {
    const seen = new Set();
    return list.map(cleanTag).filter(t => t && !seen.has(t) && seen.add(t)).slice(0, 300);
  }
  const typeOf = t => { const k = t.indexOf(':'); return k > 0 && TYPES.includes(t.slice(0, k)) ? t.slice(0, k) : 'general'; };
  const nameOf = t => typeOf(t) === 'general' ? t : t.slice(t.indexOf(':') + 1);
  const pretty = t => nameOf(t).replace(/_/g, ' ');
  // (kept per item until its tags change: the Tags panel counts every shown item's tags on each redraw)
  const tagSplits = new WeakMap();
  const tagsOf = item => {
    const hit = tagSplits.get(item);
    if (hit && hit[0] === item.tags) return hit[1];
    const list = splitTags(item.tags);
    tagSplits.set(item, [item.tags, list]);
    return list;
  };

  // every tag in the library with how many items have it (worked out again when the list changes)
  let countsFor = null, countsKey = '';
  function tagCounts() {
    const key = library ? library.length + ':' + tagsVersion : '';
    if (countsFor && key === countsKey) return countsFor;
    const m = new Map();
    for (const i of library || []) for (const t of tagsOf(i)) m.set(t, (m.get(t) || 0) + 1);
    countsKey = key;
    return countsFor = m;
  }
  let tagsVersion = 0;
  const tagsChanged = () => { tagsVersion++; };

  // up to `n` known tags matching what's typed: the exact name, then names starting with it, then
  // containing it; most used first within each
  function suggest(typed, have = [], n = 8) {
    let q = String(typed || '').toLowerCase().replace(/^[-~]/, '');
    const k = q.indexOf(':');
    let kind = '';
    if (k > 0 && (SHORT[q.slice(0, k)] || TYPES.includes(q.slice(0, k)))) { kind = SHORT[q.slice(0, k)] || q.slice(0, k); q = q.slice(k + 1); }
    q = q.replace(/\s+/g, '_');
    if (!q && !kind) return [];
    const out = [];
    for (const [t, c] of tagCounts()) {
      if (have.includes(t) || (kind && typeOf(t) !== kind)) continue;
      const name = nameOf(t), at = name.indexOf(q);
      if (at < 0) continue;
      out.push({ t, c, rank: name === q ? 0 : at === 0 ? 1 : 2 }); // exact, then starts with, then contains
    }
    return out.sort((a, b) => a.rank - b.rank || b.c - a.c || a.t.localeCompare(b.t)).slice(0, n);
  }

  // recently used tags, newest first (this computer only)
  const RECENT_KEY = 'imageVault.recentTags';
  const recentTags = () => { try { return JSON.parse(localStorage.getItem(RECENT_KEY)) || []; } catch (e) { return []; } };
  function rememberTags(list) {
    const next = [...list, ...recentTags().filter(t => !list.includes(t))].slice(0, 30);
    try { localStorage.setItem(RECENT_KEY, JSON.stringify(next)); } catch (e) {}
  }

  // ---------- small helpers ----------
  const make = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined) e.textContent = text; return e; };
  function tagChip(t, { count, onRemove, onClick } = {}) {
    const chip = make('span', 'tchip t-' + typeOf(t));
    chip.title = t;
    const label = make(onClick ? 'button' : 'span', 'tchip-name', pretty(t));
    if (onClick) { label.type = 'button'; label.onclick = () => onClick(t); }
    chip.append(label);
    if (count !== undefined) chip.append(make('span', 'tchip-count', fmtN(count)));
    if (onRemove) {
      const x = make('button', 'tchip-x', '×');
      x.type = 'button'; x.title = 'Remove'; x.tabIndex = -1;
      x.onclick = e => { e.stopPropagation(); onRemove(t); };
      chip.append(x);
    }
    return chip;
  }

  // A suggestion list under an input. choose(tag) is called with the picked suggestion.
  function suggestions(anchor, input, choose) {
    const list = make('div', 'tsuggest hidden');
    anchor.append(list);
    let rows = [], at = -1;
    const draw = () => {
      list.replaceChildren(...rows.map((r, k) => {
        const row = make('div', 'tsuggest-row t-' + typeOf(r.t) + (k === at ? ' on' : ''));
        row.append(make('span', 'tdot'), make('span', 'tsuggest-name', pretty(r.t)), make('span', 'tsuggest-type', typeOf(r.t) === 'general' ? '' : typeOf(r.t)), make('span', 'tsuggest-count', fmtN(r.c)));
        row.onmousedown = e => { e.preventDefault(); choose(r.t); };
        return row;
      }));
      list.classList.toggle('hidden', !rows.length);
    };
    return {
      // nothing is picked until you use the arrows (so Enter keeps what you typed); Tab takes the first
      show(found) { rows = found; at = -1; draw(); },
      hide() { rows = []; at = -1; draw(); },
      // arrow keys and accepting; returns true when it handled the key
      key(e) {
        if (!rows.length) return false;
        if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { e.preventDefault(); at = (at + (e.key === 'ArrowDown' ? 1 : -1) + rows.length) % rows.length; draw(); return true; }
        if (e.key === 'Escape') { e.preventDefault(); this.hide(); return true; }
        if (e.key === 'Tab' || (e.key === 'Enter' && at >= 0)) { e.preventDefault(); choose(rows[Math.max(0, at)].t); return true; }
        return false;
      }
    };
  }

  // A tag box: chips plus a text field. Space, comma, Enter or Tab finishes a tag; Backspace in the
  // empty field takes the last one off; arrows pick from the suggestions.
  // More than 10 tags: the rest fold away behind "+N more" (and "Show less" folds them again)
  const TAG_FOLD = 10;
  function foldToggle(total, open, onClick) {
    const b = make('button', 'tmore', open ? 'Show less' : `+${total - TAG_FOLD} more`);
    b.type = 'button';
    b.addEventListener('mousedown', e => e.preventDefault()); // the tag field keeps the focus
    b.onclick = e => { e.stopPropagation(); onClick(); };
    return b;
  }
  function foldChips(wrap) {
    const chips = [...wrap.children].filter(c => !c.classList.contains('tmore'));
    if (chips.length <= TAG_FOLD) return;
    let open = false;
    const draw = () => {
      chips.forEach((c, k) => c.classList.toggle('folded', !open && k >= TAG_FOLD));
      wrap.querySelector(':scope > .tmore')?.remove();
      wrap.append(foldToggle(chips.length, open, () => { open = !open; draw(); }));
    };
    draw();
  }
  function tagBox({ placeholder, onChange = () => {}, onChipClick } = {}) {
    const box = make('div', 'tagbox');
    const input = make('input');
    Object.assign(input, { type: 'text', placeholder, spellcheck: false, autocomplete: 'off' });
    box.append(input);
    let tags = [], open = false;
    const sugg = suggestions(box, input, t => { add(t, true); input.value = ''; sugg.hide(); input.focus(); });
    function render() {
      box.querySelectorAll('.tchip, .tmore').forEach(c => c.remove());
      tags.forEach((t, k) => {
        const chip = tagChip(t, { onRemove: drop, onClick: onChipClick });
        if (!open && k >= TAG_FOLD) chip.classList.add('folded');
        box.insertBefore(chip, input);
      });
      if (tags.length > TAG_FOLD) box.insertBefore(foldToggle(tags.length, open, () => { open = !open; render(); }), input);
      input.placeholder = tags.length ? '' : placeholder;
    }
    function add(raw, picked) {
      const fresh = cleanTags(picked ? [raw] : splitTags(raw)).filter(t => !tags.includes(t));
      if (!fresh.length) return;
      tags.push(...fresh); render(); onChange(tags);
    }
    function drop(t) { tags = tags.filter(x => x !== t); render(); onChange(tags); input.focus(); }
    input.addEventListener('keydown', e => {
      if (sugg.key(e)) return;
      const text = input.value.trim();
      if ((e.key === 'Enter' || e.key === 'Tab' || e.key === ',' || e.key === ' ') && text) {
        e.preventDefault(); add(text); input.value = ''; sugg.hide();
      } else if (e.key === 'Backspace' && !input.value && tags.length) {
        drop(tags[tags.length - 1]);
      } else if (e.key === ' ' || e.key === ',') e.preventDefault();
    });
    input.addEventListener('input', () => {
      const parts = input.value.split(/[\s,]+/);
      if (parts.length > 1) { add(parts.slice(0, -1).join(' ')); input.value = parts[parts.length - 1]; }
      sugg.show(suggest(input.value, tags));
    });
    input.addEventListener('blur', () => { setTimeout(() => sugg.hide(), 100); if (input.value.trim()) { add(input.value); input.value = ''; } });
    box.addEventListener('mousedown', e => { if (e.target === box) { e.preventDefault(); input.focus(); } });
    return {
      el: box, input,
      get: () => tags.slice(),
      set(list) { tags = cleanTags(Array.isArray(list) ? list : splitTags(list)); open = false; input.value = ''; render(); },
      add(t) { add(t, true); },
      flush() { if (input.value.trim()) add(input.value); input.value = ''; }
    };
  }

  // ---------- search queries ----------
  const wildcard = s => new RegExp('^' + s.split('*').map(p => p.replace(/[.+?^${}()|[\]\\]/g, '\\$&')).join('.*') + '$');
  function tagTest(tok) {
    const k = tok.indexOf(':');
    if (k > 0 && SHORT[tok.slice(0, k)]) tok = SHORT[tok.slice(0, k)] + tok.slice(k);
    const typed = typeOf(tok) !== 'general';
    if (tok.includes('*')) {
      const re = wildcard(tok);
      return tags => tags.some(t => re.test(t) || (!typed && re.test(nameOf(t))));
    }
    return tags => tags.some(t => t === tok || (!typed && nameOf(t) === tok));
  }
  function metaTest(key, v) {
    const like = s => v.includes('*') ? wildcard(v).test(s) : s === v;
    const norm = s => String(s || '').toLowerCase().replace(/\s+/g, '_');
    if (key === 'category') return (_, i) => like(norm(i.category));
    if (key === 'collection') return (_, i) => like(norm(i.collection));
    if (key === 'name') return (_, i) => norm(i.name).includes(v);
    if (key === 'kind') return (_, i) => i.kind === v || (v === 'moving' && i.kind !== 'image');
    if (key === 'tagcount') {
      const m = /^(<=|>=|<|>|=)?(\d+)$/.exec(v);
      if (!m) return () => true;
      const n = +m[2], op = m[1] || '=';
      return tags => ({ '<': tags.length < n, '>': tags.length > n, '<=': tags.length <= n, '>=': tags.length >= n, '=': tags.length === n })[op];
    }
    return () => true;
  }
  // Turns a query into a test (item -> true/false), or null for an empty query.
  function compile(query) {
    const must = [], not = [], any = [];
    for (let tok of String(query || '').toLowerCase().split(/\s+/).filter(Boolean)) {
      let into = must;
      if (tok.length > 1 && tok[0] === '-') { into = not; tok = tok.slice(1); }
      else if (tok.length > 1 && tok[0] === '~') { into = any; tok = tok.slice(1); }
      const m = /^(category|collection|name|kind|tagcount):(.*)$/.exec(tok);
      into.push(m ? metaTest(m[1], m[2]) : tagTest(tok));
    }
    if (!must.length && !not.length && !any.length) return null;
    return item => {
      const tags = tagsOf(item);
      return must.every(f => f(tags, item)) && !not.some(f => f(tags, item)) && (!any.length || any.some(f => f(tags, item)));
    };
  }

  // A search field with suggestions for the word being typed (keeping its - or ~).
  function searchField(input, onRun) {
    const wrap = input.parentElement;
    const lastWord = () => { const m = /(^|\s)([-~]?)(\S*)$/.exec(input.value); return { op: m[2], word: m[3] }; };
    const sugg = suggestions(wrap, input, t => {
      const { op } = lastWord();
      input.value = input.value.replace(/(^|\s)[-~]?\S*$/, (m0, sp) => sp + op + t) + ' ';
      sugg.hide(); input.focus(); onRun();
    });
    let timer = null;
    input.addEventListener('input', () => {
      const { word } = lastWord();
      sugg.show(/^(category|collection|name|kind|tagcount):/.test(word) ? [] : suggest(word, [], 10));
      clearTimeout(timer); timer = setTimeout(onRun, 160);
    });
    input.addEventListener('keydown', e => { if (!sugg.key(e) && e.key === 'Enter') { clearTimeout(timer); onRun(); sugg.hide(); } });
    input.addEventListener('blur', () => setTimeout(() => sugg.hide(), 100));
  }

  // ---------- Add: tags for the items being saved ----------
  const taggerBox = tagBox({ placeholder: 'Type a tag, then space: blue_hair, artist:name…', onChange: drawTagHints });
  const tagLabel = make('label', 'f tags-label');
  tagLabel.append('Tags ', make('span', 'opt', '· as many as you like'));
  const hints = make('div', 'tag-hints');
  const clearTags = make('button', 'linkish', 'Clear tags');
  clearTags.type = 'button';
  clearTags.onclick = () => { taggerBox.set([]); drawTagHints(); taggerBox.input.focus(); };
  const tagHelp = make('p', 'hint', 'Space or Enter finishes a tag; use _ inside one. Prefixes give it a type: a: artist, c: character, s: series, m: meta.');
  $('catChips').after(tagLabel, taggerBox.el, hints, tagHelp);
  tagLabel.append(clearTags);

  // suggestions under the tag box: popular in this category, then recently used
  function drawTagHints() {
    const have = taggerBox.get(), cat = $('category').value.trim();
    const inCat = new Map();
    for (const i of library || []) if (i.category === cat) for (const t of tagsOf(i)) inCat.set(t, (inCat.get(t) || 0) + 1);
    const popular = [...inCat].filter(([t]) => !have.includes(t)).sort((a, b) => b[1] - a[1]).slice(0, 12);
    const recent = recentTags().filter(t => !have.includes(t) && !inCat.has(t)).slice(0, 10);
    hints.replaceChildren();
    const row = (title, list) => {
      if (!list.length) return;
      const r = make('div', 'tag-hint-row');
      r.append(make('span', 'tag-hint-title', title));
      for (const [t, c] of list) r.append(tagChip(t, { count: c, onClick: x => { taggerBox.add(x); drawTagHints(); } }));
      hints.append(r);
    };
    row(cat ? `Popular in ${cat}` : 'Popular', cat ? popular : [...tagCounts()].sort((a, b) => b[1] - a[1]).filter(([t]) => !have.includes(t)).slice(0, 12));
    row('Recent', recent.map(t => [t, undefined]));
  }
  $('category').addEventListener('input', drawTagHints);
  $('catChips').addEventListener('click', () => setTimeout(drawTagHints));
  tagsForSave = () => {
    taggerBox.flush();
    const tags = taggerBox.get();
    if (tags.length) { rememberTags(tags); tagsChanged(); }
    return tags.join(' ');
  };
  // a new batch is usually a new person or set: start it without tags (like the collection)
  const startQueueOrig = startQueue;
  startQueue = function (files) { taggerBox.set([]); const r = startQueueOrig.apply(this, arguments); drawTagHints(); return r; };

  // ---------- Library: search bar and tag panel ----------
  const lib = $('tab-library');
  const layout = make('div', 'lib-layout'), libMain = make('div', 'lib-main'), panel = make('aside', 'tag-panel');
  const search = make('div', 'searchbar');
  const searchInput = make('input');
  Object.assign(searchInput, { type: 'search', id: 'tagSearch', spellcheck: false, autocomplete: 'off',
                               placeholder: 'Search tags…  blue_hair -sketch ~cat ~dog artist:*' });
  const searchHelp = make('button', 'search-help', '?');
  searchHelp.type = 'button';
  searchHelp.title = 'tag1 tag2: both · -tag: without · ~a ~b: either · blue_*: wildcard · category:x collection:x name:x kind:video tagcount:0 · a: c: s: m: = artist, character, series, meta';
  search.append(make('span', 'search-icon'), searchInput, searchHelp);
  libMain.append(search, ...lib.children);
  layout.append(libMain, panel);
  lib.append(layout);

  let libraryTest = null, shownList = [];
  function runSearch() {
    libraryTest = compile(searchInput.value);
    search.classList.toggle('active', !!libraryTest);
    renderLibrary();
  }
  searchField(searchInput, runSearch);
  tagFilter = item => !libraryTest || libraryTest(item);
  function searchFor(q) {
    searchInput.value = q + ' ';
    document.querySelector('nav [data-tab="library"]').click();
    runSearch();
  }
  // + adds a tag to the search, - excludes it; the name searches for just that tag
  function addToSearch(t, op = '') {
    const words = searchInput.value.split(/\s+/).filter(w => w && w.replace(/^[-~]/, '') !== t);
    searchInput.value = [...words, op + t].join(' ') + ' ';
    runSearch();
  }

  const ORDER = ['artist', 'character', 'series', 'general', 'meta'];
  const TITLES = { artist: 'Artists', character: 'Characters', series: 'Series', general: 'Tags', meta: 'Meta' };
  // The panel is drawn just after the grid's first frame, not before it: counting the tags of
  // thousands of items shouldn't hold up the pictures.
  let panelTimer = 0;
  afterRenderLibrary = list => {
    shownList = list;
    const navLib = document.querySelector('nav [data-tab="library"]');
    if (navLib && library) navLib.dataset.count = fmtN(library.length);
    cancelAnimationFrame(panelTimer);
    panelTimer = requestAnimationFrame(() => setTimeout(() => { if (shownList === list) drawPanel(list); }, 0));
  };
  function drawPanel(list) {
    const counts = new Map();
    for (const i of list) for (const t of tagsOf(i)) counts.set(t, (counts.get(t) || 0) + 1);
    const top = [...counts].sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0])).slice(0, 80);
    panel.replaceChildren();
    const head = make('div', 'panel-head');
    head.append(make('h3', '', 'Tags'), make('span', 'panel-sub', counts.size ? `${fmtN(counts.size)} in these ${fmtN(list.length)}` : ''));
    panel.append(head);
    if (!top.length) panel.append(make('p', 'panel-empty', library && library.some(i => i.tags) ? 'No tags in these results.' : 'No tags yet. Add them when saving, in an item\'s details, or for everything shown below.'));
    for (const kind of ORDER) {
      const rows = top.filter(([t]) => typeOf(t) === kind);
      if (!rows.length) continue;
      panel.append(make('h4', 'panel-group t-' + kind, TITLES[kind]));
      const ul = make('ul', 'tag-list');
      for (const [t, c] of rows) {
        const li = make('li', 't-' + kind);
        const plus = make('button', 'tl-op', '+'), minus = make('button', 'tl-op', '–'), name = make('button', 'tl-name', pretty(t));
        [plus, minus, name].forEach(b => b.type = 'button');
        plus.title = 'Also require this tag'; minus.title = 'Leave out this tag'; name.title = 'Show only ' + t;
        plus.onclick = () => addToSearch(t); minus.onclick = () => addToSearch(t, '-'); name.onclick = () => { searchInput.value = t + ' '; runSearch(); };
        li.append(plus, minus, name, make('span', 'tl-count', fmtN(c)));
        ul.append(li);
      }
      panel.append(ul);
    }
    panel.append(bulkBox);
    bulkCount.textContent = `${fmtN(list.length)} shown ${list.length === 1 ? 'item' : 'items'}`;
    panel.dispatchEvent(new Event('drawn', { bubbles: true }));
  }

  // tag everything shown at once
  const bulkBox = make('div', 'bulk');
  const bulkCount = make('span');
  const bulkTags = tagBox({ placeholder: 'tags…' });
  const bulkAdd = make('button', 'btn primary', 'Add'), bulkRemove = make('button', 'btn', 'Remove');
  const bulkHead = make('h4', 'panel-group', 'Tag all shown');
  const bulkSub = make('p', 'panel-sub');
  bulkSub.append(bulkCount);
  const bulkActions = make('div', 'bulk-actions');
  bulkActions.append(bulkAdd, bulkRemove);
  bulkBox.append(bulkHead, bulkSub, bulkTags.el, bulkActions);
  async function bulk(mode) {
    bulkTags.flush();
    const tags = bulkTags.get(), ids = shownList.map(i => i.id);
    if (!tags.length || !ids.length) return toast('Type the tags first.', 'err');
    const what = `${mode === 'add' ? 'Add' : 'Remove'} ${tags.map(pretty).join(', ')} ${mode === 'add' ? 'to' : 'from'} ${ids.length} ${ids.length === 1 ? 'item' : 'items'}?`;
    if (ids.length > 1 && !confirm(what)) return;
    try {
      const r = await api('setTags', ids, tags.join(' '), mode);
      const pick = new Set(ids);
      for (const i of library) {
        if (!pick.has(i.id)) continue;
        const have = tagsOf(i);
        i.tags = cleanTags(mode === 'add' ? [...have, ...tags] : have.filter(t => !tags.includes(t))).join(' ');
      }
      libraryWritten(r); tagsChanged();
      if (mode === 'add') rememberTags(tags);
      bulkTags.set([]);
      toast(`Tags ${mode === 'add' ? 'added' : 'removed'}`, 'ok');
      renderLibrary();
    } catch (e) { toast(errMsg(e), 'err'); }
  }
  bulkAdd.onclick = () => bulk('add');
  bulkRemove.onclick = () => bulk('remove');

  // ---------- details: edit the tags; click one to search for it ----------
  const detailTags = tagBox({ placeholder: 'Add tags…', onChipClick: t => { closeDetail(); searchFor(t); } });
  const dTagLabel = make('label', 'f', 'Tags');
  $('dCat').after(dTagLabel, detailTags.el);
  const openDetailOrig = openDetail;
  let preloaded = [];
  openDetail = function (item) {
    detailTags.set(item.tags || '');
    // a soft glow of the picture's colours behind it (a small grid thumbnail, blurred by CSS)
    $('detail').style.setProperty('--ambient', item.thumb ? `url("${thumbSrc(item, 240)}")` : 'none');
    const r = openDetailOrig.apply(this, arguments);
    // the pictures either side are decoded now, so ← → shows them at once
    if (detailList) {
      const k = detailList.indexOf(item);
      preloaded = [1, -1, 2].map(d => detailList[(k + d + detailList.length) % detailList.length])
        .filter(x => x && x !== item && x.kind === 'image')
        .map(x => { const im = new Image(); im.src = vaultUrl(x.id); im.decode().catch(() => {}); return im; });
    }
    return r;
  };
  $('detailImg').addEventListener('load', e => {
    if (!e.target.src.startsWith(vaultOrigin() + '/item')) return;
    e.target.animate([{ opacity: .55, filter: 'blur(6px)' }, { opacity: 1, filter: 'blur(0)' }], { duration: 260, easing: 'ease-out' });
  });
  window.saveDetailTags = async item => {
    detailTags.flush();
    const want = detailTags.get().join(' ');
    if (want === (item.tags || '')) return;
    const r = await api('setTags', [item.id], want, 'set');
    item.tags = want;
    libraryWritten(r); tagsChanged();
    rememberTags(detailTags.get());
  };
  // and open the file in its own program
  const openBtn = make('button', 'btn', 'Open');
  openBtn.title = 'Open with its default program';
  openBtn.onclick = () => current && api('openFile', current.id).catch(e => toast(errMsg(e), 'err'));
  $('dOpen').before(openBtn);
  $('dOpen').textContent = 'Show in folder';
  // hide the details panel (button or I) so the picture gets the whole window; remembered
  const INFO_KEY = 'imageVault.detailInfo';
  const infoBtn = make('button', 'info-toggle');
  infoBtn.type = 'button';
  const setInfo = show => {
    $('detail').classList.toggle('no-info', !show);
    infoBtn.textContent = show ? 'Hide details' : 'Details';
    infoBtn.title = (show ? 'Hide the details panel' : 'Show the details panel') + ' (I)';
    try { localStorage.setItem(INFO_KEY, show ? '1' : '0'); } catch (e) {}
  };
  setInfo((() => { try { return localStorage.getItem(INFO_KEY) !== '0'; } catch (e) { return true; } })());
  infoBtn.onclick = () => setInfo($('detail').classList.contains('no-info'));
  document.querySelector('#detail .big').append(infoBtn);
  document.addEventListener('keydown', e => {
    if (current && e.key.toLowerCase() === 'i' && !/INPUT|SELECT|TEXTAREA/.test(e.target.tagName) && !e.ctrlKey) infoBtn.click();
  });
  $('dTrash').textContent = 'Delete';
  const bigClose = make('button', 'view-close');
  bigClose.type = 'button'; bigClose.title = 'Close (Esc)';
  // the one close button of the full view (top left, a cross and the word); the panel's own Close is hidden
  bigClose.innerHTML = '<svg viewBox="0 0 24 24"><path d="M6 6l12 12M18 6L6 18"/></svg><span>Close</span>';
  bigClose.onclick = () => closeDetail();
  const bigBin = make('button', 'view-bin');
  bigBin.type = 'button'; bigBin.title = 'Delete (Del)';
  bigBin.innerHTML = '<svg viewBox="0 0 24 24"><g class="lid"><path d="M3.5 6.5h17"/><path d="M9 6.5V4h6v2.5"/></g><path d="M5.5 6.5l1.2 13.5h10.6l1.2-13.5"/><path d="M10 10.5v6M14 10.5v6"/></svg>';
  bigBin.onclick = () => {
    bigBin.classList.remove('toss'); void bigBin.offsetWidth; bigBin.classList.add('toss');
    setTimeout(() => $('dTrash').click(), 380); // the lid opens and the bin gulps, then it asks
  };
  document.querySelector('#detail .big').append(bigClose, bigBin);
  // nothing scrolls behind a full view (the Library here, Discover's post view too)
  document.addEventListener('wheel', e => {
    const full = e.target.closest('#detail, .dc-view');
    if (full && !e.target.closest('aside, .dc-side')) e.preventDefault();
  }, { passive: false });
  document.addEventListener('keydown', e => {
    if (current && e.key === 'Delete' && !/INPUT|SELECT|TEXTAREA/.test(e.target.tagName)) bigBin.click();
  });
  $('saveBtn').textContent = 'Save';

  // ---------- slideshow: only items matching a tag search ----------
  const SHOW_KEY = 'imageVault.showTags';
  const showBox = make('div', 'show-tags');
  const showLabel = make('h3', '', 'Tags');
  const showWrap = make('div', 'searchbar small');
  const showInput = make('input');
  Object.assign(showInput, { type: 'search', spellcheck: false, autocomplete: 'off', placeholder: 'Only items matching… e.g. artist:name -sketch' });
  try { showInput.value = localStorage.getItem(SHOW_KEY) || ''; } catch (e) {}
  showWrap.append(make('span', 'search-icon'), showInput);
  showBox.append(showLabel, showWrap);
  $('showBasic').before(showBox);
  let showTest = compile(showInput.value);
  showTagOk = item => !showTest || showTest(item);
  searchField(showInput, () => {
    showTest = compile(showInput.value);
    try { localStorage.setItem(SHOW_KEY, showInput.value); } catch (e) {}
    if (typeof drawPlan === 'function') drawPlan();
  });

  // ---------- stats: the most used tags ----------
  const renderStatsOrig = renderStats;
  renderStats = function () {
    renderStatsOrig.apply(this, arguments);
    const charts = $('stats').querySelector('.charts');
    if (!charts || !library) return;
    const rows = [...tagCounts()].sort((a, b) => b[1] - a[1]).slice(0, 15).map(([t, c]) => ({ label: pretty(t) + (typeOf(t) === 'general' ? '' : ` (${typeOf(t)})`), value: c }));
    const untagged = library.filter(i => !i.tags).length;
    if (rows.length) charts.prepend(chart('Most used tags', `${fmtN(tagCounts().size)} tags · ${fmtN(untagged)} items without tags`, hBars(rows, 'items'), rows, 'items'));
  };

  // ---------- drop files anywhere in the window ----------
  const dropCover = make('div', 'drop-cover hidden');
  dropCover.append(make('div', 'drop-cover-box', 'Drop to add'));
  document.body.append(dropCover);
  const hasFiles = e => e.dataTransfer && [...e.dataTransfer.types].includes('Files');
  let dragDepth = 0;
  window.addEventListener('dragenter', e => { if (!hasFiles(e)) return; dragDepth++; dropCover.classList.remove('hidden'); });
  window.addEventListener('dragleave', e => { if (!hasFiles(e)) return; if (--dragDepth <= 0) { dragDepth = 0; dropCover.classList.add('hidden'); } });
  window.addEventListener('dragover', e => { if (hasFiles(e)) e.preventDefault(); });
  window.addEventListener('drop', e => {
    if (!hasFiles(e)) return;
    e.preventDefault(); dragDepth = 0; dropCover.classList.add('hidden');
    if (e.target.closest && e.target.closest('#drop, #tab-audio')) return; // the Add tab's drop area and the Audio page handle their own
    document.querySelector('nav [data-tab="add"]').click();
    startQueue(e.dataTransfer.files);
  });

  // ---------- settings that only matter in a browser go; the data folder comes in ----------
  for (let k = SETTINGS.length - 1; k >= 0; k--) if (SETTINGS[k].section === 'Loading' || SETTINGS[k].key === 'cacheGB') SETTINGS.splice(k, 1);
  const storageHead = $('storageInfo').previousElementSibling;
  storageHead.textContent = 'Data folder';
  const folderBtn = make('button', 'btn', 'Open data folder');
  folderBtn.onclick = () => api('openDataFolder').catch(e => toast(errMsg(e), 'err'));
  $('clearCache').replaceWith(folderBtn);
  const renderSettingsOrig = renderSettings;
  renderSettings = function () {
    const r = renderSettingsOrig.apply(this, arguments);
    $('storageInfo').textContent = (setup && setup.dataFolder) || '';
    return r;
  };
  showStorage = () => {};

  // ---------- header: icons for the sections, and a little brand mark ----------
  const brand = document.querySelector('header h1');
  if (brand) brand.innerHTML = '<span class="brand-mark"></span><span class="brand-name">Lewd Vault</span>';
  // the number of items next to "Library", from the moment the list is loaded
  const loadLibraryDataOrig = loadLibraryData;
  loadLibraryData = async function () {
    const r = await loadLibraryDataOrig.apply(this, arguments);
    const navLib = document.querySelector('nav [data-tab="library"]');
    if (library && navLib) navLib.dataset.count = fmtN(library.length);
    return r;
  };

  // ---------- thumbnails and clips straight from disk ----------
  // The web version downloads each thumbnail into a Blob and shows it through an object URL. Here
  // the <img> and <video> point straight at the app's file server: nothing is copied in memory, the
  // browser decodes off the main thread, and thumbnails come at the size the card needs (the backend
  // makes and keeps card-sized copies of the 900 x 1200 thumbnails, see grid_thumb in main.rs).
  const WIDTHS = [240, 360, 480, 720, 960];
  const thumbSrc = (item, w) => `${vaultOrigin()}/t/${encodeURIComponent(item.thumb)}?w=${w}`;
  function neededWidth(item) {
    const [tw, th] = (item.thumbSize || '').split('x').map(Number);
    const ar = tw && th ? tw / th : item.width && item.height ? item.width / item.height : 0.75;
    const dpr = window.devicePixelRatio || 1;
    // grid cards are 3:4 and cover-cropped; rows show the whole picture at the row's height
    const layout = S('libraryLayout');
    const css = layout === 'rows' ? rowHeight * Math.min(3, ar)
      : layout === 'masonry' ? cardWidth * 1.1 // whole pictures, a column wide
      : layout === 'big' ? 520 * Math.max(1, 0.75 * ar) // big cards (library.js)
      : layout === 'list' ? 160
      : Math.max(cardWidth * 1.35, cardWidth * 1.35 * 4 / 3 * ar);
    const px = css * dpr;
    return WIDTHS.find(w => w >= px) || WIDTHS[WIDTHS.length - 1];
  }
  thumbUrl = async item => {
    const url = thumbSrc(item, neededWidth(item));
    thumbUrls.set(item.id, url);
    return url;
  };
  clipUrl = async id => vaultUrl(id);

  // ---------- zoom: card size / row height ----------
  const ZOOM_KEY = 'imageVault.zoom';
  let zoom = 40, cardWidth = 0, rowHeight = 0;
  try { zoom = Math.min(100, Math.max(0, +(localStorage.getItem(ZOOM_KEY) ?? 40))); } catch (e) {}
  function applyZoom() {
    cardWidth = Math.round(140 + zoom * 3.2);
    rowHeight = Math.round(150 + zoom * 3.5);
    document.documentElement.style.setProperty('--card', cardWidth + 'px');
  }
  applyZoom();
  const justifyRowsOrig = justifyRows;
  justifyRows = function (list, width, target) {
    return arguments.length >= 3 ? justifyRowsOrig.apply(this, arguments) : justifyRowsOrig(list, width, width < 600 ? Math.min(rowHeight, 200) : rowHeight);
  };
  // the toolbar: filters as pills, then the count and the zoom slider on the same line
  const zoomBox = make('label', 'zoom');
  zoomBox.title = 'Size of the pictures';
  const zoomIn = make('input');
  Object.assign(zoomIn, { type: 'range', min: 0, max: 100, step: 1, value: zoom });
  zoomBox.append(make('span', 'zoom-icon small'), zoomIn, make('span', 'zoom-icon'));
  const filters = lib.querySelector('.filters');
  filters.append($('count'), zoomBox);
  let zoomTimer = null;
  zoomIn.addEventListener('input', () => {
    zoom = +zoomIn.value; applyZoom();
    try { localStorage.setItem(ZOOM_KEY, zoom); } catch (e) {}
    clearTimeout(zoomTimer);
    // rows are laid out for one height, so they're redone; the grid follows by itself (CSS)
    zoomTimer = setTimeout(() => { if (S('libraryLayout') === 'rows' || cardWidth > 300) renderLibrary(); }, 140);
  });
  // whole pictures in justified rows unless you picked the grid yourself
  setupReady.then(s => {
    if (s && !(s.settings && 'libraryLayout' in s.settings)) { settings.libraryLayout = 'rows'; renderLibrary(); }
  });

  // ---------- start on the section you were using ----------
  const TAB_KEY = 'imageVault.tab';
  document.querySelectorAll('nav button').forEach(b => b.addEventListener('click', () => { try { localStorage.setItem(TAB_KEY, b.dataset.tab); } catch (e) {} }));
  const startTab = (async () => {
    for (let i = 0; i < 60 && !library; i++) await sleep(25);
    let tab = null;
    try { tab = localStorage.getItem(TAB_KEY); } catch (e) {}
    if (!tab && library && library.length) tab = 'library';
    const button = tab && tab !== 'add' && document.querySelector(`nav [data-tab="${tab}"]`);
    if (button && $('tagger').classList.contains('hidden')) button.click();
  })();

  // ---------- title bar ----------
  // The window has no Windows frame (it would be white in every theme): this bar can be dragged
  // (double-click maximizes) and has the usual three buttons, drawn with Windows' own icon font.
  const bar = make('div', 'titlebar');
  bar.setAttribute('data-tauri-drag-region', '');
  const barTitle = make('div', 'tb-title');
  barTitle.setAttribute('data-tauri-drag-region', '');
  bar.append(barTitle);
  const glyph = { minimize: '\uE921', maximize: '\uE922', restore: '\uE923', close: '\uE8BB' };
  const tbButtons = {};
  for (const act of ['minimize', 'maximize', 'close']) {
    const b = make('button', 'tb-btn ' + act, glyph[act]);
    b.type = 'button'; b.tabIndex = -1;
    b.title = { minimize: 'Minimize', maximize: 'Maximize', close: 'Close' }[act];
    b.onclick = () => tauriInvoke('window_action', { action: act }).then(showMaximized).catch(() => {});
    tbButtons[act] = b;
    bar.append(b);
  }
  function showMaximized(max) {
    tbButtons.maximize.textContent = max ? glyph.restore : glyph.maximize;
    tbButtons.maximize.title = max ? 'Restore' : 'Maximize';
    document.documentElement.classList.toggle('maximized', !!max);
  }
  document.body.append(bar);
  document.querySelector('header').setAttribute('data-tauri-drag-region', '');
  if (brand) brand.setAttribute('data-tauri-drag-region', '');
  let sizeTimer = null;
  window.addEventListener('resize', () => {
    clearTimeout(sizeTimer);
    sizeTimer = setTimeout(() => tauriInvoke('window_action', { action: 'state' }).then(showMaximized).catch(() => {}), 120);
  });
  // the section's name in the bar
  const showTitle = () => { const on = document.querySelector('nav button.on'); barTitle.textContent = on ? (on.querySelector('.nt-title') || on).textContent.trim() : ''; };
  document.querySelectorAll('nav button').forEach(b => b.addEventListener('click', () => setTimeout(showTitle)));
  showTitle();

  // ---------- keyboard ----------
  // / or Ctrl+F: search the Library · Esc in the search: clear it · Ctrl+1…6: the sections
  document.addEventListener('keydown', e => {
    const typing = /INPUT|SELECT|TEXTAREA/.test(e.target.tagName);
    if ((e.key === '/' && !typing) || (e.key.toLowerCase() === 'f' && e.ctrlKey)) {
      if (current || !$('show').classList.contains('hidden')) return;
      e.preventDefault();
      document.querySelector('nav [data-tab="library"]').click();
      searchInput.focus(); searchInput.select();
    } else if (e.ctrlKey && /^[1-6]$/.test(e.key)) {
      e.preventDefault();
      document.querySelectorAll('nav button')[+e.key - 1].click();
    }
  });
  searchInput.addEventListener('keydown', e => { if (e.key === 'Escape' && searchInput.value) { e.stopPropagation(); searchInput.value = ''; runSearch(); } });

  // ---------- empty results: a way back ----------
  const afterRenderOrig = afterRenderLibrary;
  afterRenderLibrary = list => {
    afterRenderOrig(list);
    if (list.length) return;
    queueMicrotask(() => {
      const empty = $('grid').querySelector('.empty');
      if (!empty || !libraryTest) return;
      empty.textContent = 'Nothing matches this search.';
      const clear = make('button', 'btn', 'Clear the search');
      clear.onclick = () => { searchInput.value = ''; runSearch(); };
      empty.append(make('br'), clear);
    });
  };

  // ---------- show the window once it's drawn ----------
  // It starts hidden in the theme's colour (no white flash). With the heart loading screen it's
  // shown at once and the heart fills as things load; without it, when the first section is in
  // place (at most 1.5 s; the backend shows it after 3 s regardless).
  const firstScreen = async () => {
    for (let i = 0; i < 150; i++) {
      const imgs = [...document.querySelectorAll('#grid img, #cleanGrid img')].filter(im => { const r = im.getBoundingClientRect(); return r.width && r.bottom > 0 && r.top < innerHeight; });
      if (!imgs.length && !$('tab-library').classList.contains('hidden') && library && library.length && i < 40) { await sleep(10); continue; }
      if (imgs.every(im => im.complete)) return;
      await sleep(10);
    }
  };
  if (splash) {
    document.fonts.load('800 15px Nunito').catch(() => {}).then(() => setTimeout(() => tauriInvoke('window_ready').catch(() => {}), 20));
    setupReady.then(() => splash.reach(0.35));
    (async () => { while (!library) await sleep(20); splash.reach(0.62); await startTab; splash.reach(0.8); await firstScreen(); await decorReady; splash.reach(1); })();
  } else {
    Promise.race([Promise.all([startTab, document.fonts.ready, decorReady]), sleep(1500)])
      .then(() => new Promise(r => setTimeout(r, 30)))
      .then(() => tauriInvoke('window_ready'))
      .catch(() => {});
  }

  // the loading screen's switch, in Settings → Look
  SETTINGS.splice(SETTINGS.findIndex(s => s.key === 'theme') + 1, 0,
    { section: 'Look', key: 'splash', label: 'Heart loading screen at start-up', type: 'toggle', def: true });
  settings.splash = splashOn;
  // Animations: Always (default: you asked for the motion), Follow Windows (off when Windows has
  // "Show animations" turned off), or Off. The heart loading screen always moves.
  SETTINGS.splice(SETTINGS.findIndex(s => s.key === 'splash') + 1, 0,
    { section: 'Look', key: 'motion', label: 'Animations', type: 'select', def: 'always',
      options: [['always', 'Always'], ['system', 'Follow Windows'], ['off', 'Off']] });
  const savedLook = (() => { try { return JSON.parse(localStorage.getItem('imageVault.settings')) || {}; } catch (e) { return {}; } })();
  settings.motion = ['always', 'system', 'off'].includes(savedLook.motion) ? savedLook.motion : 'always';

  // ---------- Rose: light gray and white with the rose palette, the default theme ----------
  THEME_LIST.unshift(['rose', 'Rose']);
  SETTINGS.find(s => s.key === 'theme').def = 'rose';
  // nothing saved here (or in the data folder): Rose
  if (!savedLook.theme) { settings.theme = 'rose'; applyTheme(); }
  setupReady.then(s => { if (s && !(s.settings && s.settings.theme)) { settings.theme = 'rose'; applyTheme(); } });

  // ---------- decorations: the rose-tinted girls (art/decor, made by tools/make-decor.mjs) ----------
  // A column of three standing beside the page on Add, Stats and Settings (where the window is wide
  // enough), one under the slideshow plan, and tiny ones peeking over lines: the slideshow plan, the
  // tag panel, the tagging form, the clean-up bar. All of them are decoded before the loading screen
  // closes, so they're there with everything else. Settings → Look → Decorations hides them all.
  const decor = (name, cls) => { const im = make('img', 'decor ' + cls); im.src = `/decor/${name}.webp`; im.alt = ''; im.draggable = false; return im; };
  // a girl peeking over the top edge of `host` (head and shoulders show above the line)
  const peek = (host, name, { side = 'right', at = 18, flip = false, tilt = 0 } = {}) => {
    if (!host || host.querySelector(':scope > .peek')) return;
    host.classList.add('peek-host'); // (relative unless it's positioned already: desktop.css. Asking with getComputedStyle cost a full layout of the new grid)
    const box = make('span', 'decor peek');
    box.style[side] = at + 'px';
    const im = make('img');
    im.src = `/decor/${name}.webp`; im.alt = ''; im.draggable = false;
    if (flip || tilt) im.style.transform = `${flip ? 'scaleX(-1) ' : ''}rotate(${tilt}deg)`;
    box.append(im);
    host.append(box);
  };
  const column = (tab, names) => {
    const rail = make('div', 'decor-rail');
    rail.append(...names.map(n => decor(n, 'rail')));
    $(tab).append(rail);
    $(tab).classList.add('has-rail');
  };
  column('tab-add', ['bikini', 'croptop', 'corset']);
  column('tab-stats', ['yoruichi', 'bikini', 'tearju']);
  column('tab-settings', ['tearju', 'sonico', 'yoruichi']);
  $('tab-slideshow').append(decor('sonico', 'decor-under-plan'));
  peek($('tagger'), 'croptop', { at: 60, tilt: -4 }); // over the tagging form (which scrolls, so not inside it)
  peek(document.querySelector('.show-summary'), 'corset', { at: 30 });
  peek(document.querySelector('.cleanup-bar'), 'suit', { at: 40 });
  if (splash) document.getElementById('splash').prepend(decor('corset', 'decor-splash left'), decor('tearju', 'decor-splash right'));
  panel.addEventListener('drawn', () => peek(panel.querySelector('.panel-head'), 'yoruichi', { at: 8, flip: true }));
  // click one and she bounces (with a little heart)
  document.addEventListener('click', e => {
    const im = e.target.closest('.decor-rail .decor, .decor-under-plan, .peek img, .decor-empty, .decor-drop');
    if (!im) return;
    im.animate([{ translate: '0 0', scale: '1 1' }, { translate: '0 -16px', scale: '.97 1.04', offset: .3 }, { translate: '0 0', scale: '1.04 .95', offset: .55 },
      { translate: '0 -5px', scale: '1 1', offset: .75 }, { translate: '0 0', scale: '1 1' }], { duration: 620, easing: 'ease-out' });
    const h = make('span', 'decor-heart', '\u2665');
    h.style.left = e.clientX + 'px'; h.style.top = e.clientY + 'px';
    document.body.append(h);
    setTimeout(() => h.remove(), 900);
  });
  // every picture decoded up front (the loading screen waits for this, at most 2 s)
  const decorReady = Promise.race([
    Promise.all(['bikini', 'corset', 'croptop', 'sonico', 'suit', 'tearju', 'yoruichi'].map(n => {
      const im = new Image(); im.src = `/decor/${n}.webp`; return im.decode().catch(() => {});
    })),
    sleep(2000)
  ]);
  SETTINGS.splice(SETTINGS.findIndex(s => s.key === 'motion') + 1, 0,
    { section: 'Look', key: 'decor', label: 'Decorations', help: 'The little rose-tinted girls around the app.', type: 'toggle', def: true });
  settings.decor = savedLook.decor !== false;
  const applyDecor = () => document.documentElement.classList.toggle('no-decor', !S('decor'));
  applyDecor();
  const saveSettingsDecor = saveSettingsSoon;
  saveSettingsSoon = function () { applyDecor(); return saveSettingsDecor.apply(this, arguments); };
  setupReady.then(() => applyDecor());

  // ---------- time spent looking at each item ----------
  // More telling than how often something was opened: how long it was actually on screen. Counted
  // every second while an item is in the full view or on screen in the slideshow, the window is in
  // front and visible, and (for a still picture) there was some mouse or keyboard activity in the
  // last 3 minutes, so walking away doesn't count. Kept in views.csv's seconds_viewed; seconds not
  // sent yet wait in localStorage, like views.
  const TIME_KEY = 'imageVault.pendingTime', IDLE_MS = 180e3, TIME_FLUSH_MS = 20e3;
  let pendingTime = (() => { try { return JSON.parse(localStorage.getItem(TIME_KEY)) || {}; } catch (e) { return {}; } })();
  const savePendingTime = () => { try { localStorage.setItem(TIME_KEY, JSON.stringify(pendingTime)); } catch (e) {} };
  timeOf = id => ((viewCounts.get(id) || {}).seconds || 0) + ((pendingTime[id] || {}).s || 0);
  let lastInput = Date.now(), lastTick = performance.now();
  for (const ev of ['mousemove', 'pointerdown', 'keydown', 'wheel']) window.addEventListener(ev, () => { lastInput = Date.now(); }, { passive: true, capture: true });
  function onScreenItem() {
    if (current && !$('detail').classList.contains('hidden')) {
      const v = $('detailVideo');
      return { item: current, moving: !v.classList.contains('hidden') && !v.paused };
    }
    if (!$('show').classList.contains('hidden') && !showPaused && onScreen && onScreen.entry) return { item: onScreen.entry.item, moving: true };
    return null;
  }
  setInterval(() => {
    const now = performance.now(), dt = Math.min(3, (now - lastTick) / 1000);
    lastTick = now;
    if (document.visibilityState !== 'visible' || !document.hasFocus()) return;
    const seen = onScreenItem();
    if (!seen || (!seen.moving && Date.now() - lastInput > IDLE_MS)) return;
    const t = pendingTime[seen.item.id] || (pendingTime[seen.item.id] = { uid: seen.item.uid || '', s: 0 });
    t.s += dt;
    if (Math.round(t.s) % 5 === 0) savePendingTime();
  }, 1000);
  let timeFlushing = false;
  async function flushTime() {
    const batch = Object.entries(pendingTime).filter(([, t]) => t.s >= 1).map(([id, t]) => ({ id, uid: t.uid, seconds: Math.round(t.s * 10) / 10 }));
    if (timeFlushing || !batch.length) return;
    timeFlushing = true;
    try {
      const r = await api('recordTime', batch);
      for (const b of batch) {
        const t = pendingTime[b.id];
        if (t) { t.s -= b.seconds; if (t.s < 0.05) delete pendingTime[b.id]; }
        const c = viewCounts.get(b.id) || { uid: b.uid, views: 0, last: '', likes: 0, lastLike: '' };
        c.seconds = (c.seconds || 0) + b.seconds;
        viewCounts.set(b.id, c);
      }
      savePendingTime();
      viewsWritten(r);
    } catch (e) {
      console.warn('Viewing time not saved yet: ' + errMsg(e));
    } finally {
      timeFlushing = false;
    }
  }
  setInterval(flushTime, TIME_FLUSH_MS);
  document.addEventListener('visibilitychange', () => { if (document.visibilityState === 'hidden') { savePendingTime(); flushTime(); } });
  window.addEventListener('beforeunload', savePendingTime);
  const fmtSpent = s => s >= 3600 ? `${(s / 3600).toFixed(1).replace(/\.0$/, '')} h` : s >= 60 ? `${Math.round(s / 60)} min` : `${Math.round(s)} s`;

  // in the details: how long it has been looked at
  const openDetailTime = openDetail;
  openDetail = function (item) {
    const out = openDetailTime.apply(this, arguments);
    const spent = timeOf(item.id);
    if (spent >= 1 && current === item) $('dName').textContent += ` · ${fmtSpent(spent)} looked at`;
    return out;
  };
  // in the Library: sort by it
  $('fSort').add(new Option('Most time spent', 'time'), $('fSort').querySelector('[value="views"]').nextSibling);
  // in Stats: the total and the items looked at longest
  const renderStatsTime = renderStats;
  renderStats = function () {
    const r = renderStatsTime.apply(this, arguments);
    if (!library || !library.length) return r;
    const total = library.reduce((sum, i) => sum + timeOf(i.id), 0);
    const tiles = $('stats').querySelector('.tiles');
    if (tiles) tiles.prepend(tile('Time looking', fmtSpent(total)));
    const charts = $('stats').querySelector('.charts');
    const longest = library.filter(i => timeOf(i.id) >= 1).sort((a, b) => timeOf(b.id) - timeOf(a.id)).slice(0, 10);
    if (charts) charts.prepend(chart('Most time spent', 'How long each was on screen, in the full view and the slideshow', topList(longest, i => fmtSpent(timeOf(i.id)), '')));
    peek($('stats').querySelector('.chart'), 'tearju', { at: 26 });
    return r;
  };
  const reducedMotion = matchMedia('(prefers-reduced-motion: reduce)');
  const applyMotion = () => document.documentElement.classList.toggle('motion-off', S('motion') === 'off' || (S('motion') === 'system' && reducedMotion.matches));
  applyMotion();
  reducedMotion.addEventListener('change', applyMotion);
  const applyThemeOrig = applyTheme;
  applyTheme = function () { const r = applyThemeOrig.apply(this, arguments); applyMotion(); return r; };
  const saveSettingsSoonOrig = saveSettingsSoon; // a changed setting applies at once
  saveSettingsSoon = function () {
    applyMotion();
    // never save before the data folder's own settings are in: that would write defaults over them
    if (!setup) { setupReady.then(s => s && saveSettingsSoonOrig()); return; }
    return saveSettingsSoonOrig.apply(this, arguments);
  };

  // ---------- the sidebar: big tiles, like Spotify's category cards ----------
  // Each section is a tile filling its share of the sidebar: a bold title, a live line under it and a
  // big tilted icon in the corner; the Library tile shows the newest pictures, tilted, instead.
  const SUBS = { add: 'Drop or choose files', library: '', slideshow: 'Timed NSFW levels', stats: 'Your viewing', cleanup: 'Least opened', settings: 'Theme, look & more' };
  document.querySelectorAll('nav button').forEach(b => {
    const label = b.textContent.trim();
    b.textContent = '';
    b.append(make('span', 'nt-title', label), make('span', 'nt-sub', SUBS[b.dataset.tab] || ''), make('span', 'nt-art'));
  });
  showTitle();
  function drawTiles() {
    if (!library) return;
    const lib = document.querySelector('nav [data-tab="library"]');
    lib.querySelector('.nt-sub').textContent = `${fmtN(library.length)} ${library.length === 1 ? 'item' : 'items'}`;
    // and the space it all takes on disk, under the count
    let size = lib.querySelector('.nt-size');
    if (!size) { size = make('span', 'nt-size'); lib.querySelector('.nt-sub').after(size); }
    clearTimeout(drawTiles.sizeTimer);
    drawTiles.sizeTimer = setTimeout(() => api('vaultSize').then(n => {
      size.textContent = n >= 1e9 ? `${(n / 1e9).toFixed(n >= 1e11 ? 0 : 1)} GB` : `${Math.max(1, Math.round(n / 1e6))} MB`;
      size.title = `Your vault takes ${fmtN(n)} bytes on disk`;
    }).catch(() => {}), 1500);
    const newest = library.filter(i => i.thumb).sort((a, b) => b.added.localeCompare(a.added)).slice(0, 3);
    const art = lib.querySelector('.nt-art');
    art.classList.toggle('pics', newest.length > 0);
    art.replaceChildren(...newest.map(i => { const im = make('img'); im.src = thumbSrc(i, 240); im.alt = ''; im.draggable = false; return im; }));
    const total = library.reduce((sum, i) => sum + timeOf(i.id), 0);
    document.querySelector('nav [data-tab="stats"] .nt-sub').textContent = total >= 1 ? `${fmtSpent(total)} looked at` : 'Your viewing';
    document.querySelector('nav [data-tab="slideshow"] .nt-sub').textContent = session ? 'Session running' : 'Timed NSFW levels';
  }
  (async () => { while (!library) await sleep(50); drawTiles(); })();
  const afterRenderTiles = afterRenderLibrary;
  afterRenderLibrary = list => { afterRenderTiles(list); drawTiles(); };
  setInterval(drawTiles, 30000);
  document.querySelectorAll('nav button').forEach(b => b.addEventListener('click', () => setTimeout(drawTiles, 50)));

  // ---------- welcome: where the vault lives ----------
  // On first start (and from Settings → Storage) the vault's place is chosen: a cloud service's synced
  // folder, where the app makes its own "Image Vault Desktop" folder and the service's app keeps it
  // backed up and in sync; this PC; or any folder. See storage.rs.
  const SERVICE_ICONS = {
    gdrive: '<svg viewBox="0 0 87.3 75"><path d="M29.1 0h29.1l29.1 50H58.2z" fill="#f4b400"/><path d="M29.1 0L0 50l14.5 25 29.1-50z" fill="#0f9d58"/><path d="M14.5 75h58.2l14.6-25H29.1z" fill="#4285f4"/></svg>',
    onedrive: '<svg viewBox="0 0 64 40"><path d="M52 40H14C6.3 40 0 34 0 26.6 0 19.8 5.3 14.2 12.1 13.4 14.6 5.6 21.9 0 30.5 0c7.7 0 14.4 4.6 17.3 11.3C57 11.9 64 19.1 64 28c0 6.6-5.4 12-12 12z" fill="#0078d4"/></svg>',
    dropbox: '<svg viewBox="0 0 64 56"><path d="M16 0L0 10.2l16 10.2 16-10.2zM48 0L32 10.2l16 10.2 16-10.2zM0 30.6l16 10.2 16-10.2-16-10.2zM48 20.4L32 30.6l16 10.2 16-10.2zM16 44.2L32 54.4l16-10.2-16-10.2z" fill="#0061fe"/></svg>',
    icloud: '<svg viewBox="0 0 64 40"><defs><linearGradient id="icg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#7cc4ff"/><stop offset="1" stop-color="#2f8bea"/></linearGradient></defs><path d="M50 40H15C6.7 40 0 33.6 0 25.5 0 18 5.7 11.9 13 11.1 15.8 4.6 22.3 0 29.9 0c8.9 0 16.3 6.2 18.1 14.5C57 15 64 21.4 64 29.5 64 35.3 57.7 40 50 40z" fill="url(#icg)"/></svg>',
    local: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><rect x="2.5" y="3.5" width="19" height="13" rx="2.5"/><path d="M8 20.5h8M12 16.5v4"/></svg>',
    custom: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round"><path d="M3 7.5A2.5 2.5 0 0 1 5.5 5H10l2 2.5h6.5A2.5 2.5 0 0 1 21 10v7.5a2.5 2.5 0 0 1-2.5 2.5h-13A2.5 2.5 0 0 1 3 17.5z"/></svg>',
    untraceable: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M12 2.8 4.5 5.6v5.6c0 4.6 3.1 8.4 7.5 9.9 4.4-1.5 7.5-5.3 7.5-9.9V5.6z"/><rect x="8.6" y="10.6" width="6.8" height="5.4" rx="1.3"/><path d="M10.3 10.6V9.2a1.7 1.7 0 0 1 3.4 0v1.4"/></svg>',
    gdriveweb: '<svg viewBox="0 0 87.3 75"><path d="M29.1 0h29.1l29.1 50H58.2z" fill="#f4b400"/><path d="M29.1 0L0 50l14.5 25 29.1-50z" fill="#0f9d58"/><path d="M14.5 75h58.2l14.6-25H29.1z" fill="#4285f4"/></svg>'
  };
  const welcome = make('div', 'welcome hidden');
  welcome.id = 'welcome';
  welcome.innerHTML = `
    <div class="wl-card">
      <img class="wl-icon" src="/app-icon.png" alt="">
      <h1 class="wl-title"></h1>
      <p class="wl-lead">Choose where your library lives. In a cloud service, Lewd Vault makes its own folder,
        <b>${storageFolderName()}</b>, and the service's app keeps it backed up and in sync. Everything still opens straight from this PC, so it stays fast, even offline.</p>
      <div class="wl-step wl-choose">
        <div class="wl-grid"></div>
        <p class="wl-hint">Installed a cloud app just now? <button class="linkish wl-recheck" type="button">Check again</button></p>
      </div>
      <div class="wl-step wl-confirm hidden"></div>
      <div class="wl-step wl-busy hidden">
        <p class="wl-busy-text">Copying your library…</p>
        <div class="wl-bar"><div class="wl-bar-fill"></div></div>
        <p class="wl-busy-sub"></p>
      </div>
      <button class="btn wl-cancel hidden" type="button">Cancel</button>
    </div>`;
  document.body.append(welcome);
  welcome.prepend(decor('corset', 'decor-welcome left'), decor('tearju', 'decor-welcome right'));
  function storageFolderName() { return 'Image Vault Desktop'; }
  const wl = sel => welcome.querySelector(sel);
  const fmtBytes = n => n >= 1e9 ? (n / 1e9).toFixed(1) + ' GB' : n >= 1e6 ? Math.round(n / 1e6) + ' MB' : Math.round(n / 1e3) + ' KB';
  let wlData = null;
  const step = name => welcome.querySelectorAll('.wl-step').forEach(s => s.classList.toggle('hidden', !s.classList.contains(name)));

  async function openWelcome(firstRun) {
    wl('.wl-title').textContent = firstRun ? 'Welcome to Lewd Vault' : 'Where your vault lives';
    wl('.wl-cancel').classList.toggle('hidden', firstRun);
    welcome.classList.toggle('first', firstRun);
    welcome.classList.remove('hidden');
    step('wl-choose');
    await drawOptions();
  }
  const closeWelcome = () => welcome.classList.add('hidden');
  wl('.wl-cancel').onclick = closeWelcome;
  wl('.wl-recheck').onclick = () => drawOptions();

  async function drawOptions() {
    wlData = await api('storageOptions');
    const cards = wlData.options.map(o => {
      const card = make('button', 'wl-opt' + (o.found ? '' : ' missing') + (o.current ? ' current' : ''));
      card.type = 'button';
      const icon = make('span', 'wl-opt-icon s-' + o.id);
      icon.innerHTML = SERVICE_ICONS[o.id] || SERVICE_ICONS.custom;
      const name = make('span', 'wl-opt-name', o.name);
      const line = make('span', 'wl-opt-line');
      if (o.id === 'untraceable') line.textContent = !o.found ? 'Needs the portable app' : o.current ? 'Encrypted and hidden' : o.exists ? 'A locked vault is there' : 'Encrypted, no traces on this PC';
      else if (!o.found) line.textContent = o.id === 'gdrive' ? 'Drive app not on this PC' : 'Not on this PC';
      else if (o.items) line.textContent = `${fmtN(o.items)} ${o.items === 1 ? 'item' : 'items'} there`;
      else line.textContent = o.id === 'local' ? 'Next to the app' : 'Ready';
      card.append(icon, name, line);
      if (o.current) card.append(make('span', 'wl-badge', 'Now'));
      if (!o.found && o.download) {
        const get = make('span', 'wl-get', 'Get the app');
        get.onclick = e => { e.stopPropagation(); api('openDownload', o.id).catch(err => toast(errMsg(err), 'err')); };
        card.append(get);
      }
      card.title = o.why || o.folder || '';
      card.onclick = () => { if (o.found) confirmOption(o); else if (o.why) toast(o.why, 'err'); else if (o.download) api('openDownload', o.id).catch(() => {}); };
      if (o.id === 'untraceable') card.classList.add('wl-untraceable');
      return card;
    });
    // Google Drive without its app (it needs an administrator): the vault on this PC or Untraceable,
    // synced through a sign-in in the browser (drivesync.js, gdrive.rs)
    const web = make('button', 'wl-opt');
    web.type = 'button';
    const wi = make('span', 'wl-opt-icon s-gdrive');
    wi.innerHTML = SERVICE_ICONS.gdriveweb;
    web.append(wi, make('span', 'wl-opt-name', 'Google Drive in your browser'), make('span', 'wl-opt-line', 'No Drive app or admin needed'));
    web.onclick = () => confirmDriveWeb();
    const at = cards.findIndex(c => c.classList.contains('s-gdrive') || c.querySelector('.s-gdrive'));
    cards.splice(at + 1, 0, web);
    const other = make('button', 'wl-opt');
    other.type = 'button';
    const oi = make('span', 'wl-opt-icon s-custom');
    oi.innerHTML = SERVICE_ICONS.custom;
    other.append(oi, make('span', 'wl-opt-name', 'Another folder…'), make('span', 'wl-opt-line', 'A USB drive, NAS, any synced folder'));
    other.onclick = async () => {
      const picked = await tauriInvoke('pick_folder').catch(() => null);
      if (picked) confirmOption(await api('storageForPicked', picked));
    };
    wl('.wl-grid').replaceChildren(...cards.filter(c => !c.classList.contains('custom')), other);
  }

  // "Google Drive in your browser": where the vault itself lives, then the sign-in after the restart
  function confirmDriveWeb() {
    const box = wl('.wl-confirm');
    box.replaceChildren();
    const head = make('div', 'wl-confirm-head');
    const icon = make('span', 'wl-opt-icon s-gdrive');
    icon.innerHTML = SERVICE_ICONS.gdriveweb;
    head.append(icon, make('h2', '', 'Google Drive in your browser'));
    box.append(head, make('p', 'wl-note', 'For PCs where Google Drive\'s app can\'t be installed (it needs an administrator). Your vault syncs with a folder in your Drive; you sign in once in your web browser, right after this. Where should the vault itself be kept on this PC?'));
    const local = wlData.options.find(x => x.id === 'local'), untr = wlData.options.find(x => x.id === 'untraceable');
    const pick = make('div', 'wl-choice');
    const choice = (o, title, sub) => {
      const b = make('button', 'wl-pick');
      b.type = 'button';
      b.append(make('b', '', title), make('small', '', sub));
      b.disabled = !o || !o.found;
      if (o && o.why) b.title = o.why;
      b.onclick = () => confirmOption(o, { drive: true });
      return b;
    };
    pick.append(choice(local, 'On this PC', 'A normal folder, fastest'), choice(untr, 'Untraceable', 'Encrypted and hidden, nothing else on this PC'));
    const actions = make('div', 'wl-actions');
    const back = make('button', 'btn', 'Back');
    back.type = 'button';
    back.onclick = () => step('wl-choose');
    actions.append(back);
    box.append(pick, actions);
    step('wl-confirm');
  }

  function confirmOption(o, extra = {}) {
    if (o.id === 'untraceable') return confirmUntraceable(o, extra);
    const box = wl('.wl-confirm');
    box.replaceChildren();
    const head = make('div', 'wl-confirm-head');
    const icon = make('span', 'wl-opt-icon s-' + o.id);
    icon.innerHTML = SERVICE_ICONS[o.id] || SERVICE_ICONS.custom;
    head.append(icon, make('h2', '', o.current ? `Keep it on ${o.name}` : `Use ${o.name}`));
    const where = make('p', 'wl-where');
    where.append('Your vault: ', make('code', '', o.folder));
    box.append(head, where);
    const here = wlData.currentItems || 0;
    let copy = null;
    if (o.current) {
      box.append(make('p', 'wl-note', here ? `This is where your ${fmtN(here)} items are now.` : 'This is where your vault is now.'));
    } else if (o.items) {
      box.append(make('p', 'wl-note', `There's already a vault there with ${fmtN(o.items)} items. Lewd Vault will open it; the one you have now stays where it is.`));
    } else if (here) {
      copy = make('div', 'wl-choice');
      copy.innerHTML = `<label><input type="radio" name="wlcopy" value="1" checked> <span><b>Copy my ${fmtN(here)} items there</b><small>Your current folder stays as it is, as a backup.</small></span></label>
        <label><input type="radio" name="wlcopy" value="0"> <span><b>Start with an empty vault</b><small>Your current library stays where it is.</small></span></label>`;
      box.append(copy);
    } else if (o.id !== 'local' && o.id !== 'custom') {
      box.append(make('p', 'wl-note', `${o.name} will upload everything you add and keep it in sync on your other devices.`));
    }
    if (extra.drive) box.append(make('p', 'wl-note', 'After this, Lewd Vault restarts and you sign in to Google Drive in your web browser.'));
    if (wlData.untraceable && !o.current) box.append(make('p', 'wl-note wl-warn', 'Your vault is untraceable now. Here it will be kept unencrypted again.'));
    const actions = make('div', 'wl-actions');
    const back = make('button', 'btn', 'Back');
    const go = make('button', 'btn primary', o.current ? 'Keep it here' : o.items ? 'Open that vault' : 'Create my vault');
    back.type = go.type = 'button';
    back.onclick = () => step('wl-choose');
    go.onclick = () => useOption(o, copy ? copy.querySelector('input:checked').value === '1' : false, { driveSetup: !!extra.drive });
    actions.append(back, go);
    box.append(actions);
    step('wl-confirm');
  }

  // Untraceable (crypt.rs): a password, what to bring along, what to clear away
  function confirmUntraceable(o, extra) {
    const box = wl('.wl-confirm');
    box.replaceChildren();
    const head = make('div', 'wl-confirm-head');
    const icon = make('span', 'wl-opt-icon s-untraceable');
    icon.innerHTML = SERVICE_ICONS.untraceable;
    head.append(icon, make('h2', '', o.current ? 'Untraceable' : 'Make it untraceable'));
    box.append(head);
    if (o.current) {
      box.append(make('p', 'wl-note', 'Your vault is untraceable already: encrypted in a hidden folder.'));
      const where = make('p', 'wl-where');
      where.append('Kept in: ', make('code', '', o.folder));
      box.append(where);
    } else {
      const fresh = !o.exists;
      box.append(make('p', 'wl-note', fresh
        ? 'For a PC that isn\'t only yours. Your vault is kept encrypted in a hidden folder, and nothing else is stored anywhere on this PC: no thumbnails, no caches, no browser history. Without the password the files can\'t be opened, found by Windows Search or previewed. If you forget it, nothing can bring the vault back.'
        : 'There\'s an untraceable vault here already. Its password opens it.'));
      const pw = make('input'); Object.assign(pw, { type: 'password', placeholder: fresh ? 'Password (at least 6 characters)' : 'Its password', autocomplete: 'off' });
      const pw2 = make('input'); Object.assign(pw2, { type: 'password', placeholder: 'The same again', autocomplete: 'off' });
      // where: next to the app (portable), Documents (installed), or another place
      const where = make('p', 'wl-where');
      where.append('Kept in: ', make('code', '', o.folder));
      const other = make('button', 'linkish', 'Another place…');
      other.type = 'button';
      other.onclick = async () => {
        const picked = await tauriInvoke('pick_folder').catch(() => null);
        if (picked) confirmUntraceable(await api('untraceableForPicked', picked), extra);
      };
      where.append(' ', other);
      box.append(where);
      if (o.synced) box.append(make('p', 'wl-note wl-warn', 'This folder is uploaded by a cloud service (OneDrive often backs up Documents on work PCs). The files stay encrypted, but they\'d be in that cloud too: pick another place if that matters.'));
      const fields = make('div', 'wl-pw');
      fields.append(pw);
      if (fresh) fields.append(pw2);
      box.append(fields);
      const here = wlData.currentItems || 0;
      let copy = null, del = null;
      if (fresh && here) {
        copy = make('div', 'wl-choice');
        copy.innerHTML = `<label><input type="radio" name="wlcopy" value="1" checked> <span><b>Move my ${fmtN(here)} items in, encrypted</b><small>They're copied in and encrypted on the way.</small></span></label>
          <label><input type="radio" name="wlcopy" value="0"> <span><b>Start with an empty vault</b><small>Your current library stays where it is.</small></span></label>`;
        box.append(copy);
        del = make('label', 'wl-check');
        const cb = make('input'); cb.type = 'checkbox';
        del.append(cb, make('span', '', 'Then delete the unencrypted library (permanently, not to the Recycle Bin; from the cloud too if it\'s a synced folder)'));
        box.append(del);
      }
      const scrub = make('label', 'wl-check');
      const sc = make('input'); sc.type = 'checkbox'; sc.checked = true;
      scrub.append(sc, make('span', '', 'Clear what Lewd Vault kept on this PC before (thumbnail and web caches, the built-in browser\'s data)'));
      box.append(scrub);
      box.append(make('p', 'wl-note wl-tip', 'Tip: Windows remembers the names of programs that ran and installed. The portable app (renamed to something dull, say "Notes.exe") leaves less behind than the installer, which shows in the Start menu and in Settings → Apps.'));
      if (extra.drive) box.append(make('p', 'wl-note', 'After this, Lewd Vault restarts and you sign in to Google Drive in your web browser. In Drive, the vault stays encrypted too.'));
      const actions = make('div', 'wl-actions');
      const back = make('button', 'btn', 'Back');
      const go = make('button', 'btn primary', fresh ? 'Make it untraceable' : 'Open that vault');
      back.type = go.type = 'button';
      back.onclick = () => step('wl-choose');
      go.onclick = () => {
        if (fresh && pw.value.length < 6) { toast('Use a password of at least 6 characters', 'err'); pw.focus(); return; }
        if (fresh && pw.value !== pw2.value) { toast('The two passwords are different', 'err'); pw2.focus(); return; }
        if (!pw.value) { pw.focus(); return; }
        const moving = copy ? copy.querySelector('input:checked').value === '1' : false;
        useOption(o, moving, { untraceable: true, password: pw.value, scrub: sc.checked, deleteOld: moving && !!(del && del.querySelector('input').checked), driveSetup: !!extra.drive });
      };
      actions.append(back, go);
      box.append(actions);
      step('wl-confirm');
      setTimeout(() => pw.focus(), 30);
      return;
    }
    const actions = make('div', 'wl-actions');
    const back = make('button', 'btn', 'Back');
    back.type = 'button';
    back.onclick = () => step('wl-choose');
    actions.append(back);
    if (extra.drive) {
      const go = make('button', 'btn primary', 'Sign in to Google Drive');
      go.type = 'button';
      go.onclick = () => useOption(o, false, { driveSetup: true });
      actions.append(go);
    }
    box.append(actions);
    step('wl-confirm');
  }

  async function useOption(o, copy, opts = {}) {
    step('wl-busy');
    wl('.wl-busy-text').textContent = copy ? (opts.untraceable ? 'Encrypting your library…' : 'Copying your library…') : 'Setting up your vault…';
    const fill = wl('.wl-bar-fill'), sub = wl('.wl-busy-sub');
    fill.style.width = copy ? '0%' : '60%';
    sub.textContent = '';
    const poll = copy ? setInterval(async () => {
      const p = await api('copyProgress').catch(() => null);
      if (p && p.total) { fill.style.width = (p.done / p.total * 100).toFixed(1) + '%'; sub.textContent = `${fmtBytes(p.done)} of ${fmtBytes(p.total)}`; }
    }, 300) : null;
    try {
      const r = await api('useStorage', o.folder, copy, opts);
      clearInterval(poll);
      fill.style.width = '100%';
      if (!r.restart) { closeWelcome(); return; }
      wl('.wl-busy-text').textContent = 'Opening your vault…';
      sub.textContent = o.folder;
      setTimeout(() => tauriInvoke('restart_app'), 600);
    } catch (e) {
      clearInterval(poll);
      step('wl-confirm');
      toast(errMsg(e), 'err');
    }
  }

  // first start: ask (behind the loading screen, which fades to reveal it)
  setupReady.then(s => { if (s && s.firstRun) openWelcome(true); });
  // Settings → Storage
  storageHead.textContent = 'Storage';
  const changeBtn = make('button', 'btn primary', 'Change where it lives…');
  changeBtn.onclick = () => openWelcome(false);
  folderBtn.after(changeBtn);

  // ---------- category fields: a dropdown of the categories you already have ----------
  function categoryCombo(input) {
    if (!input || input.closest('.combo')) return;
    input.removeAttribute('list'); // the plain datalist popup is replaced by this one
    input.autocomplete = 'off';
    const wrap = make('div', 'combo');
    input.before(wrap);
    const btn = make('button', 'combo-btn');
    btn.type = 'button'; btn.tabIndex = -1; btn.title = 'Your categories';
    const menu = make('div', 'combo-menu hidden');
    wrap.append(input, btn, menu);
    let mark = -1;
    const names = () => [...new Set([...$('catList').options].map(o => o.value).filter(Boolean))].sort((a, b) => a.localeCompare(b));
    function draw(all) {
      const text = input.value.trim().toLowerCase();
      const list = names().filter(n => all || !text || n.toLowerCase().includes(text));
      mark = -1;
      menu.replaceChildren(...list.map(n => {
        const o = make('button', 'combo-opt' + (n === input.value ? ' on' : ''), n);
        o.type = 'button';
        o.addEventListener('mousedown', e => { e.preventDefault(); pick(n); });
        return o;
      }));
      if (!list.length) menu.append(make('div', 'combo-empty', text ? `"${input.value.trim()}" will be a new category` : 'No categories yet. Type one.'));
      menu.classList.remove('hidden');
    }
    const close = () => menu.classList.add('hidden');
    function pick(n) {
      input.value = n; close();
      input.dispatchEvent(new Event('input', { bubbles: true })); input.dispatchEvent(new Event('change', { bubbles: true }));
    }
    btn.addEventListener('mousedown', e => { e.preventDefault(); if (menu.classList.contains('hidden')) { input.focus(); draw(true); } else close(); });
    input.addEventListener('input', e => { if (e.isTrusted) draw(false); });
    input.addEventListener('focus', () => draw(true));
    input.addEventListener('blur', () => setTimeout(close, 120));
    input.addEventListener('keydown', e => {
      const opts = [...menu.querySelectorAll('.combo-opt')];
      if (menu.classList.contains('hidden') || !opts.length) return;
      if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
        e.preventDefault();
        mark = (mark + (e.key === 'ArrowDown' ? 1 : -1) + opts.length) % opts.length;
        opts.forEach((o, k) => o.classList.toggle('mark', k === mark));
        opts[mark].scrollIntoView({ block: 'nearest' });
      } else if (e.key === 'Enter' && mark >= 0) { e.preventDefault(); pick(opts[mark].textContent); }
      else if (e.key === 'Escape') { e.stopPropagation(); close(); }
    });
  }
  categoryCombo($('dCat'));
  categoryCombo($('category'));
  // and any added later (Discover's "Save as")
  new MutationObserver(() => document.querySelectorAll('input[list="catList"]').forEach(categoryCombo))
    .observe(document.body, { childList: true, subtree: true });

  // shared with discover.js
  // look time from elsewhere (library.js: moving previews on screen count 0.1 s a second)
  const addTime = (item, seconds) => { const t = pendingTime[item.id] || (pendingTime[item.id] = { uid: item.uid || '', s: 0 }); t.s += seconds; };
  // a little celebration: hearts and sparkles fly out of an element (a save that finished...)
  function burst(el, n = 14) {
    if (!el || !el.isConnected) return;
    const r = el.getBoundingClientRect(), cx = r.left + r.width / 2, cy = r.top + r.height / 2;
    for (let k = 0; k < n; k++) {
      const p = make('span', 'burst-bit', ['♥', '✦', '♥', '✧', '♥'][k % 5]);
      const a = (k / n) * Math.PI * 2 + Math.random() * 0.4, d = 40 + Math.random() * 50;
      p.style.left = cx + 'px'; p.style.top = cy + 'px';
      p.style.setProperty('--x', Math.cos(a) * d * (r.width > 120 ? 1.6 : 1) + 'px'); p.style.setProperty('--y', Math.sin(a) * d - 18 + 'px');
      p.style.setProperty('--r', (Math.random() * 120 - 60) + 'deg');
      p.style.fontSize = (10 + Math.random() * 12) + 'px';
      p.style.color = ['#ff4f9a', '#ffd36b', '#ff8ec2', '#ffffff', '#c58bff'][k % 5];
      document.body.append(p);
      setTimeout(() => p.remove(), 900);
    }
    el.classList.remove('celebrate'); void el.offsetWidth; el.classList.add('celebrate');
  }
  // a ripple where a chip or tag is clicked
  document.addEventListener('pointerdown', e => {
    const chip = e.target.closest('.tchip, .dc-chip, .vd-tag, .ask-chip, .ask-lv, .ask-star, .au-src, .dc-vw, .dc-rate, .dc-span, .lv-item');
    if (!chip) return;
    const r = chip.getBoundingClientRect(), rip = make('span', 'ripple');
    const size = Math.max(r.width, r.height) * 2;
    Object.assign(rip.style, { width: size + 'px', height: size + 'px', left: (e.clientX - r.left - size / 2) + 'px', top: (e.clientY - r.top - size / 2) + 'px' });
    if (getComputedStyle(chip).position === 'static') chip.style.position = 'relative';
    chip.style.overflow = 'hidden';
    chip.append(rip);
    setTimeout(() => rip.remove(), 600);
    chip.classList.remove('squish'); void chip.offsetWidth; chip.classList.add('squish');
  }, true);
  // Two PCs changed the vault at once and the cloud service kept both ("index (1).csv"): fold the
  // copy in (main.rs merge_conflicts) at start, every few minutes and on coming back to the window.
  async function mergeConflicts() {
    try {
      const r = await api('mergeConflicts');
      if (!r.files.length) return;
      const what = [r.added && `${fmtN(r.added)} ${r.added === 1 ? 'item' : 'items'}`, r.counts && 'view counts'].filter(Boolean).join(' and ');
      toast(`Merged ${r.files.join(', ')} from another PC${what ? ` (${what})` : ''}. The copy is in _app/backups/conflicts.`, 'ok');
      if (r.added || r.counts) loadLibrary();
    } catch (e) { console.warn('Conflict check: ' + errMsg(e)); }
  }
  setTimeout(mergeConflicts, 4000);
  // thumbnail packs: leftovers of deleted items and replaced thumbnails cleared out (main.rs
  // compact_packs), once a session, a while after start so it doesn't compete with loading
  setTimeout(async () => {
    try {
      const r = await api('compactPacks');
      if (r.rewritten) loadLibrary();
      if (r.freed >= 1e6) toast(`Tidied thumbnail packs: ${Math.round(r.freed / 1e6)} MB freed`, 'ok');
    } catch (e) { console.warn('Pack tidy: ' + errMsg(e)); }
  }, 45e3);
  setInterval(mergeConflicts, 3 * 60e3);
  window.addEventListener('focus', () => { clearTimeout(mergeConflicts.t); mergeConflicts.t = setTimeout(mergeConflicts, 1500); });
  window.vaultUI = { make, burst, tagChip, tagBox, pretty, typeOf, nameOf, cleanTags, showTitle, peek, decor, TAB_KEY, foldChips,
    tagsChanged, rememberTags, tagsOf, addTime, thumbSrc };
  window.vaultTags = { cleanTag, cleanTags, compile, suggest, tagCounts }; // for testing
})();
