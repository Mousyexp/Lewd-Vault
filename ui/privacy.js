// Lewd Vault desktop: privacy (Phase 8 of PLAN.md).
//   - Discreet names (Settings → Storage; asked on the welcome screen, on by default for a new vault):
//     every file in one neutral folder, Media, named by its number only (src-tauri/src/discreet.rs)
//   - the emergency key (Ctrl+Shift+Space by default, works even when another app is in front):
//     everything pauses and the app turns into a screensaver or a cat video (or a video you pick),
//     full screen; press it again to come back (optionally behind a plain cover you click away).
//     Or, if chosen, the window vanishes, from the taskbar too (the key or starting the app again
//     brings it back). Settings → Privacy.
//   - Export to Excel (Settings → Storage): the library as a styled workbook (src-tauri/src/excel.rs)

(() => {
  'use strict';
  if (!window.vaultUI || !inTauri) return;
  const { make } = window.vaultUI;
  const DEFAULT_KEY = 'Ctrl+Shift+Space';

  // ---------- the settings this adds (kept in settings.json, read by the app at start) ----------
  SETTINGS.push(
    { section: 'Privacy', key: 'panicOn', label: 'Emergency key', type: 'toggle', def: true,
      help: 'One key combination, pressed anywhere (even in another app), pauses everything playing and turns Lewd Vault into something harmless at once. Press it again to come back.' },
    { section: 'Privacy', key: 'panicMode', label: 'The emergency key shows', type: 'select', def: 'screensaver',
      options: [['screensaver', 'A screensaver (a clock on soft colours)'], ['video', 'A cat video (or a video you pick)'], ['hide', 'Nothing: the app disappears from the screen and taskbar']] },
    { section: 'Privacy', key: 'panicCover', label: 'Come back behind a cover', type: 'toggle', def: false,
      help: 'When the emergency key brings the app back, it shows a plain cover first; click it (or press any key) to see your vault again.' });
  const savedLocal = (() => { try { return JSON.parse(localStorage.getItem('imageVault.settings')) || {}; } catch (e) { return {}; } })();
  settings.panicOn = savedLocal.panicOn !== false;
  settings.panicCover = savedLocal.panicCover === true;
  settings.panicKey = typeof savedLocal.panicKey === 'string' ? savedLocal.panicKey : DEFAULT_KEY;
  settings.coverVideo = typeof savedLocal.coverVideo === 'string' ? savedLocal.coverVideo : '';
  setupReady.then(s => {
    const v = (s && s.settings) || {};
    if (typeof v.panicKey === 'string') settings.panicKey = v.panicKey; // (not in SETTINGS: kept by hand)
    if (typeof v.coverVideo === 'string') settings.coverVideo = v.coverVideo; // "" (the cat), a file id, or "yt:<video id>"
  });
  const setMode = () => tauriInvoke('set_panic_mode', { mode: S('panicMode') }).catch(() => {});
  const setKey = async key => {
    try { await tauriInvoke('set_panic_key', { key }); return true; }
    catch (e) { toast(String(e), 'err'); return false; }
  };
  // the toggle turns the key on and off right away
  const saveSettingsPrivacy = saveSettingsSoon;
  let lastOn = null, lastMode = null;
  saveSettingsSoon = function () {
    if (lastOn !== null && lastOn !== S('panicOn')) setKey(S('panicOn') ? settings.panicKey : '');
    lastOn = S('panicOn');
    if (lastMode !== null && lastMode !== S('panicMode')) { setMode(); setTimeout(drawVideoRow); }
    lastMode = S('panicMode');
    return saveSettingsPrivacy.apply(this, arguments);
  };
  setupReady.then(() => { lastOn = S('panicOn'); lastMode = S('panicMode'); setMode(); });

  // a key combination, typed: "Ctrl+Shift+Space"
  function captureKey(button, done) {
    button.textContent = 'Press the keys…';
    button.classList.add('capturing');
    const onKey = e => {
      e.preventDefault(); e.stopPropagation();
      if (['Control', 'Shift', 'Alt', 'Meta'].includes(e.key)) return;
      if (e.key === 'Escape') return finish(null);
      const mods = [e.ctrlKey && 'Ctrl', e.altKey && 'Alt', e.shiftKey && 'Shift', e.metaKey && 'Super'].filter(Boolean);
      const name = e.code.startsWith('Key') ? e.code.slice(3) : e.code.startsWith('Digit') ? e.code.slice(5) : e.code === 'Space' ? 'Space' : e.key.length === 1 ? e.key.toUpperCase() : e.code;
      if (!mods.length && !/^F\d+$/.test(name)) { toast('Use Ctrl, Alt or Shift with it (or an F key), so it can\'t be pressed by accident', 'err'); return; }
      finish([...mods, name].join('+'));
    };
    const finish = key => { removeEventListener('keydown', onKey, true); button.classList.remove('capturing'); done(key); };
    addEventListener('keydown', onKey, true);
  }
  function keyRow(where) {
    const row = make('div', 'pv-keyrow');
    const show = make('kbd', 'pv-key', settings.panicKey || DEFAULT_KEY);
    const change = make('button', 'btn', 'Change…');
    change.type = 'button';
    change.onclick = () => captureKey(change, async key => {
      change.textContent = 'Change…';
      if (!key) return;
      if (await setKey(key)) { settings.panicKey = key; show.textContent = key; saveSettingsSoon(); toast(`Emergency key: ${key}`, 'ok'); }
      else setKey(settings.panicKey);
    });
    row.append(make('span', 'pv-keylabel', where === 'welcome' ? 'Emergency key' : 'The key'), show, change);
    return row;
  }

  // ---------- the emergency itself ----------
  // main.rs panic_toggle sends { on, mode }: "screensaver" / "video" (the page covers itself and the
  // window goes full screen), "hide" (the window disappears), and { on: false } to come back.
  let cover = null, screen = null;
  if (window.__TAURI__ && window.__TAURI__.event) {
    window.__TAURI__.event.listen('panic', e => {
      const p = typeof e.payload === 'object' && e.payload ? e.payload : { on: !!e.payload, mode: 'hide' };
      if (p.on) {
        // everything that plays, stops (nothing starts again by itself)
        document.querySelectorAll('video, audio').forEach(m => { try { m.pause(); } catch (x) {} });
        if (window.vaultAudio) window.vaultAudio.stop();
        if (typeof showOpen !== 'undefined' && showOpen && !showPaused) setPaused(true);
        if (window.vaultCoach) window.vaultCoach.pause();
        if (p.mode === 'hide') { if (S('panicCover')) showCover(); }
        else showScreen(p.mode);
      } else if (screen) {
        hideScreen();
        if (S('panicCover')) showCover();
      }
    });
  }
  function showCover() {
    if (cover) return;
    cover = make('div', 'pv-cover');
    cover.innerHTML = '<div class="pv-cover-box"><b>Notes</b><span>Click to continue</span></div>';
    const go = () => { cover.remove(); cover = null; removeEventListener('keydown', go, true); };
    cover.onclick = go;
    addEventListener('keydown', go, true);
    document.body.append(cover);
  }

  // The screensaver: soft colours drifting behind a big clock, like a lock screen. The video: a cat
  // (Keyboard Cat, on YouTube) or the video picked in Settings, looping, over the screensaver (which
  // shows if the video can't load, e.g. offline). Clicks and keys do nothing: only the key brings
  // the vault back, so nobody else can click their way in.
  const CAT = 'J---aiyznGQ';
  const swallow = e => { if (screen) { e.preventDefault(); e.stopPropagation(); } };
  function showScreen(mode) {
    hideScreen();
    screen = make('div', 'pv-screen');
    screen.innerHTML = '<div class="pv-aurora"><i></i><i></i><i></i></div><div class="pv-clock"><b></b><span></span></div>';
    const tick = () => {
      if (!screen) return;
      const now = new Date();
      screen.querySelector('.pv-clock b').textContent = now.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
      screen.querySelector('.pv-clock span').textContent = now.toLocaleDateString([], { weekday: 'long', month: 'long', day: 'numeric' });
    };
    tick();
    screen.tick = setInterval(tick, 1000);
    if (mode === 'video') screen.append(coverVideo());
    document.body.append(screen);
    for (const ev of ['keydown', 'keyup', 'wheel', 'contextmenu']) addEventListener(ev, swallow, true);
  }
  function coverVideo() {
    const v = settings.coverVideo || '';
    if (v && !v.startsWith('yt:')) {
      const el = make('video', 'pv-video');
      Object.assign(el, { src: vaultUrl(v), autoplay: true, loop: true, playsInline: true });
      el.onerror = () => el.remove(); // (the screensaver behind it shows instead)
      el.play().catch(() => { el.muted = true; el.play().catch(() => {}); });
      return el;
    }
    const id = v ? v.slice(3) : CAT;
    const f = make('iframe', 'pv-video');
    f.allow = 'autoplay; encrypted-media';
    f.referrerPolicy = 'strict-origin-when-cross-origin'; // (YouTube refuses embeds without one; the page itself sends none)
    f.src = `https://www.youtube-nocookie.com/embed/${encodeURIComponent(id)}?autoplay=1&loop=1&playlist=${encodeURIComponent(id)}&controls=0&rel=0&modestbranding=1`;
    return f;
  }
  function hideScreen() {
    if (!screen) return;
    clearInterval(screen.tick);
    screen.querySelectorAll('video').forEach(m => { try { m.pause(); } catch (x) {} });
    screen.remove(); screen = null;
    for (const ev of ['keydown', 'keyup', 'wheel', 'contextmenu']) removeEventListener(ev, swallow, true);
  }

  // Settings → Privacy: which video the cover plays (only shown for "A cat video")
  const ytId = text => {
    const m = String(text).trim().match(/(?:youtu\.be\/|[?&]v=|\/embed\/|\/shorts\/)([\w-]{11})/) || String(text).trim().match(/^([\w-]{11})$/);
    return m ? m[1] : '';
  };
  function videoRow() {
    const row = make('div', 'pv-keyrow pv-videorow');
    const v = settings.coverVideo || '';
    const what = !v ? 'Keyboard Cat (YouTube)' : v.startsWith('yt:') ? 'A YouTube video' : 'Your video';
    row.append(make('span', 'pv-keylabel', 'The video'), make('kbd', 'pv-key', what));
    const pick = make('button', 'btn', 'Pick a video…'); pick.type = 'button';
    pick.onclick = async () => {
      try {
        const id = await tauriInvoke('pick_cover_video');
        if (id) { settings.coverVideo = id; saveSettingsSoon(); drawVideoRow(); toast('The emergency key will play your video', 'ok'); }
      } catch (e) { toast(String(e), 'err'); }
    };
    const yt = make('button', 'btn', 'YouTube link…'); yt.type = 'button';
    yt.onclick = () => {
      const text = prompt('Paste a YouTube link (it plays when you press the emergency key; needs the internet):');
      if (text == null) return;
      const id = ytId(text);
      if (!id) { toast('That doesn\'t look like a YouTube link', 'err'); return; }
      settings.coverVideo = 'yt:' + id; saveSettingsSoon(); drawVideoRow(); toast('Saved', 'ok');
    };
    row.append(pick, yt);
    if (v) {
      const back = make('button', 'btn', 'Back to the cat'); back.type = 'button';
      back.onclick = () => { settings.coverVideo = ''; saveSettingsSoon(); drawVideoRow(); };
      row.append(back);
    }
    const test = make('button', 'btn', 'Preview'); test.type = 'button';
    test.title = 'Shows it for 5 seconds';
    test.onclick = () => { showScreen('video'); setTimeout(hideScreen, 5000); };
    row.append(test);
    return row;
  }
  function drawVideoRow() {
    const old = document.querySelector('#settingsForm .pv-videorow');
    const keyRowEl = document.querySelector('#settingsForm .pv-keyrow:not(.pv-videorow)');
    if (old) old.remove();
    if (keyRowEl && S('panicMode') === 'video') keyRowEl.after(videoRow());
  }

  // ---------- Settings: Privacy (the key) and Storage (discreet names, Excel) ----------
  const renderSettingsPrivacy = renderSettings;
  renderSettings = function () {
    const r = renderSettingsPrivacy.apply(this, arguments);
    const priv = [...document.querySelectorAll('#settingsForm h3')].find(h => h.textContent === 'Privacy');
    if (priv && !document.querySelector('#settingsForm .pv-keyrow')) {
      let last = priv.nextElementSibling;
      while (last && last.nextElementSibling && last.nextElementSibling.tagName !== 'H3') last = last.nextElementSibling;
      last.after(keyRow('settings'));
    }
    drawVideoRow();
    drawStorage();
    return r;
  };
  const storageBox = make('div', 'pv-storage');
  $('storageInfo').parentElement.append(storageBox);
  async function drawStorage() {
    const st = await api('discreetProgress').catch(() => ({}));
    storageBox.replaceChildren();
    const dRow = make('div', 'setting pv-discreet');
    const text = make('span', 's-label', 'Discreet names');
    text.append(make('small', '', 'Every file in one neutral folder, Media, named by its number only (000123_file.webp), so the folders and file names give nothing away. Categories, scores and dates stay inside the app. Switching moves the files once (it can take a while for a big library, and can be undone).'));
    const tog = make('input'); tog.type = 'checkbox'; tog.checked = !!st.discreet;
    const prog = make('div', 'pv-progress hidden'); prog.append(make('i'));
    tog.onchange = () => switchDiscreet(tog.checked, prog, tog);
    dRow.append(text, tog);
    const xl = make('div', 'setting');
    const xt = make('span', 's-label', 'Export to Excel');
    xt.append(make('small', '', 'Your library as a spreadsheet: one row per item with its category, scores, tags, views and time, in pink, with filters. index.csv and views.csv themselves stay plain CSV (the app needs them that way, and CSV can\'t hold colours).'));
    const xb = make('button', 'btn', 'Export…'); xb.type = 'button';
    xb.onclick = async () => {
      xb.disabled = true; xb.textContent = 'Saving…';
      try { const path = await tauriInvoke('export_excel'); toast('Saved: ' + path, 'ok'); }
      catch (e) { if (!/Cancelled/.test(String(e))) toast(String(e), 'err'); }
      xb.disabled = false; xb.textContent = 'Export…';
    };
    xl.append(xt, xb);
    storageBox.append(dRow, prog, xl);
    if (st.status === 'running') watchMove(prog, tog);
  }
  async function switchDiscreet(on, prog, tog) {
    if (!confirm(on ? 'Move every file into one neutral folder with number-only names?' : 'Move every file back into its category folder with descriptive names?')) { tog.checked = !on; return; }
    try { await api('setDiscreet', on); watchMove(prog, tog); }
    catch (e) { tog.checked = !on; toast(errMsg(e), 'err'); }
  }
  async function watchMove(prog, tog) {
    prog.classList.remove('hidden'); tog.disabled = true;
    let p;
    do {
      await new Promise(r => setTimeout(r, 400));
      p = await api('discreetProgress');
      prog.firstChild.style.width = (p.total ? p.done / p.total * 100 : 0) + '%';
    } while (p.status === 'running');
    tog.disabled = false; prog.classList.add('hidden');
    if (p.status === 'done') { toast(p.discreet ? `Discreet names on: ${fmtN(p.moved)} files moved` : `Descriptive names back: ${fmtN(p.moved)} files moved`, 'ok'); await reloadAll(); }
    else toast(p.status || 'Moving stopped', 'err');
  }
  async function reloadAll() {
    // the file names changed: the index is read again
    try { if (typeof ensureData === 'function') { library = null; await ensureData(true); } } catch (e) {}
    renderLibrary();
  }

  // ---------- the welcome screen: discreet names (on) and the emergency key ----------
  const card = document.querySelector('#welcome .wl-card');
  if (card) {
    const box = make('div', 'pv-welcome');
    const lab = make('label', 'pv-check');
    const cb = make('input'); cb.type = 'checkbox';
    try { cb.checked = localStorage.getItem('imageVault.discreetChoice') !== '0'; } catch (e) { cb.checked = true; }
    cb.onchange = () => { try { localStorage.setItem('imageVault.discreetChoice', cb.checked ? '1' : '0'); } catch (e) {} };
    lab.append(cb, make('span', '', 'Discreet names: files in one neutral folder, named by number only'));
    box.append(lab, keyRow('welcome'));
    const pointer = card.querySelector('.wl-pointer');
    (pointer || card.querySelector('.wl-step.wl-choose')).after(box);
  }
  // a new, empty vault takes the welcome screen's choice (on unless unticked)
  setupReady.then(async s => {
    if (!s || s.firstRun || s.discreetChosen) return;
    while (!library) await new Promise(r => setTimeout(r, 300));
    if (library.length) return; // an existing library is never renamed without asking
    let on = true;
    try { on = localStorage.getItem('imageVault.discreetChoice') !== '0'; } catch (e) {}
    api('setDiscreet', on).catch(() => {});
  });
})();
