// Lewd Vault desktop: Audio (Phase 5 of PLAN.md), a sidebar section of its own.
//   - Yours: your audio (JOI, hypno, ASMR, erotic stories, ...), in categories. Add files by dropping
//     them on the page or with the button; they're kept in the vault (_app/audio), listed in
//     _app/kv/audio.json.
//   - Find: audio from many places: Reddit's audio communities (one tab), soundgasm performers, Audio
//     Desires (voice actors' stories, previews), Classic erotica (LibriVox), Freesound (with your key),
//     or any link you paste. Listen right away; saving asks for its category, then keeps it in Yours.
//   - The player sits at the bottom of the window and keeps playing whatever page you're on: play,
//     ±15 s, a scrub bar, speed, volume, a sleep timer; media keys work too. It remembers where each
//     one was left.

(() => {
  'use strict';
  if (!window.vaultUI || !inTauri) return;
  const { make, showTitle, TAB_KEY } = window.vaultUI;
  const enc = encodeURIComponent;
  const fmt = s => { s = Math.max(0, Math.round(s || 0)); const h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60, x = s % 60; return (h ? h + ':' + String(m).padStart(2, '0') : m) + ':' + String(x).padStart(2, '0'); };
  const proxy = url => url ? 'http://vault.localhost/web?u=' + enc(url) : '';
  const clean = text => !window.vaultDiscover || !window.vaultDiscover.NEVER_TEXT.test(text || '');
  const CATS = ['JOI', 'Hypno', 'ASMR', 'Erotic stories', 'Moans', 'Music', 'Other'];
  const AUDIO_EXT = /\.(mp3|m4a|aac|ogg|oga|opus|flac|wav)$/i;

  // ---------- the list, kept with the vault ----------
  let list = { items: [] };
  const ready = setupReady.then(async () => { list = (await api('kvGet', 'audio').catch(() => null)) || { items: [] }; });
  let saveTimer = null;
  const save = () => { clearTimeout(saveTimer); saveTimer = setTimeout(() => api('kvSet', 'audio', list).catch(e => toast(errMsg(e), 'err')), 600); };

  // ---------- the player at the bottom ----------
  const audio = new Audio();
  audio.preload = 'auto';
  const dock = make('div', 'au-dock hidden');
  dock.innerHTML = `
    <div class="au-art"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><path d="M4 14v-2a8 8 0 0 1 16 0v2"/><rect x="3" y="14" width="4" height="7" rx="1.5"/><rect x="17" y="14" width="4" height="7" rx="1.5"/></svg></div>
    <div class="au-meta"><b class="au-name"></b><span class="au-from"></span></div>
    <div class="au-mid">
      <div class="au-btns">
        <button class="au-b au-back" title="Back 15 s">−15</button>
        <button class="au-b au-play" title="Play / pause">▶</button>
        <button class="au-b au-fwd" title="Forward 15 s">+15</button>
      </div>
      <div class="au-scrub"><span class="au-t">0:00</span><input type="range" class="au-seek" min="0" max="1000" value="0"><span class="au-d">0:00</span></div>
    </div>
    <div class="au-right">
      <button class="au-b au-speed" title="Speed">1×</button>
      <button class="au-b au-sleep" title="Sleep timer: stop after…">☾</button>
      <input type="range" class="au-vol" min="0" max="1" step="0.02" title="Volume">
      <button class="au-b au-save hidden" title="Save into your audio">♥ Save</button>
      <button class="au-b au-close" title="Stop and close">×</button>
    </div>`;
  document.body.append(dock);
  const d = s => dock.querySelector(s);
  let playing = null; // { id?, title, from, url, item? }
  try { audio.volume = +(localStorage.getItem('imageVault.audioVolume') || 1); } catch (e) {}
  d('.au-vol').value = audio.volume;
  d('.au-vol').oninput = e => { audio.volume = +e.target.value; try { localStorage.setItem('imageVault.audioVolume', audio.volume); } catch (x) {} };
  d('.au-play').onclick = () => audio.paused ? audio.play().catch(() => {}) : audio.pause();
  d('.au-back').onclick = () => { audio.currentTime = Math.max(0, audio.currentTime - 15); };
  d('.au-fwd').onclick = () => { audio.currentTime = Math.min(audio.duration || 0, audio.currentTime + 15); };
  const SPEEDS = [0.75, 1, 1.25, 1.5];
  d('.au-speed').onclick = () => { const k = SPEEDS.indexOf(audio.playbackRate); audio.playbackRate = SPEEDS[(k + 1) % SPEEDS.length]; d('.au-speed').textContent = audio.playbackRate + '×'; };
  let sleepAt = 0, sleepTimer = null;
  d('.au-sleep').onclick = () => {
    const steps = [0, 15, 30, 45, 60];
    const now = sleepAt ? Math.round((sleepAt - Date.now()) / 60000) : 0;
    const next = steps[(steps.findIndex(m => m >= now) + 1) % steps.length];
    clearTimeout(sleepTimer); sleepAt = 0;
    if (next) { sleepAt = Date.now() + next * 60000; sleepTimer = setTimeout(() => { audio.pause(); sleepAt = 0; d('.au-sleep').textContent = '☾'; toast('Sleep timer: stopped', 'ok'); }, next * 60000); }
    d('.au-sleep').textContent = next ? `☾ ${next}m` : '☾';
    toast(next ? `Stops in ${next} minutes` : 'Sleep timer off', 'ok');
  };
  let seeking = false;
  d('.au-seek').addEventListener('input', () => { seeking = true; d('.au-t').textContent = fmt(d('.au-seek').value / 1000 * (audio.duration || 0)); });
  d('.au-seek').addEventListener('change', () => { audio.currentTime = d('.au-seek').value / 1000 * (audio.duration || 0); seeking = false; });
  d('.au-close').onclick = () => { remember(); audio.pause(); audio.removeAttribute('src'); audio.load(); playing = null; dock.classList.add('hidden'); document.documentElement.classList.remove('au-open'); drawList(); };
  d('.au-save').onclick = () => playing && saveFound(playing, d('.au-save'));
  const draw = () => {
    d('.au-play').textContent = audio.paused ? '▶' : '❚❚';
    d('.au-d').textContent = fmt(audio.duration);
    if (!seeking) { d('.au-t').textContent = fmt(audio.currentTime); d('.au-seek').value = audio.duration ? audio.currentTime / audio.duration * 1000 : 0; }
    dock.classList.toggle('playing', !audio.paused);
  };
  for (const ev of ['timeupdate', 'play', 'pause', 'durationchange', 'loadedmetadata']) audio.addEventListener(ev, draw);
  // where each one was left
  let lastKept = 0;
  const remember = () => {
    const it = playing && playing.item;
    if (!it || !audio.currentTime) return;
    it.pos = audio.currentTime > (audio.duration || 0) - 10 ? 0 : Math.round(audio.currentTime);
    save();
  };
  audio.addEventListener('timeupdate', () => { if (Math.abs(audio.currentTime - lastKept) > 10) { lastKept = audio.currentTime; remember(); } });
  audio.addEventListener('ended', () => { remember(); const it = playing && playing.item; if (it) { it.plays = (it.plays || 0) + 1; save(); } playNext(); });
  audio.addEventListener('error', () => { if (playing) toast(`Couldn't play "${playing.title}"`, 'err'); });
  function play(entry) {
    remember();
    playing = entry;
    audio.src = entry.url;
    audio.playbackRate = 1; d('.au-speed').textContent = '1×';
    if (entry.item && entry.item.pos) audio.addEventListener('loadedmetadata', () => { audio.currentTime = entry.item.pos; }, { once: true });
    audio.play().catch(() => {});
    d('.au-name').textContent = entry.title;
    d('.au-from').textContent = entry.from || '';
    d('.au-save').classList.toggle('hidden', !!entry.item);
    d('.au-save').disabled = false; d('.au-save').classList.remove('added', 'busy', 'done'); d('.au-save').textContent = '♥ Save';
    dock.classList.remove('hidden');
    document.documentElement.classList.add('au-open');
    if ('mediaSession' in navigator) {
      navigator.mediaSession.metadata = new MediaMetadata({ title: entry.title, artist: entry.from || 'Lewd Vault', album: 'Lewd Vault' });
      navigator.mediaSession.setActionHandler('play', () => audio.play());
      navigator.mediaSession.setActionHandler('pause', () => audio.pause());
      navigator.mediaSession.setActionHandler('seekbackward', () => d('.au-back').click());
      navigator.mediaSession.setActionHandler('seekforward', () => d('.au-fwd').click());
      navigator.mediaSession.setActionHandler('nexttrack', playNext);
    }
    drawList();
  }
  let queue = [];
  function playNext() {
    if (!playing || !queue.length) return;
    const k = queue.findIndex(e => e.url === playing.url);
    const n = queue[k + 1];
    if (n) play(n);
  }
  window.vaultAudio = { stop: () => audio.pause(), audio };

  // ---------- the sidebar tile and the page ----------
  const navBtn = make('button');
  navBtn.dataset.tab = 'audio';
  navBtn.append(make('span', 'nt-title', 'Audio'), make('span', 'nt-sub', 'JOI, hypno, ASMR'), make('span', 'nt-art'));
  (document.querySelector('nav [data-tab="videos"]') || document.querySelector('nav [data-tab="add"]')).after(navBtn);
  const tab = make('section', 'hidden');
  tab.id = 'tab-audio';
  tab.innerHTML = `
    <div class="vd-top">
      <div class="vd-modes"><button data-mode="yours" class="on">Yours</button><button data-mode="find">Find</button></div>
      <div class="au-cats"></div>
      <div class="au-tools">
        <button class="btn primary au-add" type="button">＋ Add audio files</button>
        <input type="file" class="au-file hidden" multiple accept="audio/*,.m4a,.mp3,.ogg,.opus,.flac,.wav">
      </div>
    </div>
    <div class="au-find hidden">
      <div class="au-findbar">
        <div class="au-subs"></div>
        <div class="searchbar au-paste"><span class="search-icon"></span><input type="text" spellcheck="false" placeholder="Search, or paste a soundgasm.net link"></div>
      </div>
      <div class="au-rail dc-rail"></div>
    </div>
    <div class="au-body"></div>`;
  document.querySelector('main').append(tab);
  const q = s => tab.querySelector(s);
  let mode = 'yours', cat = 'All';
  navBtn.onclick = async () => {
    document.querySelectorAll('nav button').forEach(x => { x.classList.toggle('on', x === navBtn); const t = $('tab-' + x.dataset.tab); if (t) t.classList.toggle('hidden', x !== navBtn); });
    stopGrid();
    try { localStorage.setItem(TAB_KEY, 'audio'); } catch (e) {}
    showTitle();
    await ready;
    drawAll();
  };
  tab.querySelectorAll('.vd-modes button').forEach(b => b.onclick = () => { mode = b.dataset.mode; drawAll(); });
  function drawAll() {
    tab.querySelectorAll('.vd-modes button').forEach(x => x.classList.toggle('on', x.dataset.mode === mode));
    q('.au-cats').classList.toggle('hidden', mode !== 'yours');
    q('.au-tools').classList.toggle('hidden', mode !== 'yours');
    q('.au-find').classList.toggle('hidden', mode !== 'find');
    if (mode === 'yours') { drawCats(); drawList(); } else drawFind();
  }

  // ---------- Yours ----------
  function drawCats() {
    const used = [...new Set([...CATS, ...list.items.map(i => i.category)])];
    q('.au-cats').replaceChildren(...['All', ...used].map(c => {
      const n = c === 'All' ? list.items.length : list.items.filter(i => i.category === c).length;
      const b = make('button', 'dc-cat' + (c === cat ? ' on' : ''), `${c} ${n ? '· ' + n : ''}`);
      b.onclick = () => { cat = c; drawCats(); drawList(); };
      return b;
    }));
  }
  function drawList() {
    if (mode !== 'yours') return;
    const box = q('.au-body');
    const items = list.items.filter(i => cat === 'All' || i.category === cat).sort((a, b) => (b.added || '').localeCompare(a.added || ''));
    if (!items.length) {
      box.replaceChildren(make('div', 'empty vd-empty', list.items.length ? 'Nothing in this category yet.' : 'No audio yet. Drop audio files here, add them with the button, or find some under Find.'));
      return;
    }
    queue = items.map(i => ({ item: i, title: i.title, from: i.category, url: vaultUrl(i.id) }));
    box.replaceChildren(...items.map((i, k) => {
      const r = make('div', 'au-row' + (playing && playing.item === i ? ' on' : ''));
      const playB = make('button', 'au-rowplay', playing && playing.item === i && !audio.paused ? '❚❚' : '▶');
      playB.onclick = () => { if (playing && playing.item === i) d('.au-play').click(); else play(queue[k]); };
      const name = make('div', 'au-rowname');
      name.append(make('b', '', i.title), make('span', '', [i.category, i.tags].filter(Boolean).join(' · ')));
      const len = make('span', 'au-rowlen', (i.pos ? fmt(i.pos) + ' / ' : '') + fmt(i.duration));
      const plays = make('span', 'au-rowlen', i.plays ? `${i.plays}× played` : '');
      const catSel = make('select', 'au-rowcat');
      [...new Set([...CATS, i.category])].forEach(c => catSel.add(new Option(c, c)));
      catSel.value = i.category;
      catSel.onchange = () => { i.category = catSel.value; save(); drawCats(); };
      const ren = make('button', 'au-b', 'Rename');
      ren.onclick = () => { const t = prompt('Name', i.title); if (t && t.trim()) { i.title = t.trim(); save(); drawList(); } };
      const del = make('button', 'au-b danger', '🗑'); del.title = 'Delete (to the Recycle Bin)';
      del.onclick = async () => {
        if (!confirm(`Delete "${i.title}"? It goes to the Recycle Bin.`)) return;
        try { await api('trashAudio', i.id); list.items = list.items.filter(x => x !== i); save(); drawCats(); drawList(); } catch (e) { toast(errMsg(e), 'err'); }
      };
      r.append(playB, name, len, plays, catSel, ren, del);
      return r;
    }));
  }
  const fileInput = q('.au-file');
  q('.au-add').onclick = () => fileInput.click();
  fileInput.onchange = () => { addFiles([...fileInput.files]); fileInput.value = ''; };
  // dropped on the Audio page: added here (not in the Library)
  tab.addEventListener('drop', e => {
    const files = [...e.dataTransfer.files].filter(f => f.type.startsWith('audio/') || AUDIO_EXT.test(f.name));
    if (!files.length) return toast('Only audio files can be added here', 'err');
    e.preventDefault();
    addFiles(files);
  });
  const durationOf = url => new Promise(ok => { const a = new Audio(); a.preload = 'metadata'; a.onloadedmetadata = () => ok(a.duration || 0); a.onerror = () => ok(0); a.src = url; });
  async function addFiles(files, extra = {}) {
    let n = 0;
    for (const f of files) {
      if (!(f.type.startsWith('audio/') || AUDIO_EXT.test(f.name))) { toast(`${f.name} isn't audio`, 'err'); continue; }
      try {
        const ext = (f.name.match(AUDIO_EXT) || ['.m4a'])[0].toLowerCase();
        const name = `${Date.now().toString(36)}${Math.random().toString(36).slice(2, 6)}${ext}`;
        const id = await driveUpload(f, name, 'app-audio');
        const url = URL.createObjectURL(f);
        const duration = await durationOf(url);
        URL.revokeObjectURL(url);
        list.items.push({ id, title: extra.title || f.name.replace(/\.[^.]+$/, '').replace(/[_-]+/g, ' ').trim(), category: extra.category || (cat !== 'All' ? cat : 'Other'),
          tags: extra.tags || '', source: extra.source || '', duration, added: new Date().toISOString(), plays: 0, pos: 0 });
        n++;
      } catch (e) { toast(`${f.name}: ${errMsg(e)}`, 'err'); }
    }
    if (n) { save(); toast(n === 1 ? 'Audio added' : `${n} audio files added`, 'ok'); if (mode === 'yours') { drawCats(); drawList(); } }
    return n;
  }

  // ---------- Find: audio from many places ----------
  //   Reddit: a few audio communities, in one tab (their posts' soundgasm links)
  //   soundgasm: performers' pages, the ones you pin and the ones Reddit brings up
  //   Audio Desires: voice actors' erotic stories (free previews; the whole story on their site)
  //   Classic erotica: public-domain books read aloud (LibriVox, through the Internet Archive)
  //   Freesound: sounds (moans, breathing, whispers...), with your free key
  //   or any soundgasm / audio link pasted in the box
  const SUBS = [['gonewildaudio', 'All kinds'], ['GWASapphic', 'Sapphic'], ['gonewildaudible', 'Audible'], ['GoneWildAudioGay', 'Gay'], ['eroticaudio', 'Erotic audio'], ['JOIAudio', 'JOI'], ['EroticHypnosis', 'Hypnosis']];
  const badge = (text, color) => 'data:image/svg+xml,' + enc(`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"><rect width="64" height="64" rx="16" fill="${color}"/><text x="32" y="42" font-family="Segoe UI, Arial, sans-serif" font-weight="800" font-size="26" fill="#fff" text-anchor="middle">${text}</text></svg>`);
  const FIND = {
    reddit: { name: 'Reddit', icon: proxy('https://www.reddit.com/apple-touch-icon-precomposed.png'), line: 'Audio posted to Reddit\'s audio communities' },
    soundgasm: { name: 'soundgasm', icon: badge('sg', '#1aa3ff'), line: 'Performers\' pages: pin your favourites' },
    audiodesires: { name: 'Audio Desires', icon: proxy('https://www.audiodesires.com/icons/icon-512x512.png'), line: 'Stories by voice actors: free previews here, the whole story on their site' },
    classics: { name: 'Classic erotica', icon: badge('LV', '#8a5a2b'), line: 'Public-domain books read aloud by LibriVox volunteers' },
    freesound: { name: 'Freesound', icon: badge('fs', '#e8743b'), line: 'Sounds: moans, breathing, whispers, rain (with your free key)' }
  };
  // ---------- your own audio sites (sites.js), and which sources show, in what order (cfg.audio) ----------
  const BUILT_IN = Object.keys(FIND);
  let acfg = null;
  const hostOf = url => String(url || '').replace(/^https?:\/\/(www\.)?/, '').split('/')[0];
  async function loadOwn() {
    acfg = await window.vaultDiscover.config();
    acfg.audio = acfg.audio || { order: [], hidden: [] };
    for (const id of Object.keys(FIND)) if (FIND[id].custom) delete FIND[id];
    for (const r of (acfg.sites || []).filter(x => x.section === 'audio')) FIND[r.id] = { name: r.name, icon: proxy(`${r.base}/favicon.ico`), line: `Your own site · ${hostOf(r.base)}`, custom: { ...r, section: 'audio' } };
  }
  const shownFind = () => {
    const order = acfg ? acfg.audio.order : [];
    return Object.keys(FIND).sort((a, b) => (order.indexOf(a) + 1 || 999) - (order.indexOf(b) + 1 || 999)).filter(id => !(acfg && acfg.audio.hidden.includes(id)));
  };
  async function editOwnSite(r) {
    const made = await window.vaultSites.build('audio', r);
    if (!made) return;
    const k = acfg.sites.findIndex(x => x.id === made.id);
    if (k >= 0) acfg.sites[k] = { ...made, section: 'audio' }; else acfg.sites.push({ ...made, section: 'audio' });
    await window.vaultDiscover.saveConfig();
    await loadOwn();
    const P = findPrefs(); P.source = made.id; save();
    found = []; findPage = 1; findDone = false;
    toast(k >= 0 ? `${made.name} changed` : `${made.name} added to Audio`, 'ok');
    drawFind();
  }
  function customiseFind() {
    const S = window.vaultSites, A = acfg.audio;
    S.sheet('Customise Find', (body, close) => {
      body.append(make('p', 'dc-sheet-lead', 'Which places to look for audio, in what order. Your own sites can be changed or removed here.'));
      const list = make('div', 'st-sites');
      const order = () => Object.keys(FIND).sort((a, b) => (A.order.indexOf(a) + 1 || 999) - (A.order.indexOf(b) + 1 || 999));
      const drawList = () => {
        const ids = order();
        list.replaceChildren(...ids.map((id, k) => {
          const f = FIND[id], row = make('div', 'st-site');
          const tick = make('input'); tick.type = 'checkbox'; tick.checked = !A.hidden.includes(id);
          tick.onchange = () => { A.hidden = tick.checked ? A.hidden.filter(x => x !== id) : [...A.hidden, id]; };
          const up = make('button', 'btn sm', '↑'), down = make('button', 'btn sm', '↓');
          const move = d => { const all = order(); const j = all.indexOf(id); if (all[j + d] === undefined) return; [all[j], all[j + d]] = [all[j + d], all[j]]; A.order = all; drawList(); };
          up.onclick = () => move(-1); down.onclick = () => move(1); up.disabled = !k; down.disabled = k === ids.length - 1;
          row.append(tick, make('b', '', f.name), make('span', 'dc-srckind', f.custom ? 'Your own · ' + hostOf(f.custom.base) : 'Built in'), up, down);
          if (f.custom) {
            const ed = make('button', 'btn sm', 'Edit'); ed.onclick = () => { close(); editOwnSite(f.custom); };
            const rm = make('button', 'btn sm', 'Remove'); rm.onclick = () => { if (!confirm(`Remove ${f.name}?`)) return; acfg.sites = acfg.sites.filter(x => x.id !== id); delete FIND[id]; drawList(); };
            row.append(ed, rm);
          }
          return row;
        }));
      };
      drawList();
      const addOwn = make('button', 'btn', '✨ Build your own site'); addOwn.onclick = () => { close(); editOwnSite(null); };
      const ok = make('button', 'btn primary', 'Done');
      ok.onclick = async () => {
        A.order = order();
        await window.vaultDiscover.saveConfig(); await loadOwn();
        const P = findPrefs();
        if (!shownFind().includes(P.source)) { P.source = shownFind()[0] || 'reddit'; save(); }
        close(); found = []; findPage = 1; findDone = false; drawFind();
      };
      const btns = make('div', 'dc-actions st-actions'); btns.append(addOwn, ok);
      body.append(list, btns);
    });
  }
  // Configure sources: pick where to look, add your own site, or change which ones are listed
  function pickSource() {
    const P = findPrefs();
    window.vaultSites.sheet('Configure sources', (body, close) => {
      body.append(make('p', 'dc-sheet-lead', 'Choose where to look for audio.'));
      const grid = make('div', 'au-subs');
      grid.append(...shownFind().map(id => {
        const s = FIND[id], b = make('button', 'au-src' + (id === P.source ? ' on' : ''));
        const im = make('img'); im.alt = ''; im.src = s.icon; im.onerror = () => im.remove();
        b.append(im, make('span', '', s.name));
        if (id === 'reddit') b.append(make('small', 'au-src-sub', 'r/' + P.sub));
        b.title = s.line;
        b.onclick = () => { close(); if (P.source === id) return; P.source = id; save(); found = []; findPage = 1; findDone = false; drawFind(); };
        return b;
      }));
      const own = make('button', 'au-src au-src-add', '＋ Your own site'); own.title = 'Any other site with audio: give its addresses and the app reads it'; own.onclick = () => { close(); editOwnSite(null); };
      const tune = make('button', 'au-src au-src-add', '⚙ Customise list'); tune.title = 'Which places are listed, in what order'; tune.onclick = () => { close(); customiseFind(); };
      grid.append(own, tune);
      body.append(grid);
    });
  }
  const findPrefs = () => (list.find = list.find || { source: 'reddit', sub: 'gonewildaudio', performers: [], seen: [], fsKey: '', fantasy: 'joi' });
  let found = [], findTicket = 0, findPage = 1, findDone = false;
  const pasteInput = q('.au-paste input');
  function drawFind() {
    if (!acfg) return loadOwn().then(drawFind, () => { acfg = { sites: [], audio: { order: [], hidden: [] } }; drawFind(); });
    const P = findPrefs();
    if (!FIND[P.source]) P.source = shownFind()[0] || 'reddit';
    // the sources stay out of the way: one button shows the current one and opens the picker
    const cur = FIND[P.source], pickBtn = make('button', 'au-src on');
    const im = make('img'); im.alt = ''; im.src = cur.icon; im.onerror = () => im.remove();
    pickBtn.append(im, make('span', '', cur.name));
    if (P.source === 'reddit') pickBtn.append(make('small', 'au-src-sub', 'r/' + P.sub));
    pickBtn.title = cur.line;
    const conf = make('button', 'au-src au-src-add', '⚙ Configure sources'); conf.title = 'Choose where to look for audio';
    pickBtn.onclick = conf.onclick = () => pickSource();
    q('.au-subs').replaceChildren(pickBtn, conf);
    pasteInput.placeholder = { reddit: `Search r/${P.sub}, or paste a soundgasm link`, soundgasm: 'A performer\'s soundgasm name, or paste a link', audiodesires: 'Search these stories, or paste a link',
      classics: 'Search the classics (Fanny Hill, Venus in Furs…)', freesound: 'Search sounds: moan, breathing, whisper…' }[P.source] || `Search ${FIND[P.source].name}…`;
    drawRail();
    if (!found.length) loadFind();
    else drawFound();
  }
  // under the sources: Reddit's communities, the performers, Audio Desires' fantasies, sound ideas
  let fantasies = null;
  function drawRail() {
    const P = findPrefs(), rail = q('.au-rail');
    const chip = (label, on, run, title) => { const b = make('button', 'dc-chip' + (on ? ' on nav' : ''), label); b.type = 'button'; if (title) b.title = title; b.onclick = run; return b; };
    const go = fn => () => { fn(); save(); found = []; findPage = 1; findDone = false; pasteInput.value = ''; drawFind(); };
    rail.replaceChildren();
    if (P.source === 'reddit') rail.append(...SUBS.map(([s, label]) => chip(label, s === P.sub, go(() => { P.sub = s; }), 'r/' + s)));
    else if (P.source === 'soundgasm') {
      const names = [...new Set([...P.performers, ...P.seen])];
      if (!names.length) rail.append(make('span', 'au-rail-note', 'Performers appear here as you browse Reddit, or type a soundgasm name above.'));
      for (const n of names) {
        const pinned = P.performers.includes(n);
        const c = chip((pinned ? '★ ' : '') + n, P.performer === n, go(() => { P.performer = n; }), pinned ? 'Pinned' : 'Seen on Reddit');
        rail.append(c);
      }
      if (P.performer) {
        const pinned = P.performers.includes(P.performer);
        const pin = make('button', 'btn au-pin', pinned ? '★ Pinned' : '☆ Pin ' + P.performer); pin.type = 'button';
        pin.onclick = () => { P.performers = pinned ? P.performers.filter(x => x !== P.performer) : [...P.performers, P.performer]; save(); drawRail(); };
        rail.append(pin);
      }
    } else if (P.source === 'audiodesires') {
      const list2 = fantasies || [];
      rail.append(...list2.map(([slug, name]) => chip(name, slug === P.fantasy, go(() => { P.fantasy = slug; }))));
      if (!fantasies) loadFantasies().then(() => { if (findPrefs().source === 'audiodesires') drawRail(); });
    } else if (FIND[P.source] && FIND[P.source].custom) {
      const r = FIND[P.source].custom, my = (P.own = P.own || {})[r.id] || (P.own[r.id] = { sort: '', cat: '', random: false });
      const pick = fn => go(() => { fn(); });
      for (const [label, v] of r.sorts || []) rail.append(chip(label, !my.random && my.sort === v, pick(() => { my.sort = v; my.random = false; })));
      rail.append(chip('🎲 Random', my.random, pick(() => { my.random = !my.random; })));
      for (const [label, v] of r.cats || []) rail.append(chip(label, my.cat === v, pick(() => { my.cat = my.cat === v ? '' : v; my.random = false; })));
    } else if (P.source === 'freesound') {
      rail.append(...['moan', 'breathing', 'whisper', 'kiss', 'heartbeat', 'asmr', 'rain'].map(w => chip(w, pasteInput.value.trim() === w, () => { pasteInput.value = w; found = []; findPage = 1; findDone = false; loadFind(); drawRail(); })));
    }
    rail.classList.toggle('hidden', !rail.children.length);
  }
  async function loadFantasies() {
    try {
      const d = JSON.parse(await api('webJson', 'https://www.audiodesires.com/page-data/index/page-data.json'));
      fantasies = (d.result.pageContext.data.fantasies || []).map(f => f.en || {}).filter(f => f.slug && f.fantasy && clean(f.fantasy)).map(f => [f.slug, f.fantasy]);
    } catch (e) { fantasies = []; }
  }
  pasteInput.addEventListener('keydown', async e => {
    if (e.key !== 'Enter') return;
    const v = pasteInput.value.trim(), P = findPrefs();
    if (/^https?:\/\/(www\.)?soundgasm\.net\/u\/[^/]+\/./i.test(v)) {
      try { const f = await resolveGasm({ title: '', link: v, from: 'soundgasm' }); play({ title: f.title, from: 'soundgasm', url: f.url, link: v, found: f }); } catch (err) { toast(errMsg(err), 'err'); }
      return;
    }
    if (/^https?:\/\/\S+\.(mp3|m4a|ogg|opus|wav|flac)(\?\S*)?$/i.test(v)) { const f = { title: decodeURIComponent(v.split('/').pop().split('?')[0]), link: v, url: v, from: v.split('/')[2] }; play({ title: f.title, from: f.from, url: v, link: v, found: f }); return; }
    if (P.source === 'soundgasm' && /^[\w.-]{2,40}$/.test(v)) { P.performer = v; save(); pasteInput.value = ''; }
    found = []; findPage = 1; findDone = false;
    drawRail(); loadFind();
  });
  // one page from the chosen source
  async function loadFind() {
    const P = findPrefs(), box = q('.au-body'), my = ++findTicket, words = pasteInput.value.trim();
    if (findDone) return;
    if (!FIND[P.source]) return;
    if (findPage === 1) box.replaceChildren(make('div', 'vd-loading', `Looking on ${P.source === 'reddit' ? 'r/' + P.sub : FIND[P.source].name}…`));
    const keep = items => { if (my !== findTicket) return false; const seen = new Set(found.map(f => f.link)); found.push(...items.filter(f => f.link && !seen.has(f.link) && clean(f.title + ' ' + (f.tags || []).join(' ')))); return true; };
    try {
      if (P.source === 'reddit') {
        // Reddit's public feed of the community (no app id needed): each post's text has its soundgasm link
        const posts = await window.vaultDiscover.redditFeed(P.sub, words, {
          wanted: () => my === findTicket,
          onWait: s => { if (my === findTicket) box.replaceChildren(make('div', 'vd-loading', `Without an app id of your own, Reddit allows one look a minute: r/${P.sub} in ${s} s…`)); }
        });
        const items = [];
        for (const p of posts) {
          const html = new DOMParser().parseFromString(p.html, 'text/html');
          const text = p.title + '\n' + html.body.textContent;
          const link = ([...html.querySelectorAll('a')].map(a => a.href).find(h => /soundgasm\.net\/u\//i.test(h)) || (text.match(/https?:\/\/(?:www\.)?soundgasm\.net\/u\/[^\s)\]]+/i) || [])[0] || '');
          if (!link) continue;
          const performer = (link.match(/soundgasm\.net\/u\/([^/]+)/i) || [])[1];
          if (performer && !P.seen.includes(performer)) P.seen = [performer, ...P.seen].slice(0, 30);
          items.push({ title: p.title, link, from: 'u/' + p.author + ' · r/' + p.sub, page: p.page, kind: 'gasm', tags: (p.title.match(/\[[^\]]+\]/g) || []).map(t => t.slice(1, -1)) });
        }
        if (!keep(items)) return;
        save();
        findDone = true; // (a feed gives one page)
      } else if (P.source === 'soundgasm') {
        if (!P.performer) { box.replaceChildren(make('div', 'empty vd-empty', 'Pick a performer above, or type a soundgasm name and press Enter.')); return; }
        const html = await api('webJson', `https://soundgasm.net/u/${enc(P.performer)}`, { Accept: 'text/html' });
        const doc = new DOMParser().parseFromString(html, 'text/html');
        let items = [...doc.querySelectorAll('.sound-details')].map(el => {
          const a = el.querySelector('a'), plays = ((el.querySelector('.playCount') || {}).textContent || '').replace(/\D+/g, '');
          return { title: a ? a.textContent.trim() : '', link: a ? a.getAttribute('href') : '', from: P.performer + (plays ? ` · ${fmtN(+plays)} plays` : ''), plays: +plays || 0, kind: 'gasm', page: a ? a.getAttribute('href') : '' };
        });
        if (words && words !== P.performer) items = items.filter(f => f.title.toLowerCase().includes(words.toLowerCase()));
        if (!keep(items)) return;
        findDone = true;
      } else if (P.source === 'audiodesires') {
        const path = `fantasy/${P.fantasy}/${findPage > 1 ? findPage + '/' : ''}`;
        const d = JSON.parse(await api('webJson', `https://www.audiodesires.com/page-data/${path}page-data.json`));
        const c = d.result.pageContext, stories = (c.data && c.data.stories) || [];
        let items = stories.map(s => ({ title: s.title, link: `https://www.audiodesires.com/story/${s.slug}/`, page: `https://www.audiodesires.com/story/${s.slug}/`, art: s.imageUrl, slug: s.slug, kind: 'ad', preview: true,
          from: ['Audio Desires', ...(s.voices || []).map(v => v.name)].join(' · '), tags: (s.tags || []).map(t => t.fantasy) }));
        if (words) items = items.filter(f => (f.title + ' ' + f.tags.join(' ')).toLowerCase().includes(words.toLowerCase()));
        if (!keep(items)) return;
        findDone = !c.numPages || findPage >= c.numPages; findPage++;
      } else if (P.source === 'classics') {
        const base = 'collection:librivoxaudio AND (subject:erotica OR subject:Erotica OR subject:erotic OR title:"Fanny Hill" OR title:"Venus in Furs" OR title:"Kama Sutra" OR title:"Perfumed Garden" OR title:"Lady Chatterley")';
        const qy = words ? `${base} AND (title:(${words}) OR creator:(${words}))` : base;
        const d = JSON.parse(await api('webJson', `https://archive.org/advancedsearch.php?q=${enc(qy)}&fl[]=identifier&fl[]=title&fl[]=creator&fl[]=downloads&sort[]=downloads+desc&rows=40&output=json`));
        const items = (d.response.docs || []).map(b => ({ title: b.title, link: `https://archive.org/details/${b.identifier}`, page: `https://librivox.org/search?q=${enc(b.title)}`, id: b.identifier,
          art: `https://archive.org/services/img/${b.identifier}`, from: `${[].concat(b.creator || [])[0] || 'LibriVox'} · read by LibriVox volunteers`, kind: 'book' }));
        if (!keep(items)) return;
        findDone = true;
      } else if (FIND[P.source] && FIND[P.source].custom) {
        const r = FIND[P.source].custom, my = (P.own || {})[r.id] || {};
        const got = my.random ? await window.vaultSites.random(r, { cat: my.cat }) : await window.vaultSites.list(r, { q: words, sort: my.sort || '', cat: my.cat || '', page: findPage });
        const items = got.map(x => ({ title: x.title, link: x.url, page: x.url, art: x.thumb, from: hostOf(r.base), duration: x.duration, kind: 'recipe', recipe: r }));
        if (!keep(items)) return;
        findDone = !items.length || my.random; findPage++;
      } else if (P.source === 'freesound') {
        if (!P.fsKey) {
          const card = make('div', 'au-keycard');
          card.append(make('b', '', 'Freesound needs your free key'), make('p', '', 'Make a free account on freesound.org, then get a key under "API credentials" (any name, no special access). Paste it here; it stays on this PC in your vault\'s settings.'));
          const row = make('div', 'au-keyrow');
          const inp = make('input'); Object.assign(inp, { type: 'password', placeholder: 'Your Freesound API key' });
          const ok = make('button', 'btn primary', 'Use it'); ok.onclick = () => { if (!inp.value.trim()) return; P.fsKey = inp.value.trim(); save(); found = []; findDone = false; loadFind(); };
          const get = make('button', 'btn', 'Get a key ↗'); get.onclick = () => api('openLink', 'https://freesound.org/apiv2/apply/').catch(() => {});
          row.append(inp, ok, get); card.append(row);
          box.replaceChildren(card);
          return;
        }
        const d = JSON.parse(await api('webJson', `https://freesound.org/apiv2/search/text/?query=${enc(words || 'moan')}&fields=id,name,username,duration,previews,tags,license,url&page_size=40&page=${findPage}&token=${enc(P.fsKey)}`));
        const items = (d.results || []).map(s => ({ title: s.name, link: s.url, page: s.url, url: s.previews && (s.previews['preview-hq-mp3'] || s.previews['preview-lq-mp3']), duration: s.duration,
          from: `${s.username} · ${String(s.license || '').split('/').slice(-3, -2)[0] || 'Freesound'}`, tags: (s.tags || []).slice(0, 8), kind: 'fs' }));
        if (!keep(items)) return;
        findDone = !d.next; findPage++;
      }
      drawFound();
    } catch (e) {
      if (my !== findTicket) return;
      if (P.source === 'freesound' && /^401/.test(errMsg(e))) { P.fsKey = ''; save(); toast('Freesound refused that key', 'err'); return loadFind(); }
      box.replaceChildren(make('div', 'empty vd-empty', errMsg(e)));
    }
  }
  async function resolveGasm(f) {
    if (f.url) return f;
    const html = await api('webJson', f.link.replace(/^http:/, 'https:'), { Accept: 'text/html' });
    const m = html.match(/m4a:\s*"([^"]+)"/) || html.match(/(https:\/\/media\.soundgasm\.net\/sounds\/[^"']+\.m4a)/);
    if (!m) throw new Error('That soundgasm page has no audio anymore');
    f.url = m[1];
    if (!f.title) f.title = ((html.match(/<div class="jp-title"[^>]*>([^<]+)</) || [])[1] || 'soundgasm audio').trim();
    return f;
  }
  // what to play: soundgasm's file, Audio Desires' preview, a book's chapters, Freesound's preview
  async function resolveFound(f) {
    if (f.kind === 'gasm') return resolveGasm(f);
    if (f.url) return f;
    if (f.kind === 'recipe') {
      const d = await window.vaultSites.details(f.recipe, f.link);
      f.url = d.file; f.tags = d.tags; if (!f.title) f.title = d.title;
      return f;
    }
    if (f.kind === 'ad') {
      const text = await api('webJson', `https://www.audiodesires.com/page-data/story/${f.slug}/page-data.json`);
      const m = text.match(/https:\/\/firebasestorage\.googleapis\.com\/[^"]+?audio\.mp3\?[^"]+/);
      if (!m) throw new Error('This story has no free preview');
      f.url = m[0].replace(/\\u0026/g, '&');
      return f;
    }
    if (f.kind === 'book') {
      const d = JSON.parse(await api('webJson', `https://archive.org/metadata/${f.id}`));
      const files = (d.files || []).filter(x => /_64kb\.mp3$/.test(x.name)).sort((a, b) => a.name.localeCompare(b.name));
      if (!files.length) throw new Error('This book has no recordings');
      f.chapters = files.map((x, k) => ({ title: `${f.title} · ${x.title || 'part ' + (k + 1)}`, url: `https://archive.org/download/${f.id}/${encodeURIComponent(x.name)}`, link: `${f.link}#${k + 1}`, from: f.from, kind: 'chapter', page: f.page, art: f.art }));
      f.url = f.chapters[0].url;
      return f;
    }
    return f;
  }
  function drawFound() {
    const box = q('.au-body'), P = findPrefs();
    if (!found.length) return box.replaceChildren(make('div', 'empty vd-empty', { reddit: 'Nothing with a soundgasm link here. Try another community or other words.', soundgasm: 'Nothing on this performer\'s page.' }[P.source] || 'Nothing found. Try other words.'));
    queue = [];
    const rowFor = (f, sub) => {
      const r = make('div', 'au-row' + (sub ? ' au-chapter' : '') + (playing && playing.link === f.link ? ' on' : ''));
      const playB = make('button', 'au-rowplay', '▶');
      playB.onclick = async () => {
        playB.textContent = '…';
        try {
          await resolveFound(f);
          if (f.chapters) queue = f.chapters.map(c => ({ title: c.title, from: c.from, url: c.url, link: c.link, found: c }));
          play({ title: f.chapters ? f.chapters[0].title : f.title, from: f.from, url: f.url, link: f.chapters ? f.chapters[0].link : f.link, found: f.chapters ? f.chapters[0] : f });
          if (f.chapters && !r.nextElementSibling?.classList.contains('au-chapters')) toggleChapters();
        } catch (e) { toast(errMsg(e), 'err'); }
        playB.textContent = '▶';
      };
      if (f.art) { const im = make('img', 'au-art-sm'); im.alt = ''; im.loading = 'lazy'; im.src = proxy(f.art); im.onerror = () => im.remove(); r.append(playB, im); }
      else r.append(playB);
      const name = make('div', 'au-rowname');
      name.append(make('b', '', f.title), make('span', '', [f.from, f.preview ? 'free preview' : '', f.duration ? fmt(f.duration) : ''].filter(Boolean).join(' · ')));
      r.append(name);
      const toggleChapters = async () => {
        const next = r.nextElementSibling;
        if (next && next.classList.contains('au-chapters')) return next.remove();
        try { await resolveFound(f); } catch (e) { return toast(errMsg(e), 'err'); }
        const box2 = make('div', 'au-chapters');
        box2.append(...f.chapters.map(c => rowFor(c, true)));
        r.after(box2);
      };
      if (f.kind === 'book') { const ch = make('button', 'au-b', 'Chapters ▾'); ch.onclick = toggleChapters; r.append(ch); }
      if (f.kind !== 'book') { const saveB = make('button', 'btn', '♥ Save'); saveB.onclick = () => saveFound({ found: f, title: f.title }, saveB); r.append(saveB); }
      const open = make('button', 'au-b', '↗'); open.title = f.preview ? 'The whole story, on Audio Desires' : 'Open where it came from'; open.onclick = () => api('openLink', f.page || f.link).catch(() => {});
      r.append(open);
      return r;
    };
    box.replaceChildren(...found.map(f => rowFor(f)));
    if (!findDone) {
      const more = make('button', 'btn au-more', 'More'); more.onclick = () => { more.remove(); loadFind(); };
      box.append(more);
    }
  }
  async function saveFound(entry, btn) {
    const f = entry.found;
    if (!f) return;
    // it asks for the category (and suggests one from the title), then saves into Yours
    const guess = /joi|jerk off instruction/i.test(f.title) ? 'JOI' : /hypno|trance/i.test(f.title) ? 'Hypno' : /asmr|whisper/i.test(f.title) ? 'ASMR' : /moan|orgasm/i.test(f.title) && f.kind === 'fs' ? 'Moans' : 'Erotic stories';
    const tags = window.vaultUI.cleanTags((f.tags || (f.title.match(/\[[^\]]+\]/g) || []).map(t => t.slice(1, -1))).map(t => String(t).trim().toLowerCase().replace(/\s+/g, '_')).filter(Boolean).slice(0, 12));
    const answer = await window.vaultDiscover.ask({ title: f.title, thumb: f.art ? proxy(f.art) : '', source: f.page || f.link, tags, audio: [guess, ...CATS.filter(c => c !== guess)] });
    if (!answer) return;
    btn.disabled = true; btn.classList.add('added', 'busy'); btn.innerHTML = window.vaultDiscover.spinTick() + '<span>Added</span>';
    try {
      await resolveFound(f);
      const blob = await window.vaultDiscover.downloadBlob(f.url, p => { if (p.total) btn.title = Math.floor(p.done / p.total * 100) + '%'; });
      const ext = (f.url.split('?')[0].match(AUDIO_EXT) || ['.m4a'])[0].toLowerCase();
      const file = new File([blob], 'audio' + ext, { type: blob.type && blob.type !== 'application/octet-stream' ? blob.type : ext === '.mp3' ? 'audio/mpeg' : 'audio/mp4' });
      if (await addFiles([file], { title: f.title.replace(/\[[^\]]+\]/g, '').replace(/\s+/g, ' ').trim() || f.title, category: answer.category, tags: answer.tags.join(' '), source: f.page || f.link })) {
        btn.classList.replace('busy', 'done');
        window.vaultUI.burst(btn, 12);
        if (!btn.isConnected) window.vaultDiscover.imported({ thumb: f.art ? proxy(f.art) : '', text: 'Imported!', sub: `Into ${answer.category} · in Audio` });
      } else { btn.disabled = false; btn.classList.remove('added', 'busy'); btn.textContent = '♥ Save'; }
    } catch (e) { btn.disabled = false; btn.classList.remove('added', 'busy'); btn.textContent = '♥ Save'; toast(errMsg(e), 'err'); }
  }
  ready.then(() => { navBtn.querySelector('.nt-sub').textContent = list.items.length ? `${fmtN(list.items.length)} in yours` : 'JOI, hypno, ASMR'; });
})();
