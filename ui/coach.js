// Lewd Vault desktop: the coach (Phase 8 of PLAN.md). A small floating companion (the heart button at
// the top right opens her) with two modes:
//   - Kegel: pelvic floor exercises after the Mayo Clinic's guide (squeeze, hold, relax: start with
//     3 seconds each, 10 in a row, at least 3 sets a day; work up to 10 seconds). A ring fills while
//     you squeeze and empties while you relax, she counts, and days you finish are kept as a streak.
//   - JOI: she sets a rhythm (a beating heart, a soft click, and her voice if you like): slow, faster,
//     edges ("stop, hands off"), and an ending you choose (or leave to fate). "I'm close" makes her stop you.
//     It runs over anything, the slideshow too.
// Her lines show in her bubble (your PC's own voice can read them, off by default). When a session
// starts she moans: a recording from your Audio's Moans (you pick which), or one of MagicalMysticVA's
// (freesound.org, CC BY 4.0).
// Nothing leaves the PC. Kept with the vault:
// _app/kv/coach.json (the Kegel days, your best streak, JOI sessions finished).

(() => {
  'use strict';
  if (!window.vaultUI || !inTauri) return;
  const { make } = window.vaultUI;
  const wait = ms => new Promise(r => setTimeout(r, ms));
  const pad = n => String(n).padStart(2, '0');
  const dayKey = (d = new Date()) => `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`;

  let data = { kegelDays: {}, kegelBest: 0, joiSessions: 0 };
  setupReady.then(async () => { data = { ...data, ...((await api('kvGet', 'coach').catch(() => null)) || {}) }; });
  const save = () => api('kvSet', 'coach', data).catch(() => {});
  const kegelStreak = () => { let n = 0; for (let d = new Date(); data.kegelDays[dayKey(d)]; d.setDate(d.getDate() - 1)) n++; return n; };

  // VOICE-START
  // Her lines (shown in her bubble; your PC's voice can read them).
  const VOICE_LINES = {
    // Kegel
    'k-start': 'Get comfortable for me. We start in a moment.',
    'k-squeeze-hold': 'Squeeze. And hold it.',
    'k-squeeze': 'Squeeze.',
    'k-relax': 'And relax.',
    'k-rest': 'Good. Rest for thirty seconds.',
    'k-done': 'All done. That was so good. I\'m proud of you.',
    // JOI: the start, the pace, the edges
    'j-start': 'I set the pace, and you follow it. Hands on. Nice and slow.',
    'slow-1': 'Slowly. Follow my heart.', 'slow-2': 'Nice and slow for me.', 'slow-3': 'Take your time. I want to enjoy this.', 'slow-4': 'Long, slow strokes.', 'slow-5': 'Mmm. Just like that. Slowly.',
    'faster-1': 'A little faster.', 'faster-2': 'Faster now.', 'faster-3': 'Keep up with me.', 'faster-4': 'Good. Faster.', 'faster-5': 'That\'s it. A bit faster for me.',
    'fast-1': 'Fast. Don\'t you dare stop.', 'fast-2': 'As fast as my heart.', 'fast-3': 'Faster. Faster.', 'fast-4': 'Yes. Keep going. Just like that.',
    'slower-1': 'Slow down.', 'slower-2': 'Easy. Slower.', 'slower-3': 'Gently now.', 'slower-4': 'Shh. Slow down for me.',
    'edge-1': 'Get close for me. Edge.', 'edge-2': 'Right to the edge. Not over it.', 'edge-3': 'Closer. Closer.', 'edge-4': 'Get right there for me. Don\'t you dare finish.',
    'stop-1': 'Stop. Hands off.', 'stop-2': 'Stop. Let go. Breathe.', 'stop-3': 'Hands off. Stay right there.', 'stop-4': 'Stop. Good. Look at you, so close.',
    'back-1': 'Good. Hands back on, slowly.', 'back-2': 'Again. Start slow.', 'back-3': 'Well done. Slowly again.', 'back-4': 'Mmm, good. Touch yourself again. Slowly.',
    // JOI: the end
    'last-minute': 'One last minute. Faster.',
    'countdown': 'Ten. Nine. Eight. Seven. Six. Five. Four. Three. Two. One.',
    'end-finish': 'Now. You may finish. Let go for me.',
    'end-ruin': 'Let go. Hands off, right now. Ruin it.',
    'end-deny': 'Stop. Hands off. Denied. Not today.',
    'after': 'Good. You did so well.',
    'after-deny': 'Good. You did so well. Next time, maybe.'
  };
  // VOICE-END

  // ---------- her voice and the click ----------
  let voiceOn = false, clickOn = true, moan = 'auto', ctx = null;
  try { const p = JSON.parse(localStorage.getItem('imageVault.coach')) || {}; voiceOn = p.pcVoice === true; clickOn = p.click !== false; moan = p.moan || 'auto'; } catch (e) {}
  const savePrefs = () => { try { localStorage.setItem('imageVault.coach', JSON.stringify({ pcVoice: voiceOn, click: clickOn, moan })); } catch (e) {} };
  const clip = new Audio();
  // the moan when a session starts: one of your recordings (Audio → Moans), or a soft built-in one
  let moans = [];
  const loadMoans = () => api('kvGet', 'audio').then(a => { moans = ((a && a.items) || []).filter(i => i.category === 'Moans'); }).catch(() => {});
  setupReady.then(loadMoans);
  // hers: MagicalMysticVA's moans (freesound.org, CC BY 4.0), shipped in /sounds
  function builtinMoan() {
    clip.src = `/sounds/coach-${1 + Math.floor(Math.random() * 5)}.ogg`; clip.volume = 0.9;
    clip.play().catch(() => {});
  }
  function startMoan() {
    if (moan === 'off') return;
    const pick = moan === 'auto' ? moans[Math.floor(Math.random() * moans.length)] : moans.find(m => m.id === moan);
    if (!pick || moan === 'builtin') return builtinMoan();
    clip.src = vaultUrl(pick.id); clip.volume = 0.9; clip.currentTime = 0;
    clip.play().catch(builtinMoan);
    setTimeout(() => clip.pause(), 7000);
  }
  let voice = null;
  const pickVoice = () => {
    const vs = speechSynthesis.getVoices().filter(v => /^en/i.test(v.lang));
    voice = vs.find(v => /zira|aria|jenny|female|susan|hazel|libby|sonia/i.test(v.name)) || vs[0] || null;
  };
  if ('speechSynthesis' in window) { pickVoice(); speechSynthesis.onvoiceschanged = pickVoice; }
  function say(text, opts = {}) {
    line.textContent = text;
    bubble.classList.remove('pop'); void bubble.offsetWidth; bubble.classList.add('pop');
    if (!voiceOn || opts.silent || !('speechSynthesis' in window)) return;
    speechSynthesis.cancel();
    const u = new SpeechSynthesisUtterance(text.replace(/[…]/g, '.'));
    if (voice) u.voice = voice;
    u.rate = opts.rate || 0.92; u.pitch = 1.15; u.volume = 0.9;
    speechSynthesis.speak(u);
  }
  function click(strong) {
    if (!clickOn) return;
    ctx = ctx || new AudioContext();
    const t = ctx.currentTime, o = ctx.createOscillator(), g = ctx.createGain();
    o.frequency.value = strong ? 880 : 660; o.type = 'sine';
    g.gain.setValueAtTime(0.0001, t); g.gain.exponentialRampToValueAtTime(strong ? 0.12 : 0.06, t + 0.005); g.gain.exponentialRampToValueAtTime(0.0001, t + 0.09);
    o.connect(g); g.connect(ctx.destination); o.start(t); o.stop(t + 0.1);
  }

  // ---------- the panel ----------
  const btn = make('button', 'tb-coach');
  btn.type = 'button'; btn.title = 'Your coach: Kegel and JOI';
  btn.innerHTML = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 20s-7-4.4-7-9.6A4 4 0 0 1 12 8a4 4 0 0 1 7 2.4c0 5.2-7 9.6-7 9.6z"/><path d="M9 12h1.5l1-2 1.5 4 1-2H15"/></svg>';
  const clockBtn = document.querySelector('.tb-clock');
  (clockBtn || document.querySelector('.titlebar .tb-btn')).before(btn);
  const panel = make('div', 'coach hidden');
  panel.innerHTML = `
    <div class="coach-head">
      <span class="coach-face" title="Your coach"></span>
      <div class="coach-bubble"><span class="coach-line">Hi. Pelvic floor workout, or shall I take control for a while?</span></div>
      <button class="coach-min" type="button" title="Make small">–</button>
      <button class="coach-x" type="button" title="Stop and close">×</button>
    </div>
    <div class="coach-modes"><button data-m="kegel" class="on">Kegel</button><button data-m="joi">JOI</button></div>
    <div class="coach-body"></div>
    <div class="coach-foot"><label class="coach-voicepick">Start <select class="coach-moansel"></select></label><label><input type="checkbox" class="coach-pcvoice"> PC voice</label><label><input type="checkbox" class="coach-click"> Beat click</label></div>`;
  document.body.append(panel);
  const q = s => panel.querySelector(s);
  const line = q('.coach-line'), bubble = q('.coach-bubble'), body = q('.coach-body');
  q('.coach-click').checked = clickOn; q('.coach-pcvoice').checked = voiceOn;
  q('.coach-pcvoice').onchange = e => { voiceOn = e.target.checked; savePrefs(); if (!voiceOn) speechSynthesis.cancel(); };
  const drawMoans = () => {
    const sel = q('.coach-moansel');
    sel.replaceChildren(new Option(moans.length ? 'A moan of yours' : 'Her moan', 'auto'), new Option('Her moan (MagicalMysticVA)', 'builtin'), ...moans.map(m => new Option(m.title.slice(0, 40), m.id)), new Option('No sound', 'off'));
    sel.value = [...sel.options].some(o => o.value === moan) ? moan : 'auto';
    sel.title = 'The sound when a session starts: your recordings in Audio → Moans, or a built-in one';
    sel.onchange = () => { moan = sel.value; savePrefs(); startMoan(); };
  };
  q('.coach-click').onchange = e => { clickOn = e.target.checked; savePrefs(); };
  btn.onclick = () => { loadMoans().then(drawMoans); panel.classList.toggle('hidden'); panel.classList.remove('small'); if (!panel.classList.contains('hidden') && !running) draw(); };
  q('.coach-x').onclick = () => { stop(); panel.classList.add('hidden'); };
  q('.coach-min').onclick = () => panel.classList.toggle('small');
  q('.coach-face').onclick = () => panel.classList.remove('small');
  // drag it by its head
  q('.coach-head').addEventListener('pointerdown', e => {
    if (e.target.closest('button')) return;
    const r = panel.getBoundingClientRect(), dx = e.clientX - r.left, dy = e.clientY - r.top;
    const move = ev => { panel.style.left = Math.max(0, Math.min(innerWidth - r.width, ev.clientX - dx)) + 'px'; panel.style.top = Math.max(40, Math.min(innerHeight - 80, ev.clientY - dy)) + 'px'; panel.style.right = 'auto'; panel.style.bottom = 'auto'; };
    const up = () => { removeEventListener('pointermove', move); removeEventListener('pointerup', up); };
    addEventListener('pointermove', move); addEventListener('pointerup', up);
  });
  let mode = 'kegel', running = null; // running: { stop(), pause() }
  panel.querySelectorAll('.coach-modes button').forEach(b => b.onclick = () => { if (running) return toast('Stop the current one first', 'err'); mode = b.dataset.m; draw(); });
  function stop() { if (running) running.stop(); running = null; speechSynthesis.cancel(); panel.classList.remove('live'); }
  window.vaultCoach = { pause: () => { if (running) running.pause(); }, open: () => btn.click() };

  // ---------- Kegel ----------
  const KEGEL = [['gentle', 'Gentle: 3 s squeeze, 3 s rest, 10 times', 3, 3, 10], ['steady', 'Steady: 5 s and 5 s, 10 times', 5, 5, 10],
    ['strong', 'Strong: 10 s and 10 s, 10 times', 10, 10, 10], ['flicks', 'Quick flicks: 1 s and 1 s, 20 times', 1, 1, 20]];
  function draw() {
    panel.querySelectorAll('.coach-modes button').forEach(b => b.classList.toggle('on', b.dataset.m === mode));
    body.replaceChildren();
    if (mode === 'kegel') {
      const lvl = make('select', 'coach-sel'); KEGEL.forEach(([id, label]) => lvl.add(new Option(label, id)));
      try { lvl.value = localStorage.getItem('imageVault.kegel') || 'gentle'; } catch (e) {}
      const sets = make('select', 'coach-sel'); [1, 2, 3, 4, 5].forEach(n => sets.add(new Option(`${n} ${n === 1 ? 'set' : 'sets'}`, n))); sets.value = 3;
      const go = make('button', 'btn primary coach-go', 'Start');
      go.onclick = () => { try { localStorage.setItem('imageVault.kegel', lvl.value); } catch (e) {} kegel(KEGEL.find(k => k[0] === lvl.value), +sets.value); };
      const streak = kegelStreak();
      body.append(make('p', 'coach-note', streak ? `Streak: ${streak} ${streak === 1 ? 'day' : 'days'} · best ${data.kegelBest}` : 'Three sets a day, every day, works best.'), lvl, sets, go,
        make('p', 'coach-safe', 'Squeeze the muscles you\'d use to stop peeing, without tightening your belly, thighs or buttocks, and keep breathing. Don\'t practise while actually peeing. Stop if it hurts, and ask a doctor if you\'re unsure.'));
      say(`Hi. ${streak ? `Day ${streak + 1} of your streak?` : 'Ready for your pelvic floor workout?'} Pick a level.`, { silent: true });
    } else {
      const len = make('select', 'coach-sel'); [5, 10, 15, 20, 30].forEach(n => len.add(new Option(`${n} minutes`, n))); len.value = 10;
      const pace = make('select', 'coach-sel'); [['gentle', 'Gentle'], ['normal', 'Normal'], ['intense', 'Intense']].forEach(([v, l]) => pace.add(new Option(l, v))); pace.value = 'normal';
      const end = make('select', 'coach-sel'); [['fate', 'The ending: let fate decide'], ['finish', 'The ending: you may finish'], ['ruin', 'The ending: a ruined one'], ['deny', 'The ending: denied']].forEach(([v, l]) => end.add(new Option(l, v)));
      const go = make('button', 'btn primary coach-go', 'Start');
      go.onclick = () => joi(+len.value, pace.value, end.value);
      body.append(make('p', 'coach-note', `Sessions finished: ${data.joiSessions}`), len, pace, end, go);
      say('I\'ll set the pace, you follow it. Choose how long, and how it ends.', { silent: true });
    }
  }

  async function kegel([id, , hold, rest, reps], sets) {
    stop();
    startMoan();
    panel.classList.add('live');
    let alive = true, paused = false;
    running = { stop: () => { alive = false; }, pause: () => { paused = true; } };
    body.replaceChildren();
    const ring = make('div', 'coach-ring');
    ring.innerHTML = '<svg viewBox="0 0 120 120"><circle cx="60" cy="60" r="52" class="cr-bg"/><circle cx="60" cy="60" r="52" class="cr-on"/></svg><b></b><span></span>';
    const count = make('p', 'coach-count');
    const ctrl = make('div', 'coach-ctrl');
    const pauseBtn = make('button', 'btn', 'Pause'), stopBtn = make('button', 'btn', 'Stop');
    pauseBtn.onclick = () => { paused = !paused; pauseBtn.textContent = paused ? 'Go on' : 'Pause'; };
    stopBtn.onclick = () => { stop(); draw(); };
    ctrl.append(pauseBtn, stopBtn);
    body.append(ring, count, ctrl);
    const on = ring.querySelector('.cr-on'), big = ring.querySelector('b'), small = ring.querySelector('span'), C = 2 * Math.PI * 52;
    on.style.strokeDasharray = C;
    const phase = async (label, secs, from, to) => {
      big.textContent = label; ring.classList.toggle('hold', label === 'Squeeze');
      const t0 = performance.now();
      let left = secs * 1000;
      while (alive && left > 0) {
        await wait(50);
        if (paused) { continue; }
        left -= 50;
        const p = 1 - left / (secs * 1000);
        on.style.strokeDashoffset = C * (1 - (from + (to - from) * p));
        small.textContent = Math.ceil(left / 1000) + ' s';
      }
      return alive;
    };
    say(`${KEGEL.find(k => k[0] === id)[1].split(':')[0]}. ${sets} ${sets === 1 ? 'set' : 'sets'} of ${reps}. Get comfortable.`, { v: 'k-start' });
    await wait(2500);
    for (let s = 1; s <= sets && alive; s++) {
      for (let r = 1; r <= reps && alive; r++) {
        count.textContent = `Set ${s} of ${sets} · ${r} of ${reps}`;
        if (hold >= 3 || r === 1) say(hold >= 3 ? 'Squeeze… and hold.' : 'Squeeze', { rate: 1.05, v: hold >= 3 ? 'k-squeeze-hold' : 'k-squeeze' }); click(true);
        if (!await phase('Squeeze', hold, 0, 1)) break;
        if (hold >= 3) say('And relax.', { v: 'k-relax' }); click(false);
        if (!await phase('Relax', rest, 1, 0)) break;
      }
      if (alive && s < sets) { say('Good. Rest for thirty seconds.', { v: 'k-rest' }); if (!await phase('Rest', 30, 0, 0)) break; }
    }
    if (!alive) return;
    // done: today counts
    data.kegelDays[dayKey()] = true;
    data.kegelBest = Math.max(data.kegelBest || 0, kegelStreak());
    save();
    running = null; panel.classList.remove('live');
    const st = kegelStreak();
    say(`All done. That's ${st} ${st === 1 ? 'day' : 'days'} in a row. Well done.`, { v: 'k-done' });
    big.textContent = '♥'; small.textContent = 'done';
    count.textContent = `Streak: ${st} · best ${data.kegelBest}`;
    ctrl.replaceChildren(make('button', 'btn primary', 'Back'));
    ctrl.firstChild.onclick = draw;
  }

  // ---------- JOI ----------
  async function joi(minutes, pace, ending) {
    stop();
    startMoan();
    panel.classList.add('live');
    let alive = true, paused = false, close = false;
    running = { stop: () => { alive = false; }, pause: () => { paused = true; pauseBtn.textContent = 'Go on'; } };
    const speed = { gentle: 0.75, normal: 1, intense: 1.3 }[pace];
    if (ending === 'fate') ending = ['finish', 'finish', 'ruin', 'deny'][Math.floor(Math.random() * 4)];
    body.replaceChildren();
    const heart = make('div', 'coach-beat');
    heart.innerHTML = '<svg viewBox="0 0 24 24"><path d="M12 21C5 16 2 12.5 2 8.6 2 5.5 4.4 3 7.4 3c1.8 0 3.5.9 4.6 2.4C13.1 3.9 14.8 3 16.6 3 19.6 3 22 5.5 22 8.6c0 3.9-3 7.4-10 12.4z"/></svg><b></b>';
    const bar = make('div', 'coach-time'); bar.append(make('i'));
    const ctrl = make('div', 'coach-ctrl');
    const closeBtn = make('button', 'btn primary', 'I\'m close'), pauseBtn = make('button', 'btn', 'Pause'), stopBtn = make('button', 'btn', 'Stop');
    closeBtn.onclick = () => { close = true; };
    pauseBtn.onclick = () => { paused = !paused; pauseBtn.textContent = paused ? 'Go on' : 'Pause'; };
    stopBtn.onclick = () => { stop(); draw(); };
    ctrl.append(closeBtn, pauseBtn, stopBtn);
    body.append(heart, bar, ctrl);
    const bps = heart.querySelector('b');
    const total = minutes * 60 * 1000;
    let elapsed = 0;
    // the plan: stretches of a steady beat, now and then an edge, and the last minute counting down
    // the lines she picks from: every one is recorded (VOICE_LINES), e.g. slow-1 … slow-5
    const pickLine = k => {
      const ids = Object.keys(VOICE_LINES).filter(id => id.startsWith(k + '-'));
      const id = ids[Math.floor(Math.random() * ids.length)];
      return { text: VOICE_LINES[id], v: id };
    };
    const sayLine = k => { const l = pickLine(k); say(l.text, { v: l.v }); };
    async function beat(rate, ms) {
      // rate: beats a second; the heart pulses with each
      const start = elapsed;
      const period = 1000 / Math.max(0.2, rate * speed);
      bps.textContent = (rate * speed).toFixed(1) + '/s';
      heart.style.setProperty('--period', period + 'ms');
      heart.classList.toggle('still', rate === 0);
      let next = 0;
      while (alive && elapsed - start < ms) {
        await wait(50);
        if (paused) continue;
        elapsed += 50; next -= 50;
        bar.firstChild.style.width = Math.min(100, elapsed / total * 100) + '%';
        if (rate > 0 && next <= 0) { click(false); heart.classList.remove('thump'); void heart.offsetWidth; heart.classList.add('thump'); next += period; }
        if (close) { close = false; return 'close'; }
      }
      return alive ? 'ok' : 'stop';
    }
    async function edge() {
      sayLine('stop');
      const hold = 15000 + Math.random() * 15000;
      if (await beat(0, hold) === 'stop') return false;
      sayLine('back');
      return true;
    }
    say(`${minutes} minutes. I set the pace, and you follow it. Hands on. Nice and slow.`, { v: 'j-start' });
    await beat(0, 3500);
    while (alive && elapsed < total - 60000) {
      const r = Math.random();
      let step, rate, len = 20000 + Math.random() * 30000;
      if (r < 0.3) { step = 'slow'; rate = 0.8 + Math.random() * 0.4; }
      else if (r < 0.6) { step = 'faster'; rate = 1.4 + Math.random() * 0.6; }
      else if (r < 0.8) { step = 'fast'; rate = 2.2 + Math.random() * 0.8; len = 12000 + Math.random() * 15000; }
      else if (r < 0.9) { step = 'slower'; rate = 0.5 + Math.random() * 0.3; }
      else { step = 'edge'; rate = 2.6 + Math.random() * 0.6; len = 15000; }
      sayLine(step);
      const res = await beat(rate, Math.min(len, total - 60000 - elapsed));
      if (res === 'stop') return;
      if (res === 'close' || step === 'edge') { if (!await edge()) return; }
    }
    if (!alive) return;
    // the last minute
    say('One last minute. Faster.', { v: 'last-minute' });
    if (await beat(2.2, 30000) === 'stop') return;
    say('Ten… nine… eight… seven… six… five… four… three… two… one…', { rate: 0.85, v: 'countdown' });
    if (await beat(3, 12000) === 'stop') return;
    say(VOICE_LINES['end-' + ending], { v: 'end-' + ending });
    await beat(0, ending === 'finish' ? 20000 : 8000);
    data.joiSessions = (data.joiSessions || 0) + 1;
    save();
    running = null; panel.classList.remove('live');
    say(ending === 'deny' ? VOICE_LINES['after-deny'] : VOICE_LINES.after, { v: ending === 'deny' ? 'after-deny' : 'after' });
    ctrl.replaceChildren(make('button', 'btn primary', 'Back'));
    ctrl.firstChild.onclick = draw;
  }
})();
