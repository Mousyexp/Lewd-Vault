// Lewd Vault desktop: the video player (Phase 5 of PLAN.md). One themed player for every video: the
// full view, Discover, Videos (long-form) and anything else. vaultPlayer.attach(video, host) puts it
// on a <video> inside `host`:
//   - a pink scrub bar with what's loaded, a time bubble and a little picture of the moment under
//     the mouse; seeking jumps to the nearest keyframe while dragging (fast) and to the exact moment
//     when let go
//   - play/pause, ±10 s, speed, loop A–B, mute and volume, picture fit (fit / fill / actual size,
//     remembered), full screen (the whole monitor)
//   - drag up and down on the left edge for brightness, on the right edge for volume (or use the
//     wheel there), like phone players
//   - keys: Space/K play, J/L ±10 s, Shift+←/→ ±5 s, ,/. frame by frame (paused), ↑/↓ volume, M mute,
//     F full screen, [ ] speed, Home/End
// The controls fade after 2.5 s without the mouse while it plays.

(() => {
  'use strict';
  const make = window.vaultUI.make;
  const PREF = 'imageVault.player';
  const pref = (() => { try { return JSON.parse(localStorage.getItem(PREF)) || {}; } catch (e) { return {}; } })();
  const savePref = () => { try { localStorage.setItem(PREF, JSON.stringify(pref)); } catch (e) {} };
  pref.volume = pref.volume ?? 1; pref.fit = pref.fit || 'contain'; pref.bright = pref.bright ?? 100;
  const fmt = s => { if (!isFinite(s)) return '0:00'; s = Math.max(0, Math.floor(s)); const h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60, x = s % 60; return (h ? h + ':' + String(m).padStart(2, '0') : m) + ':' + String(x).padStart(2, '0'); };
  const ICON = {
    play: '<svg viewBox="0 0 24 24"><path d="M7 4.5v15l12.5-7.5z" fill="currentColor"/></svg>',
    pause: '<svg viewBox="0 0 24 24"><rect x="6" y="4.5" width="4" height="15" rx="1.2" fill="currentColor"/><rect x="14" y="4.5" width="4" height="15" rx="1.2" fill="currentColor"/></svg>',
    back: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M4 12a8 8 0 1 0 2.4-5.7M4 4v4h4"/><text x="12" y="15.5" font-size="7.5" text-anchor="middle" fill="currentColor" stroke="none" font-weight="700">10</text></svg>',
    fwd: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M20 12a8 8 0 1 1-2.4-5.7M20 4v4h-4"/><text x="12" y="15.5" font-size="7.5" text-anchor="middle" fill="currentColor" stroke="none" font-weight="700">10</text></svg>',
    vol: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M4 9.5h3.5L12 5v14l-4.5-4.5H4z" fill="currentColor"/><path d="M15.5 8.5a5 5 0 0 1 0 7M18.5 6a8.5 8.5 0 0 1 0 12"/></svg>',
    mute: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M4 9.5h3.5L12 5v14l-4.5-4.5H4z" fill="currentColor"/><path d="M16 9.5l5 5M21 9.5l-5 5"/></svg>',
    full: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M4 9V4h5M20 9V4h-5M4 15v5h5M20 15v5h-5"/></svg>',
    fit: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linejoin="round"><rect x="3" y="5" width="18" height="14" rx="2"/><rect x="7" y="8" width="10" height="8" rx="1" fill="currentColor" opacity=".35"/></svg>',
    loop: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M17 3l3 3-3 3M4 11V9a3 3 0 0 1 3-3h13M7 21l-3-3 3-3M20 13v2a3 3 0 0 1-3 3H4"/></svg>',
    sun: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M2 12h2M20 12h2M5 5l1.4 1.4M17.6 17.6L19 19M5 19l1.4-1.4M17.6 6.4L19 5"/></svg>'
  };
  const FITS = [['contain', 'Fit'], ['cover', 'Fill'], ['none', 'Actual size']];
  const SPEEDS = [0.5, 0.75, 1, 1.25, 1.5, 2];
  let active = null; // the player the keys go to (the last one used)

  function attach(video, host) {
    if (video._player) return video._player;
    video.controls = false;
    video.volume = pref.volume;
    host.classList.add('vp-host');
    const ui = make('div', 'vp');
    ui.innerHTML = `
      <div class="vp-edge left" title="Drag up or down: brightness"><div class="vp-meter"><div></div></div><span class="vp-ico">${ICON.sun}</span></div>
      <div class="vp-edge right" title="Drag up or down: volume"><div class="vp-meter"><div></div></div><span class="vp-ico">${ICON.vol}</span></div>
      <div class="vp-flash"></div>
      <div class="vp-bar">
        <div class="vp-track"><div class="vp-buffered"></div><div class="vp-loop"></div><div class="vp-played"><span class="vp-knob"></span></div>
          <div class="vp-tip"><canvas width="160" height="90"></canvas><b></b></div></div>
        <div class="vp-row">
          <button class="vp-btn vp-play" title="Play (Space)">${ICON.play}</button>
          <button class="vp-btn vp-back" title="Back 10 s (J)">${ICON.back}</button>
          <button class="vp-btn vp-fwd" title="Forward 10 s (L)">${ICON.fwd}</button>
          <span class="vp-time"></span>
          <span class="vp-gap"></span>
          <button class="vp-btn vp-loopbtn" title="Loop the video (R)">${ICON.loop}</button>
          <button class="vp-btn vp-ab" title="Loop a part: set the start, then the end, then off (Shift+R)">A–B<i></i></button>
          <button class="vp-btn vp-speed" title="Speed ([ and ])">1×</button>
          <button class="vp-btn vp-fitbtn" title="Picture: fit, fill or actual size">${ICON.fit}<i></i></button>
          <button class="vp-btn vp-mute" title="Mute (M)">${ICON.vol}</button>
          <input class="vp-vol" type="range" min="0" max="1" step="0.02" title="Volume">
          <button class="vp-btn vp-full" title="Full screen (F)">${ICON.full}</button>
        </div>
      </div>`;
    host.append(ui);
    const $q = s => ui.querySelector(s);
    const track = $q('.vp-track'), played = $q('.vp-played'), buffered = $q('.vp-buffered'), tip = $q('.vp-tip'), loopBar = $q('.vp-loop');
    const flash = $q('.vp-flash');
    let ab = null, dragging = false, hideTimer = null;
    const p = { video, host, ui };
    video._player = p;

    // ---------- looks: fit and brightness ----------
    const applyLook = () => {
      video.style.objectFit = pref.fit;
      video.style.filter = pref.bright === 100 ? '' : `brightness(${pref.bright}%)`;
      $q('.vp-fitbtn i').textContent = FITS.find(f => f[0] === pref.fit)[1];
      ui.querySelector('.vp-edge.left .vp-meter div').style.height = ((pref.bright - 40) / 120 * 100) + '%';
      ui.querySelector('.vp-edge.right .vp-meter div').style.height = (video.muted ? 0 : video.volume * 100) + '%';
      $q('.vp-vol').value = video.muted ? 0 : video.volume;
      $q('.vp-mute').innerHTML = video.muted || !video.volume ? ICON.mute : ICON.vol;
    };
    const say = text => { flash.textContent = text; flash.classList.remove('on'); void flash.offsetWidth; flash.classList.add('on'); };
    applyLook();

    // ---------- time, buffer, play state ----------
    const draw = () => {
      const d = video.duration || 0, t = dragging ? +track.dataset.t : video.currentTime;
      played.style.width = d ? (t / d * 100) + '%' : '0';
      $q('.vp-time').textContent = `${fmt(t)} / ${fmt(d)}`;
      const b = video.buffered;
      let end = 0;
      for (let k = 0; k < b.length; k++) if (b.start(k) <= video.currentTime + 0.5) end = Math.max(end, b.end(k));
      buffered.style.width = d ? (end / d * 100) + '%' : '0';
      $q('.vp-play').innerHTML = video.paused ? ICON.play : ICON.pause;
      ui.classList.toggle('paused', video.paused);
      if (ab && ab.b != null && d) { loopBar.style.left = (ab.a / d * 100) + '%'; loopBar.style.width = ((ab.b - ab.a) / d * 100) + '%'; }
      loopBar.classList.toggle('on', !!(ab && ab.b != null));
    };
    for (const ev of ['timeupdate', 'progress', 'play', 'pause', 'durationchange', 'loadedmetadata', 'volumechange', 'seeked']) video.addEventListener(ev, draw);
    video.addEventListener('timeupdate', () => { if (ab && ab.b != null && video.currentTime >= ab.b) video.currentTime = ab.a; });
    video.addEventListener('volumechange', () => { if (!video.muted) pref.volume = video.volume; savePref(); applyLook(); });
    video.addEventListener('emptied', () => { ab = null; $q('.vp-ab').classList.remove('on', 'half'); draw(); });
    const toggle = () => { if (video.paused) video.play().catch(() => {}); else video.pause(); };
    const skip = d => { video.currentTime = Math.max(0, Math.min((video.duration || 0) - 0.05, video.currentTime + d)); say((d > 0 ? '+' : '') + d + ' s'); };
    p.toggle = toggle; p.skip = skip;

    // ---------- seeking: keyframes while dragging, exact on release ----------
    const timeAt = x => { const r = track.getBoundingClientRect(); return Math.max(0, Math.min(1, (x - r.left) / r.width)) * (video.duration || 0); };
    let lastSeek = 0;
    const seekTo = (t, exact) => {
      track.dataset.t = t;
      if (exact) { video.currentTime = t; return; }
      const now = performance.now();
      if (now - lastSeek < 90) return;
      lastSeek = now;
      if (video.fastSeek) video.fastSeek(t); else video.currentTime = t;
    };
    track.addEventListener('pointerdown', e => {
      if (e.button !== 0 || !video.duration) return;
      track.setPointerCapture(e.pointerId);
      dragging = true; ui.classList.add('dragging');
      p.wasPaused = video.paused; video.pause();
      seekTo(timeAt(e.clientX), false); draw();
    });
    track.addEventListener('pointermove', e => {
      const t = timeAt(e.clientX), r = track.getBoundingClientRect();
      tip.style.left = Math.max(80, Math.min(r.width - 80, e.clientX - r.left)) + 'px';
      tip.querySelector('b').textContent = fmt(t);
      previewAt(t);
      if (dragging) { seekTo(t, false); draw(); }
    });
    const endDrag = e => {
      if (!dragging) return;
      dragging = false; ui.classList.remove('dragging');
      seekTo(timeAt(e.clientX), true);
      if (!p.wasPaused) video.play().catch(() => {});
    };
    track.addEventListener('pointerup', endDrag);
    track.addEventListener('pointercancel', endDrag);
    // the little picture under the mouse: a second, hidden copy of the video seeks there
    let ghost = null, ghostBusy = false, ghostWant = null;
    function previewAt(t) {
      const src = video.currentSrc || video.src;
      if (!src || src.startsWith('blob:') && !video.duration) return;
      if (!ghost || ghost._src !== src) {
        ghost = document.createElement('video');
        Object.assign(ghost, { muted: true, preload: 'auto', src, crossOrigin: undefined });
        ghost._src = src;
        ghost.addEventListener('seeked', () => {
          ghostBusy = false;
          try {
            // the moment's picture, whole (a portrait video sits in the middle with black either side)
            const g = tip.querySelector('canvas').getContext('2d'), w = ghost.videoWidth || 16, h = ghost.videoHeight || 9;
            const k = Math.min(160 / w, 90 / h);
            g.fillStyle = '#000'; g.fillRect(0, 0, 160, 90);
            g.drawImage(ghost, (160 - w * k) / 2, (90 - h * k) / 2, w * k, h * k);
            tip.classList.add('pic');
          } catch (e) { tip.classList.remove('pic'); }
          if (ghostWant != null) { const w = ghostWant; ghostWant = null; previewAt(w); }
        });
      }
      if (ghostBusy) { ghostWant = t; return; }
      ghostBusy = true;
      if (ghost.fastSeek) ghost.fastSeek(t); else ghost.currentTime = t;
      setTimeout(() => { ghostBusy = false; }, 600);
    }

    // ---------- buttons ----------
    $q('.vp-play').onclick = toggle;
    $q('.vp-back').onclick = () => skip(-10);
    $q('.vp-fwd').onclick = () => skip(10);
    $q('.vp-mute').onclick = () => { video.muted = !video.muted; if (!video.muted && !video.volume) video.volume = 0.5; };
    $q('.vp-vol').oninput = e => { video.muted = false; video.volume = +e.target.value; };
    $q('.vp-speed').onclick = () => { const k = SPEEDS.indexOf(video.playbackRate); setSpeed(SPEEDS[(k + 1) % SPEEDS.length]); };
    const setSpeed = s => { video.playbackRate = s; $q('.vp-speed').textContent = s + '×'; say(s + '×'); };
    p.setSpeed = setSpeed;
    video.addEventListener('ratechange', () => { $q('.vp-speed').textContent = video.playbackRate + '×'; });
    $q('.vp-fitbtn').onclick = () => { const k = FITS.findIndex(f => f[0] === pref.fit); pref.fit = FITS[(k + 1) % FITS.length][0]; savePref(); applyLook(); say(FITS.find(f => f[0] === pref.fit)[1]); };
    // the whole video on repeat (as the right-click menu's Loop): the button shows which it is
    const loopBtn = $q('.vp-loopbtn');
    const showLoop = () => { loopBtn.classList.toggle('on', video.loop); loopBtn.title = video.loop ? 'Looping: click to play once (R)' : 'Loop the video (R)'; };
    loopBtn.onclick = () => { video.loop = !video.loop; showLoop(); say(video.loop ? 'Loop on' : 'Loop off'); };
    p.toggleLoop = () => loopBtn.click();
    p.toggleAB = () => $q('.vp-ab').click();
    ['loadedmetadata', 'play', 'emptied'].forEach(t => video.addEventListener(t, showLoop));
    host.addEventListener('pointerenter', showLoop); // (the right-click menu may have changed it)
    showLoop();
    $q('.vp-ab').onclick = () => {
      const b = $q('.vp-ab');
      if (!ab) { ab = { a: video.currentTime, b: null }; b.classList.add('half'); say('Loop from ' + fmt(ab.a) + ': press again at the end'); }
      else if (ab.b == null) { if (video.currentTime <= ab.a + 0.3) return say('Pick an end after the start'); ab.b = video.currentTime; b.classList.remove('half'); b.classList.add('on'); say(`Looping ${fmt(ab.a)}–${fmt(ab.b)}`); video.currentTime = ab.a; }
      else { ab = null; b.classList.remove('on', 'half'); say('Loop off'); }
      draw();
    };
    const fullscreen = async () => {
      const on = await tauriInvoke('window_action', { action: 'fullscreen' }).catch(() => false);
      document.documentElement.classList.toggle('vp-fullscreen', !!on);
      host.classList.toggle('vp-full', !!on);
    };
    $q('.vp-full').onclick = fullscreen;
    p.fullscreen = fullscreen;
    video.addEventListener('click', toggle);
    video.addEventListener('dblclick', fullscreen);

    // ---------- the edges: brightness (left), volume (right) ----------
    for (const side of ['left', 'right']) {
      const edge = ui.querySelector('.vp-edge.' + side);
      const change = d => {
        if (side === 'left') { pref.bright = Math.max(40, Math.min(160, pref.bright + d * 120)); savePref(); applyLook(); say(`Brightness ${Math.round(pref.bright)}%`); }
        else { video.muted = false; video.volume = Math.max(0, Math.min(1, video.volume + d)); say(`Volume ${Math.round(video.volume * 100)}%`); }
        edge.classList.add('show'); clearTimeout(edge._t); edge._t = setTimeout(() => edge.classList.remove('show'), 900);
      };
      edge.addEventListener('wheel', e => { e.preventDefault(); e.stopPropagation(); change(e.deltaY < 0 ? 0.05 : -0.05); }, { passive: false });
      edge.addEventListener('pointerdown', e => {
        edge.setPointerCapture(e.pointerId);
        let y = e.clientY;
        const move = ev => { const h = edge.getBoundingClientRect().height || 300; change((y - ev.clientY) / h); y = ev.clientY; };
        const up = () => { edge.removeEventListener('pointermove', move); edge.removeEventListener('pointerup', up); };
        edge.addEventListener('pointermove', move); edge.addEventListener('pointerup', up);
      });
    }

    // ---------- the controls fade while it plays ----------
    const wake = () => {
      ui.classList.add('awake'); host.classList.remove('vp-idle');
      clearTimeout(hideTimer);
      hideTimer = setTimeout(() => { if (!video.paused && !dragging) { ui.classList.remove('awake'); host.classList.add('vp-idle'); } }, 2500);
    };
    host.addEventListener('pointermove', wake);
    host.addEventListener('pointerdown', () => { active = p; wake(); });
    video.addEventListener('play', () => { active = p; wake(); });
    video.addEventListener('pause', wake);
    wake(); draw();
    return p;
  }

  // ---------- keys: to the player last used, when it's on screen ----------
  document.addEventListener('keydown', e => {
    const p = active;
    if (!p || !p.video.isConnected || !p.video.offsetParent || /INPUT|SELECT|TEXTAREA/.test(e.target.tagName) || e.ctrlKey || e.altKey) return;
    if (document.querySelector('.ask-back')) return; // (a dialog is open: its keys are its own)
    const v = p.video, k = e.key;
    let done = true;
    if (k === ' ' || k.toLowerCase() === 'k') p.toggle();
    else if (k.toLowerCase() === 'j') p.skip(-10);
    else if (k.toLowerCase() === 'l') p.skip(10);
    else if (e.shiftKey && k === 'ArrowLeft') p.skip(-5);
    else if (e.shiftKey && k === 'ArrowRight') p.skip(5);
    else if (k === 'ArrowUp' && !e.shiftKey) { v.muted = false; v.volume = Math.min(1, v.volume + 0.05); }
    else if (k === 'ArrowDown' && !e.shiftKey) { v.volume = Math.max(0, v.volume - 0.05); }
    else if (k.toLowerCase() === 'm') v.muted = !v.muted;
    else if (k.toLowerCase() === 'f') p.fullscreen();
    else if (k === '[' || k === ']') { const i = SPEEDS.indexOf(v.playbackRate); p.setSpeed(SPEEDS[Math.max(0, Math.min(SPEEDS.length - 1, (i < 0 ? 2 : i) + (k === ']' ? 1 : -1)))]); }
    else if ((k === ',' || k === '.') && v.paused) v.currentTime = Math.max(0, v.currentTime + (k === '.' ? 1 / 30 : -1 / 30));
    else if (k.toLowerCase() === 'r' && !e.ctrlKey && !e.metaKey) e.shiftKey ? p.toggleAB() : p.toggleLoop();
    else if (k === 'Home') v.currentTime = 0;
    else if (k === 'End') v.currentTime = Math.max(0, (v.duration || 0) - 0.1);
    else if (k === 'Escape' && document.documentElement.classList.contains('vp-fullscreen')) { p.fullscreen(); }
    else done = false;
    if (done) { e.preventDefault(); e.stopImmediatePropagation(); }
  }, true);

  window.vaultPlayer = { attach };

  // ---------- the full view's video uses it, played straight from disk ----------
  const dv = document.getElementById('detailVideo');
  attach(dv, document.querySelector('#detail .big'));
  // the speed buttons of the details panel become the player's
  document.getElementById('dSpeedBox').classList.add('hidden-forever');
  // leaving the full view leaves full screen too
  new MutationObserver(() => {
    if (document.getElementById('detail').classList.contains('hidden') && document.documentElement.classList.contains('vp-fullscreen')) {
      tauriInvoke('window_action', { action: 'unfullscreen' }).catch(() => {});
      document.documentElement.classList.remove('vp-fullscreen');
      document.querySelectorAll('.vp-full').forEach(h => h.classList.remove('vp-full'));
    }
  }).observe(document.getElementById('detail'), { attributes: true, attributeFilter: ['class'] });
})();
