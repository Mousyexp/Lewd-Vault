// The phone layer, part two (part one is phone.js, which it builds on through window.__phone).
// Only runs on a phone. What's here:
//   1. Settings → Phone: everything below in one place (the More sheet and the Settings tab open it)
//   2. themes made for the phone, animated (petals, stars, bubbles, paws...), an accent of your own,
//      seasonal touches and milestone celebrations
//   3. the Library: pinch to change the grid, drag to select, a fast scrubber, smart albums and
//      saved filters
//   4. privacy: Face ID / fingerprint, a decoy PIN, lock after, shake to hide, hiding from screen
//      recordings, and the break-in photo
//   5. pictures shared to the app (the share sheet, "Open in"), the app icon presets, the reminder to
//      delete from Photos, text size, density and low-data mode
// The native parts (haptics, Face ID, the inbox, the icon, the screen guard) are src-tauri/src/phone.rs.
(() => {
  'use strict';
  const P = window.__phone;
  if (!P) return;
  const { html, make, icon, sheet, opt, saveOpt, native, buzz, dial, layer, unlayer } = P;
  const $ = id => document.getElementById(id);
  const android = html.classList.contains('android');
  const reduced = () => matchMedia('(prefers-reduced-motion: reduce)').matches;
  const DEF = {
    theme: 'app', accent: '', fx: true, seasonal: true, milestones: true, textSize: 'auto', density: 'normal', lowData: 'off',
    bio: false, decoy: null, autolock: 0, shake: false, shakeLevel: 2, guard: false, breakin: false, icon: 'default', photosReminder: false,
    gridCols: 3, presets: [], done: []
  };
  const fill = () => { for (const [k, v] of Object.entries(DEF)) if (!(k in opt)) opt[k] = Array.isArray(v) ? v.slice() : v; };
  fill();
  Object.assign(P.ICON, {
    phone: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linejoin='round'><rect x='6' y='2.500' width='12' height='19' rx='3'/><path d='M10.500 18.500h3' stroke-linecap='round'/></svg>",
    grip: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='black'><circle cx='9' cy='6' r='1.600'/><circle cx='15' cy='6' r='1.600'/><circle cx='9' cy='12' r='1.600'/><circle cx='15' cy='12' r='1.600'/><circle cx='9' cy='18' r='1.600'/><circle cx='15' cy='18' r='1.600'/></svg>"
  });

  // =====================================================================================
  // 2. themes
  // =====================================================================================
  // Each sets the app's colours (on top of the theme chosen in Settings) and brings its own little
  // world: something drifting behind everything, a mark that hops over the chosen tab, and the shape
  // of the sparks when you appreciate something. "The app's theme" leaves the colours alone.
  const THEMES = [
    ['app', 'The app\'s theme', ''], ['sakura', 'Sakura', 'petals'], ['midnight', 'Midnight', 'stars'], ['bubblegum', 'Bubblegum', 'bubbles'],
    ['aurora', 'Aurora', 'aurora'], ['kitten', 'Kitten', 'paws'], ['neon', 'Neon', 'neon'], ['lavender', 'Lavender', 'sparkles'], ['mono', 'Mono', '']
  ];
  const ACCENTS = ['#ff6f96', '#ff8fb3', '#ff6ad5', '#b69cff', '#8f9bff', '#4fe3c1', '#7bd88f', '#ffb07c', '#ffd166', '#ffffff'];
  const fxOf = () => (THEMES.find(t => t[0] === opt.theme) || THEMES[0])[2];
  // the season, when Seasonal touches is on: snow in winter, hearts mid-February, leaves in autumn
  const season = () => {
    const d = new Date(), m = d.getMonth() + 1, day = d.getDate();
    if (m === 12 || m === 1) return 'snow';
    if (m === 2 && day <= 15) return 'hearts';
    if (m === 10 || m === 11) return 'leaves';
    if (m === 4) return 'petals';
    return '';
  };
  let accentMine = false;
  function applyTheme() {
    if (opt.theme && opt.theme !== 'app') html.dataset.ptheme = opt.theme; else delete html.dataset.ptheme;
    // (an accent of your own; taken off again only if it was this that put it there)
    if (/^#[0-9a-f]{6}$/i.test(opt.accent || '')) { html.style.setProperty('--accent', opt.accent); accentMine = true; }
    else if (accentMine) { html.style.removeProperty('--accent'); accentMine = false; }
    html.dataset.pfx = fxOf();
    fx.start();
  }

  // the drifting things: one canvas behind the page (the page's own panels sit over it)
  const fx = (() => {
    const canvas = make('canvas', 'ph-fx');
    canvas.setAttribute('aria-hidden', 'true');
    document.body.prepend(canvas);
    const ctx = canvas.getContext('2d');
    let parts = [], kind = '', extra = '', raf = 0, last = 0, w = 0, h = 0, dpr = 1;
    const size = () => { dpr = Math.min(2, devicePixelRatio || 1); w = innerWidth; h = innerHeight; canvas.width = w * dpr; canvas.height = h * dpr; ctx.setTransform(dpr, 0, 0, dpr, 0, 0); };
    addEventListener('resize', size);
    const rnd = (a, b) => a + Math.random() * (b - a);
    const accent = () => getComputedStyle(html).getPropertyValue('--accent').trim() || '#ff6f96';
    const make1 = (k, fresh) => {
      const p = { k, x: rnd(0, w), y: fresh ? rnd(-h, 0) : rnd(0, h), s: rnd(.6, 1.3), a: rnd(0, Math.PI * 2), va: rnd(-.02, .02), t: rnd(0, 100) };
      if (k === 'petals' || k === 'leaves') { p.vy = rnd(.35, .9); p.vx = rnd(-.3, .3); }
      if (k === 'snow') { p.vy = rnd(.3, .8); p.vx = rnd(-.2, .2); p.s = rnd(.5, 1.6); }
      if (k === 'hearts' || k === 'bubbles') { p.vy = -rnd(.25, .7); p.vx = rnd(-.15, .15); p.y = fresh ? h + rnd(0, h) : rnd(0, h); }
      if (k === 'stars' || k === 'sparkles') { p.vy = 0; p.vx = 0; p.tw = rnd(.01, .04); }
      if (k === 'paws') { p.vy = 0; p.vx = 0; p.life = 0; p.y = rnd(h * .15, h * .9); p.x = rnd(20, w - 20); }
      return p;
    };
    const COUNT = { petals: 22, leaves: 16, snow: 40, hearts: 14, bubbles: 16, stars: 46, sparkles: 26, paws: 7 };
    const draw = p => {
      ctx.save(); ctx.translate(p.x, p.y); ctx.rotate(p.a); ctx.scale(p.s, p.s);
      switch (p.k) {
        case 'petals': {
          ctx.fillStyle = 'rgba(255, 183, 206, .75)';
          ctx.beginPath(); ctx.moveTo(0, -7); ctx.bezierCurveTo(7, -6, 6, 6, 0, 8); ctx.bezierCurveTo(-6, 6, -7, -6, 0, -7); ctx.fill();
          ctx.fillStyle = 'rgba(255, 255, 255, .35)'; ctx.beginPath(); ctx.ellipse(-1.5, -2, 1.5, 3, 0, 0, 7); ctx.fill();
          break;
        }
        case 'leaves': { ctx.fillStyle = ['rgba(226,122,62,.75)', 'rgba(205,82,52,.75)', 'rgba(232,170,64,.75)'][Math.floor(p.t) % 3]; ctx.beginPath(); ctx.moveTo(0, -9); ctx.quadraticCurveTo(8, 0, 0, 9); ctx.quadraticCurveTo(-8, 0, 0, -9); ctx.fill(); break; }
        case 'snow': { ctx.fillStyle = 'rgba(255,255,255,.8)'; ctx.beginPath(); ctx.arc(0, 0, 2.2, 0, 7); ctx.fill(); break; }
        case 'hearts': { ctx.fillStyle = accent(); ctx.globalAlpha = .55; ctx.beginPath(); ctx.moveTo(0, 4); ctx.bezierCurveTo(-8, -2, -4, -9, 0, -4); ctx.bezierCurveTo(4, -9, 8, -2, 0, 4); ctx.fill(); break; }
        case 'bubbles': {
          ctx.strokeStyle = 'rgba(255,255,255,.45)'; ctx.lineWidth = 1.2; ctx.beginPath(); ctx.arc(0, 0, 9, 0, 7); ctx.stroke();
          ctx.fillStyle = 'rgba(255,255,255,.35)'; ctx.beginPath(); ctx.arc(-3, -3, 2.4, 0, 7); ctx.fill();
          ctx.fillStyle = accent(); ctx.globalAlpha = .5; ctx.beginPath(); ctx.moveTo(0, 3); ctx.bezierCurveTo(-4, 0, -2, -4, 0, -1.5); ctx.bezierCurveTo(2, -4, 4, 0, 0, 3); ctx.fill();
          break;
        }
        case 'stars': { const g = .35 + .65 * Math.abs(Math.sin(p.t)); ctx.fillStyle = `rgba(255,255,255,${g * .8})`; ctx.beginPath(); ctx.arc(0, 0, 1.3, 0, 7); ctx.fill(); break; }
        case 'sparkles': {
          const g = Math.max(0, Math.sin(p.t)); ctx.globalAlpha = g * .8; ctx.fillStyle = accent();
          ctx.beginPath(); ctx.moveTo(0, -7); ctx.quadraticCurveTo(1, -1, 7, 0); ctx.quadraticCurveTo(1, 1, 0, 7); ctx.quadraticCurveTo(-1, 1, -7, 0); ctx.quadraticCurveTo(-1, -1, 0, -7); ctx.fill();
          break;
        }
        case 'paws': {
          const g = p.life < 1 ? p.life : Math.max(0, 2.2 - p.life); ctx.globalAlpha = g * .35; ctx.fillStyle = accent();
          ctx.beginPath(); ctx.ellipse(0, 3, 5, 4, 0, 0, 7); ctx.fill();
          for (const [x, y] of [[-5, -3], [-1.8, -6], [1.8, -6], [5, -3]]) { ctx.beginPath(); ctx.ellipse(x, y, 1.7, 2.2, 0, 0, 7); ctx.fill(); }
          break;
        }
      }
      ctx.restore();
    };
    // a shooting star now and then, on Midnight
    let shoot = null;
    const frame = now => {
      raf = requestAnimationFrame(frame);
      if (now - last < 33) return; // (30 frames a second is plenty for drifting)
      const dt = Math.min(3, (now - last) / 16.7); last = now;
      if (document.hidden || html.classList.contains('pin-locked') || document.querySelector('#detail:not(.hidden), #show:not(.hidden), .ph-story, .pv-screen, .ios-calc')) return;
      ctx.clearRect(0, 0, w, h);
      for (const p of parts) {
        p.t += (p.tw || .02) * dt; p.a += p.va * dt;
        p.x += (p.vx + Math.sin(p.t) * .25) * dt; p.y += p.vy * dt;
        if (p.k === 'paws') { p.life += .006 * dt; if (p.life > 2.2) Object.assign(p, make1('paws'), { life: 0 }); }
        if (p.y > h + 20 || p.y < -30 || p.x < -30 || p.x > w + 30) Object.assign(p, make1(p.k, true));
        draw(p);
      }
      if (kind === 'stars') {
        if (!shoot && Math.random() < .004 * dt) shoot = { x: rnd(w * .2, w), y: rnd(0, h * .4), life: 0 };
        if (shoot) {
          shoot.life += .03 * dt;
          const x = shoot.x - shoot.life * 260, y = shoot.y + shoot.life * 120;
          const g = ctx.createLinearGradient(x, y, x + 60, y - 28); g.addColorStop(0, 'rgba(255,255,255,.9)'); g.addColorStop(1, 'rgba(255,255,255,0)');
          ctx.strokeStyle = g; ctx.lineWidth = 2; ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(x + 60, y - 28); ctx.stroke();
          if (shoot.life > 1) shoot = null;
        }
      }
    };
    return {
      start() {
        kind = opt.fx && !reduced() ? fxOf() : '';
        extra = opt.seasonal && !reduced() ? season() : '';
        const want = [kind, extra].filter(k => COUNT[k]);
        cancelAnimationFrame(raf);
        size();
        ctx.clearRect(0, 0, w, h);
        parts = want.flatMap(k => Array.from({ length: COUNT[k] }, () => make1(k, false)));
        canvas.classList.toggle('on', !!parts.length);
        html.classList.toggle('ph-fx-on', !!parts.length);
        if (parts.length) raf = requestAnimationFrame(frame);
      }
    };
  })();

  // confetti, for the milestones (in front of everything, for a moment)
  function confetti(n = 90) {
    if (reduced()) return;
    const c = make('canvas', 'ph-confetti');
    document.body.append(c);
    const dpr = Math.min(2, devicePixelRatio || 1), w = innerWidth, h = innerHeight;
    c.width = w * dpr; c.height = h * dpr;
    const ctx = c.getContext('2d'); ctx.scale(dpr, dpr);
    const colours = [getComputedStyle(html).getPropertyValue('--accent').trim() || '#ff6f96', '#ffd166', '#7bd88f', '#8f9bff', '#ffffff'];
    const bits = Array.from({ length: n }, () => ({ x: w / 2 + (Math.random() - .5) * 60, y: h * .45, vx: (Math.random() - .5) * 14, vy: -Math.random() * 16 - 4, a: Math.random() * 6, va: (Math.random() - .5) * .4, c: colours[Math.floor(Math.random() * colours.length)], s: 4 + Math.random() * 5, heart: Math.random() < .3 }));
    let t = 0;
    const step = () => {
      t++;
      ctx.clearRect(0, 0, w, h);
      for (const b of bits) {
        b.vy += .45; b.vx *= .99; b.x += b.vx; b.y += b.vy; b.a += b.va;
        ctx.save(); ctx.translate(b.x, b.y); ctx.rotate(b.a); ctx.fillStyle = b.c; ctx.globalAlpha = Math.max(0, 1 - t / 110);
        if (b.heart) { ctx.beginPath(); ctx.moveTo(0, 3); ctx.bezierCurveTo(-6, -2, -3, -7, 0, -3); ctx.bezierCurveTo(3, -7, 6, -2, 0, 3); ctx.fill(); }
        else ctx.fillRect(-b.s / 2, -b.s / 4, b.s, b.s / 2);
        ctx.restore();
      }
      if (t < 120) requestAnimationFrame(step); else c.remove();
    };
    requestAnimationFrame(step);
  }
  function cheer(title, sub) {
    confetti();
    buzz('success');
    const card = make('div', 'ph-cheer glass');
    card.append(make('b', '', title), make('span', '', sub));
    document.body.append(card);
    requestAnimationFrame(() => card.classList.add('in'));
    setTimeout(() => { card.classList.remove('in'); setTimeout(() => card.remove(), 400); }, 3200);
  }
  // milestones: the library reaching a size, a picture reaching 100 appreciations (each only once)
  const SIZES = [10, 50, 100, 250, 500, 1000, 2500, 5000, 10000];
  const milestone = (key, title, sub) => {
    if (!opt.milestones || opt.done.includes(key) || (key[0] === 'l' && html.classList.contains('no-fun'))) return;
    opt.done.push(key);
    saveOpt();
    cheer(title, sub);
  };
  const checkSize = () => {
    const n = P.all().length;
    if (!n) return;
    // (a vault opened for the first time on this phone counts what it has as already celebrated)
    if (!opt.done.includes('seen')) { opt.done.push('seen', ...SIZES.filter(s => s <= n).map(s => 'n' + s)); saveOpt(); return; }
    const hit = SIZES.filter(s => s <= n).pop();
    if (hit) milestone('n' + hit, `${hit.toLocaleString()} in your vault!`, hit >= 1000 ? 'That\'s a real collection.' : 'And counting.');
  };
  const like = $('dLike');
  if (like) like.addEventListener('click', () => setTimeout(() => {
    const it = typeof current !== 'undefined' ? current : null;
    if (it && typeof likesOf === 'function') { const n = likesOf(it.id); if (n >= 100) milestone('l' + it.id, '100 appreciations!', `${it.name || 'This one'} is a favourite.`); }
  }, 50));

  // =====================================================================================
  // 3. the Library
  // =====================================================================================
  const lib = $('tab-library'), grid = $('grid');
  const COLS = [2, 3, 5, 7];
  const applyCols = () => { html.style.setProperty('--ph-cols', String(COLS.includes(+opt.gridCols) ? +opt.gridCols : 3)); };
  if (grid) {
    // pinch the grid: fewer, bigger pictures, or more, smaller ones (3, 5, 7 across, or 2)
    let pinch = null;
    const dist = t => Math.hypot(t[0].clientX - t[1].clientX, t[0].clientY - t[1].clientY);
    grid.addEventListener('touchstart', e => {
      if (e.touches.length !== 2 || grid.dataset.layout !== 'grid') return;
      const t = e.touches, mx = (t[0].clientX + t[1].clientX) / 2, my = (t[0].clientY + t[1].clientY) / 2;
      const card = document.elementFromPoint(mx, my);
      pinch = { d: dist(t), mx, my, card: card && card.closest('.card'), k: 1 };
      grid.style.transformOrigin = `${mx}px ${my + scrollY - grid.getBoundingClientRect().top - scrollY}px`;
    }, { passive: true });
    grid.addEventListener('touchmove', e => {
      if (!pinch || e.touches.length !== 2) return;
      pinch.k = Math.max(.55, Math.min(1.8, dist(e.touches) / pinch.d));
      grid.style.transition = 'none';
      grid.style.transform = `scale(${pinch.k})`;
    }, { passive: true });
    const end = () => {
      if (!pinch) return;
      const p = pinch; pinch = null;
      const cols = COLS.includes(+opt.gridCols) ? +opt.gridCols : 3, k = COLS.indexOf(cols);
      const next = p.k > 1.18 ? COLS[Math.max(0, k - 1)] : p.k < .85 ? COLS[Math.min(COLS.length - 1, k + 1)] : cols;
      grid.style.transition = 'transform .32s cubic-bezier(.2, .8, .2, 1)';
      if (next !== cols) {
        opt.gridCols = next; saveOpt(); applyCols(); buzz('light');
        // the picture that was under your fingers stays in view
        if (p.card) p.card.scrollIntoView({ block: 'center' });
        grid.style.transition = 'none';
        grid.style.transform = `scale(${p.k > 1 ? .8 : 1.2})`;
        requestAnimationFrame(() => { grid.style.transition = 'transform .38s cubic-bezier(.3, 1.4, .5, 1)'; grid.style.transform = ''; });
      } else grid.style.transform = '';
    };
    grid.addEventListener('touchend', end, { passive: true });
    grid.addEventListener('touchcancel', end, { passive: true });

    // drag to select: once selecting (Select, or a picture already picked), slide a finger across the
    // pictures and every one between where you started and where you are is picked (like Photos)
    const selecting = () => !!document.querySelector('.ios-libtools .ios-pill.on') || !!grid.querySelector('.card.picked-many');
    let sweep = null;
    const cards = () => [...grid.querySelectorAll('.card')];
    const setPicked = (c, on) => { if (c.classList.contains('picked-many') !== on) c.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true, ctrlKey: true })); };
    grid.addEventListener('touchstart', e => {
      sweep = null;
      if (e.touches.length !== 1 || !selecting()) return;
      const c = e.target.closest('.card');
      if (!c) return;
      const t = e.touches[0];
      sweep = { x: t.clientX, y: t.clientY, start: c, on: !c.classList.contains('picked-many'), active: false, was: new Map(), last: -1 };
    }, { passive: true });
    grid.addEventListener('touchmove', e => {
      if (!sweep) return;
      const t = e.touches[0], dx = t.clientX - sweep.x, dy = t.clientY - sweep.y;
      if (!sweep.active) {
        if (Math.hypot(dx, dy) < 10) return;
        if (Math.abs(dy) > Math.abs(dx) * 1.3) { sweep = null; return; } // (an up-down start scrolls)
        sweep.active = true;
        const all = cards();
        sweep.list = all; sweep.from = all.indexOf(sweep.start);
        all.forEach(c => sweep.was.set(c, c.classList.contains('picked-many')));
      }
      e.preventDefault();
      // (the card under the finger, looking through anything floating over the grid)
      const c = document.elementsFromPoint(t.clientX, t.clientY).map(x => x.closest('#grid .card')).find(Boolean);
      if (c) {
        const at = sweep.list.indexOf(c);
        if (at >= 0 && at !== sweep.last) {
          sweep.last = at;
          const a = Math.min(at, sweep.from), b = Math.max(at, sweep.from);
          sweep.list.forEach((x, i) => setPicked(x, i >= a && i <= b ? sweep.on : sweep.was.get(x)));
          buzz('select');
        }
      }
      // near the top or bottom: the grid scrolls on its own
      const edge = 90;
      if (t.clientY > innerHeight - edge) scrollBy(0, 14); else if (t.clientY < edge + 40) scrollBy(0, -14);
    }, { passive: false });
    // (a drag makes no tap: the moves were held back, so nothing needs swallowing)
    grid.addEventListener('touchend', () => { sweep = null; }, { passive: true });

    // the scrubber: a handle on the right edge to fling through the whole library, with the month
    const scrub = make('div', 'ph-scrubber');
    const knob = make('div', 'ph-scrubber-knob'), when = make('span', 'ph-scrubber-when');
    scrub.append(knob, when);
    lib.append(scrub);
    let idle = null, dragging = false;
    const monthAt = () => {
      const el = document.elementFromPoint(innerWidth / 2, innerHeight * .35), c = el && el.closest('#grid .card');
      const it = c && c._item;
      if (!it || !it.added) return '';
      const d = new Date(it.added);
      return isNaN(d) ? '' : d.toLocaleDateString(undefined, { month: 'long', year: 'numeric' });
    };
    const place = () => {
      const max = document.documentElement.scrollHeight - innerHeight;
      const f = max > 0 ? scrollY / max : 0;
      knob.style.transform = `translateY(${f * (scrub.clientHeight - 44)}px)`;
      when.style.transform = `translateY(${f * (scrub.clientHeight - 44)}px)`;
    };
    addEventListener('scroll', () => {
      if (lib.classList.contains('hidden')) return;
      const long = document.documentElement.scrollHeight > innerHeight * 4;
      if (!long) return;
      scrub.classList.add('on');
      place();
      if (dragging) when.textContent = monthAt();
      clearTimeout(idle);
      idle = setTimeout(() => { if (!dragging) scrub.classList.remove('on'); }, 1400);
    }, { passive: true });
    knob.addEventListener('pointerdown', e => { dragging = true; knob.setPointerCapture(e.pointerId); scrub.classList.add('drag'); buzz('light'); });
    knob.addEventListener('pointermove', e => {
      if (!dragging) return;
      const r = scrub.getBoundingClientRect(), f = Math.max(0, Math.min(1, (e.clientY - r.top - 22) / (r.height - 44)));
      scrollTo(0, f * (document.documentElement.scrollHeight - innerHeight));
      when.textContent = monthAt();
    });
    const stop = () => { dragging = false; scrub.classList.remove('drag'); clearTimeout(idle); idle = setTimeout(() => scrub.classList.remove('on'), 1200); };
    knob.addEventListener('pointerup', stop);
    knob.addEventListener('pointercancel', stop);
  }

  // smart albums and saved filters: a row of chips over the grid
  const SMART = [
    ['never', 'Never opened', i => typeof viewsOf === 'function' && viewsOf(i.id) === 0],
    ['forgotten', 'Forgotten', i => { const v = typeof viewCounts !== 'undefined' && viewCounts.get(i.id); const last = v && v.last ? Date.parse(v.last) : 0; return (last || Date.parse(i.added) || 0) < Date.now() - 180 * 864e5; }],
    ['long', 'Long videos', i => i.kind === 'video' && +i.duration >= 120],
    ['loved', 'Most loved', i => typeof likesOf === 'function' && likesOf(i.id) >= 3],
    ['unsure', '1 star', i => +i.quality === 1]
  ];
  let smart = '', decoy = false;
  const filterNow = () => {
    const s = SMART.find(x => x[0] === smart);
    window.vaultLibraryFilter = decoy ? () => false : s ? s[2] : null;
  };
  const rerender = () => { if (typeof renderLibrary === 'function') renderLibrary(); };
  const FIELDS = ['fCat', 'fKind', 'fNsfw', 'fQ', 'fSort'];
  const searchInput = () => lib && lib.querySelector('.searchbar input');
  const chips = make('div', 'ph-chips');
  chips.setAttribute('role', 'toolbar');
  function drawChips() {
    chips.replaceChildren();
    const chip = (text, on, onTap, cls) => {
      const b = make('button', 'ph-chip' + (on ? ' on' : '') + (cls ? ' ' + cls : ''), text);
      b.type = 'button';
      b.onclick = onTap;
      chips.append(b);
      return b;
    };
    for (const [id, text] of SMART) {
      const c = chip(text, smart === id, () => { smart = smart === id ? '' : id; filterNow(); rerender(); drawChips(); buzz('select'); scrollTo({ top: 0, behavior: 'smooth' }); }, 'smart');
      if (id === 'loved' || id === 'unsure') c.dataset.fun = '1'; // (hearts and stars: fun features)
    }
    if (opt.presets.length) chips.append(make('i', 'ph-chips-sep'));
    opt.presets.forEach((p, k) => {
      const b = chip(p.name, false, () => {
        for (const id of FIELDS) { const f = $(id); if (f && p.f[id] !== undefined) f.value = p.f[id]; }
        const si = searchInput();
        if (si) { si.value = p.f.q || ''; si.dispatchEvent(new Event('input', { bubbles: true })); }
        smart = p.f.smart || ''; filterNow();
        const k0 = $('fKind'); if (k0) k0.dispatchEvent(new Event('change', { bubbles: true }));
        rerender(); drawChips(); buzz('select');
      }, 'preset');
      // a long press removes it
      let t = null;
      b.addEventListener('touchstart', () => { t = setTimeout(() => { t = null; P.setLastTap(b); if (confirm(`Remove the saved filter "${p.name}"?`)) { opt.presets.splice(k, 1); saveOpt(); drawChips(); } }, 600); }, { passive: true });
      for (const ev of ['touchend', 'touchmove', 'touchcancel']) b.addEventListener(ev, () => clearTimeout(t), { passive: true });
    });
    const save = chip('+ Save filters', false, () => {
      const name = prompt('Name these filters', '');
      if (!name || !name.trim()) return;
      const f = { smart };
      for (const id of FIELDS) { const el = $(id); if (el) f[id] = el.value; }
      const si = searchInput(); if (si) f.q = si.value;
      opt.presets.push({ name: name.trim().slice(0, 30), f });
      saveOpt(); drawChips(); toast('Saved', 'ok');
    }, 'add');
    save.title = 'Keep the search, filters and order you have now as a chip';
  }
  if (grid) { grid.before(chips); drawChips(); }

  // =====================================================================================
  // 4. privacy
  // =====================================================================================
  // Face ID / fingerprint: offered on the PIN lock, and asked for at once when it appears
  let bio = { available: false, kind: '' };
  const bioName = () => (bio.kind === 'face' ? 'Face ID' : android ? 'fingerprint' : 'Touch ID');
  native('bioStatus').then(s => { if (s) bio = s; drawLockButton(); }).catch(() => {});
  const lockCard = document.querySelector('.pin-lock .pin-card');
  const bioBtn = make('button', 'ph-biobtn hidden');
  bioBtn.type = 'button';
  if (lockCard) lockCard.append(bioBtn);
  function drawLockButton() { bioBtn.textContent = 'Use ' + bioName(); bioBtn.classList.toggle('hidden', !(opt.bio && bio.available)); }
  let asking = false;
  async function askBio() {
    if (asking || !opt.bio || !bio.available || !window.vaultPin || !window.vaultPin.isLocked()) return;
    asking = true;
    try { await native('bioAuth', 'Open Lewd Vault'); window.vaultPin.unlock(); buzz('success'); } catch (e) {} finally { asking = false; }
  }
  bioBtn.onclick = askBio;
  new MutationObserver(() => { if (html.classList.contains('pin-locked')) { drawLockButton(); setTimeout(askBio, 350); } })
    .observe(html, { attributes: true, attributeFilter: ['class'] });

  // the decoy PIN: it opens the app too, onto an empty library (and the Browser, You and Coach out of
  // sight) until the real PIN is typed or the app starts again
  window.vaultPinAlt = async v => {
    if (!opt.decoy || !opt.decoy.hash || !window.vaultPin) return false;
    if (await window.vaultPin.hash(v, opt.decoy.salt) !== opt.decoy.hash) return false;
    decoy = true; html.classList.add('ph-decoy'); filterNow(); rerender();
    return true;
  };
  addEventListener('vault-pin', e => {
    const d = e.detail || {};
    if (d.ok && decoy && !d.by) { decoy = false; html.classList.remove('ph-decoy'); filterNow(); rerender(); }
    if (!d.ok && d.tries >= 3 && opt.breakin) intruder(d.tries);
  });

  // the break-in photo: after the third wrong PIN in a row, one picture from the front camera
  let caught = 0;
  async function intruder(tries) {
    if (Date.now() - caught < 60e3 || !navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) return;
    caught = Date.now();
    let stream = null;
    try {
      stream = await navigator.mediaDevices.getUserMedia({ video: { facingMode: 'user', width: { ideal: 640 } }, audio: false });
      const v = make('video'); v.muted = true; v.playsInline = true; v.setAttribute('playsinline', ''); v.srcObject = stream;
      await v.play();
      await new Promise(r => setTimeout(r, 700));
      const c = make('canvas'), k = 480 / Math.max(v.videoWidth || 480, 1);
      c.width = Math.round((v.videoWidth || 480) * k); c.height = Math.round((v.videoHeight || 360) * k);
      c.getContext('2d').drawImage(v, 0, 0, c.width, c.height);
      const shot = c.toDataURL('image/jpeg', .72);
      let list = [];
      try { list = (await api('kvGet', 'phone-intruders')) || []; } catch (e) {}
      list.unshift({ at: new Date().toISOString(), tries, shot });
      await api('kvSet', 'phone-intruders', list.slice(0, 12));
    } catch (e) { /* no camera, or no permission: nothing */ } finally { if (stream) stream.getTracks().forEach(t => t.stop()); }
  }

  // shake to hide: two hard shakes close together do what Hide now does
  let shakes = [], lastA = null;
  const LEVELS = { 1: 16, 2: 23, 3: 32 };
  addEventListener('devicemotion', e => {
    if (!opt.shake) return;
    const a = e.accelerationIncludingGravity;
    if (!a || a.x == null) return;
    if (lastA) {
      const d = Math.abs(a.x - lastA.x) + Math.abs(a.y - lastA.y) + Math.abs(a.z - lastA.z);
      if (d > (LEVELS[opt.shakeLevel] || 23)) {
        const now = Date.now();
        shakes = shakes.filter(t => now - t < 700); shakes.push(now);
        if (shakes.length >= 2) { shakes = []; P.hideNow(); }
      }
    }
    lastA = { x: a.x, y: a.y, z: a.z };
  });
  const askMotion = async () => {
    if (typeof DeviceMotionEvent !== 'undefined' && typeof DeviceMotionEvent.requestPermission === 'function') {
      try { return (await DeviceMotionEvent.requestPermission()) === 'granted'; } catch (e) { return false; }
    }
    return true;
  };

  // hiding from screen recordings and screenshots: Android blocks them outright; iOS can't block a
  // screenshot, but the app goes blank while the screen is being recorded or mirrored
  let capTimer = null;
  function applyGuard() {
    native('secure', !!opt.guard).catch(() => {});
    clearInterval(capTimer);
    if (opt.guard && !android) capTimer = setInterval(() => {
      if (document.hidden) return;
      native('captured').then(on => { if (html.classList.contains('ph-captured') !== !!on) html.classList.toggle('ph-captured', !!on); }).catch(() => {});
    }, 1500);
    else html.classList.remove('ph-captured');
  }

  // =====================================================================================
  // 5. shared pictures, the app icon, Photos, text size, density, low data
  // =====================================================================================
  // pictures and videos shared to the app wait in its inbox; they go through Add like picked files
  let taking = false;
  async function takeShared() {
    if (taking || document.hidden || html.classList.contains('pin-locked') || decoy) return;
    const tagger = $('tagger');
    if (tagger && !tagger.classList.contains('hidden')) return; // (busy tagging: next time)
    let list = [];
    try { list = (await native('inbox')) || []; } catch (e) { return; }
    if (!list.length) return;
    taking = true;
    try {
      const files = [];
      for (const f of list) {
        try {
          const r = await fetch(vaultOrigin() + '/inbox/' + encodeURIComponent(f.name));
          if (!r.ok) continue;
          const blob = await r.blob();
          files.push(new File([blob], f.name.replace(/^\d+-(\d+-)?/, ''), { type: blob.type }));
        } catch (e) {}
      }
      if (!files.length) return;
      const input = $('fileInput'), addTab = document.querySelector('header nav button[data-tab="add"]');
      if (!input || typeof DataTransfer === 'undefined') return;
      if (addTab) addTab.click();
      const dt = new DataTransfer();
      files.forEach(f => dt.items.add(f));
      input.files = dt.files;
      fromShare = true;
      input.dispatchEvent(new Event('change', { bubbles: true }));
      for (const f of list) native('inboxDone', f.name).catch(() => {});
      toast(files.length === 1 ? 'A shared picture to add' : `${files.length} shared pictures to add`, 'ok');
    } finally { taking = false; }
  }
  document.addEventListener('visibilitychange', () => { if (!document.hidden) setTimeout(takeShared, 600); });
  addEventListener('vault-pin', e => { if (e.detail && e.detail.ok) setTimeout(takeShared, 600); });
  if (typeof setupReady !== 'undefined') setupReady.then(() => setTimeout(takeShared, 1500));

  // after adding from Photos: a reminder to delete them there (iOS lets no app delete from Photos
  // without full access to it, so the app only opens Photos for you)
  let fromShare = false, picked = 0;
  const fileInput = $('fileInput');
  if (fileInput) fileInput.addEventListener('change', () => { if (fromShare) { fromShare = false; return; } picked = fileInput.files ? fileInput.files.length : 0; });
  const taggerEl = $('tagger');
  if (taggerEl) new MutationObserver(() => {
    if (!taggerEl.classList.contains('hidden') || !picked || !opt.photosReminder) return;
    const n = picked; picked = 0;
    const s = sheet('ios-ask');
    s.body.append(make('p', 'ios-ask-text', `Delete the ${n === 1 ? 'picture' : n + ' pictures'} you just added from ${android ? 'your gallery' : 'Photos'}?`),
      make('p', 'ph-small', android ? 'They\'re safe in your vault now. Your gallery still has them: delete them there so they only live here.' : 'They\'re safe in your vault now. Photos still has them (iOS lets no app delete from it): they\'re at the top of Recents.'));
    const acts = make('div', 'ios-ask-acts');
    const no = make('button', 'btn', 'Not now'), yes = make('button', 'btn primary', android ? 'Open the gallery' : 'Open Photos');
    no.type = yes.type = 'button';
    no.onclick = () => s.close();
    yes.onclick = () => { s.close(); native('openPhotos').catch(() => toast('Open it from your home screen', 'ok')); };
    acts.append(no, yes); s.body.append(acts);
  }).observe(taggerEl, { attributes: true, attributeFilter: ['class'] });

  // text size (following the phone's own setting by default) and density
  function applyText() {
    let k = { s: .9, m: 1, l: 1.12, xl: 1.25 }[opt.textSize];
    if (!k) {
      // iOS's text size (Dynamic Type): the system body font is 17 px at the standard size
      const probe = make('span'); probe.style.font = '-apple-system-body'; probe.style.position = 'absolute'; probe.style.visibility = 'hidden';
      document.body.append(probe);
      const px = parseFloat(getComputedStyle(probe).fontSize) || 17;
      probe.remove();
      k = Math.max(.85, Math.min(1.45, px / 17));
    }
    html.style.setProperty('--ph-text', k.toFixed(3));
    html.classList.toggle('ph-compact', opt.density === 'compact');
  }
  // low-data mode: nothing played or fetched unless you open it (Automatic: on mobile data, Android)
  function applyData() {
    const c = navigator.connection;
    const on = opt.lowData === 'on' || (opt.lowData === 'auto' && !!c && (c.saveData || c.type === 'cellular'));
    window.vaultLowData = on;
    html.classList.toggle('ph-lowdata', on);
  }
  if (navigator.connection && navigator.connection.addEventListener) navigator.connection.addEventListener('change', applyData);

  // =====================================================================================
  // 1. Settings → Phone
  // =====================================================================================
  const ICONS = [['default', 'Lewd Vault'], ['violet', 'Violet'], ['mint', 'Mint'], ['midnight', 'Midnight'], ['mono', 'Mono'],
    ['calculator', 'Calculator'], ['notes', 'Notes'], ['weather', 'Weather'], ['clock', 'Clock'], ['files', 'Files'], ['compass', 'Compass']];
  const GESTURE = [['zoom', 'Zoom'], ['like', 'Appreciate'], ['info', 'Details'], ['level', 'Level dial'], ['hide', 'Hide now'], ['close', 'Close'], ['none', 'Nothing']];
  function openSettings() {
    const el = make('div', 'ph-settings');
    el.setAttribute('role', 'dialog'); el.setAttribute('aria-label', 'Phone settings');
    const head = make('div', 'ph-set-head glass');
    const done = make('button', 'ios-pill', 'Done'); done.type = 'button';
    head.append(make('h2', '', 'Phone'), done);
    const body = make('div', 'ph-set-body');
    el.append(head, body);
    const section = (title, help) => { const s = make('section', 'ph-set-sec'); s.append(make('h3', '', title)); if (help) s.append(make('p', 'ph-small', help)); body.append(s); return s; };
    const seg = (sec, title, choices, key, after) => {
      const g = make('div', 'ph-opt');
      if (title) g.append(make('span', 'ios-peek-label', title));
      const row = make('div', 'ph-optseg');
      for (const [v, text] of choices) {
        const b = make('button', 'ios-chip' + (opt[key] === v ? ' on' : ''), text);
        b.type = 'button';
        b.onclick = () => { opt[key] = v; row.querySelectorAll('button').forEach(x => x.classList.toggle('on', x === b)); saveOpt(); buzz('select'); if (after) after(v); };
        row.append(b);
      }
      g.append(row); sec.append(g);
      return g;
    };
    const toggles = sec => { const l = make('div', 'ios-list ph-toggles'); sec.append(l); return l; };
    const toggle = (list, key, text, help, before) => {
      const l = make('label', 'ios-row plain ph-toggle');
      const w = make('span', 'ph-toggle-words'); w.append(make('b', '', text)); if (help) w.append(make('small', '', help));
      const cb = make('input'); cb.type = 'checkbox'; cb.checked = !!opt[key];
      cb.onchange = async () => {
        if (cb.checked && before && !(await before())) { cb.checked = false; return; }
        opt[key] = cb.checked; saveOpt(); buzz('select');
      };
      l.append(w, cb); list.append(l);
      return l;
    };
    const rowBtn = (list, text, sub, onTap, cls) => {
      const r = make('button', 'ios-row plain ph-rowbtn' + (cls ? ' ' + cls : ''));
      r.type = 'button';
      const w = make('span', 'ph-toggle-words'); w.append(make('b', '', text)); if (sub) w.append(make('small', '', sub));
      r.append(w, make('span', 'ph-chev', '›'));
      r.onclick = onTap; list.append(r);
      return r;
    };

    // --- the look ---
    const look = section('Theme', 'Colours, and a little something drifting in the background.');
    const cards = make('div', 'ph-themes');
    THEMES.forEach(([id, name, kind], k) => {
      const c = make('button', 'ph-theme' + (opt.theme === id ? ' on' : ''));
      c.type = 'button'; c.dataset.t = id; c.dataset.fx = kind; c.style.setProperty('--n', k);
      const sw = make('span', 'ph-theme-sw');
      for (let i = 0; i < 5; i++) sw.append(make('i'));
      c.append(sw, make('b', '', name));
      c.onclick = e => {
        const go = () => { opt.theme = id; saveOpt(); cards.querySelectorAll('.ph-theme').forEach(x => x.classList.toggle('on', x === c)); };
        buzz('light');
        // the new colours spread out from where you tapped
        if (document.startViewTransition && !reduced()) {
          html.style.setProperty('--vt-x', e.clientX + 'px'); html.style.setProperty('--vt-y', e.clientY + 'px');
          document.startViewTransition(go);
        } else go();
      };
      cards.append(c);
    });
    look.append(cards);
    const acc = make('div', 'ph-accents');
    const none = make('button', 'ph-accent auto' + (!opt.accent ? ' on' : ''), 'Auto'); none.type = 'button';
    none.onclick = () => { opt.accent = ''; saveOpt(); acc.querySelectorAll('.ph-accent').forEach(x => x.classList.toggle('on', x === none)); };
    acc.append(none);
    for (const c of ACCENTS) {
      const b = make('button', 'ph-accent' + (opt.accent === c ? ' on' : '')); b.type = 'button'; b.style.setProperty('--c', c); b.setAttribute('aria-label', 'Accent ' + c);
      b.onclick = () => { opt.accent = c; saveOpt(); acc.querySelectorAll('.ph-accent').forEach(x => x.classList.toggle('on', x === b)); buzz('select'); };
      acc.append(b);
    }
    const own = make('label', 'ph-accent own'); own.setAttribute('aria-label', 'Your own colour');
    const pick = make('input'); pick.type = 'color'; pick.value = /^#/.test(opt.accent) ? opt.accent : '#ff6f96';
    pick.oninput = () => { opt.accent = pick.value; saveOpt(); acc.querySelectorAll('.ph-accent').forEach(x => x.classList.toggle('on', x === own)); };
    own.append(pick); acc.append(own);
    const accWrap = make('div', 'ph-opt'); accWrap.append(make('span', 'ios-peek-label', 'Accent'), acc); look.append(accWrap);
    const lt = toggles(look);
    toggle(lt, 'fx', 'Animated backgrounds', 'Off saves a little battery.');
    toggle(lt, 'seasonal', 'Seasonal touches', 'Snow in winter, hearts in February, leaves in autumn.');
    toggle(lt, 'milestones', 'Celebrate milestones', 'Confetti when the vault reaches 100, 1,000… and when a picture gets 100 appreciations.');
    toggle(lt, 'haptics', 'Haptics', 'Little taps you feel: the dial, sorting, saving.');
    seg(look, 'Text size', [['auto', 'Auto'], ['s', 'S'], ['m', 'M'], ['l', 'L'], ['xl', 'XL']], 'textSize');
    seg(look, 'Density', [['normal', 'Comfortable'], ['compact', 'Compact']], 'density');

    // --- the tab bar ---
    const tabs = section('Tab bar', 'Drag to choose: the first five sit in the tab bar; the rest are in More, behind your picture at the top of the Library.');
    const tl = make('div', 'ios-list ph-tabsort');
    const all = P.sections();
    const order = [...(opt.tabs || []).filter(t => all.some(s => s.tab === t)), ...all.map(s => s.tab).filter(t => !(opt.tabs || []).includes(t))];
    const drawOrder = () => {
      tl.replaceChildren();
      order.forEach((t, k) => {
        const s = all.find(x => x.tab === t);
        if (k === 5) tl.append(make('div', 'ph-tabsort-more', 'In More'));
        const r = make('div', 'ios-row plain ph-tabrow');
        r.dataset.tab = t;
        const ic = make('span', 'ph-tabrow-ico'); ic.style.setProperty('--icon', s.icon);
        const grip = make('span', 'ph-grip'); grip.style.setProperty('--icon', icon('grip'));
        r.append(ic, make('span', 'ph-tabrow-name', s.label), grip);
        tl.append(r);
        // drag by the grip
        grip.addEventListener('pointerdown', e => {
          e.preventDefault();
          grip.setPointerCapture(e.pointerId);
          r.classList.add('drag');
          const y0 = e.clientY, h = r.offsetHeight;
          let at = k;
          const move = ev => {
            const d = ev.clientY - y0;
            r.style.transform = `translateY(${d}px)`;
            const to = Math.max(0, Math.min(order.length - 1, k + Math.round(d / h)));
            if (to !== at) { at = to; buzz('select'); }
          };
          const up = () => {
            grip.removeEventListener('pointermove', move); grip.removeEventListener('pointerup', up); grip.removeEventListener('pointercancel', up);
            order.splice(at, 0, order.splice(k, 1)[0]);
            opt.tabs = order.slice(0, 5); saveOpt(); drawOrder();
          };
          grip.addEventListener('pointermove', move); grip.addEventListener('pointerup', up); grip.addEventListener('pointercancel', up);
        });
      });
    };
    drawOrder();
    tabs.append(tl);

    // --- gestures ---
    const ges = section('Gestures in the viewer');
    const sel = (title, key) => {
      const g = make('label', 'ph-select');
      g.append(make('span', '', title));
      const s = make('select');
      for (const [v, t] of GESTURE) if (!(v === 'zoom' && key !== 'gDouble')) s.add(new Option(t, v));
      s.value = opt[key] || P.OPT[key];
      s.onchange = () => { opt[key] = s.value; saveOpt(); };
      g.append(s); ges.append(g);
    };
    sel('Double tap', 'gDouble'); sel('Two-finger tap', 'gTwo'); sel('Long press', 'gHold');
    ges.append(make('p', 'ph-small', 'Pinch always zooms. On videos: drag sideways in the lower half to scrub, double-tap a side to skip 10 seconds, hold for 2×.'));
    seg(ges, 'Library grid', [[2, '2 across'], [3, '3'], [5, '5'], [7, '7']], 'gridCols', applyCols).querySelector('.ios-peek-label').textContent = 'Library grid (or pinch it)';

    // --- privacy ---
    const priv = section('Privacy');
    const pl = toggles(priv);
    if (bio.available) toggle(pl, 'bio', `Unlock with ${bioName()}`, 'Instead of typing your PIN (the PIN still works). Needs a PIN.', async () => {
      if (!(window.vaultPin && window.vaultPin.isSet())) { toast('Set a PIN first (Settings → Privacy)', 'err'); return false; }
      try { await native('bioAuth', `Use ${bioName()} for Lewd Vault`); return true; } catch (e) { return false; }
    });
    else pl.append(make('p', 'ph-small ph-pad', android ? 'Fingerprint unlock: this phone has none set up.' : 'Face ID / Touch ID: this phone has none set up.'));
    rowBtn(pl, opt.decoy ? 'Decoy PIN: on' : 'Decoy PIN', 'A second PIN that opens an empty vault. Type the real one later to see everything.', () => decoySheet(), 'decoy');
    toggle(pl, 'shake', 'Shake to hide', 'Two hard shakes do what Hide now does.', askMotion);
    toggle(pl, 'guard', android ? 'Block screenshots and recording' : 'Hide during screen recording', android ? 'Screenshots and screen recordings of the app come out black (the app switcher too).' : 'The app goes blank while the screen is recorded or mirrored. (iOS lets no app block a screenshot.)');
    toggle(pl, 'breakin', 'Break-in photo', 'After three wrong PINs, the front camera quietly takes one picture.', async () => {
      try { const s = await navigator.mediaDevices.getUserMedia({ video: { facingMode: 'user' } }); s.getTracks().forEach(t => t.stop()); return true; } catch (e) { toast('The camera isn\'t allowed for Lewd Vault', 'err'); return false; }
    });
    rowBtn(pl, 'Break-in photos', 'See who tried', () => intrudersSheet());
    seg(priv, 'Lock again after leaving the app (with a PIN)', [[0, 'At once'], [1, '1 min'], [5, '5 min'], [15, '15 min'], [-1, 'Never']], 'autolock');
    seg(priv, 'Shake strength', [[1, 'Gentle'], [2, 'Normal'], [3, 'Hard']], 'shakeLevel');

    // --- the app icon ---
    const ico = section('App icon', android ? 'The icon and the name on your home screen. It changes when you leave the app.' : 'The icon on your home screen. (iOS keeps the name: to rename it, make a Shortcut that opens Lewd Vault and give it any name and picture.)');
    const ig = make('div', 'ph-icons');
    ICONS.forEach(([id, name], k) => {
      const b = make('button', 'ph-icon' + (opt.icon === id ? ' on' : ''));
      b.type = 'button'; b.style.setProperty('--n', k);
      const im = make('img'); im.alt = ''; im.src = '/icon-presets/' + id + '.png';
      b.append(im, make('span', '', name));
      b.onclick = async () => {
        try { await native('icon', id); } catch (e) { toast('Only in the app on a phone', 'err'); return; }
        opt.icon = id; saveOpt(); buzz('success');
        ig.querySelectorAll('.ph-icon').forEach(x => x.classList.toggle('on', x === b));
        if (android) toast('The new icon appears when you leave the app', 'ok');
      };
      ig.append(b);
    });
    ico.append(ig);

    // --- adding, data ---
    const more = section('Adding and data');
    const ml = toggles(more);
    toggle(ml, 'photosReminder', android ? 'Remind me to delete from the gallery' : 'Remind me to delete from Photos', 'After adding pictures you picked, so they only live in the vault.');
    seg(more, 'Low-data mode', android ? [['off', 'Off'], ['on', 'On'], ['auto', 'On mobile data']] : [['off', 'Off'], ['on', 'On']], 'lowData');
    more.append(make('p', 'ph-small', 'Low data: the Browser only fetches what you open, nothing plays by itself, and Stories show videos as pictures.'));
    rowBtn(toggles(more), 'Customise the Browser', 'Address bar, layout, filters, swipes', () => { el._close(); const m = document.querySelector('.ph-sb.more'); const tab = document.querySelector('header nav button[data-tab="discover"]'); if (tab) tab.click(); setTimeout(() => { if (m) m.click(); setTimeout(() => { const r = [...document.querySelectorAll('.ios-row')].find(x => /Customise/.test(x.textContent)); if (r) r.click(); }, 350); }, 400); });

    let closed = false;
    el._close = () => { if (closed) return; closed = true; unlayer(el); el.classList.remove('in'); setTimeout(() => el.remove(), 320); };
    done.onclick = el._close;
    layer(el, el._close);
    requestAnimationFrame(() => el.classList.add('in'));
  }

  function decoySheet() {
    const s = sheet('ios-ask');
    s.body.append(make('h2', 'ios-sheet-title', 'Decoy PIN'), make('p', 'ph-small', 'Typed on the lock screen instead of your PIN, it opens Lewd Vault onto an empty library, with the Browser, You and Coach out of sight. Type your real PIN at the next lock to see everything again.'));
    const a = make('input', 'ios-ask-input'); Object.assign(a, { type: 'password', inputMode: 'numeric', placeholder: 'Decoy PIN (4–12 digits)', maxLength: 12 });
    const msg = make('p', 'ph-small');
    const acts = make('div', 'ios-ask-acts');
    if (opt.decoy) { const off = make('button', 'btn danger', 'Turn off'); off.type = 'button'; off.onclick = () => { opt.decoy = null; saveOpt(); s.close(); toast('Decoy PIN off', 'ok'); }; acts.append(off); }
    const save = make('button', 'btn primary', 'Save'); save.type = 'button';
    save.onclick = async () => {
      if (!/^\d{4,12}$/.test(a.value)) { msg.textContent = 'Use 4 to 12 digits.'; return; }
      if (!window.vaultPin || !window.vaultPin.isSet()) { msg.textContent = 'Set your real PIN first (Settings → Privacy).'; return; }
      if (await window.vaultPin.hash(a.value, settings.pin.salt) === settings.pin.hash) { msg.textContent = 'That\'s your real PIN: choose another.'; return; }
      const salt = [...crypto.getRandomValues(new Uint8Array(16))].map(x => x.toString(16).padStart(2, '0')).join('');
      opt.decoy = { salt, hash: await window.vaultPin.hash(a.value, salt) };
      saveOpt(); s.close(); toast('Decoy PIN set', 'ok');
    };
    acts.append(save);
    s.body.append(a, msg, acts);
    setTimeout(() => a.focus(), 280);
  }
  async function intrudersSheet() {
    let list = [];
    try { list = (await api('kvGet', 'phone-intruders')) || []; } catch (e) {}
    const s = sheet('ph-intruders');
    s.body.append(make('h2', 'ios-sheet-title', 'Break-in photos'));
    if (!list.length) { s.body.append(make('p', 'ph-small', 'Nobody so far.')); return; }
    const g = make('div', 'ph-intruder-grid');
    for (const x of list) {
      const f = make('figure', 'ph-intruder');
      const im = make('img'); im.alt = ''; im.src = x.shot;
      f.append(im, make('figcaption', '', `${new Date(x.at).toLocaleString()} · ${x.tries} wrong PINs`));
      g.append(f);
    }
    const clear = make('button', 'btn danger', 'Delete them all'); clear.type = 'button';
    clear.onclick = async () => { await api('kvSet', 'phone-intruders', []); s.close(); toast('Deleted', 'ok'); };
    s.body.append(g, clear);
  }

  // the way in: a tile in More, and a card at the top of Settings
  const moreObserver = new MutationObserver(() => {
    const tiles = document.querySelector('.ios-more .ios-tiles');
    if (!tiles || tiles.querySelector('.ph-settile')) return;
    // (the decoy shows an ordinary, empty vault: no Browser, no You, no Coach, no Sort)
    if (decoy) tiles.querySelectorAll('.ios-tile').forEach(t => { if (/^(Browser|You|Coach|Sort|Stats|Videos|Audio)/.test(t.textContent.trim())) t.remove(); });
    const t = make('button', 'ios-tile ph-settile');
    t.type = 'button'; t.style.setProperty('--icon', icon('phone'));
    t.append(make('span', '', 'Phone settings'));
    t.onclick = () => { const back = t.closest('.ios-back'); if (back) back.click(); setTimeout(openSettings, 150); };
    tiles.insertBefore(t, tiles.querySelector('.wide') || null);
  });
  moreObserver.observe(document.body, { childList: true });
  const settingsTab = $('tab-settings');
  if (settingsTab) {
    const card = make('button', 'ph-setcard');
    card.type = 'button';
    card.append(make('b', '', 'Phone settings'), make('span', '', 'Themes, the tab bar, gestures, Face ID, the app icon…'));
    card.onclick = openSettings;
    settingsTab.prepend(card);
  }

  // everything that follows the settings, now and whenever they change
  // (each part only when what it shows changed: saving a setting must not redraw what's under a finger)
  let was = '';
  function applyAll() {
    fill();
    const key = k => JSON.stringify(k.map(x => opt[x]));
    const now = { theme: key(['theme', 'accent', 'fx', 'seasonal']), cols: key(['gridCols']), text: key(['textSize', 'density']), data: key(['lowData']), guard: key(['guard']), chips: key(['presets']), bio: key(['bio']) };
    const prev = was ? JSON.parse(was) : {};
    if (now.theme !== prev.theme) applyTheme();
    if (now.cols !== prev.cols) applyCols();
    if (now.text !== prev.text) applyText();
    if (now.data !== prev.data) applyData();
    if (now.guard !== prev.guard) applyGuard();
    if (now.chips !== prev.chips) drawChips();
    if (now.bio !== prev.bio) drawLockButton();
    filterNow();
    was = JSON.stringify(now);
  }
  addEventListener('phone-opt', applyAll);
  applyAll();
  new MutationObserver(() => checkSize()).observe($('count') || document.body, { childList: true, characterData: true, subtree: true });

  // =====================================================================================
  // 6. the welcome, on a phone: an opening worth remembering
  // =====================================================================================
  // The first time the app opens: the heart fills up, the name rises letter by letter, hearts drift
  // up, and "Get started" swipes it all away into the welcome's own steps (the fun features, then
  // where the vault lives), each sliding in, with dots for where you are. At the end: confetti.
  const HEART = 'M50 90C22 68 3 50 3 28 3 13 14 3 28 3c9 0 17 5 22 13C55 8 63 3 72 3c14 0 25 10 25 25 0 22-19 40-47 62z';
  const welcomeEl = $('welcome');
  function hello() {
    if (document.querySelector('.ph-hello')) return;
    const el = make('div', 'ph-hello');
    el.setAttribute('role', 'dialog'); el.setAttribute('aria-label', 'Welcome');
    el.innerHTML = `
      <div class="ph-hello-sky" aria-hidden="true"><i></i><i></i><i></i></div>
      <canvas class="ph-hello-hearts" aria-hidden="true"></canvas>
      <div class="ph-hello-mid">
        <svg class="ph-hello-heart" viewBox="-6 -8 112 110" aria-hidden="true">
          <defs>
            <clipPath id="phh-c"><path d="${HEART}"/></clipPath>
            <linearGradient id="phh-f" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffc2d6"/><stop offset=".5" stop-color="var(--accent, #ff6f96)"/><stop offset="1" stop-color="#7a2a48"/></linearGradient>
          </defs>
          <path d="${HEART}" fill="rgba(255,255,255,.08)" stroke="rgba(255,255,255,.75)" stroke-width="3.5" stroke-linejoin="round"/>
          <g clip-path="url(#phh-c)"><g class="ph-hello-fill">
            <path class="ph-hello-wave" d="M-100 20Q-87.5 12 -75 20T-50 20T-25 20T0 20T25 20T50 20T75 20T100 20T125 20T150 20T175 20T200 20V140H-100Z" fill="url(#phh-f)"/>
            <path class="ph-hello-wave b" d="M-100 24Q-87.5 18 -75 24T-50 24T-25 24T0 24T25 24T50 24T75 24T100 24T125 24T150 24T175 24T200 24V140H-100Z" fill="rgba(255,255,255,.25)"/>
          </g></g>
          <ellipse cx="30" cy="24" rx="14" ry="9" fill="rgba(255,255,255,.35)"/>
        </svg>
        <h1 class="ph-hello-name" aria-label="Lewd Vault">${[...'Lewd Vault'].map((ch, k) => `<span style="--k:${k}">${ch === ' ' ? '&nbsp;' : ch}</span>`).join('')}</h1>
        <p class="ph-hello-line">Your private collection. Only on this ${android ? 'phone' : 'iPhone'}.</p>
        <div class="ph-hello-pills"><span>Private</span><span>Sorted</span><span>Yours</span></div>
      </div>
      <div class="ph-hello-foot">
        <button class="ph-hello-go" type="button"><span>Get started</span></button>
        <p class="ph-hello-swipe">or swipe up</p>
      </div>`;
    document.body.append(el);
    // hearts drifting up behind it all
    const cv = el.querySelector('canvas'), cx = cv.getContext('2d');
    const dpr = Math.min(2, devicePixelRatio || 1);
    cv.width = innerWidth * dpr; cv.height = innerHeight * dpr; cx.scale(dpr, dpr);
    const hearts = Array.from({ length: 18 }, () => ({ x: Math.random() * innerWidth, y: innerHeight + Math.random() * innerHeight, s: .5 + Math.random() * 1.1, v: .4 + Math.random() * .9, t: Math.random() * 6 }));
    let raf = 0;
    const draw = () => {
      cx.clearRect(0, 0, innerWidth, innerHeight);
      for (const h of hearts) {
        h.y -= h.v; h.t += .02;
        if (h.y < -30) { h.y = innerHeight + 20; h.x = Math.random() * innerWidth; }
        cx.save(); cx.translate(h.x + Math.sin(h.t) * 14, h.y); cx.scale(h.s, h.s); cx.globalAlpha = .18 + .2 * Math.abs(Math.sin(h.t));
        cx.fillStyle = '#ff8fb3'; cx.beginPath(); cx.moveTo(0, 6); cx.bezierCurveTo(-11, -2, -6, -12, 0, -6); cx.bezierCurveTo(6, -12, 11, -2, 0, 6); cx.fill(); cx.restore();
      }
      raf = requestAnimationFrame(draw);
    };
    if (!reduced()) raf = requestAnimationFrame(draw);
    requestAnimationFrame(() => el.classList.add('in'));
    const go = () => {
      if (el.classList.contains('out')) return;
      buzz('medium');
      el.classList.add('out');
      welcomeEl.classList.add('ph-steps-in');
      setTimeout(() => { cancelAnimationFrame(raf); el.remove(); }, 750);
    };
    el.querySelector('.ph-hello-go').onclick = go;
    let y0 = null;
    el.addEventListener('touchstart', e => { y0 = e.touches[0].clientY; }, { passive: true });
    el.addEventListener('touchmove', e => { if (y0 != null && y0 - e.touches[0].clientY > 0) el.style.setProperty('--lift', Math.min(160, y0 - e.touches[0].clientY) + 'px'); }, { passive: true });
    el.addEventListener('touchend', e => { const d = y0 == null ? 0 : y0 - e.changedTouches[0].clientY; y0 = null; el.style.removeProperty('--lift'); if (d > 90) go(); }, { passive: true });
  }
  if (welcomeEl) {
    // step dots and slide-ins for the welcome's own steps
    const dots = make('div', 'ph-wl-dots');
    dots.setAttribute('aria-hidden', 'true');
    for (let k = 0; k < 3; k++) dots.append(make('i'));
    const card = welcomeEl.querySelector('.wl-card');
    if (card) card.prepend(dots);
    const markDot = () => {
      const fun = welcomeEl.classList.contains('on-fun');
      const at = document.querySelector('.ph-hello') ? 0 : fun ? 1 : 2;
      [...dots.children].forEach((d, k) => d.classList.toggle('on', k === at));
    };
    let wasFirst = false, helloShown = false;
    const watch = () => {
      const open = !welcomeEl.classList.contains('hidden');
      const first = welcomeEl.classList.contains('first');
      if (open && first) { wasFirst = true; if (!helloShown) { helloShown = true; hello(); } }
      // the first welcome done: you're all set
      if (!open && wasFirst) { wasFirst = false; setTimeout(() => cheer('You\'re all set!', 'Add your first pictures in Add.'), 400); }
      markDot();
    };
    new MutationObserver(watch).observe(welcomeEl, { attributes: true, attributeFilter: ['class'] });
    watch(); // (it may have opened before this file loaded)
    // each step slides in as it appears
    // (only a step going from hidden to shown: its own animation class changing must not set it off again)
    new MutationObserver(list => {
      for (const m of list) {
        const t = m.target;
        if (!t.classList || !t.classList.contains('wl-step') || t.classList.contains('hidden') || !/\bhidden\b/.test(m.oldValue || '')) continue;
        t.classList.remove('ph-step-in'); void t.offsetWidth; t.classList.add('ph-step-in');
      }
      markDot();
    }).observe(welcomeEl, { subtree: true, attributes: true, attributeFilter: ['class'], attributeOldValue: true });
  }

  // ---------- the loading screen: its ring follows the bar (desktop.js fills the bar) ----------
  const spl = $('splash'), spBar = spl && spl.querySelector('.sp-bar-fill'), spStatus = spl && spl.querySelector('.sp-status');
  if (spBar) new MutationObserver(() => spl.style.setProperty('--p', String((parseFloat(spBar.style.width) || 0) / 100))).observe(spBar, { attributes: true, attributeFilter: ['style'] });
  // (each new line of the status fades in)
  if (spStatus) new MutationObserver(() => { spStatus.style.animation = 'none'; void spStatus.offsetWidth; spStatus.style.animation = ''; }).observe(spStatus, { childList: true });

  // ---------- the profile button: on the main screen (the Library's top row), for You or Settings ----------
  // (as on Instagram: your picture opens your corner of the app; Settings isn't in the bar)
  const meBtn = make('button', 'ph-me');
  meBtn.type = 'button';
  meBtn.setAttribute('aria-label', 'Your profile and settings');
  // it lives at the end of the Library's top row, beside Select and search (phone.js builds that row once)
  const dock = () => { const tools = document.querySelector('.ios-libtools'); if (tools && meBtn.parentElement !== tools) tools.append(meBtn); };
  dock();
  if (typeof setupReady !== 'undefined') setupReady.then(dock);
  const profileNow = () => (window.vaultProfile && window.vaultProfile.get && window.vaultProfile.get()) || {};
  const avatarOf = (pf, size) => {
    const lib = typeof library !== 'undefined' && Array.isArray(library) ? library : [];
    const it = pf.avatarId && lib.find(i => i.id === pf.avatarId);
    if (it && it.thumb) { const img = make('img'); img.alt = ''; img.src = `${vaultOrigin()}/t/${encodeURIComponent(it.thumb)}?w=${size}`; return img; }
    return make('span', '', ((pf.name || '').trim()[0] || '♥').toUpperCase());
  };
  const drawMe = () => { meBtn.replaceChildren(avatarOf(profileNow(), 96)); };
  if (window.vaultProfile && window.vaultProfile.ready) Promise.resolve(window.vaultProfile.ready).then(drawMe).catch(() => {});
  if (typeof setupReady !== 'undefined') setupReady.then(() => setTimeout(drawMe, 1500));
  drawMe();
  meBtn.onclick = () => {
    buzz('light');
    drawMe();
    // a narrow column under your picture: an icon and a word each
    const back = make('div', 'ph-me-back');
    const card = make('div', 'ph-me-card');
    const row = (cls, title, go) => {
      const b = make('button', 'ph-me-row ' + cls); b.type = 'button';
      b.append(make('i'), make('span', '', title));
      b.onclick = () => { close(); setTimeout(go, 120); };
      card.append(b);
    };
    row('you', 'Profile', () => window.vaultProfile && window.vaultProfile.open());
    row('gear', 'Settings', () => { const s = document.querySelector('header nav button[data-tab="settings"]'); if (s) s.click(); });
    row('more', 'More', () => P.openMore && P.openMore());
    back.append(card);
    document.body.append(back);
    const r = meBtn.getBoundingClientRect();
    card.style.top = (r.bottom + 8) + 'px';
    card.style.right = Math.max(8, innerWidth - r.right) + 'px';
    requestAnimationFrame(() => back.classList.add('in'));
    function close() { back.classList.remove('in'); setTimeout(() => back.remove(), 200); }
    back.onclick = e => { if (e.target === back) close(); };
  };

  window.vaultPhonePlus = { openSettings, cheer, confetti, hello };
})();
