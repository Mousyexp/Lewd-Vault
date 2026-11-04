// Lewd Vault desktop: the look (Phase 2 of PLAN.md). Loaded after desktop.js and uses the page's own
// settings (SETTINGS, settings, S, saveSettingsSoon, renderSettings, applyTheme). It adds:
//   - Font: Nunito (round) by default, or another round font
//   - Mouse pointer: Windows' own by default, or a heart or a paw in the theme's colour (also asked
//     on the welcome screen)
//   - the heart logo, with slow still water inside
//   - plain-spoken explanations under every setting
//   - A theme of your own (Custom): your main colour and background colour
//   - the decorations tinted to the theme's colour
//   - a slowly moving background: faint hearts and sparkles drifting, now and then a soft glow
//   - the sidebar: drag its edge to resize it, double-click the edge to make it compact (icons
//     only), and it scrolls (with a faint arrow) when the tiles don't fit, instead of squashing them

(() => {
  'use strict';
  const html = document.documentElement;
  const make = window.vaultUI.make;
  const saved = (() => { try { return JSON.parse(localStorage.getItem('imageVault.settings')) || {}; } catch (e) { return {}; } })();

  // ---------- the options, in Settings → Look ----------
  const FONTS = {
    nunito: ['Nunito (round)', '"Nunito", "Segoe UI", system-ui, sans-serif'],
    quicksand: ['Quicksand', '"Quicksand", "Nunito", system-ui, sans-serif'],
    comfortaa: ['Comfortaa', '"Comfortaa", "Nunito", system-ui, sans-serif'],
    fredoka: ['Fredoka', '"Fredoka", "Nunito", system-ui, sans-serif'],
    inter: ['Inter', '"Inter Variable", "Segoe UI Variable", "Segoe UI", system-ui, sans-serif'],
    theme: ["The theme's own", '']
  };
  const CURSORS = [['system', "Windows' own"], ['heart', 'Pink heart'], ['paw', 'Paw']];
  const after = (key, ...specs) => SETTINGS.splice(SETTINGS.findIndex(s => s.key === key) + 1, 0, ...specs);
  after('decor',
    { section: 'Look', key: 'font', label: 'Font', type: 'select', def: 'nunito',
      options: Object.entries(FONTS).map(([k, [name]]) => [k, name]) },
    { section: 'Look', key: 'uiSize', label: 'Size of everything', type: 'select', def: '1.1',
      options: [['1', 'Normal'], ['1.1', 'Big'], ['1.2', 'Bigger'], ['1.3', 'Biggest']],
      help: 'Makes the whole app larger: text, buttons, icons and pictures, for easier clicking.' },
    { section: 'Look', key: 'cursor', label: 'Mouse pointer', type: 'select', def: 'system', options: CURSORS },
    { section: 'Look', key: 'bgMotion', label: 'Moving background', type: 'toggle', def: true });
  after('maxPlaying',
    { section: 'Library', key: 'fastGpu', label: 'Use the faster graphics card', type: 'toggle', def: false,
      help: 'On a laptop with two graphics chips (Intel plus NVIDIA or AMD), draws the app with the stronger one. Uses more battery. Takes effect the next time Lewd Vault starts.' });
  for (const key of ['font', 'uiSize', 'cursor', 'bgMotion', 'fastGpu']) {
    const spec = SETTINGS.find(s => s.key === key);
    settings[key] = saved[key] !== undefined ? cleanSetting(spec, saved[key]) : spec.def;
  }

  // ---------- Custom: a theme of your own ----------
  THEME_LIST.push(['custom', 'Custom']);
  const isHex = v => typeof v === 'string' && /^#[0-9a-f]{6}$/i.test(v);
  settings.customAccent = isHex(saved.customAccent) ? saved.customAccent : '#e27589';
  settings.customBase = isHex(saved.customBase) ? saved.customBase : '#231b2b';
  const rgb = hex => [1, 3, 5].map(i => parseInt(hex.slice(i, i + 2), 16));
  const hexOf = c => '#' + c.map(v => Math.round(Math.max(0, Math.min(255, v))).toString(16).padStart(2, '0')).join('');
  const mix = (a, b, t) => hexOf(rgb(a).map((v, i) => v + (rgb(b)[i] - v) * t));
  const lum = hex => { const [r, g, b] = rgb(hex).map(v => v / 255); return 0.2126 * r + 0.7152 * g + 0.0722 * b; };
  function customVars(accent, base) {
    const dark = lum(base) < 0.45, text = dark ? '#f6eef2' : '#211a1e';
    return {
      '--bg': base, '--panel': mix(base, dark ? '#ffffff' : '#ffffff', dark ? 0.06 : 0.7), '--line': mix(base, dark ? '#ffffff' : '#000000', dark ? 0.14 : 0.12),
      '--text': text, '--dim': mix(text, base, 0.42), '--accent': accent, '--on-accent': lum(accent) > 0.6 ? '#1c1418' : '#ffffff',
      'color-scheme': dark ? 'dark' : 'light'
    };
  }
  const CUSTOM_KEYS = Object.keys(customVars('#000000', '#000000'));
  function applyCustom() {
    const on = S('theme') === 'custom';
    const vars = customVars(settings.customAccent, settings.customBase);
    for (const k of CUSTOM_KEYS) on ? html.style.setProperty(k, vars[k]) : html.style.removeProperty(k);
  }

  // ---------- the font ----------
  function applyFont() {
    const f = FONTS[S('font')] || FONTS.nunito;
    f[1] ? html.style.setProperty('--font', f[1]) : html.style.removeProperty('--font');
  }

  // ---------- the decorations in the theme's colour ----------
  // They're drawn in rose (hue about 349°): turned to the accent's hue, and grey for a grey accent.
  const accentNow = () => {
    const v = getComputedStyle(html).getPropertyValue('--accent').trim();
    if (isHex(v)) return v;
    const m = v.match(/\d+(\.\d+)?/g);
    return m && m.length >= 3 ? hexOf(m.slice(0, 3).map(Number)) : '#e27589';
  };
  function hueSat(hex) {
    const [r, g, b] = rgb(hex).map(v => v / 255), max = Math.max(r, g, b), min = Math.min(r, g, b), d = max - min;
    let h = 0;
    if (d) h = max === r ? ((g - b) / d) % 6 : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
    const l = (max + min) / 2;
    return [(h * 60 + 360) % 360, d ? d / (1 - Math.abs(2 * l - 1)) : 0];
  }
  function applyDecorTint() {
    const [h, s] = hueSat(accentNow());
    const turn = Math.round(((h - 349) % 360 + 540) % 360 - 180);
    html.style.setProperty('--decor-tint', S('theme') === 'rose' || Math.abs(turn) < 6 ? 'hue-rotate(0deg)' : s < 0.2 ? 'grayscale(1)' : `hue-rotate(${turn}deg)`);
  }

  // ---------- the cursor ----------
  const HEART = 'M50 90C22 68 3 50 3 28 3 13 14 3 28 3c9 0 17 5 22 13C55 8 63 3 72 3c14 0 25 10 25 25 0 22-19 40-47 62z';
  function cursorSvg(name, c) {
    const ink = '#2a0d17', st = `stroke="${ink}" stroke-width="1.6" stroke-linejoin="round" stroke-linecap="round"`;
    const parts = {
      heart: [`<g transform="rotate(135 16 16) translate(3.5 4) scale(.25)"><path d="${HEART}" fill="${c}" stroke="${ink}" stroke-width="7" stroke-linejoin="round"/></g>`, 7, 7],
      paw: [`<ellipse cx="16" cy="21" rx="7" ry="6" fill="${c}" ${st}/><circle cx="7.5" cy="12.5" r="3" fill="${c}" ${st}/><circle cx="12.8" cy="7.5" r="3" fill="${c}" ${st}/><circle cx="19.2" cy="7.5" r="3" fill="${c}" ${st}/><circle cx="24.5" cy="12.5" r="3" fill="${c}" ${st}/>`, 16, 16]
    };
    const [body, x, y] = parts[name];
    return [`<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" viewBox="0 0 32 32">${body}</svg>`, x, y];
  }
  function applyCursor() {
    const name = S('cursor');
    html.classList.toggle('own-cursor', name !== 'system');
    if (name === 'system') return html.style.removeProperty('--cursor');
    const [svg, x, y] = cursorSvg(name, accentNow());
    html.style.setProperty('--cursor', `url("data:image/svg+xml,${encodeURIComponent(svg)}") ${x} ${y}, auto`);
  }

  // ---------- the size of everything: the window's own zoom (main.rs window_action "zoom:") ----------
  let sizeNow = null;
  function applySize() {
    const v = String(S('uiSize') || '1.1');
    if (v === sizeNow || typeof tauriInvoke !== 'function') return;
    sizeNow = v;
    tauriInvoke('window_action', { action: 'zoom:' + v }).catch(() => {});
  }

  // ---------- everything, whenever the theme or a setting changes ----------
  function applyLook() {
    applySize();
    applyCustom();
    applyFont();
    applyDecorTint();
    applyCursor();
    background.update();
  }
  const applyThemeLook = applyTheme;
  applyTheme = function () { const r = applyThemeLook.apply(this, arguments); applyLook(); return r; };
  const saveLook = saveSettingsSoon;
  saveSettingsSoon = function () { applyLook(); return saveLook.apply(this, arguments); };
  setupReady.then(s => {
    const v = (s && s.settings) || {};
    if (isHex(v.customAccent)) settings.customAccent = v.customAccent;
    if (isHex(v.customBase)) settings.customBase = v.customBase;
    applyLook();
  });

  // ---------- Settings: the theme builder, under the theme swatches ----------
  const renderSettingsLook = renderSettings;
  renderSettings = function () {
    const r = renderSettingsLook.apply(this, arguments);
    const sw = document.querySelector('#settingsForm .swatches');
    if (!sw) return r;
    const customSwatch = sw.querySelector('[data-theme="custom"]');
    if (customSwatch) {
      const v = customVars(settings.customAccent, settings.customBase);
      for (const k of CUSTOM_KEYS) customSwatch.style.setProperty(k, v[k]);
    }
    const box = make('div', 'theme-builder' + (S('theme') === 'custom' ? ' on' : ''));
    box.append(make('div', 'tb-head', 'Make your own'), make('p', 'tb-lead', 'Pick a main colour and a background. Changing either switches to Custom.'));
    const row = make('div', 'tb-row');
    const field = (label, key) => {
      const wrap = make('label', 'tb-field');
      const input = make('input');
      input.type = 'color'; input.value = settings[key];
      input.oninput = () => {
        settings[key] = input.value;
        if (S('theme') !== 'custom') { settings.theme = 'custom'; sw.querySelectorAll('.swatch').forEach(b => b.classList.toggle('on', b.dataset.theme === 'custom')); box.classList.add('on'); }
        applyTheme();
        if (customSwatch) { const v = customVars(settings.customAccent, settings.customBase); for (const k of CUSTOM_KEYS) customSwatch.style.setProperty(k, v[k]); }
      };
      input.onchange = () => saveSettingsSoon();
      wrap.append(input, make('span', '', label));
      return wrap;
    };
    row.append(field('Main colour', 'customAccent'), field('Background', 'customBase'));
    // a few backgrounds to start from
    const bases = make('div', 'tb-bases');
    for (const [name, base] of [['Wine', '#231b2b'], ['Night', '#121218'], ['Plum', '#2a1630'], ['Ocean', '#10202a'], ['Forest', '#14221c'], ['Blush', '#fbeff3'], ['Cream', '#faf6ee']]) {
      const b = make('button', 'tb-base');
      b.type = 'button'; b.title = name; b.style.background = base;
      b.onclick = () => { const inp = row.querySelectorAll('input')[1]; inp.value = base; inp.oninput(); saveSettingsSoon(); };
      bases.append(b);
    }
    row.append(bases);
    // the font, here too
    const font = make('select', 'tb-font');
    for (const [k, [name]] of Object.entries(FONTS)) font.add(new Option(name, k));
    font.value = S('font');
    font.onchange = () => { settings.font = font.value; saveSettingsSoon(); renderSettings(); };
    const fontWrap = make('label', 'tb-field');
    fontWrap.append(font, make('span', '', 'Font'));
    row.append(fontWrap);
    box.append(row);
    sw.after(box);
    return r;
  };

  // ---------- the moving background ----------
  // One canvas behind the page (at most 30 frames a second, and none while the window is in the
  // background, a picture is open full size, or the setting or Animations are off).
  const background = (() => {
    const canvas = make('canvas', 'bg-motion');
    document.body.prepend(canvas);
    const ctx = canvas.getContext('2d');
    // drawn as paths (the symbol font's sparkles are missing on some PCs): hearts, and four-point stars
    const SHAPES = [[new Path2D(HEART), 100, 47], [new Path2D('M16 1c1 10 5 14 15 15-10 1-14 5-15 15-1-10-5-14-15-15 10-1 14-5 15-15z'), 32, 16]];
    let things = [], glows = [], running = false, last = 0, w = 0, h = 0, colour = '#e27589';
    function size() {
      w = canvas.width = Math.ceil(innerWidth / 1.5); h = canvas.height = Math.ceil(innerHeight / 1.5); // drawn at 2/3 size: it's soft anyway
      things = Array.from({ length: Math.round(w * h / 26000) + 6 }, () => fresh(true));
    }
    function fresh(anywhere) {
      return { x: Math.random() * w, y: anywhere ? Math.random() * h : h + 20, s: 9 + Math.random() * 16, v: 4 + Math.random() * 9,
        a: 0.035 + Math.random() * 0.05, sway: Math.random() * 6.28, spin: (Math.random() - 0.5) * 0.4, r: Math.random() * 6.28, ch: SHAPES[Math.random() < 0.6 ? 0 : 1] };
    }
    // not behind the Library or Discover either: their tiles cover the whole page, so it would only cost time
    const covered = () => !$('tab-library').classList.contains('hidden') || !!document.querySelector('#tab-discover:not(.hidden)');
    const wanted = () => S('bgMotion') && !html.classList.contains('motion-off') && document.visibilityState === 'visible' && document.hasFocus()
      && $('detail').classList.contains('hidden') && $('show').classList.contains('hidden') && !covered();
    function frame(now) {
      if (!wanted()) { running = false; return; }
      requestAnimationFrame(frame);
      if (now - last < 33) return;
      const dt = Math.min(0.1, (now - last) / 1000);
      last = now;
      ctx.clearRect(0, 0, w, h);
      // now and then a soft glow somewhere, fading in and out over a few seconds
      if (Math.random() < dt / 7 && glows.length < 2) glows.push({ x: Math.random() * w, y: Math.random() * h, r: 120 + Math.random() * 160, t: 0, life: 5 + Math.random() * 4 });
      glows = glows.filter(g => (g.t += dt) < g.life);
      for (const g of glows) {
        const k = Math.sin(Math.PI * g.t / g.life);
        const grad = ctx.createRadialGradient(g.x, g.y, 0, g.x, g.y, g.r);
        grad.addColorStop(0, colour + Math.round(k * 34).toString(16).padStart(2, '0'));
        grad.addColorStop(1, colour + '00');
        ctx.fillStyle = grad;
        ctx.fillRect(g.x - g.r, g.y - g.r, g.r * 2, g.r * 2);
      }
      ctx.fillStyle = colour;
      for (let k = 0; k < things.length; k++) {
        const t = things[k];
        t.y -= t.v * dt; t.sway += dt * 0.6; t.r += t.spin * dt;
        if (t.y < -30) things[k] = fresh(false);
        ctx.globalAlpha = t.a;
        ctx.save();
        ctx.translate(t.x + Math.sin(t.sway) * 14, t.y);
        ctx.rotate(t.r);
        const [path, box, mid] = t.ch, sc = t.s / box;
        ctx.scale(sc, sc);
        ctx.translate(-box / 2, -mid);
        ctx.fill(path);
        ctx.restore();
      }
      ctx.globalAlpha = 1;
    }
    function update() {
      colour = accentNow();
      canvas.classList.toggle('hidden', !S('bgMotion') || covered());
      if (!w) size();
      if (!running && wanted()) { running = true; last = performance.now(); requestAnimationFrame(frame); }
    }
    addEventListener('resize', () => { size(); });
    for (const ev of ['focus', 'visibilitychange']) addEventListener(ev, update);
    document.addEventListener('visibilitychange', update);
    // the full view and the slideshow pause it; closing them starts it again
    new MutationObserver(update).observe($('detail'), { attributes: true, attributeFilter: ['class'] });
    new MutationObserver(update).observe($('show'), { attributes: true, attributeFilter: ['class'] });
    document.querySelectorAll('main > section').forEach(sec => new MutationObserver(update).observe(sec, { attributes: true, attributeFilter: ['class'] }));
    setTimeout(() => { const d = document.getElementById('tab-discover'); if (d) new MutationObserver(update).observe(d, { attributes: true, attributeFilter: ['class'] }); }, 0);
    return { update };
  })();

  // ---------- the sidebar: resize, compact, scroll ----------
  const SIDE_KEY = 'imageVault.side';
  const header = document.querySelector('header'), nav = document.querySelector('nav');
  const sideRule = make('style');
  document.head.append(sideRule);
  let side = (() => { try { return JSON.parse(localStorage.getItem(SIDE_KEY)) || {}; } catch (e) { return {}; } })();
  function applySide() {
    const width = Math.max(200, Math.min(400, side.width || 268));
    html.classList.toggle('side-compact', !!side.compact);
    // only for wide windows: narrow ones keep their column of icons
    sideRule.textContent = `@media (min-width: 901px) { html.desktop { --side: ${side.compact ? 76 : width}px; } }`;
    dispatchEvent(new Event('resize'));
    moreArrows();
  }
  const saveSide = () => { try { localStorage.setItem(SIDE_KEY, JSON.stringify(side)); } catch (e) {} };
  const grip = make('div', 'side-grip');
  grip.title = 'Drag to resize · double-click for icons only';
  header.append(grip);
  grip.addEventListener('pointerdown', e => {
    e.preventDefault();
    grip.setPointerCapture(e.pointerId);
    html.classList.add('side-dragging');
    const move = ev => {
      const x = ev.clientX;
      if (x < 150) { if (!side.compact) { side.compact = true; applySide(); } return; }
      side.compact = false; side.width = Math.max(200, Math.min(400, x));
      applySide();
    };
    const up = () => { grip.removeEventListener('pointermove', move); grip.removeEventListener('pointerup', up); html.classList.remove('side-dragging'); saveSide(); };
    grip.addEventListener('pointermove', move);
    grip.addEventListener('pointerup', up);
  });
  grip.addEventListener('dblclick', () => { side.compact = !side.compact; applySide(); saveSide(); });
  // when the tiles don't fit, the list scrolls, with a faint arrow at the edge that has more
  const up = make('button', 'side-more up'), down = make('button', 'side-more down');
  up.type = down.type = 'button';
  up.tabIndex = down.tabIndex = -1;
  up.onclick = () => nav.scrollBy({ top: -160, behavior: 'smooth' });
  down.onclick = () => nav.scrollBy({ top: 160, behavior: 'smooth' });
  nav.before(up); nav.after(down);
  function moreArrows() {
    up.classList.toggle('show', nav.scrollTop > 4);
    down.classList.toggle('show', nav.scrollTop + nav.clientHeight < nav.scrollHeight - 4);
  }
  nav.addEventListener('scroll', moreArrows, { passive: true });
  addEventListener('resize', moreArrows);
  new ResizeObserver(moreArrows).observe(nav);
  applySide();

  // ---------- the heart logo: slow, still water inside ----------
  const mark = document.querySelector('header .brand-mark');
  if (mark) {
    mark.classList.add('alive');
    mark.innerHTML = `<svg viewBox="0 0 100 94" aria-hidden="true">
      <defs>
        <clipPath id="lvClip"><path d="${HEART}"/></clipPath>
        <linearGradient id="lvFill" gradientUnits="userSpaceOnUse" x1="0" y1="20" x2="0" y2="94"><stop offset="0" stop-color="#F29BAE"/><stop offset=".5" stop-color="#E27589"/><stop offset="1" stop-color="#8a3550"/></linearGradient>
        <radialGradient id="lvShine" cx=".3" cy=".25" r=".32"><stop offset="0" stop-color="#fff" stop-opacity=".6"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></radialGradient>
      </defs>
      <path d="${HEART}" fill="#4a2232" stroke="#2a0d17" stroke-width="7" stroke-linejoin="round"/>
      <g clip-path="url(#lvClip)"><g class="lv-level">
        <path class="lv-wave back" d="M0 33Q12.5 29 25 33T50 33T75 33T100 33T125 33T150 33T175 33T200 33V120H0Z"/>
        <path class="lv-wave front" d="M0 35Q12.5 32 25 35T50 35T75 35T100 35T125 35T150 35T175 35T200 35V120H0Z"/>
      </g></g>
      <path d="${HEART}" fill="url(#lvShine)"/>
      <path d="${HEART}" fill="none" stroke="#F9CDD4" stroke-width="3" stroke-linejoin="round"/>
    </svg>`;
  }

  // ---------- the welcome screen also asks for the mouse pointer ----------
  const welcomeCard = document.querySelector('#welcome .wl-card');
  if (welcomeCard) {
    const row = make('div', 'wl-pointer');
    row.append(make('span', 'wl-pointer-label', 'Mouse pointer'));
    // drawn each time the screen shows, in the theme's colour (a little room around them, so nothing's clipped)
    const draw = () => row.querySelectorAll('.wl-pt').forEach(b => {
      b.classList.toggle('on', b.dataset.cursor === S('cursor'));
      if (b.dataset.cursor !== 'system') b.firstChild.innerHTML = cursorSvg(b.dataset.cursor, accentNow())[0].replace('viewBox="0 0 32 32"', 'viewBox="-3 -3 38 38"');
    });
    for (const [key, name] of CURSORS) {
      const b = make('button', 'wl-pt');
      b.type = 'button'; b.dataset.cursor = key;
      const pic = make('span', 'wl-pt-pic');
      if (key === 'system') pic.innerHTML = '<svg viewBox="0 0 32 32"><path d="M6 3v22l6-6 4 9 3-1.4-4-8.6h8z" fill="#fff" stroke="#000" stroke-width="1.6" stroke-linejoin="round"/></svg>';
      b.append(pic, make('span', '', name));
      b.onclick = () => { settings.cursor = key; applyCursor(); saveSettingsSoon(); draw(); };
      row.append(b);
    }
    welcomeCard.querySelector('.wl-step.wl-choose').after(row);
    setupReady.then(draw);
    new MutationObserver(draw).observe(document.getElementById('welcome'), { attributes: true, attributeFilter: ['class'] });
  }

  // ---------- every setting says plainly what it does ----------
  const HELP = {
    splash: 'On: a big pink heart fills up while your vault opens, so start-up never looks frozen. Off: the app opens straight to your last tab (a little faster).',
    motion: 'Always: everything glides and fades. Follow Windows: animations stop when Windows has "Show animations" turned off. Off: nothing moves (fastest, easiest on old PCs).',
    decor: 'The little anime girls standing beside pages, peeking over boxes and waiting in empty screens. Click one and she bounces. Off hides every one of them.',
    font: 'The typeface for all the text (menus, tags, buttons, settings). Nunito is round and soft. Only the "Lewd Vault" name uses the handwritten script.',
    cursor: "Windows' own is the normal arrow. Pink heart and Paw replace it everywhere in the app (in the theme's colour); text boxes keep the I-beam so you can still click into text.",
    bgMotion: 'Faint hearts and sparkles drift slowly up behind every page, with a soft glow appearing now and then. It pauses on its own while a picture is open, during the slideshow, and when the app is in the background.',
    libraryLayout: 'Rows: every picture whole, in rows of the same height (nothing cut off). Even grid: same-size squares, so wide and tall pictures are cropped to fit.',
    previewSpeed: 'How fast videos and GIFs play inside their Library tiles (the moving previews). Opening one full size always plays it at normal speed.',
    maxPlaying: 'How many tiles can play their moving preview at the same time. Tiles past this number show a still picture until you scroll. Lower it if scrolling stutters; raise it on a fast PC.',
    webpQuality: 'When you add a photo it is saved as WebP at this quality. 92 looks identical to the original at about a third of the size. 80 is noticeably smaller and still looks good. 100 keeps almost every detail (big files).',
    videoQuality: 'When a video has to be converted (Windows cannot play its format), this is how much detail the new copy keeps. Very high is closest to the original but biggest; Medium saves the most space.',
    keepVideoCodec: 'On: iPhone (HEVC), VP9 and AV1 videos are kept exactly as they are when this PC can play them: no quality lost, smaller files. Off: every video is converted to H.264, which plays everywhere but is bigger.',
    previewSeconds: 'Each video gets a short silent clip made when you add it, played in its Library tile. Longer clips show more but take more space and time to make. Only affects videos added from now on.',
    cleanupCount: 'Clean up lists the items you look at the least, so you can decide what to delete. This is how many it lists at once.',
    cleanupGraceDays: 'Things added in the last this-many days are never listed in Clean up, because you have not had time to look at them yet.',
    likeWeight: 'Clean up ranks items by how much you use them. Each Appreciate click on an item counts as this many views, so the things you love are kept off the list.'
  };
  for (const spec of SETTINGS) if (HELP[spec.key]) spec.help = HELP[spec.key];

  applyLook();
})();
