// Lewd Vault desktop: you (Phase 7 of PLAN.md).
//   - an activity log: the active time of each day, and your sessions (a session ends after 10 minutes
//     away). "Active" means the window is in front and you're doing something, or something is playing.
//     Kept with the vault: _app/kv/daily.json and sessions.json.
//   - the stopwatch at the top right: this session's active time (it stops while you're away); click it
//     for your sessions, in a popup
//   - XP and levels: from your time, items, tags, appreciations, active days, slideshows and badges;
//     each level takes 100 · level^1.5 XP. Titles come with levels.
//   - badges (30): Disciplined, Night owl, Curator, ... A pink note when one is earned.
//   - the profile (the round picture at the top right): a picture or moving clip from the library
//     (placed and zoomed in its circle), a banner, your vault's name, your title, level, the road of
//     what each level unlocks (your own picture at 2, banners from the vault at 5, moving ones at 8...),
//     stats and badges
//   - on the Stats page: minutes per day and per week, a calendar of the last half year, and sessions

(() => {
  'use strict';
  if (!window.vaultUI || !inTauri) return;
  const { make, showTitle, thumbSrc } = window.vaultUI;
  const IDLE = 180e3, GAP = 600e3;
  const pad = n => String(n).padStart(2, '0');
  const dayKey = (d = new Date()) => `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`;
  const clock = s => { s = Math.floor(s); return `${Math.floor(s / 3600)}:${pad(Math.floor(s / 60) % 60)}:${pad(s % 60)}`; };
  const fmtLen = s => s >= 3600 ? `${Math.floor(s / 3600)} h ${Math.round(s % 3600 / 60)} min` : s >= 60 ? `${Math.round(s / 60)} min` : `${Math.round(s)} s`;

  // =====================================================================================
  // The activity log
  // =====================================================================================
  let daily = {}, sessions = [], cur = null, profile = { name: '', title: '', unlocked: {} };
  let lastInput = Date.now();
  for (const ev of ['pointermove', 'pointerdown', 'keydown', 'wheel', 'scroll']) addEventListener(ev, () => { lastInput = Date.now(); }, { passive: true, capture: true });
  const watching = () => {
    const dv = $('detailVideo');
    return (!$('detail').classList.contains('hidden') && dv && !dv.paused) || (typeof showOpen !== 'undefined' && showOpen && !showPaused)
      || (window.vaultAudio && !window.vaultAudio.audio.paused);
  };
  const active = () => document.visibilityState === 'visible' && (document.hasFocus() || watching()) && (Date.now() - lastInput < IDLE || watching());
  const loaded = setupReady.then(async () => {
    const [d, s, p] = await Promise.all(['daily', 'sessions', 'profile'].map(n => api('kvGet', n).catch(() => null)));
    daily = d || {}; sessions = s || []; profile = { name: '', title: '', unlocked: {}, ...(p || {}) };
  });
  const today = () => daily[dayKey()] || (daily[dayKey()] = { secs: 0, views: 0, likes: 0 });
  function closeSession() {
    if (cur && cur.secs >= 60) {
      const tags = Object.entries(cur.tags || {}).sort((a, b) => b[1] - a[1]).slice(0, 5).map(([t]) => t);
      sessions.unshift({ start: cur.start, end: cur.last, secs: cur.secs, views: cur.views, likes: cur.likes, tags });
      sessions = sessions.slice(0, 300);
    }
    cur = null;
  }
  loaded.then(() => {
    setInterval(() => {
      const now = Date.now();
      if (!active()) { drawClock(false); return; }
      if (!cur || now - cur.last > GAP) { closeSession(); cur = { start: now, last: now, secs: 0, views: 0, likes: 0, tags: {} }; }
      cur.secs++; cur.last = now;
      today().secs++;
      drawClock(true);
    }, 1000);
    setInterval(saveLog, 30000);
    setInterval(checkBadges, 60000);
    setTimeout(checkBadges, 8000);
  });
  function saveLog() {
    const keep = cur && cur.secs >= 60 ? [{ start: cur.start, end: cur.last, secs: cur.secs, views: cur.views, likes: cur.likes, open: true,
      tags: Object.entries(cur.tags).sort((a, b) => b[1] - a[1]).slice(0, 5).map(([t]) => t) }, ...sessions] : sessions;
    api('kvSet', 'daily', daily).catch(() => {});
    api('kvSet', 'sessions', keep.slice(0, 300)).catch(() => {});
  }
  addEventListener('beforeunload', saveLog);
  // views and appreciations, for the day and the session (and what was looked at)
  const recordViewLog = recordView;
  recordView = function (item, type = 'view') {
    const r = recordViewLog.apply(this, arguments);
    const t = today();
    if (type === 'like') t.likes++; else t.views++;
    if (cur) { if (type === 'like') cur.likes++; else cur.views++; for (const tag of window.vaultUI.tagsOf(item)) cur.tags[tag] = (cur.tags[tag] || 0) + 1; }
    return r;
  };

  // =====================================================================================
  // The stopwatch, and the profile picture, at the top right
  // =====================================================================================
  const bar = document.querySelector('.titlebar');
  const watch = make('button', 'tb-clock', '0:00:00');
  watch.type = 'button';
  watch.title = 'This session\'s active time (it stops while you\'re away). Click for your sessions.';
  watch.onclick = () => toggleSessions();
  const me = make('button', 'tb-me');
  me.type = 'button';
  me.title = 'You: level, badges, sessions';
  me.onclick = () => openProfile();
  // the streak, like a game's: a flame and the days in a row (it opens the sessions too)
  const flame = make('button', 'tb-streak');
  flame.type = 'button';
  flame.onclick = () => toggleSessions();
  const firstBtn = bar.querySelector('.tb-btn');
  bar.insertBefore(flame, firstBtn); bar.insertBefore(watch, firstBtn); bar.insertBefore(me, firstBtn);
  const drawFlame = () => { const st = streaks(); flame.replaceChildren(make('span', 'tb-flame', '🔥'), make('b', '', String(st.now))); flame.classList.toggle('cold', !st.now); flame.title = st.now ? `${st.now} ${st.now === 1 ? 'day' : 'days'} in a row (best ${st.best}). Click for your sessions.` : 'No streak yet: use the app today to start one'; };
  loaded.then(() => { drawFlame(); setInterval(drawFlame, 60000); });
  function drawClock(on) {
    watch.textContent = clock(cur ? cur.secs : 0);
    watch.classList.toggle('paused', !on);
  }
  function drawMe() {
    const it = profile.avatarId && (library || []).find(i => i.id === profile.avatarId);
    me.replaceChildren();
    // the picture in a round window of its own (the level badge sits outside it, over the edge)
    const pic = make('span', 'tb-me-pic');
    if (it) {
      if (profile.avatarMoving && it.previewId) { const v = make('video'); Object.assign(v, { muted: true, loop: true, autoplay: true, playsInline: true, src: vaultUrl(it.previewId) }); pic.append(v); }
      else { const im = make('img'); im.src = thumbSrc(it, 240); im.alt = ''; pic.append(im); }
      place(pic.firstChild);
    } else pic.append(make('span', '', '♥'));
    me.append(pic);
    const lv = make('i', 'tb-me-lv', String(levelOf(xpOf(statsNow())).level));
    me.append(lv);
  }
  loaded.then(async () => { while (!library) await new Promise(r => setTimeout(r, 300)); drawMe(); });
  // where the picture sits in its circle (dragged and zoomed when it was chosen)
  function place(el) {
    if (!el || !el.style) return;
    const a = profile.avatarPos || { x: 50, y: 40, z: 1 };
    el.style.objectPosition = `${a.x}% ${a.y}%`;
    el.style.transform = a.z > 1 ? `scale(${a.z})` : '';
    el.style.transformOrigin = `${a.x}% ${a.y}%`;
  }

  // =====================================================================================
  // Your sessions: a popup under the stopwatch
  // =====================================================================================
  const sesPop = make('div', 'ses-pop hidden');
  document.body.append(sesPop);
  function toggleSessions() {
    if (!sesPop.classList.contains('hidden')) return sesPop.classList.add('hidden');
    loaded.then(drawSessions);
  }
  addEventListener('pointerdown', e => { if (!sesPop.contains(e.target) && !watch.contains(e.target) && !flame.contains(e.target)) sesPop.classList.add('hidden'); }, true);
  addEventListener('keydown', e => { if (e.key === 'Escape') sesPop.classList.add('hidden'); });
  function drawSessions() {
    const S = statsNow(), st = streaks();
    const all = [...(cur && cur.secs >= 60 ? [{ start: cur.start, secs: cur.secs, views: cur.views, likes: cur.likes, tags: Object.keys(cur.tags).slice(0, 5), now: true }] : []), ...sessions];
    sesPop.replaceChildren();
    // now: this session, today, the streak
    const head = make('div', 'ses-head');
    const big = make('div', 'ses-now'); big.append(make('small', '', 'This session'), make('b', '', clock(cur ? cur.secs : 0)));
    const today = make('div', 'ses-stat'); today.append(make('b', '', fmtLen((daily[dayKey()] || {}).secs || 0)), make('small', '', 'today'));
    const streak = make('div', 'ses-stat ses-streak'); streak.append(make('b', '', `${st.now}`), make('small', '', st.now === 1 ? 'day streak' : 'days streak'));
    head.append(big, today, streak);
    // the last 7 days, a bar each
    const week = make('div', 'ses-week');
    const vals = [];
    for (let k = 6; k >= 0; k--) { const d = new Date(); d.setDate(d.getDate() - k); vals.push([d, (daily[dayKey(d)] || {}).secs || 0]); }
    const max = Math.max(600, ...vals.map(v => v[1]));
    for (const [d, secs] of vals) {
      const col = make('div', 'ses-day' + (dayKey(d) === dayKey() ? ' today' : ''));
      const bar = make('i'); bar.style.height = Math.max(4, secs / max * 100) + '%';
      col.append(bar, make('span', '', d.toLocaleDateString(undefined, { weekday: 'narrow' })));
      col.title = `${d.toLocaleDateString(undefined, { weekday: 'long', day: 'numeric', month: 'short' })}: ${secs < 60 ? 'not used' : fmtLen(secs)}`;
      week.append(col);
    }
    // the sessions, newest first, grouped by day
    const list = make('div', 'ses-list');
    let lastDay = '';
    for (const s of all.slice(0, 40)) {
      const d = new Date(s.start), k = dayKey(d);
      if (k !== lastDay) { lastDay = k; list.append(make('div', 'ses-dayhead', k === dayKey() ? 'Today' : d.toLocaleDateString(undefined, { weekday: 'long', day: 'numeric', month: 'long' }))); }
      const r = make('div', 'ses-row' + (s.now ? ' now' : ''));
      const len = make('b', '', fmtLen(s.secs));
      const when = make('span', '', `${s.now ? 'Now · ' : ''}${pad(d.getHours())}:${pad(d.getMinutes())} · ${s.views || 0} ${s.views === 1 ? 'view' : 'views'}${s.likes ? ` · ${s.likes} ♥` : ''}`);
      const meter = make('span', 'ses-meter'); meter.append(make('i')); meter.firstChild.style.width = Math.min(100, s.secs / Math.max(1, S.longest) * 100) + '%';
      r.append(len, when, meter);
      if ((s.tags || []).length) r.append(make('span', 'ses-tags', s.tags.map(x => x.replace(/^\w+:/, '').replace(/_/g, ' ')).join(', ')));
      list.append(r);
    }
    if (!all.length) list.append(make('p', 'ses-empty', 'Your sessions show here: each time you use the app, how long, and what you looked at.'));
    const foot = make('div', 'ses-foot');
    foot.append(make('span', '', `${fmtN(S.sessions)} sessions · longest ${fmtLen(S.longest)} · best streak ${st.best}`));
    sesPop.append(head, week, list, foot);
    const r = watch.getBoundingClientRect();
    sesPop.style.top = (r.bottom + 8) + 'px';
    sesPop.style.right = Math.max(8, innerWidth - r.right - 40) + 'px';
    sesPop.classList.remove('hidden');
  }

  // =====================================================================================
  // Stats, XP, levels, titles, badges
  // =====================================================================================
  function streaks() {
    const days = Object.keys(daily).filter(k => daily[k].secs >= 60).sort();
    let best = 0, run = 0, prev = null;
    for (const k of days) {
      const d = new Date(k + 'T12:00');
      run = prev && Math.round((d - prev) / 864e5) === 1 ? run + 1 : 1;
      best = Math.max(best, run); prev = d;
    }
    // the current one: up to today or yesterday
    let now = 0;
    for (let d = new Date(); ; d.setDate(d.getDate() - 1)) {
      const e = daily[dayKey(d)];
      if (e && e.secs >= 60) now++;
      else if (dayKey(d) !== dayKey()) break;
      if (now > 3650) break;
    }
    return { best, now, days: days.length };
  }
  let kvCache = { showhistory: [], audio: { items: [] }, coach: {} };
  setupReady.then(async () => {
    for (const n of ['showhistory', 'audio', 'coach']) kvCache[n] = (await api('kvGet', n).catch(() => null)) || kvCache[n];
  });
  function statsNow() {
    const lib = library || [];
    const all = [...(cur && cur.secs >= 60 ? [{ start: cur.start, secs: cur.secs }] : []), ...sessions];
    const tagged = lib.filter(i => i.tags).length;
    const tagSet = new Set(); lib.forEach(i => window.vaultUI.tagsOf(i).forEach(t => tagSet.add(t)));
    const st = streaks();
    const look = lib.reduce((n, i) => n + timeOf(i.id), 0);
    const likes = lib.reduce((n, i) => n + likesOf(i.id), 0);
    const hours = s => new Date(s.start).getHours();
    const show = kvCache.showhistory || [];
    const cats = {};
    lib.forEach(i => { cats[i.category] = (cats[i.category] || 0) + timeOf(i.id) + viewsOf(i.id) * 5; });
    return {
      items: lib.length, tagged, tags: tagSet.size, categories: new Set(lib.map(i => i.category)).size, collections: new Set(lib.map(i => i.collection).filter(Boolean)).size,
      look, likes, longest: Math.max(0, ...all.map(s => s.secs)), sessions: all.length, night: all.some(s => hours(s) < 4), early: all.some(s => hours(s) >= 5 && hours(s) < 7),
      streak: st.now, bestStreak: st.best, activeDays: st.days, appTime: Object.values(daily).reduce((n, d) => n + d.secs, 0),
      showSecs: show.reduce((n, h) => n + h.secs, 0), showLongest: Math.max(0, ...show.map(h => h.secs)), showTop: show.some(h => /10/.test(h.reached)),
      fromWeb: lib.filter(i => /^https?:/.test(i.source || '')).length, longVideos: lib.filter(i => i.kind === 'video' && i.duration >= 600).length,
      videos: lib.filter(i => i.kind !== 'image').length, oc: lib.filter(i => window.vaultUI.tagsOf(i).includes('meta:oc')).length,
      audio: ((kvCache.audio || {}).items || []).length, hypnoPlays: ((kvCache.audio || {}).items || []).filter(a => a.category === 'Hypno').reduce((n, a) => n + (a.plays || 0), 0),
      kegelStreak: (kvCache.coach || {}).kegelBest || 0, joiSessions: (kvCache.coach || {}).joiSessions || 0,
      favourite: Object.entries(cats).sort((a, b) => b[1] - a[1])[0]?.[0] || '', badges: Object.keys(profile.unlocked || {}).length
    };
  }
  const xpOf = S => Math.round(S.look / 60 + S.items * 2 + S.tagged + S.likes * 3 + S.activeDays * 20 + S.showSecs / 60 + S.badges * 50 + S.appTime / 120);
  function levelOf(xp) {
    let level = 1, left = xp;
    while (left >= Math.round(100 * level ** 1.5)) { left -= Math.round(100 * level ** 1.5); level++; }
    return { level, into: left, need: Math.round(100 * level ** 1.5) };
  }
  const TITLES = [[1, 'Curious'], [3, 'Admirer'], [5, 'Collector'], [8, 'Connoisseur'], [11, 'Devotee'], [15, 'Aficionado'], [19, 'Insatiable'], [24, 'Night owl royalty'], [30, 'Legend of the vault'], [40, 'Vault deity']];
  const BANNERS = [['rose', 'linear-gradient(120deg, #7C3042, #E27589 55%, #F9CDD4)', 1], ['night', 'linear-gradient(120deg, #120a1c, #4a1c48 50%, #c0457a)', 3],
    ['peach', 'linear-gradient(120deg, #f8a07a, #e2758a 60%, #6e2c55)', 6], ['aurora', 'linear-gradient(120deg, #1d2b64, #7c3aed 45%, #e27589)', 10], ['gold', 'linear-gradient(120deg, #3a2205, #c7902b 50%, #ffe39a)', 15], ['galaxy', 'radial-gradient(circle at 30% 40%, #e27589, #4a1c48 45%, #07050a 80%)', 20]];
  // what levels unlock (besides titles and colours): your own picture, banners from the vault, moving ones
  const UNLOCKS = { picture: 2, vaultBanner: 5, moving: 8 };
  const B = (id, icon, name, desc, test) => ({ id, icon, name, desc, test });
  const BADGES = [
    B('first-love', '💗', 'First love', 'Appreciate something', S => S.likes / 1),
    B('smitten', '😍', 'Smitten', '100 appreciations', S => S.likes / 100),
    B('besotted', '💘', 'Besotted', '1,000 appreciations', S => S.likes / 1000),
    B('collector', '📦', 'Collector', '100 items in the vault', S => S.items / 100),
    B('hoarder', '🗄️', 'Hoarder', '1,000 items', S => S.items / 1000),
    B('archivist', '🏛️', 'Archivist', '10,000 items', S => S.items / 10000),
    B('curator', '🏷️', 'Curator', '1,000 tagged items', S => S.tagged / 1000),
    B('tag-wizard', '🔖', 'Tag wizard', '300 different tags', S => S.tags / 300),
    B('organised', '🗂️', 'Organised', '10 categories', S => S.categories / 10),
    B('collections', '📚', 'Bundled up', '5 collections', S => S.collections / 5),
    B('hours', '⏳', 'Hours in', '10 hours of looking', S => S.look / 36000),
    B('hundred', '💯', 'Hundred hours', '100 hours of looking', S => S.look / 360000),
    B('marathon', '🏃', 'Marathon', 'A session over an hour', S => S.longest / 3600),
    B('disciplined', '🧘', 'Disciplined', 'A session over 5 hours', S => S.longest / 18000),
    B('night-owl', '🦉', 'Night owl', 'A session that started after midnight, before 4', S => +S.night),
    B('early-bird', '🌅', 'Early bird', 'A session that started between 5 and 7 in the morning', S => +S.early),
    B('devoted', '🔥', 'Devoted', 'Seven days in a row', S => S.bestStreak / 7),
    B('obsessed', '🌋', 'Obsessed', 'Thirty days in a row', S => S.bestStreak / 30),
    B('regular', '📅', 'Regular', '50 active days', S => S.activeDays / 50),
    B('slideshow', '🎞️', 'Slideshow star', 'A slideshow over 30 minutes', S => S.showLongest / 1800),
    B('full-throttle', '🚀', 'Full throttle', 'Reach level 10 in a slideshow', S => +S.showTop),
    B('explorer', '🧭', 'Explorer', '25 items saved from Discover', S => S.fromWeb / 25),
    B('digger', '⛏️', 'Digger', '250 items saved from Discover', S => S.fromWeb / 250),
    B('film-buff', '🎬', 'Film buff', '10 long videos', S => S.longVideos / 10),
    B('animated', '📽️', 'Animated', '100 GIFs and videos', S => S.videos / 100),
    B('original', '✨', 'Original', '10 pieces of original content', S => S.oc / 10),
    B('audiophile', '🎧', 'Audiophile', '10 audio recordings', S => S.audio / 10),
    B('hypnotised', '🌀', 'Hypnotised', 'Play hypno audio 10 times', S => S.hypnoPlays / 10),
    B('kegel', '👑', 'Pelvic royalty', 'A 30-day Kegel streak with the coach', S => S.kegelStreak / 30),
    B('edge', '⏸️', 'On the edge', '10 JOI sessions with the coach', S => S.joiSessions / 10)
  ];
  // ---------- badge art: drawn here, in the chunky style of game achievements ----------
  // A shape in the badge's colour with a darker rim below (it looks pressed out), a soft shine, and a
  // white symbol. Locked badges are drawn grey.
  const G = {
    heart: '<path d="M12 20.5C5.5 16 3 12.6 3 9.3 3 6.6 5.1 4.5 7.6 4.5c1.7 0 3.3.9 4.4 2.3 1.1-1.4 2.7-2.3 4.4-2.3 2.5 0 4.6 2.1 4.6 4.8 0 3.3-2.5 6.7-9 11.2z" fill="#fff"/>',
    hearts: '<path d="M9.5 18C5 15 3 12.6 3 10.2 3 8.3 4.5 6.8 6.3 6.8c1.3 0 2.4.7 3.2 1.7.8-1 1.9-1.7 3.2-1.7 1.8 0 3.3 1.5 3.3 3.4 0 2.4-2 4.8-6.5 7.8z" fill="#fff"/><path d="M17 13.2c-2.6-1.8-3.8-3.2-3.8-4.6 0-1.1.9-2 2-2 .7 0 1.4.4 1.8 1 .4-.6 1.1-1 1.8-1 1.1 0 2 .9 2 2 0 1.4-1.2 2.8-3.8 4.6z" fill="#fff" opacity=".85"/>',
    arrowHeart: '<path d="M12 19.5C6 15.4 3.8 12.3 3.8 9.4 3.8 7 5.7 5.1 8 5.1c1.6 0 3 .8 4 2.1 1-1.3 2.4-2.1 4-2.1 2.3 0 4.2 1.9 4.2 4.3 0 2.9-2.2 6-8.2 10.1z" fill="#fff"/><path d="M3 20L21 4M17.5 4H21v3.5M3 20h3M3 20v-3" stroke="#fff" stroke-width="1.8" stroke-linecap="round" fill="none"/>',
    box: '<path d="M4 8l8-4 8 4v8.5l-8 4-8-4z" fill="#fff"/><path d="M4 8l8 4 8-4M12 12v8.5" stroke="var(--bc)" stroke-width="1.4" fill="none" stroke-linejoin="round"/>',
    cabinet: '<rect x="6" y="3.5" width="12" height="17" rx="1.5" fill="#fff"/><path d="M6 9.2h12M6 14.8h12" stroke="var(--bc)" stroke-width="1.4"/><rect x="10.2" y="5.6" width="3.6" height="1.6" rx=".8" fill="var(--bc)"/><rect x="10.2" y="11.2" width="3.6" height="1.6" rx=".8" fill="var(--bc)"/><rect x="10.2" y="16.8" width="3.6" height="1.6" rx=".8" fill="var(--bc)"/>',
    temple: '<path d="M3 8.5L12 3.5l9 5z" fill="#fff"/><rect x="3.5" y="18" width="17" height="2.5" rx="1" fill="#fff"/><rect x="5.5" y="9.6" width="2.4" height="7.4" fill="#fff"/><rect x="10.8" y="9.6" width="2.4" height="7.4" fill="#fff"/><rect x="16.1" y="9.6" width="2.4" height="7.4" fill="#fff"/>',
    tag: '<path d="M3.5 11.6V4.5c0-.6.4-1 1-1h7.1c.3 0 .5.1.7.3l8.5 8.5c.4.4.4 1 0 1.4l-7.1 7.1c-.4.4-1 .4-1.4 0L3.8 12.3c-.2-.2-.3-.4-.3-.7z" fill="#fff"/><circle cx="8" cy="8" r="1.8" fill="var(--bc)"/>',
    wand: '<path d="M4 20L15 9" stroke="#fff" stroke-width="2.6" stroke-linecap="round"/><path d="M17 2.5l1 2.6 2.6 1-2.6 1L17 9.7l-1-2.6-2.6-1 2.6-1z" fill="#fff"/><circle cx="20.5" cy="12" r="1.1" fill="#fff"/><circle cx="11" cy="4" r=".9" fill="#fff"/>',
    folders: '<path d="M3 7.5c0-.8.7-1.5 1.5-1.5h4.6l2 2.2h8.4c.8 0 1.5.7 1.5 1.5v8.8c0 .8-.7 1.5-1.5 1.5h-15C3.7 20 3 19.3 3 18.5z" fill="#fff"/><path d="M3 11h18" stroke="var(--bc)" stroke-width="1.3"/>',
    books: '<rect x="4" y="15.5" width="16" height="4.5" rx="1" fill="#fff"/><rect x="5.5" y="10" width="13.5" height="4.5" rx="1" fill="#fff"/><rect x="4.5" y="4.5" width="14.5" height="4.5" rx="1" fill="#fff"/><path d="M7 15.5v4.5M8.5 10v4.5M16 4.5v4.5" stroke="var(--bc)" stroke-width="1.2"/>',
    hourglass: '<path d="M6 3h12M6 21h12" stroke="#fff" stroke-width="2.2" stroke-linecap="round"/><path d="M7.5 3.5v2.6c0 2.2 2.6 4 4.5 5.9 1.9-1.9 4.5-3.7 4.5-5.9V3.5zM7.5 20.5v-2.6c0-2.2 2.6-4 4.5-5.9 1.9 1.9 4.5 3.7 4.5 5.9v2.6z" fill="#fff"/><path d="M10 19.2h4l-2-2.2z" fill="var(--bc)"/>',
    hundred: '<text x="12" y="16.6" font-family="Segoe UI, Arial" font-weight="900" font-size="10.5" fill="#fff" text-anchor="middle">100</text><path d="M4 19h16" stroke="#fff" stroke-width="1.8" stroke-linecap="round"/>',
    bolt: '<path d="M13.5 2.5L5 13.5h6l-1.5 8 8.5-11h-6z" fill="#fff"/>',
    lotus: '<path d="M12 4c2 2.4 3 5 3 7.5S14 16 12 17c-2-1-3-3-3-5.5S10 6.4 12 4z" fill="#fff"/><path d="M12 17c-3.8 0-7-2-8.5-5.5 3 .2 5.5 1.4 7 3.4M12 17c3.8 0 7-2 8.5-5.5-3 .2-5.5 1.4-7 3.4" fill="#fff"/><path d="M5 19.5h14" stroke="#fff" stroke-width="1.8" stroke-linecap="round"/>',
    moon: '<path d="M15.5 3.6A8.5 8.5 0 1 0 20.4 16a7 7 0 0 1-4.9-12.4z" fill="#fff"/><path d="M18.5 4.5l.5 1.3 1.3.5-1.3.5-.5 1.3-.5-1.3-1.3-.5 1.3-.5z" fill="#fff"/>',
    sunrise: '<path d="M6 16a6 6 0 0 1 12 0z" fill="#fff"/><path d="M12 4.5v3M5 8l2 2M19 8l-2 2M2.5 15.2h3M18.5 15.2h3M3 19h18" stroke="#fff" stroke-width="1.8" stroke-linecap="round"/>',
    flame: '<path d="M12 21c-4 0-6.5-2.7-6.5-6.3 0-3.4 2.5-5.4 3.6-8.7.9 1.6 1.4 2.9 1.5 4.3 1.7-1.6 2.5-4 2.3-7.3 3.6 2.6 5.6 6.6 5.6 11.4 0 3.8-2.6 6.6-6.5 6.6z" fill="#fff"/><path d="M12 21c-1.8 0-3-1.2-3-2.9 0-1.6 1.3-2.6 2-4.2.6.9 1 1.6 1 2.6.9-.6 1.4-1.6 1.5-2.8 1.1 1.2 1.6 2.8 1.6 4.4 0 1.7-1.3 2.9-3.1 2.9z" fill="var(--bc)"/>',
    volcano: '<path d="M2.5 20.5L9 9.5h6l6.5 11z" fill="#fff"/><path d="M10 9.5c-1.5-2-1-4 .8-5 .2 1.6 1.2 2.4 1.2 2.4s.6-1.8 2.4-2.6c-.3 2 .8 3.4-.4 5.2" fill="#fff"/><path d="M9.2 13.5l1.8 1.2 1.4-1.4 1.6 1.4 1.2-1" stroke="var(--bc)" stroke-width="1.3" fill="none" stroke-linejoin="round"/>',
    calendar: '<rect x="3.5" y="5" width="17" height="15.5" rx="2.4" fill="#fff"/><path d="M3.5 9.5h17" stroke="var(--bc)" stroke-width="1.6"/><path d="M8 3v4M16 3v4" stroke="#fff" stroke-width="2.2" stroke-linecap="round"/><path d="M8 14.6l2.6 2.5 5.4-5" stroke="var(--bc)" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round"/>',
    film: '<rect x="3" y="4" width="18" height="16" rx="2" fill="#fff"/><path d="M7 4v16M17 4v16" stroke="var(--bc)" stroke-width="1.4"/><path d="M4.6 7h1M4.6 10.5h1M4.6 14h1M4.6 17.5h1M18.4 7h1M18.4 10.5h1M18.4 14h1M18.4 17.5h1" stroke="var(--bc)" stroke-width="1.4" stroke-linecap="round"/><path d="M10.4 9.4l4.2 2.6-4.2 2.6z" fill="var(--bc)"/>',
    rocket: '<path d="M12 2.5c3.5 2.3 5 6 4.2 10.8l-1.7 3.2h-5l-1.7-3.2C7 8.5 8.5 4.8 12 2.5z" fill="#fff"/><circle cx="12" cy="9" r="1.9" fill="var(--bc)"/><path d="M7.8 13.3l-2.6 3.5 3.3-.3M16.2 13.3l2.6 3.5-3.3-.3" fill="#fff"/><path d="M10.5 18.5c.4 1.2.9 2.1 1.5 3 .6-.9 1.1-1.8 1.5-3" fill="#ffd36b"/>',
    compass: '<circle cx="12" cy="12" r="8.5" fill="#fff"/><path d="M15.6 8.4l-2.2 5-5 2.2 2.2-5z" fill="var(--bc)"/><circle cx="12" cy="12" r="1.1" fill="#fff"/>',
    gem: '<path d="M6.5 4h11l3.5 5-9 11-9-11z" fill="#fff"/><path d="M3 9h18M8.5 4L12 20M15.5 4L12 20M6.5 4l2 5M17.5 4l-2 5" stroke="var(--bc)" stroke-width="1.1" fill="none" stroke-linejoin="round"/>',
    clapper: '<rect x="3" y="9.5" width="18" height="11" rx="1.6" fill="#fff"/><path d="M3.4 9.2l16.5-4.6.9 3.1L4.3 12.3z" fill="#fff"/><path d="M7 7.9l2 3M12 6.5l2 3M17 5.1l2 3" stroke="var(--bc)" stroke-width="1.5"/>',
    play: '<circle cx="12" cy="12" r="8.7" fill="#fff"/><path d="M10 8l6 4-6 4z" fill="var(--bc)"/>',
    sparkle: '<path d="M12 2.5l2.2 6.5 6.6 2.4-6.6 2.4L12 20.3l-2.2-6.5-6.6-2.4 6.6-2.4z" fill="#fff"/><path d="M19 15.5l.8 2 2 .8-2 .8-.8 2-.8-2-2-.8 2-.8z" fill="#fff" opacity=".85"/>',
    headphones: '<path d="M4.5 15v-2.5a7.5 7.5 0 0 1 15 0V15" stroke="#fff" stroke-width="2.2" fill="none"/><rect x="3" y="13.5" width="5" height="7.5" rx="2" fill="#fff"/><rect x="16" y="13.5" width="5" height="7.5" rx="2" fill="#fff"/>',
    spiral: '<path d="M12 12.2a1.4 1.4 0 1 1 1.4-1.4 3.2 3.2 0 1 1-4.2 3.1 5 5 0 1 1 7.6-5.3 6.8 6.8 0 1 1-11.8 3.8" stroke="#fff" stroke-width="2" fill="none" stroke-linecap="round"/>',
    crown: '<path d="M3.5 8l4.3 3.6L12 5l4.2 6.6L20.5 8l-1.8 10.5H5.3z" fill="#fff"/><rect x="5.3" y="18.5" width="13.4" height="2.3" rx="1" fill="#fff"/><circle cx="12" cy="14.6" r="1.4" fill="var(--bc)"/>',
    pause: '<rect x="6.5" y="4.5" width="4" height="15" rx="1.4" fill="#fff"/><rect x="13.5" y="4.5" width="4" height="15" rx="1.4" fill="#fff"/>'
  };
  //          shape     colour     symbol
  const ART = {
    'first-love': ['circle', '#ff4f8b', 'heart'], smitten: ['circle', '#ff3d7f', 'hearts'], besotted: ['circle', '#e0245e', 'arrowHeart'],
    collector: ['hex', '#1cb0f6', 'box'], hoarder: ['hex', '#1899d6', 'cabinet'], archivist: ['hex', '#0b6fb0', 'temple'],
    curator: ['shield', '#ce82ff', 'tag'], 'tag-wizard': ['shield', '#a560e8', 'wand'], organised: ['shield', '#8549ba', 'folders'], collections: ['shield', '#b56cf0', 'books'],
    hours: ['circle', '#ff9600', 'hourglass'], hundred: ['circle', '#e07a00', 'hundred'], marathon: ['hex', '#ffc800', 'bolt'], disciplined: ['hex', '#2bc3a6', 'lotus'],
    'night-owl': ['circle', '#3c4bb8', 'moon'], 'early-bird': ['circle', '#ff9a52', 'sunrise'],
    devoted: ['shield', '#ff6a2b', 'flame'], obsessed: ['shield', '#e8410f', 'volcano'], regular: ['square', '#58cc02', 'calendar'],
    slideshow: ['square', '#ffb020', 'film'], 'full-throttle': ['hex', '#ff4b4b', 'rocket'],
    explorer: ['circle', '#58cc02', 'compass'], digger: ['hex', '#2fae6a', 'gem'],
    'film-buff': ['square', '#5b5fc7', 'clapper'], animated: ['circle', '#7b6cf6', 'play'], original: ['shield', '#ff4f9a', 'sparkle'],
    audiophile: ['circle', '#00b5c4', 'headphones'], hypnotised: ['hex', '#9b51e0', 'spiral'], kegel: ['shield', '#f5b400', 'crown'], edge: ['square', '#ff4f9a', 'pause']
  };
  const SHAPES = {
    circle: 'M32 4a26 26 0 1 1 0 52a26 26 0 1 1 0-52z',
    hex: 'M32 3.5l24.5 13.4v26.2L32 56.5 7.5 43.1V16.9z',
    shield: 'M32 4l22 7.6v15.6C54 41 44.6 51.2 32 56c-12.6-4.8-22-15-22-28.8V11.6z',
    square: 'M18 5h28c7.2 0 13 5.8 13 13v24c0 7.2-5.8 13-13 13H18c-7.2 0-13-5.8-13-13V18C5 10.8 10.8 5 18 5z'
  };
  const shade = (hex, k) => { const n = parseInt(hex.slice(1), 16), c = x => Math.max(0, Math.min(255, Math.round(x * k))); return `rgb(${c(n >> 16)},${c(n >> 8 & 255)},${c(n & 255)})`; };
  function badgeArt(id, on) {
    const [shape, colour, glyph] = ART[id] || ['circle', '#888', 'sparkle'];
    const c = on ? colour : '#8f8a90', d = SHAPES[shape];
    return `<svg viewBox="0 0 64 66" aria-hidden="true" style="--bc:${shade(c, .72)}">
      <path d="${d}" transform="translate(0 5)" fill="${shade(c, .62)}"/>
      <path d="${d}" fill="${c}"/>
      <path d="${d}" fill="url(#bgloss)" opacity=".55"/>
      <path d="${d}" fill="none" stroke="rgba(255,255,255,.35)" stroke-width="2"/>
      <g transform="translate(16 14) scale(1.333)">${G[glyph]}</g>
      <defs><linearGradient id="bgloss" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff" stop-opacity=".55"/><stop offset=".45" stop-color="#fff" stop-opacity="0"/></linearGradient></defs></svg>`;
  }
  let checking = false;
  async function checkBadges() {
    if (checking || !library) return;
    checking = true;
    try {
      const S = statsNow(), fresh = [];
      for (const b of BADGES) if (!profile.unlocked[b.id] && b.test(S) >= 1) { profile.unlocked[b.id] = Date.now(); fresh.push(b); }
      if (fresh.length) {
        await api('kvSet', 'profile', profile).catch(() => {});
        fresh.forEach((b, k) => setTimeout(() => celebrate(b), k * 2600));
        drawMe();
      }
    } finally { checking = false; }
  }
  function celebrate(b) {
    const n = make('div', 'badge-toast');
    const ico = make('span', 'badge-toast-ico'); ico.innerHTML = badgeArt(b.id, true);
    n.append(ico, make('div', '', ''));
    n.lastChild.append(make('b', '', `Badge earned: ${b.name}`), make('span', '', b.desc));
    document.body.append(n);
    setTimeout(() => n.classList.add('out'), 4200);
    setTimeout(() => n.remove(), 4800);
  }

  // =====================================================================================
  // The profile page
  // =====================================================================================
  const tab = make('section', 'hidden');
  tab.id = 'tab-profile';
  document.querySelector('main').append(tab);
  const saveProfile = () => api('kvSet', 'profile', profile).catch(e => toast(errMsg(e), 'err'));
  async function openProfile(focus) {
    await loaded;
    document.querySelectorAll('nav button').forEach(x => { x.classList.remove('on'); const t = $('tab-' + x.dataset.tab); if (t) t.classList.add('hidden'); });
    stopGrid();
    tab.classList.remove('hidden');
    document.querySelector('.tb-title').textContent = 'You';
    drawProfile();
    checkBadges().then(() => { if (!tab.classList.contains('hidden')) drawProfile(); });
    if (focus === 'sessions') setTimeout(() => { const d = tab.querySelector('.pf-sessions'); if (d) { d.open = true; d.scrollIntoView({ behavior: 'smooth' }); } }, 50);
  }
  window.vaultProfile = { open: openProfile, get: () => profile, save: saveProfile, ready: loaded };
  // any sidebar tile leaves it
  document.querySelector('nav').addEventListener('click', () => tab.classList.add('hidden'), true);
  function media(it, moving, cls) {
    if (!it) return null;
    if (moving && it.previewId) { const v = make('video', cls); Object.assign(v, { muted: true, loop: true, autoplay: true, playsInline: true, src: vaultUrl(it.previewId) }); return v; }
    const im = make('img', cls); im.src = thumbSrc(it, 960); im.alt = ''; return im;
  }
  // your time looking, minutes per day over the last 60 days: a line with a soft fill, the day on hover
  function timeGraph() {
    const N = 60, vals = [], labels = [];
    for (let k = N - 1; k >= 0; k--) { const d = new Date(); d.setDate(d.getDate() - k); vals.push(((daily[dayKey(d)] || {}).secs || 0) / 60); labels.push(d.toLocaleDateString(undefined, { day: 'numeric', month: 'short' })); }
    const W = 600, H = 120, max = Math.max(10, ...vals), x = k => k / (N - 1) * W, y = v => H - 6 - v / max * (H - 16);
    const pts = vals.map((v, k) => `${x(k).toFixed(1)},${y(v).toFixed(1)}`);
    const wrap = make('div', 'pf-graph');
    wrap.append(make('span', 'pf-ov-label', 'Minutes a day · last 60 days'));
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('viewBox', `0 0 ${W} ${H + 16}`); svg.setAttribute('preserveAspectRatio', 'none'); svg.classList.add('pf-graph-svg');
    svg.innerHTML = `<defs><linearGradient id="pfFill" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="currentColor" stop-opacity=".35"/><stop offset="1" stop-color="currentColor" stop-opacity="0"/></linearGradient></defs>
      <line class="pf-axis" x1="0" x2="${W}" y1="${H - 6}" y2="${H - 6}"/>
      <path class="pf-graph-fill" d="M0,${H - 6} L${pts.join(' L')} L${W},${H - 6} Z" fill="url(#pfFill)"/>
      <polyline class="pf-graph-line" points="${pts.join(' ')}"/>
      <circle class="pf-graph-end" cx="${x(N - 1)}" cy="${y(vals[N - 1])}" r="4"/>
      <text class="pf-label" x="0" y="${H + 12}">${labels[0]}</text><text class="pf-label" x="${W}" y="${H + 12}" text-anchor="end">Today</text>`;
    // hover: the nearest day
    const dot = document.createElementNS('http://www.w3.org/2000/svg', 'circle'); dot.setAttribute('r', '4'); dot.setAttribute('class', 'pf-graph-dot'); dot.style.display = 'none';
    svg.append(dot);
    svg.addEventListener('pointermove', e => {
      const r = svg.getBoundingClientRect(), k = Math.max(0, Math.min(N - 1, Math.round((e.clientX - r.left) / r.width * (N - 1))));
      dot.setAttribute('cx', x(k)); dot.setAttribute('cy', y(vals[k])); dot.style.display = '';
      showTip(e, `${labels[k]}: ${vals[k] < 1 ? 'not used' : Math.round(vals[k]) + ' min'}`);
    });
    svg.addEventListener('pointerleave', () => { dot.style.display = 'none'; hideTip(); });
    wrap.append(svg);
    return wrap;
  }
  function drawProfile() {
    const S = statsNow(), xp = xpOf(S), L = levelOf(xp);
    const lib = library || [], byId = id => lib.find(i => i.id === id);
    const titles = TITLES.filter(([lv]) => lv <= L.level).map(([, t]) => t);
    if (!profile.title || !titles.includes(profile.title)) profile.title = titles[titles.length - 1];
    tab.replaceChildren();
    // banner
    const banner = make('div', 'pf-banner');
    const bItem = profile.bannerId && byId(profile.bannerId);
    if (bItem) banner.append(media(bItem, profile.bannerMoving, 'pf-banner-media'));
    else banner.style.background = (BANNERS.find(b => b[0] === profile.banner) || BANNERS[0])[1];
    const bBtn = make('button', 'btn pf-edit', 'Change banner'); bBtn.onclick = () => chooseBanner(L.level);
    banner.append(make('span', 'pf-banner-shine'));
    banner.append(bBtn);
    // who
    const who = make('div', 'pf-who');
    const pictureOpen = L.level >= UNLOCKS.picture;
    const av = make('button', 'pf-avatar' + (pictureOpen ? '' : ' locked'));
    av.title = pictureOpen ? 'Change your picture' : `Your own picture comes at level ${UNLOCKS.picture}`;
    const aItem = profile.avatarId && byId(profile.avatarId);
    if (aItem) { const m = media(aItem, profile.avatarMoving); place(m); av.append(m); }
    else av.append(make('span', 'pf-avatar-heart', '♥'));
    if (!pictureOpen) av.append(make('span', 'pf-lock', `🔒 Level ${UNLOCKS.picture}`));
    av.onclick = () => {
      if (!pictureOpen) return toast(`Your own picture comes at level ${UNLOCKS.picture}. Keep going!`, 'ok');
      pick('Your picture', (it, moving) => {
        if (moving && L.level < UNLOCKS.moving) { toast(`Moving pictures come at level ${UNLOCKS.moving}: a still one for now`, 'ok'); moving = false; }
        positionPicture(it, moving, pos => { profile.avatarId = it.id; profile.avatarMoving = moving; profile.avatarPos = pos; saveProfile(); drawProfile(); drawMe(); });
      }, { moving: L.level >= UNLOCKS.moving });
    };
    const names = make('div', 'pf-names');
    const name = make('input', 'pf-name'); Object.assign(name, { value: profile.name || 'My vault', spellcheck: false, title: 'Your vault\'s name' });
    name.onchange = () => { profile.name = name.value.trim().slice(0, 40) || 'My vault'; saveProfile(); };
    const title = make('select', 'pf-title');
    titles.forEach(t => title.add(new Option(t, t))); title.value = profile.title;
    title.onchange = () => { profile.title = title.value; saveProfile(); };
    const lvl = make('div', 'pf-level');
    const ring = make('div', 'pf-ring'); ring.style.setProperty('--p', (L.into / L.need * 100).toFixed(1));
    ring.append(make('b', '', String(L.level)));
    const xpText = make('div', 'pf-xp');
    const xpBar = make('div', 'pf-xpbar'); xpBar.append(make('i')); xpBar.firstChild.style.width = (L.into / L.need * 100).toFixed(1) + '%';
    xpText.append(make('b', '', `Level ${L.level}`), make('span', '', `${fmtN(L.into)} / ${fmtN(L.need)} XP to level ${L.level + 1} · ${fmtN(xp)} XP in all`), xpBar);
    lvl.append(ring, xpText);
    names.append(name, title, lvl);
    who.append(av, names);
    // the overview: headline numbers and your time as a graph on the left, every number as a list on the right
    const got = BADGES.filter(b => profile.unlocked[b.id]).length;
    const ov = make('section', 'pf-overview');
    const main = make('div', 'pf-ov-main');
    const heads = make('div', 'pf-ov-heads');
    for (const [k, v] of [['Time looking', fmtLen(S.look)], ['Items', fmtN(S.items)], ['Streak', `${S.streak} ${S.streak === 1 ? 'day' : 'days'}`]]) {
      const h = make('div', 'pf-ov-head'); h.append(make('span', '', k), make('b', '', v)); heads.append(h);
    }
    main.append(heads, timeGraph());
    // the three best badges you have (the later ones in the list are the harder ones), the rest folded below
    const earned = BADGES.map((b, k) => ({ b, k, on: profile.unlocked[b.id] })).filter(x => x.on).sort((a, c) => c.k - a.k).slice(0, 3);
    const top = make('div', 'pf-topbadges');
    top.append(make('span', 'pf-ov-label', 'Best badges'));
    if (!earned.length) top.append(make('span', 'pf-topbadge-none', 'None yet: keep going!'));
    for (const { b, on } of earned) {
      const c = make('div', 'pf-topbadge'); c.title = `${b.desc} · earned ${new Date(on).toLocaleDateString()}`;
      const ico = make('span', 'pf-badge-ico'); ico.innerHTML = badgeArt(b.id, true);
      c.append(ico, make('b', '', b.name));
      top.append(c);
    }
    main.append(top);
    const list = make('dl', 'pf-ov-list');
    const row = (k, v) => { list.append(make('dt', '', k), make('dd', '', v)); };
    row('Items', fmtN(S.items)); row('Time looking', fmtLen(S.look)); row('Time in the app', fmtLen(S.appTime)); row('Longest session', fmtLen(S.longest));
    row('Streak', `${S.streak} ${S.streak === 1 ? 'day' : 'days'} (best ${S.bestStreak})`); row('Appreciations', fmtN(S.likes)); row('Favourite', S.favourite || '–');
    row('Slideshows', fmtLen(S.showSecs)); row('Badges', `${got} of ${BADGES.length}`); row('XP', fmtN(xp));
    ov.append(main, list);
    // every badge, folded away (it remembers if you left it open)
    const all = make('details', 'pf-allbadges');
    try { all.open = localStorage.getItem('imageVault.fold.badges') === '1'; } catch (e) {}
    all.addEventListener('toggle', () => { try { localStorage.setItem('imageVault.fold.badges', all.open ? '1' : '0'); } catch (e) {} });
    all.append(make('summary', '', `All badges · ${got} of ${BADGES.length}`));
    const badges = make('div', 'pf-badges');
    for (const b of BADGES) {
      const on = profile.unlocked[b.id], p = Math.min(1, b.test(S));
      const c = make('div', 'pf-badge' + (on ? ' on' : ''));
      const ico = make('span', 'pf-badge-ico'); ico.innerHTML = badgeArt(b.id, !!on);
      c.append(ico, make('b', '', b.name), make('span', 'pf-badge-desc', b.desc));
      if (!on) { const pb = make('div', 'pf-badge-bar'); pb.append(make('i')); pb.firstChild.style.width = (p * 100).toFixed(0) + '%'; c.append(pb); }
      else c.title = 'Earned ' + new Date(on).toLocaleDateString();
      badges.append(c);
    }
    all.append(badges);
    tab.append(banner, who, ov, road(L.level, L), all);
    if (window.vaultShare) window.vaultShare.section(tab, { name: profile.name || 'My vault', level: L.level, title: profile.title, avatarId: profile.avatarId });
  }
  function chooseBanner(level) {
    const box = make('div', 'dc-sheet');
    const card = make('div', 'dc-sheet-card');
    card.innerHTML = '<div class="dc-sheet-head"><h2>Your banner</h2><button class="btn" type="button">Close</button></div><div class="dc-sheet-body"></div>';
    card.querySelector('.btn').onclick = () => box.remove();
    const body = card.querySelector('.dc-sheet-body');
    body.append(make('p', 'dc-sheet-lead', `A colour (more come with levels), or, from level ${UNLOCKS.vaultBanner}, any picture or clip from your vault.`));
    const row = make('div', 'pf-banners');
    for (const [id, bg, lv] of BANNERS) {
      const b = make('button', 'pf-bannerpick' + (lv > level ? ' locked' : ''));
      b.style.background = bg; b.title = lv > level ? `Comes at level ${lv}` : id;
      if (lv > level) b.append(make('span', '', `Level ${lv}`));
      b.onclick = () => { if (lv > level) return; profile.banner = id; profile.bannerId = ''; saveProfile(); box.remove(); drawProfile(); };
      row.append(b);
    }
    const vaultOpen = level >= UNLOCKS.vaultBanner;
    const fromVault = make('button', 'btn primary' + (vaultOpen ? '' : ' locked'), vaultOpen ? 'Choose from your vault…' : `🔒 From your vault at level ${UNLOCKS.vaultBanner}`);
    fromVault.disabled = !vaultOpen;
    fromVault.onclick = () => { box.remove(); pick('Your banner', (it, moving) => { profile.bannerId = it.id; profile.bannerMoving = moving && level >= UNLOCKS.moving; saveProfile(); drawProfile(); }, { moving: level >= UNLOCKS.moving }); };
    body.append(row, fromVault);
    box.append(card);
    document.body.append(box);
  }
  // pick something from the library (pictures, or moving clips)
  function pick(heading, done, opts = { moving: true }) {
    const box = make('div', 'dc-sheet');
    const card = make('div', 'dc-sheet-card');
    card.innerHTML = `<div class="dc-sheet-head"><h2>${heading}</h2><button class="btn" type="button">Close</button></div><div class="dc-sheet-body"></div>`;
    card.querySelector('.btn').onclick = () => box.remove();
    const body = card.querySelector('.dc-sheet-body');
    const bar = make('div', 'pf-pickbar');
    const cat = make('select'); cat.add(new Option('All categories', ''));
    [...new Set(library.map(i => i.category))].sort().forEach(c => cat.add(new Option(c, c)));
    const order = make('select'); [['views', 'Most looked at'], ['new', 'Newest'], ['random', 'Random']].forEach(([v, l]) => order.add(new Option(l, v)));
    const moving = make('label', 'pf-moving'); const mv = make('input'); mv.type = 'checkbox'; moving.append(mv, make('span', '', 'Moving (videos and GIFs play)'));
    if (!opts.moving) { mv.disabled = true; moving.title = `Moving pictures come at level ${UNLOCKS.moving}`; moving.classList.add('locked'); }
    bar.append(cat, order, moving);
    const grid = make('div', 'pf-pickgrid');
    const draw = () => {
      let list = library.filter(i => (!cat.value || i.category === cat.value) && (!mv.checked || i.previewId));
      if (order.value === 'views') list.sort((a, b) => timeOf(b.id) + viewsOf(b.id) * 10 - timeOf(a.id) - viewsOf(a.id) * 10);
      else if (order.value === 'new') list.sort((a, b) => b.added.localeCompare(a.added));
      else list.sort(() => Math.random() - 0.5);
      grid.replaceChildren(...list.slice(0, 240).map(it => {
        const b = make('button', 'pf-pick');
        const im = make('img'); im.loading = 'lazy'; im.src = thumbSrc(it, 240); im.alt = '';
        b.append(im);
        if (it.kind !== 'image') b.append(make('span', 'pf-pick-kind', '▶'));
        b.onclick = () => { box.remove(); done(it, mv.checked && !!it.previewId); };
        return b;
      }));
    };
    cat.onchange = order.onchange = mv.onchange = draw;
    draw();
    body.append(bar, grid);
    box.append(card);
    box.addEventListener('pointerdown', e => { if (e.target === box) box.remove(); });
    document.body.append(box);
  }

  // the road: each level a stop, with what it brings (a title, a colour, your picture...); you're the
  // glowing one, and the next unlock shows what's coming
  function road(level, L) {
    const rewards = {};
    const add = (lv, icon, text) => (rewards[lv] = rewards[lv] || []).push([icon, text]);
    TITLES.forEach(([lv, t]) => add(lv, '🏷️', `Title: ${t}`));
    BANNERS.forEach(([id, , lv]) => add(lv, '🎨', `Banner: ${id}`));
    add(UNLOCKS.picture, '🖼️', 'Your own picture');
    add(UNLOCKS.vaultBanner, '🌄', 'Banners from your vault');
    add(UNLOCKS.moving, '🎞️', 'Moving pictures');
    const last = Math.max(level + 6, ...Object.keys(rewards).map(Number).filter(n => n <= level + 12));
    const wrap = make('section', 'pf-road');
    const next = Object.keys(rewards).map(Number).sort((a, b) => a - b).find(n => n > level);
    wrap.append(make('h3', 'pf-h', next ? `Your road · next unlock at level ${next}` : 'Your road'));
    const track = make('div', 'pf-road-track');
    for (let lv = Math.max(1, level - 3); lv <= last; lv++) {
      const node = make('div', 'pf-stop' + (lv < level ? ' done' : lv === level ? ' here' : '') + (rewards[lv] ? ' gift' : ''));
      const dot = make('span', 'pf-stop-dot');
      dot.append(make('b', '', lv < level ? '✓' : String(lv)));
      if (lv === level) { const ring = make('span', 'pf-stop-ring'); ring.style.setProperty('--p', (L.into / L.need * 100).toFixed(1)); dot.append(ring); }
      node.append(dot);
      const what = make('div', 'pf-stop-what');
      what.append(make('small', '', lv === level ? 'You are here' : `Level ${lv}`));
      for (const [icon, text] of rewards[lv] || []) what.append(make('span', '', `${icon} ${text}`));
      node.append(what);
      track.append(node);
    }
    wrap.append(track);
    setTimeout(() => { const here = track.querySelector('.here'); if (here) track.scrollLeft = here.offsetLeft - 120; }, 0);
    return wrap;
  }
  // placing the picture in its circle: drag it, zoom it, see it as it will look
  function positionPicture(it, moving, done) {
    const box = make('div', 'dc-sheet');
    const card = make('div', 'dc-sheet-card pf-place');
    card.innerHTML = '<div class="dc-sheet-head"><h2>Place your picture</h2><button class="btn" type="button">Cancel</button></div><div class="dc-sheet-body"></div>';
    card.querySelector('.btn').onclick = () => box.remove();
    const body = card.querySelector('.dc-sheet-body');
    const pos = { x: 50, y: 40, z: 1, ...(profile.avatarId === it.id ? profile.avatarPos : {}) };
    const circle = make('div', 'pf-place-circle');
    const m = media(it, moving);
    circle.append(m);
    const small = make('div', 'pf-place-small'); const m2 = media(it, moving); small.append(m2);
    const draw = () => { for (const el of [m, m2]) { el.style.objectPosition = `${pos.x}% ${pos.y}%`; el.style.transform = pos.z > 1 ? `scale(${pos.z})` : ''; el.style.transformOrigin = `${pos.x}% ${pos.y}%`; } };
    circle.addEventListener('pointerdown', e => {
      circle.setPointerCapture(e.pointerId);
      let x = e.clientX, y = e.clientY;
      const move = ev => { pos.x = Math.max(0, Math.min(100, pos.x - (ev.clientX - x) / 2.2 / pos.z)); pos.y = Math.max(0, Math.min(100, pos.y - (ev.clientY - y) / 2.2 / pos.z)); x = ev.clientX; y = ev.clientY; draw(); };
      const up = () => { circle.removeEventListener('pointermove', move); circle.removeEventListener('pointerup', up); };
      circle.addEventListener('pointermove', move); circle.addEventListener('pointerup', up);
    });
    circle.addEventListener('wheel', e => { e.preventDefault(); pos.z = Math.max(1, Math.min(3, pos.z * (e.deltaY < 0 ? 1.08 : 1 / 1.08))); zoom.value = pos.z; draw(); }, { passive: false });
    const zoom = make('input', 'pf-place-zoom'); Object.assign(zoom, { type: 'range', min: 1, max: 3, step: 0.01, value: pos.z });
    zoom.oninput = () => { pos.z = +zoom.value; draw(); };
    const row = make('div', 'pf-place-row');
    row.append(make('span', '', 'Zoom'), zoom, small);
    const ok = make('button', 'btn primary', 'Use this');
    ok.onclick = () => { box.remove(); done({ x: Math.round(pos.x * 10) / 10, y: Math.round(pos.y * 10) / 10, z: Math.round(pos.z * 100) / 100 }); };
    body.append(make('p', 'dc-sheet-lead', 'Drag the picture to place it in the circle; the wheel or the slider zooms.'), circle, row, ok);
    draw();
    box.append(card);
    document.body.append(box);
  }

  // =====================================================================================
  // The Stats page: your time
  // =====================================================================================
  const statsTab = $('tab-stats');
  const yours = make('section', 'pf-stats');
  statsTab.prepend(yours);
  const tip = make('div', 'pf-tip hidden');
  document.body.append(tip);
  const showTip = (e, text) => { tip.textContent = text; tip.classList.remove('hidden'); tip.style.left = e.clientX + 'px'; tip.style.top = (e.clientY - 12) + 'px'; };
  const hideTip = () => tip.classList.add('hidden');
  function bars(values, labels, fmtv) {
    // one series: no legend; thin bars, rounded at the top, a recessive axis; the value on hover
    const W = 640, H = 150, max = Math.max(1, ...values), bw = W / values.length;
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('viewBox', `0 0 ${W} ${H + 18}`); svg.classList.add('pf-chart');
    svg.innerHTML = `<line x1="0" x2="${W}" y1="${H}" y2="${H}" class="pf-axis"/>`;
    values.forEach((v, k) => {
      const h = v ? Math.max(3, v / max * (H - 10)) : 0, x = k * bw + bw * 0.18, w = bw * 0.64;
      const g = document.createElementNS('http://www.w3.org/2000/svg', 'g');
      g.innerHTML = `<rect class="pf-hit" x="${k * bw}" y="0" width="${bw}" height="${H}"/>` + (h ? `<path class="pf-bar" d="M${x} ${H} V${H - h + 3} q0 -3 3 -3 h${w - 6} q3 0 3 3 V${H} z"/>` : '');
      g.addEventListener('pointermove', e => showTip(e, `${labels[k]}: ${fmtv(v)}`));
      g.addEventListener('pointerleave', hideTip);
      svg.append(g);
    });
    [0, Math.floor(values.length / 2), values.length - 1].forEach(k => {
      const t = document.createElementNS('http://www.w3.org/2000/svg', 'text');
      t.setAttribute('x', k * bw + bw / 2); t.setAttribute('y', H + 14); t.setAttribute('class', 'pf-label'); t.setAttribute('text-anchor', k === 0 ? 'start' : k === values.length - 1 ? 'end' : 'middle');
      t.textContent = labels[k]; svg.append(t);
    });
    return svg;
  }
  function heatmap() {
    // the last 26 weeks, a square per day, darker = more minutes (one hue)
    const weeks = 26, start = new Date(); start.setDate(start.getDate() - start.getDay() - (weeks - 1) * 7);
    const vals = [];
    for (let k = 0; k < weeks * 7; k++) { const d = new Date(start); d.setDate(start.getDate() + k); vals.push([d, (daily[dayKey(d)] || {}).secs || 0]); }
    const max = Math.max(60, ...vals.map(v => v[1]));
    const grid = make('div', 'pf-heat');
    for (const [d, s] of vals) {
      const c = make('span', 'pf-cell');
      const step = s < 60 ? 0 : Math.min(4, 1 + Math.floor(s / max * 4));
      c.dataset.step = step;
      if (d > new Date()) c.classList.add('future');
      c.addEventListener('pointermove', e => showTip(e, `${d.toLocaleDateString(undefined, { weekday: 'short', day: 'numeric', month: 'short' })}: ${s < 60 ? 'not used' : fmtLen(s)}`));
      c.addEventListener('pointerleave', hideTip);
      grid.append(c);
    }
    const legend = make('div', 'pf-heat-legend');
    legend.append(make('span', '', 'Less'), ...[0, 1, 2, 3, 4].map(n => { const c = make('span', 'pf-cell'); c.dataset.step = n; return c; }), make('span', '', 'More'));
    const wrap = make('div', 'pf-heat-wrap'); wrap.append(grid, legend);
    return wrap;
  }
  function drawYourTime() {
    const days = [], labels = [];
    for (let k = 29; k >= 0; k--) { const d = new Date(); d.setDate(d.getDate() - k); days.push(Math.round(((daily[dayKey(d)] || {}).secs || 0) / 60)); labels.push(d.toLocaleDateString(undefined, { day: 'numeric', month: 'short' })); }
    const weeks = [], wl = [];
    for (let w = 11; w >= 0; w--) {
      let s = 0; const end = new Date(); end.setDate(end.getDate() - w * 7);
      for (let k = 0; k < 7; k++) { const d = new Date(end); d.setDate(end.getDate() - k); s += (daily[dayKey(d)] || {}).secs || 0; }
      weeks.push(Math.round(s / 360) / 10); const st = new Date(end); st.setDate(end.getDate() - 6); wl.push('Week of ' + st.toLocaleDateString(undefined, { day: 'numeric', month: 'short' }));
    }
    const S = statsNow();
    yours.replaceChildren();
    const head = make('div', 'pf-stats-head');
    head.append(make('h2', '', 'Your time'), make('span', '', `Today ${fmtLen((daily[dayKey()] || {}).secs || 0)} · streak ${S.streak} ${S.streak === 1 ? 'day' : 'days'} · ${fmtN(S.activeDays)} active days`));
    const a = make('div', 'chart pf-card'); a.append(make('h3', '', 'Minutes per day'), make('p', 'pf-sub', 'Active time in the app, the last 30 days'), bars(days, labels, v => `${v} min`));
    const b = make('div', 'chart pf-card'); b.append(make('h3', '', 'Hours per week'), make('p', 'pf-sub', 'The last 12 weeks'), bars(weeks, wl, v => `${v} h`));
    const c = make('div', 'chart pf-card pf-wide'); c.append(make('h3', '', 'The last half year'), make('p', 'pf-sub', 'Each square is a day'), heatmap());
    const ses = make('details', 'chart pf-card pf-wide pf-sessions');
    ses.append(make('summary', '', `Sessions (${fmtN(S.sessions)})`));
    sessions.slice(0, 30).forEach(s => { const r = make('div', 'pf-session'); const d = new Date(s.start); r.append(make('b', '', fmtLen(s.secs)), make('span', '', `${d.toLocaleDateString()} ${pad(d.getHours())}:${pad(d.getMinutes())} · ${s.views || 0} views`), make('span', 'pf-session-tags', (s.tags || []).map(x => x.replace(/^\w+:/, '').replace(/_/g, ' ')).join(', '))); ses.append(r); });
    const grid = make('div', 'pf-stats-grid'); grid.append(a, b, c, ses);
    yours.append(head, grid);
  }
  document.querySelector('nav [data-tab="stats"]').addEventListener('click', () => loaded.then(drawYourTime));
  loaded.then(() => { if (!statsTab.classList.contains('hidden')) drawYourTime(); });
})();
