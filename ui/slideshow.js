// Lewd Vault desktop: the slideshow (Phase 6 of PLAN.md), on top of the page's own.
//   - Motion: none by default (no more slow zoom), or slow pan, hypnotic bounce (slow or fast),
//     breathing, drifting, or the old zoom; on videos too
//   - full screen is the window's own, so it covers the whole monitor (the taskbar too)
//   - it pauses when you switch to another window, and carries on when you come back
//   - category chips at the top: switch to one category (or a few, Shift+click) or All, mid-session
//   - a succubus tail along the bottom, always there, its heart tip showing how far the session has come
//   - a pink ripple where you click
//   - the bar's buttons in the theme, with drawn icons
//   - under the plan: the slideshows you've had, with how long each lasted (kept with the vault)

(() => {
  'use strict';
  if (!window.vaultUI || !inTauri) return;
  const { make } = window.vaultUI;
  const html = document.documentElement, show = $('show');
  const fmtMin = s => s >= 3600 ? `${Math.floor(s / 3600)} h ${Math.round(s % 3600 / 60)} min` : s >= 60 ? `${Math.round(s / 60)} min` : `${Math.round(s)} s`;

  // ---------- Motion ----------
  const k = SHOW_OPTIONS.findIndex(s => s.key === 'kenBurns');
  const MOTIONS = [['none', 'None (still)'], ['pan', 'Slow pan'], ['bounce', 'Hypnotic bounce (slow)'], ['bouncefast', 'Hypnotic bounce (fast)'], ['breathe', 'Breathing'], ['drift', 'Drifting sideways'], ['zoom', 'Slow zoom']];
  SHOW_OPTIONS.splice(k, 1,
    { section: 'Look', key: 'motion', label: 'Motion', help: 'How each slide moves while it\'s on screen. None keeps it perfectly still.', type: 'select', def: 'none', options: MOTIONS });
  SHOW_OPTIONS.push({ section: 'Playback', key: 'pauseAway', label: 'Pause when you switch to another window', help: 'And carry on when you come back.', type: 'toggle', def: true });
  const saved = (() => { try { return JSON.parse(localStorage.getItem(SHOW_OPTS_KEY)) || {}; } catch (e) { return {}; } })();
  showOpts.motion = MOTIONS.some(m => m[0] === saved.motion) ? saved.motion : 'none';
  showOpts.pauseAway = saved.pauseAway !== false;
  slideMotion = (node, ms, o) => {
    const m = o.motion || 'none', r = (a, b) => (a + Math.random() * (b - a)).toFixed(2);
    node.style.transformOrigin = '50% 50%';
    switch (m) {
      case 'pan': { const x = r(-3, 3), y = r(-2, 2); return node.animate([{ transform: `scale(1.08) translate(${x}%, ${y}%)` }, { transform: `scale(1.08) translate(${-x}%, ${-y}%)` }], { duration: ms, easing: 'ease-in-out', fill: 'forwards' }); }
      case 'bounce': return node.animate([{ transform: 'scale(1)' }, { transform: 'scale(1.035)' }, { transform: 'scale(1)' }], { duration: 1800, iterations: Infinity, easing: 'ease-in-out' });
      case 'bouncefast': return node.animate([{ transform: 'scale(1)' }, { transform: 'scale(1.05) translateY(-0.6%)' }, { transform: 'scale(1)' }], { duration: 640, iterations: Infinity, easing: 'cubic-bezier(.4, 0, .6, 1)' });
      case 'breathe': return node.animate([{ transform: 'scale(1)' }, { transform: 'scale(1.03)' }], { duration: 4200, iterations: Infinity, direction: 'alternate', easing: 'ease-in-out' });
      case 'drift': { const d = Math.random() < 0.5 ? -1 : 1; return node.animate([{ transform: `scale(1.06) translateX(${-2.5 * d}%)` }, { transform: `scale(1.06) translateX(${2.5 * d}%)` }], { duration: ms, easing: 'linear', fill: 'forwards' }); }
      case 'zoom': { const x = r(-2, 2), y = r(-2, 2); return node.animate([{ transform: 'scale(1)' }, { transform: `scale(1.12) translate(${x}%, ${y}%)` }], { duration: ms, easing: 'ease-out', fill: 'forwards' }); }
      default: return null;
    }
  };

  // ---------- full screen: the window's own ----------
  let fullBusy = false;
  showFullscreen = async on => {
    const now = html.classList.contains('show-full');
    if (on === undefined) on = !now;
    if (on === now || fullBusy) return;
    fullBusy = true;
    try {
      const r = await tauriInvoke('window_action', { action: on ? 'fullscreen' : 'unfullscreen' });
      html.classList.toggle('show-full', !!r);
    } catch (e) {} finally { fullBusy = false; }
  };

  // ---------- pause while you're in another window ----------
  let autoPaused = false;
  addEventListener('blur', () => {
    if (!showOpen || showPaused || !session || session.opts.pauseAway === false) return;
    setPaused(true); autoPaused = true;
  });
  addEventListener('focus', () => {
    if (autoPaused && showOpen && showPaused) setPaused(false);
    autoPaused = false;
  });

  // ---------- category chips ----------
  const cats = make('div', 'show-cats');
  $('showTop').append(cats);
  function drawCats() {
    if (!session) return;
    const o = session.opts;
    const kinds = i => (i.kind === 'image' ? o.images : i.kind === 'gif' ? o.gifs : o.videos);
    const names = [...new Set((library || []).filter(kinds).map(i => i.category))].sort((a, b) => a.localeCompare(b));
    const chip = (label, on, run) => { const b = make('button', 'show-cat' + (on ? ' on' : ''), label); b.type = 'button'; b.onclick = e => { e.stopPropagation(); run(e); }; return b; };
    cats.replaceChildren(chip('All', !o.categories.length, () => setCats([])),
      ...names.map(n => chip(n, o.categories.includes(n), e => setCats(e.shiftKey ? (o.categories.includes(n) ? o.categories.filter(c => c !== n) : [...o.categories, n]) : [n]))));
    cats.title = 'Click: only this category · Shift+click: add or remove it';
  }
  function setCats(list) {
    session.opts.categories = list;
    saveSession();
    queues.clear();
    ahead.splice(0).forEach(releaseSlide);
    fillAhead();
    drawCats();
    if (!showPaused) advance();
  }

  // ---------- the succubus tail ----------
  const tail = make('div', 'show-tail');
  tail.innerHTML = `<svg><path class="tail-bg"/><path class="tail-on"/></svg>
    <span class="tail-tip"><svg viewBox="0 0 24 24"><path d="M12 21C5 16 2 12.5 2 8.6 2 5.5 4.4 3 7.4 3c1.8 0 3.5.9 4.6 2.4C13.1 3.9 14.8 3 16.6 3 19.6 3 22 5.5 22 8.6c0 3.9-3 7.4-10 12.4z"/></svg></span>`;
  show.append(tail);
  const tailSvg = tail.querySelector('svg'), tailBg = tail.querySelector('.tail-bg'), tailOn = tail.querySelector('.tail-on'), tip = tail.querySelector('.tail-tip');
  let tailW = 0, len = 0;
  // the tail is drawn at the screen's own width (a stretched drawing would stretch its dashes too)
  function shapeTail() {
    const w = show.clientWidth || innerWidth, H = 26;
    if (w === tailW) return;
    tailW = w;
    tailSvg.setAttribute('viewBox', `0 0 ${w} ${H}`);
    const waves = Math.max(3, Math.round(w / 260)), step = w / waves;
    let d = `M0 ${H * 0.62}`;
    for (let k = 0; k < waves; k++) {
      const x = k * step, up = k % 2 ? H * 0.82 : H * 0.28;
      d += ` C ${x + step * 0.35} ${H * 0.62}, ${x + step * 0.45} ${up}, ${x + step * 0.7} ${up} S ${x + step} ${H * 0.62}, ${x + step} ${H * 0.62}`;
    }
    tailBg.setAttribute('d', d); tailOn.setAttribute('d', d);
    len = tailOn.getTotalLength();
    tailOn.style.strokeDasharray = `${len} ${len}`;
  }
  function drawTail() {
    if (!showOpen || !session) return;
    shapeTail();
    const total = session.stages.length * levelMs();
    const part = Math.max(0, Math.min(1, sessionElapsed() / total));
    tailOn.style.strokeDashoffset = len * (1 - part);
    const pt = tailOn.getPointAtLength(len * part);
    tip.style.left = pt.x + 'px';
    tip.style.bottom = (26 - pt.y) + 'px';
    tail.title = `${Math.round(part * 100)}% of the session`;
  }
  setInterval(drawTail, 500);

  // ---------- a ripple where you click ----------
  show.addEventListener('pointerdown', e => {
    if (e.target.closest('button, .show-hud')) return;
    const r = make('span', 'show-ripple');
    r.style.left = e.clientX + 'px'; r.style.top = e.clientY + 'px';
    show.append(r);
    setTimeout(() => r.remove(), 750);
  });

  // ---------- the bar: drawn icons instead of emoji ----------
  const ICON = {
    '🔇': '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M4 9.5h3.5L12 5v14l-4.5-4.5H4z" fill="currentColor"/><path d="M16 9.5l5 5M21 9.5l-5 5"/></svg>',
    '🔊': '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M4 9.5h3.5L12 5v14l-4.5-4.5H4z" fill="currentColor"/><path d="M15.5 8.5a5 5 0 0 1 0 7M18.5 6a8.5 8.5 0 0 1 0 12"/></svg>',
    '⛶': '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M4 9V4h5M20 9V4h-5M4 15v5h5M20 15v5h-5"/></svg>',
    '✕': '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><path d="M6 6l12 12M18 6L6 18"/></svg>'
  };
  for (const id of ['sSound', 'sFull', 'sClose']) {
    const b = $(id);
    const swap = () => { const t = b.textContent.trim(); if (ICON[t]) b.innerHTML = ICON[t]; };
    swap();
    new MutationObserver(swap).observe(b, { childList: true, characterData: true, subtree: true });
  }

  // ---------- the history: each slideshow, how long ---------
  let history = [], run = null;
  setupReady.then(async () => { history = (await api('kvGet', 'showhistory').catch(() => null)) || []; drawHistory(); });
  const box = make('div', 'show-history');
  document.querySelector('.show-summary').append(box);
  function drawHistory() {
    if (!history.length) { box.replaceChildren(); return; }
    const week = history.filter(h => Date.now() - h.start < 7 * 864e5).reduce((n, h) => n + h.secs, 0);
    box.replaceChildren(make('h3', '', 'Earlier slideshows'), make('p', 'show-history-total', `This week: ${fmtMin(week)} in ${history.filter(h => Date.now() - h.start < 7 * 864e5).length} slideshows`),
      ...history.slice(0, 8).map(h => {
        const r = make('div', 'show-history-row');
        const d = new Date(h.start);
        r.append(make('b', '', fmtMin(h.secs)), make('span', '', `${d.toLocaleDateString(undefined, { weekday: 'short', day: 'numeric', month: 'short' })} ${d.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' })} · ${h.slides} slides · reached ${h.reached}`));
        return r;
      }));
  }
  const openShowOrig = openShow;
  openShow = function () {
    run = { start: Date.now(), slides: 0 };
    const r = openShowOrig.apply(this, arguments);
    drawCats(); drawTail();
    return r;
  };
  const showSlideOrig = showSlide;
  showSlide = function () { if (run) run.slides++; return showSlideOrig.apply(this, arguments); };
  const closeShowOrig = closeShow;
  closeShow = function () {
    if (run && session) {
      const secs = (Date.now() - run.start) / 1000;
      if (secs >= 20) {
        history.unshift({ start: run.start, secs: Math.round(secs), slides: run.slides, reached: levelName(session.stages[stageNow()]) });
        history = history.slice(0, 50);
        api('kvSet', 'showhistory', history).catch(() => {});
      }
    }
    run = null;
    const r = closeShowOrig.apply(this, arguments);
    drawHistory();
    return r;
  };
})();
