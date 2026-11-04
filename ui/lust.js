// Lewd Vault desktop: the NSFW faces and the Appreciate button (Phase 7 of PLAN.md).
//   - ten faces, one for each NSFW level, drawn here (original anime-style vector art of a grown woman):
//     sweet, shy, blushing, flirty, flustered, biting her lip, heart eyes, panting, lewd, ahegao.
//     They're the NSFW picker in the details (click a level) and show on the Library's detailed layouts.
//   - Appreciate becomes a round heart button: it pops, hearts burst out of it, and it moans: a short one
//     of hers (MagicalMysticVA, freesound.org, CC BY 4.0) at random, or any recording from your Audio
//     (Settings → Look → Appreciate sound).

(() => {
  'use strict';
  if (!window.vaultUI || !inTauri) return;
  const { make } = window.vaultUI;

  // ---------- the faces: one for each NSFW level ----------
  // Drawn here (original vector art): a grown woman in anime style (long hair, lipstick, long lashes),
  // a different expression for each level, from a sweet smile (1) to full ahegao (10).
  const LEVEL_NAMES = ['Sweet', 'Shy', 'Blushing', 'Flirty', 'Flustered', 'Biting her lip', 'Heart eyes', 'Panting', 'Lewd', 'Ahegao'];
  const TIERS = [[1, 2, 'Calm'], [3, 4, 'Blushing'], [5, 6, 'Flustered'], [7, 8, 'Lewd'], [9, 10, 'Ahegao']];
  const tierOf = n => TIERS.findIndex(([a, b]) => n >= a && n <= b);
  const levelName = n => LEVEL_NAMES[Math.max(1, Math.min(10, n)) - 1];
  //            eyes            mouth          blush  extras
  const LOOK = [null,
    ['happy', 'smile', .25, 'sparkle'],
    ['open', 'small', .4, ''],
    ['aside', 'o', .6, ''],
    ['wink', 'smirk', .55, 'heart1'],
    ['open', 'wavy', .75, 'sweat'],
    ['half', 'bite', .8, ''],
    ['heart', 'open', .85, 'hearts'],
    ['half', 'pant', .9, 'steam'],
    ['rolled', 'tongue', .95, 'hearts'],
    ['rolledHeart', 'ahegao', 1, 'hearts drool']];
  let faceSeq = 0;
  function faceFor(n) {
    n = Math.max(1, Math.min(10, Math.round(n) || 1));
    const [eyes, mouth, blush, extras] = LOOK[n];
    const id = 'lf' + (++faceSeq);
    const hair = 'var(--accent)', hairDark = 'color-mix(in srgb, var(--accent) 62%, #2a0a18)', skin = '#ffe3e6', ink = '#36121f', lip = '#d93d6b', iris = `url(#${id}i)`;
    const eye = (cx, kind) => {
      const cy = 31.5;
      const lash = `<path d="M${cx - 4.6} ${cy - 2.4}Q${cx} ${cy - 6.6} ${cx + 4.8} ${cy - 2.2}" stroke="${ink}" stroke-width="2.1" fill="none" stroke-linecap="round"/><path d="M${cx + (cx < 32 ? -4.6 : 4.8)} ${cy - 2.3}l${cx < 32 ? -1.6 : 1.6} -1.1" stroke="${ink}" stroke-width="1.5" stroke-linecap="round"/>`;
      const white = `<ellipse cx="${cx}" cy="${cy}" rx="4.3" ry="4.8" fill="#fff"/>`;
      const shine = `<circle cx="${cx - 1.3}" cy="${cy - 1.4}" r="1.15" fill="#fff"/><circle cx="${cx + 1.4}" cy="${cy + 1.7}" r=".55" fill="#fff"/>`;
      const round = (dx = 0, dy = .5) => `<g clip-path="url(#${id}c${cx})"><ellipse cx="${cx + dx}" cy="${cy + dy}" rx="3.1" ry="3.9" fill="${iris}"/><ellipse cx="${cx + dx}" cy="${cy + dy + .5}" rx="1.4" ry="1.9" fill="#2a0c1d"/></g>`;
      const clip = `<clipPath id="${id}c${cx}"><ellipse cx="${cx}" cy="${cy}" rx="4.3" ry="4.8"/></clipPath>`;
      const heartAt = (x, y, s, fill) => `<path transform="translate(${x} ${y}) scale(${s})" d="M0 3C-3 1-4.2-.6-4.2-2.2-4.2-3.4-3.3-4.3-2.1-4.3-1.3-4.3-.5-3.9 0-3.2.5-3.9 1.3-4.3 2.1-4.3 3.3-4.3 4.2-3.4 4.2-2.2 4.2-.6 3 1 0 3z" fill="${fill}"/>`;
      if (kind === 'happy') return `<path d="M${cx - 4} ${cy + 1}Q${cx} ${cy - 4.2} ${cx + 4} ${cy + 1}" stroke="${ink}" stroke-width="2.2" fill="none" stroke-linecap="round"/>`;
      if (kind === 'open') return clip + white + round() + shine + lash;
      if (kind === 'aside') return clip + white + round(1.4, .9) + shine + lash;
      if (kind === 'half') return clip + white + round(0, 1.2) + `<path d="M${cx - 4.6} ${cy - 5}H${cx + 4.8}V${cy - .4}Q${cx} ${cy + .6} ${cx - 4.6} ${cy - .6}Z" fill="${skin}"/><path d="M${cx - 4.6} ${cy - .6}Q${cx} ${cy + .7} ${cx + 4.8} ${cy - .4}" stroke="${ink}" stroke-width="2" fill="none" stroke-linecap="round"/>`;
      if (kind === 'heart') return `<g>${heartAt(cx, cy + .8, 1.15, '#ff3d7f')}<circle cx="${cx - 1.3}" cy="${cy - 1.1}" r=".9" fill="#fff"/></g>` + lash;
      if (kind === 'rolled') return clip + white + `<g clip-path="url(#${id}c${cx})"><ellipse cx="${cx + (cx < 32 ? .8 : -.8)}" cy="${cy - 3.6}" rx="3.1" ry="3.6" fill="${iris}"/></g>` + lash;
      if (kind === 'rolledHeart') return clip + white + `<g clip-path="url(#${id}c${cx})"><ellipse cx="${cx + (cx < 32 ? .8 : -.8)}" cy="${cy - 3.4}" rx="3.1" ry="3.6" fill="${iris}"/>${heartAt(cx + (cx < 32 ? .8 : -.8), cy - 2.6, .55, '#ff2e74')}</g>` + lash;
      return '';
    };
    const eyesSvg = eyes === 'wink' ? eye(25, 'open') + eye(39, 'happy') : eye(25, eyes) + eye(39, eyes);
    const M = {
      smile: `<path d="M29 41.8Q32 44.6 35 41.8" stroke="${lip}" stroke-width="1.9" fill="none" stroke-linecap="round"/>`,
      small: `<path d="M30.2 42.2Q32 43.6 33.8 42.2" stroke="${lip}" stroke-width="1.8" fill="none" stroke-linecap="round"/>`,
      o: `<ellipse cx="32" cy="42.6" rx="1.5" ry="1.9" fill="${lip}"/><ellipse cx="32" cy="42.9" rx=".8" ry="1" fill="#7a1532"/>`,
      smirk: `<path d="M29.6 42.4Q32.4 44.4 35 41.4" stroke="${lip}" stroke-width="1.9" fill="none" stroke-linecap="round"/><path d="M34.1 42.4q.6 1.8 1.6 1.4" stroke="#ff7aa0" stroke-width="1.3" fill="none" stroke-linecap="round"/>`,
      wavy: `<path d="M28.4 42.8q1.2-1.3 2.4 0t2.4 0 2.4 0" stroke="${lip}" stroke-width="1.8" fill="none" stroke-linecap="round" stroke-linejoin="round"/>`,
      bite: `<path d="M28.8 41.6Q32 44.6 35.2 41.6" stroke="${lip}" stroke-width="2" fill="none" stroke-linecap="round"/><path d="M31 42.7h2v1.5q-1 .6-2 0z" fill="#fff" stroke="${ink}" stroke-width=".5"/>`,
      open: `<path d="M28.6 41.2Q32 47.4 35.4 41.2Z" fill="#7a1532" stroke="${lip}" stroke-width="1.3" stroke-linejoin="round"/><path d="M30.2 44.2q1.8 1.6 3.6 0" fill="#ff8aa8"/>`,
      pant: `<ellipse cx="32" cy="43.2" rx="2.6" ry="3" fill="#7a1532" stroke="${lip}" stroke-width="1.2"/><path d="M30.2 45.1q1.8 1.1 3.6 0" fill="#ff8aa8"/>`,
      tongue: `<path d="M28.2 41Q32 47.2 35.8 41Z" fill="#7a1532" stroke="${lip}" stroke-width="1.2" stroke-linejoin="round"/><path d="M30.2 43.4Q32 50.4 33.8 43.4Z" fill="#ff7a9e" stroke="#c9356a" stroke-width=".6"/>`,
      ahegao: `<path d="M27.4 40.6Q32 48.6 36.6 40.6Z" fill="#7a1532" stroke="${lip}" stroke-width="1.2" stroke-linejoin="round"/><path d="M29.8 43.4Q32 53.2 34.2 43.4Z" fill="#ff7a9e" stroke="#c9356a" stroke-width=".6"/><path d="M32 45.2v4.6" stroke="#e85a85" stroke-width=".6"/>`
    };
    const hearts = (extras.includes('hearts') ? [[50, 17, .8], [13, 21, .6], [54, 27, .5]] : extras.includes('heart1') ? [[50, 18, .7]] : [])
      .map(([x, y, s]) => `<path transform="translate(${x} ${y}) scale(${s})" d="M0 3C-3 1-4.2-.6-4.2-2.2-4.2-3.4-3.3-4.3-2.1-4.3-1.3-4.3-.5-3.9 0-3.2.5-3.9 1.3-4.3 2.1-4.3 3.3-4.3 4.2-3.4 4.2-2.2 4.2-.6 3 1 0 3z" fill="#ff3d7f"/>`).join('');
    const sweat = extras.includes('sweat') ? `<path d="M48.5 21q2.2 3.4 0 5.2q-2.2-1.8 0-5.2z" fill="#9fd7ff" stroke="#5aa9dd" stroke-width=".5"/>` : '';
    const steam = extras.includes('steam') ? `<path d="M50 20q-2-2 0-4t0-4M54 23q-2-2 0-4t0-4M12 22q2-2 0-4" stroke="#ffffff" stroke-opacity=".75" stroke-width="1.3" fill="none" stroke-linecap="round"/>` : '';
    const sparkle = extras.includes('sparkle') ? `<path d="M51 15l.9 2.4 2.4.9-2.4.9L51 21.6l-.9-2.4-2.4-.9 2.4-.9z" fill="#ffd36b"/>` : '';
    const drool = extras.includes('drool') ? `<path d="M35.4 43.6q.5 3.4-.2 5.6" stroke="#cfeaff" stroke-width=".9" fill="none" stroke-linecap="round"/>` : '';
    const hatch = blush >= .6 ? `<path d="M19.6 37.6l1.3-1.3M21.8 37.6l1.3-1.3M24 37.6l1.3-1.3M40 37.6l1.3-1.3M42.2 37.6l1.3-1.3M44.4 37.6l1.3-1.3" stroke="#e8557a" stroke-width=".8" stroke-linecap="round" opacity="${blush}"/>` : '';
    return `<svg viewBox="0 0 64 64" aria-hidden="true">
      <defs><radialGradient id="${id}i" cx=".5" cy=".35" r=".7"><stop offset="0" stop-color="#ff9ccf"/><stop offset=".55" stop-color="#b44fd6"/><stop offset="1" stop-color="#4b1a6e"/></radialGradient></defs>
      <path d="M9 60C5 38 7 19 19 10.5 27 5 38 5 45.5 9.5 57.5 17 59.5 38 55 60Z" fill="${hairDark}"/>
      <path d="M17.2 29.6C17.2 18.4 23.6 12.2 32 12.2S46.8 18.4 46.8 29.6C46.8 40.4 40.6 49.6 32 50.6 23.4 49.6 17.2 40.4 17.2 29.6Z" fill="${skin}"/>
      <path d="M26 48.6q6 3 12 0v6H26z" fill="${skin}"/>
      <ellipse cx="21.8" cy="37.4" rx="3.6" ry="2.1" fill="#ff6f95" opacity="${blush}"/><ellipse cx="42.2" cy="37.4" rx="3.6" ry="2.1" fill="#ff6f95" opacity="${blush}"/>${hatch}
      ${eyesSvg}
      <path d="M31.6 36.6q.6.5 1.1 0" stroke="#e7a3ad" stroke-width=".9" fill="none" stroke-linecap="round"/>
      ${M[mouth]}${drool}
      <path d="M14.6 31C13.6 16.8 21.6 8.6 32 8.6S50.6 16.8 49.4 31c-1.6-4.6-3.6-8-6.4-10.2.2 3.2-1 5.6-3 7-.6-3.6-2.4-6-4.6-7.4-1.4 3-4 5.4-7.8 6.8.8-2.6.8-4.8 0-6.8-3.2 2.6-5.8 6.2-6.6 10.6-1.4-1.6-2.2-3.4-2.4-5.4-1.6 1.8-2.8 3.6-3.6 5.4z" fill="${hair}"/>
      <path d="M16.8 27.4C15 36 15.8 45 18.6 52M47.2 27.4C49 36 48.2 45 45.4 52" stroke="${hair}" stroke-width="3.2" fill="none" stroke-linecap="round"/>
      <circle cx="17.2" cy="38.6" r="1" fill="#ffd36b"/><circle cx="46.8" cy="38.6" r="1" fill="#ffd36b"/>
      ${hearts}${sweat}${steam}${sparkle}</svg>`;
  }
  const face = t => faceFor([1, 3, 5, 7, 10][t] || 1); // (older callers: by tier)
  window.vaultFaces = { face, faceFor, levelName, tierOf, TIERS };

  // the NSFW picker in the details: the face of the level, and the ten levels to click
  const input = $('dNsfw');
  const picker = make('div', 'lust-pick');
  const big = make('span', 'lust-face');
  const name = make('span', 'lust-name');
  const levels = make('div', 'lust-levels');
  for (let n = 1; n <= 10; n++) {
    const b = make('button', 'lust-lv', String(n));
    b.type = 'button'; b.dataset.n = n; b.style.setProperty('--h', n);
    b.title = `NSFW ${n}: ${levelName(n)}`;
    b.onclick = () => { input.value = n; draw(); };
    levels.append(b);
  }
  const head = make('div', 'lust-head'); head.append(big, name);
  picker.append(head, levels);
  input.after(picker);
  input.classList.add('lust-input');
  function draw() {
    const n = Math.max(1, Math.min(10, +input.value || 1)), t = tierOf(n);
    big.innerHTML = faceFor(n);
    name.textContent = `${n} · ${levelName(n)}`;
    levels.querySelectorAll('.lust-lv').forEach(b => b.classList.toggle('on', +b.dataset.n === n));
  }
  input.addEventListener('input', draw);
  const openDetailLust = openDetail;
  openDetail = function () { const r = openDetailLust.apply(this, arguments); draw(); return r; };
  // and a little face on the Library's detailed layouts
  const makeCardLust = makeCard;
  makeCard = function (item) {
    const card = makeCardLust.apply(this, arguments);
    const info = card.querySelector('.lv-info-top');
    if (info) { const f = make('span', 'lust-mini'); f.innerHTML = faceFor(item.nsfw || 1); f.title = `NSFW ${item.nsfw}: ${levelName(item.nsfw || 1)}`; info.prepend(f); }
    return card;
  };

  // ---------- Appreciate: a round heart that pops, bursts and (if you like) sounds ----------
  const like = $('dLike');
  like.classList.add('lust-like');
  like.innerHTML = '<span class="lust-heart"><svg viewBox="0 0 24 24"><path d="M12 21C5 16 2 12.5 2 8.6 2 5.5 4.4 3 7.4 3c1.8 0 3.5.9 4.6 2.4C13.1 3.9 14.8 3 16.6 3 19.6 3 22 5.5 22 8.6c0 3.9-3 7.4-10 12.4z"/></svg></span><span class="lust-like-text">Appreciate <span id="dLikes"></span></span>';
  like.addEventListener('click', () => {
    const r = like.querySelector('.lust-heart').getBoundingClientRect();
    for (let k = 0; k < 9; k++) {
      const h = make('span', 'lust-burst', '♥');
      const a = (k / 9) * Math.PI * 2 + Math.random() * 0.5, d = 46 + Math.random() * 34;
      h.style.left = (r.left + r.width / 2) + 'px'; h.style.top = (r.top + r.height / 2) + 'px';
      h.style.setProperty('--x', Math.cos(a) * d + 'px'); h.style.setProperty('--y', Math.sin(a) * d - 20 + 'px');
      h.style.fontSize = (12 + Math.random() * 12) + 'px';
      document.body.append(h);
      setTimeout(() => h.remove(), 900);
    }
    sound();
  });
  // the sound: off, a soft built-in "mmh", or a recording of yours
  let ctx = null, playingSound = null;
  const LIKE = ['like-oh1', 'like-oh2', 'like-oh3', 'like-oh4', 'like-oh5', 'like-oh6', 'like-oh7', 'like-oh8', 'like-sigh', 'like-orgasm', 'like-moan1', 'like-moan2', 'like-moan3'];
  function hum() {
    ctx = ctx || new AudioContext();
    const t = ctx.currentTime, out = ctx.createGain();
    out.gain.setValueAtTime(0, t); out.gain.linearRampToValueAtTime(0.16, t + 0.12); out.gain.setValueAtTime(0.16, t + 0.45); out.gain.exponentialRampToValueAtTime(0.001, t + 1.15);
    out.connect(ctx.destination);
    // a hummed "mm": a low voice through two nasal formants, gliding down, with a little vibrato
    const f1 = ctx.createBiquadFilter(); f1.type = 'bandpass'; f1.frequency.value = 280; f1.Q.value = 4;
    const f2 = ctx.createBiquadFilter(); f2.type = 'bandpass'; f2.frequency.value = 2400; f2.Q.value = 8;
    const g2 = ctx.createGain(); g2.gain.value = 0.25;
    f1.connect(out); f2.connect(g2); g2.connect(out);
    const osc = ctx.createOscillator(); osc.type = 'sawtooth';
    osc.frequency.setValueAtTime(260, t); osc.frequency.exponentialRampToValueAtTime(210, t + 1.0);
    const vib = ctx.createOscillator(), vg = ctx.createGain(); vib.frequency.value = 5.5; vg.gain.value = 4; vib.connect(vg); vg.connect(osc.frequency);
    osc.connect(f1); osc.connect(f2);
    osc.start(t); vib.start(t); osc.stop(t + 1.2); vib.stop(t + 1.2);
  }
  function sound() {
    const p = window.vaultProfile && window.vaultProfile.get();
    let choice = (p && p.likeSound) || 'her';
    if (choice === 'off') return;
    if (choice === 'hum' || choice === 'her') {
      if (playingSound) playingSound.pause();
      playingSound = new Audio(`/sounds/${LIKE[Math.floor(Math.random() * LIKE.length)]}.ogg`);
      playingSound.volume = 0.85;
      playingSound.play().catch(() => {});
      return;
    }
    if (choice.startsWith('audio-')) {
      if (playingSound) { playingSound.pause(); }
      playingSound = new Audio(vaultUrl(choice));
      playingSound.volume = 0.8;
      playingSound.play().catch(() => hum());
      setTimeout(() => { if (playingSound) { playingSound.pause(); playingSound = null; } }, 6000);
    }
  }
  // the choice, in Settings → Look
  const renderSettingsLust = renderSettings;
  renderSettings = function () {
    const r = renderSettingsLust.apply(this, arguments);
    const look = [...document.querySelectorAll('#settingsForm h3')].find(h => h.textContent === 'Look');
    if (!look || document.querySelector('.lust-setting')) return r;
    const row = make('label', 'setting lust-setting');
    const text = make('span', 's-label', 'Appreciate sound');
    text.append(make('small', '', 'What the heart button says when you click it (or press A): one of her short moans at random (voiced by MagicalMysticVA, freesound.org, CC BY 4.0), nothing, or any recording from your Audio.'));
    const sel = make('select');
    sel.add(new Option('Her moans (at random)', 'her')); sel.add(new Option('No sound', 'off'));
    api('kvGet', 'audio').then(a => {
      const items = ((a && a.items) || []).slice().sort((x, y) => (y.category === 'Moans') - (x.category === 'Moans'));
      for (const it of items) sel.add(new Option(`${it.title} (${it.category})`, it.id));
      const v = (window.vaultProfile.get() || {}).likeSound; sel.value = !v || v === 'hum' ? 'her' : v;
    }).catch(() => {});
    sel.onchange = () => { const p = window.vaultProfile.get(); p.likeSound = sel.value; window.vaultProfile.save(); sound(); };
    row.append(text, sel);
    let at = look.nextElementSibling;
    while (at && at.tagName !== 'H3') at = at.nextElementSibling;
    if (at) at.before(row); else look.parentElement.append(row);
    return r;
  };
})();
