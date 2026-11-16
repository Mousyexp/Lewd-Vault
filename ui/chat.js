// Lewd Vault Chat: the screens. Messages, pictures and rooms with other Lewd Vault users, end-to-end
// encrypted, with no account. The work (keys, encryption, relays, storage) is src-tauri/src/chat.rs;
// this file draws and polls it (api('chatState', rev) answers at once when nothing changed).
//
//   the list    a big title, search, rings (Nearby first, then favourites and recent people), chips
//               (All, Unread, Favourites, Rooms, Requests), Pinned and All chats, a "New chat" pill
//   a chat      bubbles (yours on the right), names in rooms, pictures as cards ("View once"),
//               reactions, reply, long-press for the menu, a composer with "+" for the vault
//   Nearby      the AirDrop-like sheet: phones close by, over Bluetooth (the radio is native: a bridge
//               the app puts on the page, window.LvBle on Android, webkit.messageHandlers.lvBle on iOS)
//
// First use asks for a nickname and an 18+ confirmation; nothing connects before that.
(() => {
  'use strict';
  if (!window.vaultUI || !inTauri) return;
  const { make, showTitle, TAB_KEY } = window.vaultUI;
  const html = document.documentElement;
  const isPhone = () => html.classList.contains('phone');
  const local = k => { try { return localStorage.getItem(k); } catch (e) { return null; } };
  const keep = (k, v) => { try { localStorage.setItem(k, v); } catch (e) {} };
  const ON = 'lv.chatOn';
  const REACTS = ['❤️', '😂', '😮', '🔥', '👍', '😢'];
  const esc = s => String(s == null ? '' : s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
  const haptic = kind => { try { if (window.vaultPhone && window.vaultPhone.haptic) window.vaultPhone.haptic(kind); } catch (e) {} };
  const media = mid => `${vaultOrigin()}/chat/${encodeURIComponent(mid)}`;

  // ---------- the sidebar tile and the page ----------
  const navBtn = make('button');
  navBtn.dataset.tab = 'chat';
  const badge = make('span', 'ch-navbadge hidden');
  navBtn.append(make('span', 'nt-title', 'Chat'), make('span', 'nt-sub', 'Private & rooms'), make('span', 'nt-art'), badge);
  (document.querySelector('nav [data-tab="discover"]') || document.querySelector('nav [data-tab="library"]')).after(navBtn);
  const tab = make('section', 'hidden');
  tab.id = 'tab-chat';
  document.querySelector('main').append(tab);
  navBtn.onclick = () => {
    document.querySelectorAll('nav button').forEach(x => { x.classList.toggle('on', x === navBtn); const t = $('tab-' + x.dataset.tab); if (t) t.classList.toggle('hidden', x !== navBtn); });
    if (typeof stopGrid === 'function') stopGrid();
    keep(TAB_KEY, 'chat');
    showTitle();
    enter();
  };
  const visible = () => !tab.classList.contains('hidden') && !document.hidden;
  new MutationObserver(() => { if (tab.classList.contains('hidden')) leave(); }).observe(tab, { attributes: true, attributeFilter: ['class'] });

  // ---------- state ----------
  let S = null, rev = 0, started = false, openId = '', msgs = [], msgSig = '', filter = 'all', query = '', replyTo = null;
  let timer = null;
  const conv = id => S && S.convs.find(c => c.id === id);

  // avatars: a colour from the id, the first letter of the name (rooms: #)
  function avatar(id, name, size, ring) {
    const a = make('span', 'ch-av' + (ring ? ' ring' : ''));
    let h = 0;
    for (const c of String(id)) h = (h * 31 + c.charCodeAt(0)) >>> 0;
    a.style.setProperty('--h', String(h % 360));
    if (size) a.style.setProperty('--s', size + 'px');
    const n = String(name || '?').replace(/^#/, '');
    a.textContent = String(id).startsWith('r:') ? '#' : (n.trim()[0] || '?').toUpperCase();
    return a;
  }
  const when = ms => {
    if (!ms) return '';
    const d = new Date(ms), now = new Date();
    const day = x => new Date(x.getFullYear(), x.getMonth(), x.getDate()).getTime();
    const diff = Math.round((day(now) - day(d)) / 86400000);
    if (diff === 0) return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
    if (diff === 1) return 'Yesterday';
    if (diff < 7) return d.toLocaleDateString([], { weekday: 'short' });
    return d.toLocaleDateString([], { day: 'numeric', month: 'short' });
  };
  const dayLabel = ms => {
    const d = new Date(ms), now = new Date();
    const diff = Math.round((new Date(now.getFullYear(), now.getMonth(), now.getDate()) - new Date(d.getFullYear(), d.getMonth(), d.getDate())) / 86400000);
    return diff === 0 ? 'Today' : diff === 1 ? 'Yesterday' : d.toLocaleDateString([], { weekday: 'long', day: 'numeric', month: 'long' });
  };

  // ---------- the first time: a nickname, and 18+ ----------
  function drawStart() {
    tab.innerHTML = `
      <div class="ch-start">
        <div class="ch-start-art" aria-hidden="true"><span class="b1"></span><span class="b2"></span><span class="b3"></span></div>
        <h1>Lewd Vault Chat</h1>
        <p class="ch-start-lead">Private messages, pictures from your vault and rooms, with other Lewd Vault users.</p>
        <ul class="ch-points">
          <li><b>No account.</b> No phone number or email. Your identity is a key made on this device.</li>
          <li><b>End-to-end encrypted.</b> Messages travel through public relays, sealed so only the people in the chat can read them.</li>
          <li><b>Nearby.</b> On phones, send to someone close by over Bluetooth, like AirDrop.</li>
        </ul>
        <label class="ch-field"><span>Your nickname</span><input class="ch-nick" maxlength="24" placeholder="What people see" autocomplete="off" spellcheck="false"></label>
        <label class="ch-adult"><input type="checkbox"><span>I'm 18 or over, and I'll only share what I'm allowed to share.</span></label>
        <button class="btn primary ch-go" type="button" disabled>Start chatting</button>
      </div>`;
    const nick = tab.querySelector('.ch-nick'), box = tab.querySelector('.ch-adult input'), go = tab.querySelector('.ch-go');
    const ok = () => { go.disabled = !(box.checked && nick.value.trim()); };
    nick.oninput = box.onchange = ok;
    go.onclick = async () => {
      go.disabled = true;
      try {
        await api('chatNick', nick.value.trim());
        keep(ON, '1');
        start();
      } catch (e) { toast(errMsg(e), 'error'); go.disabled = false; }
    };
  }

  // ---------- the frame: list and chat side by side (one at a time on a phone) ----------
  let F = null;
  function frame() {
    if (F && tab.contains(F.app)) return F;
    tab.innerHTML = `
      <div class="ch-app">
        <div class="ch-list">
          <div class="ch-head">
            <h1>Chats</h1>
            <div class="ch-head-btns">
              <button class="ch-ic ch-nearby-btn" type="button" title="Nearby" aria-label="Nearby"></button>
              <button class="ch-ic ch-qr-btn" type="button" title="Your code" aria-label="Your code"></button>
              <button class="ch-ic ch-gear" type="button" title="Chat settings" aria-label="Chat settings"></button>
            </div>
          </div>
          <label class="ch-search"><input type="search" placeholder="Search chats and rooms" spellcheck="false" autocomplete="off"></label>
          <div class="ch-rings"></div>
          <div class="ch-chips"></div>
          <div class="ch-rows"></div>
          <div class="ch-net"></div>
          <button class="ch-new" type="button"><b>＋</b> New chat</button>
        </div>
        <div class="ch-conv"><div class="ch-pick"><span class="ch-pick-art"></span><b>Your chats</b><span>Pick one, or start a new one.</span></div></div>
      </div>`;
    const q = s => tab.querySelector(s);
    F = { app: q('.ch-app'), rings: q('.ch-rings'), chips: q('.ch-chips'), rows: q('.ch-rows'), net: q('.ch-net'), conv: q('.ch-conv'), search: q('.ch-search input') };
    F.search.oninput = () => { query = F.search.value.trim().toLowerCase(); drawList(); };
    q('.ch-new').onclick = () => newChat();
    q('.ch-qr-btn').onclick = () => newChat('code');
    q('.ch-gear').onclick = settings;
    q('.ch-nearby-btn').onclick = nearby;
    return F;
  }

  function drawList() {
    if (!S) return;
    const f = frame();
    const convs = S.convs.slice().sort((a, b) => (b.at || 0) - (a.at || 0));
    const requests = convs.filter(c => c.extra && c.extra.request);
    // the rings: Nearby, then favourites and the people you talk to most recently
    f.rings.innerHTML = '';
    const ring = (id, name, ringOn, onTap, cls) => {
      const r = make('button', 'ch-ring' + (cls ? ' ' + cls : ''));
      r.type = 'button';
      r.append(id ? avatar(id, name, 54, ringOn) : make('span', 'ch-av ch-av-nearby ring'), make('span', 'ch-ring-name', name));
      r.onclick = onTap;
      f.rings.append(r);
    };
    ring('', 'Nearby', true, nearby, 'nearby');
    const people = convs.filter(c => c.kind === 'dm' && !(c.extra && c.extra.request));
    people.sort((a, b) => (b.extra.fav - a.extra.fav) || ((b.at || 0) - (a.at || 0)));
    for (const c of people.slice(0, 12)) ring(c.id, c.name, c.unread > 0, () => openChat(c.id));
    // the chips
    const unread = convs.filter(c => c.unread > 0).length;
    const chips = [['all', 'All'], ['unread', 'Unread', unread], ['fav', 'Favourites'], ['rooms', 'Rooms']];
    if (requests.length) chips.push(['req', 'Requests', requests.length]);
    f.chips.innerHTML = '';
    for (const [k, t, n] of chips) {
      const c = make('button', 'ch-chip' + (filter === k ? ' on' : ''), t);
      c.type = 'button';
      if (n) c.append(make('em', '', String(n)));
      c.onclick = () => { filter = k; drawList(); };
      f.chips.append(c);
    }
    // the rows
    let list = convs.filter(c => filter === 'all' ? true : filter === 'unread' ? c.unread > 0 : filter === 'fav' ? c.kind === 'dm' && c.extra.fav : filter === 'rooms' ? c.kind === 'room' : c.extra && c.extra.request);
    if (query) list = list.filter(c => (c.name + ' ' + c.last).toLowerCase().includes(query));
    f.rows.innerHTML = '';
    const section = (title, items) => {
      if (!items.length) return;
      f.rows.append(make('h3', 'ch-sec', title));
      for (const c of items) f.rows.append(row(c));
    };
    section('Pinned', list.filter(c => c.pinned));
    section(list.some(c => c.pinned) ? 'All chats' : '', list.filter(c => !c.pinned));
    if (!list.length) {
      const empty = make('div', 'ch-empty');
      if (!S.convs.length) {
        empty.innerHTML = '<span class="ch-empty-art"></span><b>No chats yet</b><span>Share your code with someone, add theirs, or join a room.</span>';
        const b = make('button', 'btn primary', 'Show my code'); b.type = 'button'; b.onclick = () => newChat('code');
        empty.append(b);
      } else empty.innerHTML = `<b>${query ? 'Nothing matches' : 'Nothing here'}</b>`;
      f.rows.append(empty);
    }
    // the connection
    const up = S.relays.filter(r => r.up).length;
    f.net.className = 'ch-net' + (up ? ' up' : '');
    f.net.textContent = up ? `Connected · ${up} of ${S.relays.length} relays` + (S.outbox ? ` · ${S.outbox} waiting to send` : '') : 'Connecting…' + (S.outbox ? ` · ${S.outbox} waiting to send` : '');
  }

  function row(c) {
    const r = make('button', 'ch-row' + (c.id === openId ? ' on' : '') + (c.unread ? ' unread' : ''));
    r.type = 'button';
    r.dataset.id = c.id;
    const mid = make('span', 'ch-row-mid');
    const top = make('span', 'ch-row-top');
    top.append(make('b', '', c.name));
    if (c.kind === 'room' && c.extra.locked) top.append(make('i', 'ch-lock', ''));
    if (c.muted) top.append(make('i', 'ch-muted', ''));
    mid.append(top, make('span', 'ch-row-last', c.extra && c.extra.request ? 'Wants to chat with you' : (c.last || (c.kind === 'room' ? 'Joined the room' : 'Say hi 👋'))));
    const side = make('span', 'ch-row-side');
    side.append(make('small', '', when(c.at)));
    if (c.unread) side.append(make('em', 'ch-count', c.unread > 99 ? '99+' : String(c.unread)));
    r.append(avatar(c.id, c.name, 50, c.unread > 0), mid, side);
    r.onclick = () => openChat(c.id);
    r.oncontextmenu = e => { e.preventDefault(); chatMenu(c, e.clientX, e.clientY); };
    hold(r, (x, y) => chatMenu(c, x, y));
    return r;
  }

  // a long press (touch) or right click
  function hold(el, fn) {
    let t = null, sx = 0, sy = 0;
    el.addEventListener('touchstart', e => {
      const p = e.touches[0]; sx = p.clientX; sy = p.clientY;
      t = setTimeout(() => { t = null; haptic('medium'); el.dataset.held = '1'; fn(sx, sy); }, 460);
    }, { passive: true });
    const cancel = () => { clearTimeout(t); t = null; };
    el.addEventListener('touchmove', e => { const p = e.touches[0]; if (Math.hypot(p.clientX - sx, p.clientY - sy) > 10) cancel(); }, { passive: true });
    el.addEventListener('touchend', e => { cancel(); if (el.dataset.held) { e.preventDefault(); delete el.dataset.held; } });
    el.addEventListener('touchcancel', cancel);
  }

  // ---------- menus and sheets (the same on the PC and the phones) ----------
  function popup(x, y, build) {
    closePop();
    const back = make('div', 'ch-pop-back');
    const pop = make('div', 'ch-pop');
    back.append(pop);
    document.body.append(back);
    build(pop, closePop);
    const r = pop.getBoundingClientRect();
    pop.style.left = Math.max(8, Math.min(x, innerWidth - r.width - 8)) + 'px';
    pop.style.top = Math.max(8, Math.min(y, innerHeight - r.height - 8)) + 'px';
    back.onclick = e => { if (e.target === back) closePop(); };
    back.oncontextmenu = e => { e.preventDefault(); closePop(); };
  }
  function closePop() { document.querySelectorAll('.ch-pop-back').forEach(b => b.remove()); }
  const item = (pop, text, fn, cls) => {
    const b = make('button', 'ch-pop-item' + (cls ? ' ' + cls : ''), text);
    b.type = 'button';
    b.onclick = () => { closePop(); fn(); };
    pop.append(b);
    return b;
  };

  function sheet(title, cls) {
    const back = make('div', 'ch-sheet-back');
    const s = make('div', 'ch-sheet' + (cls ? ' ' + cls : ''));
    s.setAttribute('role', 'dialog');
    const head = make('div', 'ch-sheet-head');
    head.append(make('b', '', title));
    const x = make('button', 'ch-ic ch-x', ''); x.type = 'button'; x.setAttribute('aria-label', 'Close');
    head.append(x);
    const body = make('div', 'ch-sheet-body');
    s.append(make('span', 'ch-grab'), head, body);
    back.append(s);
    document.body.append(back);
    requestAnimationFrame(() => back.classList.add('in'));
    const close = () => { back.classList.remove('in'); setTimeout(() => back.remove(), 240); if (s.onclose) s.onclose(); };
    x.onclick = close;
    back.onclick = e => { if (e.target === back) close(); };
    return { s, body, close };
  }

  function chatMenu(c, x, y) {
    popup(x, y, pop => {
      item(pop, c.pinned ? 'Unpin' : 'Pin to the top', () => set(c.id, { pinned: !c.pinned }));
      if (c.unread) item(pop, 'Mark as read', () => set(c.id, { read: true }));
      item(pop, c.muted ? 'Unmute' : 'Mute', () => set(c.id, { muted: !c.muted }));
      if (c.kind === 'dm') {
        item(pop, c.extra.fav ? 'Remove from favourites' : 'Add to favourites', () => set(c.id, { fav: !c.extra.fav }));
        item(pop, 'Rename…', () => rename(c));
        item(pop, 'Block', () => block(c), 'danger');
      }
      item(pop, c.kind === 'room' ? 'Leave the room' : 'Delete the chat', () => leaveChat(c), 'danger');
    });
  }
  const set = (id, what) => api('chatSet', id, what).then(refresh).catch(e => toast(errMsg(e), 'error'));
  function rename(c) {
    const { body, close } = sheet('Rename');
    const input = make('input', 'ch-input'); input.value = c.name; input.maxLength = 24;
    const b = make('button', 'btn primary', 'Save'); b.type = 'button';
    b.onclick = () => { set(c.id, { nick: input.value.trim() }); close(); };
    body.append(input, b);
    setTimeout(() => input.focus(), 50);
  }
  function block(c) {
    if (!confirm(`Block ${c.name}? You won't get their messages, and they won't know.`)) return;
    set(c.id, { blocked: true });
    if (openId === c.id) closeChat();
  }
  async function leaveChat(c) {
    if (!confirm(c.kind === 'room' ? `Leave ${c.name}? Its messages are deleted from this device.` : `Delete the chat with ${c.name}? Its messages are deleted from this device.`)) return;
    await api('chatLeave', c.id).catch(e => toast(errMsg(e), 'error'));
    if (openId === c.id) closeChat();
    refresh();
  }

  // ---------- New chat: add someone, join a room, your code ----------
  function newChat(start) {
    const { body, close } = sheet('New chat', 'ch-new-sheet');
    const seg = make('div', 'ch-seg');
    const pages = {};
    const page = (k, t) => { const b = make('button', '', t); b.type = 'button'; b.dataset.k = k; seg.append(b); pages[k] = make('div', 'ch-page hidden'); return pages[k]; };
    const add = page('add', 'Add someone'), room = page('room', 'Join a room'), code = page('code', 'Your code');
    const show = k => { seg.querySelectorAll('button').forEach(b => b.classList.toggle('on', b.dataset.k === k)); for (const [p, el] of Object.entries(pages)) el.classList.toggle('hidden', p !== k); if (k === 'code') drawCode(code); };
    seg.querySelectorAll('button').forEach(b => b.onclick = () => show(b.dataset.k));
    body.append(seg, add, room, code);
    add.innerHTML = `<p class="ch-help">Paste the code they sent you. It starts with <code>lvchat:</code>.</p>
      <textarea class="ch-input ch-code-in" rows="3" placeholder="lvchat:…" spellcheck="false" autocomplete="off"></textarea>
      <input class="ch-input ch-name-in" maxlength="24" placeholder="Their name (if you like)" autocomplete="off">
      <button class="btn primary" type="button">Start the chat</button>`;
    add.querySelector('button').onclick = async () => {
      try {
        const id = await api('chatAdd', add.querySelector('.ch-code-in').value, add.querySelector('.ch-name-in').value);
        close(); await refresh(); openChat(id);
      } catch (e) { toast(errMsg(e), 'error'); }
    };
    room.innerHTML = `<p class="ch-help">A room is everyone who knows its name (and its password, if it has one). The relays never see either: only people who know them can read along.</p>
      <label class="ch-hash"><span>#</span><input class="ch-input ch-room-in" maxlength="32" placeholder="room name" autocomplete="off" spellcheck="false"></label>
      <input class="ch-input ch-pw-in" type="password" placeholder="Password (if it has one)" autocomplete="off">
      <button class="btn primary" type="button">Join</button>`;
    room.querySelector('button').onclick = async () => {
      try {
        const id = await api('chatJoin', room.querySelector('.ch-room-in').value, room.querySelector('.ch-pw-in').value);
        close(); await refresh(); openChat(id);
      } catch (e) { toast(errMsg(e), 'error'); }
    };
    show(start || 'add');
    if (!start) setTimeout(() => add.querySelector('textarea').focus(), 120);
  }

  async function drawCode(el) {
    if (el.dataset.done) return;
    el.dataset.done = '1';
    const code = S && S.me.code || '';
    el.innerHTML = `<p class="ch-help">Send this to someone you want to chat with. When they add it, you'll see them in your chats.</p>
      <div class="ch-qr"></div><code class="ch-mycode"></code><div class="ch-row-btns"></div>`;
    el.querySelector('.ch-mycode').textContent = code;
    try { el.querySelector('.ch-qr').innerHTML = await api('chatQr'); } catch (e) {}
    const btns = el.querySelector('.ch-row-btns');
    const copy = make('button', 'btn primary', 'Copy'); copy.type = 'button';
    copy.onclick = () => { navigator.clipboard.writeText(`Chat with me on Lewd Vault: ${code}`).then(() => { copy.textContent = 'Copied'; haptic('light'); }, () => toast('Could not copy', 'error')); };
    btns.append(copy);
    if (navigator.share) {
      const sh = make('button', 'btn', 'Share…'); sh.type = 'button';
      sh.onclick = () => navigator.share({ text: `Chat with me on Lewd Vault: ${code}` }).catch(() => {});
      btns.append(sh);
    }
  }

  // ---------- settings ----------
  function settings() {
    const { body } = sheet('Chat settings');
    body.innerHTML = `
      <label class="ch-field"><span>Your nickname</span><input class="ch-input ch-set-nick" maxlength="24" autocomplete="off"></label>
      <div class="ch-field"><span>Your code</span><code class="ch-mycode"></code></div>
      <label class="ch-field"><span>Relays <small>(one per line; the defaults come back if you empty it)</small></span><textarea class="ch-input ch-set-relays" rows="4" spellcheck="false"></textarea></label>
      <div class="ch-field ch-blocked"><span>Blocked</span></div>
      <p class="ch-help">Everything is end-to-end encrypted on this device before it leaves. The relays see that someone receives something, never who sent it or what it says. Pictures are made smaller before they're sent. Nothing saves itself into your vault: you choose.</p>
      <button class="btn danger ch-wipe" type="button">Erase Chat on this device</button>`;
    const nick = body.querySelector('.ch-set-nick');
    nick.value = S ? S.me.nick : '';
    nick.onchange = () => api('chatNick', nick.value.trim()).then(refresh);
    body.querySelector('.ch-mycode').textContent = S ? S.me.code : '';
    const relays = body.querySelector('.ch-set-relays');
    relays.value = S ? S.relays.map(r => r.url).join('\n') : '';
    relays.disabled = !!(S && S.custom);
    relays.onchange = () => api('chatRelays', relays.value.split(/\s+/).filter(Boolean)).then(refresh).catch(e => toast(errMsg(e), 'error'));
    const bl = body.querySelector('.ch-blocked');
    if (!S || !S.blocked.length) bl.append(make('small', '', 'Nobody'));
    else for (const b of S.blocked) {
      const r = make('div', 'ch-bl');
      r.append(make('span', '', b.nick || b.id.slice(0, 10)));
      const u = make('button', 'btn small', 'Unblock'); u.type = 'button';
      u.onclick = () => { api('chatSet', b.id, { blocked: false }).then(refresh); r.remove(); };
      r.append(u);
      bl.append(r);
    }
    body.querySelector('.ch-wipe').onclick = async () => {
      if (!confirm('Erase Chat? Your identity, contacts, rooms and every message and picture in Chat are deleted from this device. A new identity is made next time.')) return;
      await api('chatWipe');
      keep(ON, '0');
      started = false; S = null; rev = 0; openId = ''; F = null;
      document.querySelectorAll('.ch-sheet-back').forEach(b => b.remove());
      badge.classList.add('hidden');
      drawStart();
    };
  }

  // ---------- a chat ----------
  async function openChat(id) {
    if (!started && local(ON) === '1') start();
    if (!conv(id)) { rev = 0; await refresh(); }
    if (!conv(id)) return;
    if (openId && openId !== id) api('chatClose').catch(() => {});
    openId = id;
    msgSig = '';
    replyTo = null;
    const f = frame();
    f.app.classList.add('in-chat');
    html.classList.add('ch-chat-open');
    f.rows.querySelectorAll('.ch-row').forEach(r => r.classList.toggle('on', r.dataset.id === id));
    drawChat();
    await loadMsgs(true);
  }
  function closeChat() {
    api('chatClose').catch(() => {});
    openId = '';
    if (F) {
      F.app.classList.remove('in-chat');
      F.conv.innerHTML = '<div class="ch-pick"><span class="ch-pick-art"></span><b>Your chats</b><span>Pick one, or start a new one.</span></div>';
    }
    html.classList.remove('ch-chat-open');
    refresh();
  }

  let C = null;
  function drawChat() {
    const c = conv(openId);
    if (!c) return;
    const f = frame();
    f.conv.innerHTML = `
      <div class="ch-c-head">
        <button class="ch-ic ch-back" type="button" aria-label="Back"></button>
        <span class="ch-c-av"></span>
        <div class="ch-c-title"><b></b><small></small></div>
        <button class="ch-ic ch-more" type="button" aria-label="More"></button>
      </div>
      <div class="ch-req hidden"><span></span><button class="btn small primary" type="button" data-a="accept">Accept</button><button class="btn small danger" type="button" data-a="block">Block</button></div>
      <div class="ch-scroll"><div class="ch-msgs"></div></div>
      <div class="ch-replybar hidden"><span></span><button class="ch-ic ch-x" type="button" aria-label="Cancel the reply"></button></div>
      <form class="ch-composer">
        <button class="ch-plus" type="button" aria-label="Send a picture"></button>
        <textarea rows="1" placeholder="Message…" spellcheck="true"></textarea>
        <button class="ch-send" type="submit" aria-label="Send"></button>
      </form>`;
    const q = s => f.conv.querySelector(s);
    C = { head: q('.ch-c-head'), title: q('.ch-c-title b'), sub: q('.ch-c-title small'), av: q('.ch-c-av'), scroll: q('.ch-scroll'), list: q('.ch-msgs'), req: q('.ch-req'),
          replybar: q('.ch-replybar'), form: q('.ch-composer'), text: q('.ch-composer textarea'), send: q('.ch-send') };
    q('.ch-back').onclick = closeChat;
    q('.ch-more').onclick = e => { const r = e.currentTarget.getBoundingClientRect(); chatMenu(conv(openId), r.left - 180, r.bottom + 6); };
    q('.ch-plus').onclick = attach;
    C.replybar.querySelector('.ch-x').onclick = () => setReply(null);
    C.req.querySelector('[data-a="accept"]').onclick = () => set(openId, { accept: true });
    C.req.querySelector('[data-a="block"]').onclick = () => block(conv(openId));
    const grow = () => { C.text.style.height = 'auto'; C.text.style.height = Math.min(C.text.scrollHeight, 140) + 'px'; C.form.classList.toggle('typed', !!C.text.value.trim()); };
    C.text.oninput = grow;
    C.text.onkeydown = e => { if (e.key === 'Enter' && !e.shiftKey && !isPhone()) { e.preventDefault(); C.form.requestSubmit(); } };
    C.form.onsubmit = async e => {
      e.preventDefault();
      const text = C.text.value.trim();
      if (!text) return;
      C.text.value = ''; grow();
      haptic('light');
      try { await api('chatSend', openId, text, replyTo ? replyTo.id : ''); setReply(null); await loadMsgs(true); }
      catch (err) { C.text.value = text; grow(); toast(errMsg(err), 'error'); }
    };
    drawChatHead();
  }

  function drawChatHead() {
    const c = conv(openId);
    if (!c || !C) return;
    C.av.replaceChildren(avatar(c.id, c.name, 38));
    C.title.textContent = c.name;
    if (c.kind === 'room') {
      const people = new Set(msgs.filter(m => !m.mine && Date.now() - m.at < 7 * 86400000).map(m => m.from));
      C.sub.textContent = `Room · ${people.size + 1} ${people.size ? 'people' : 'person'} this week${c.extra.locked ? ' · password' : ''}`;
    } else C.sub.textContent = msgs.some(m => m.via === 'nearby') ? 'Encrypted · also nearby' : 'End-to-end encrypted';
    const req = c.extra && c.extra.request;
    C.req.classList.toggle('hidden', !req);
    if (req) C.req.querySelector('span').textContent = `${c.name} isn't in your chats yet.`;
  }

  async function loadMsgs(toEnd) {
    if (!openId) return;
    const id = openId;
    let list;
    try { list = await api('chatMessages', id); } catch (e) { return; }
    if (id !== openId || !C) return;
    const sig = list.map(m => m.id + m.state + (m.deleted ? 'd' : '') + JSON.stringify(m.reacts) + (m.pic ? m.pic.opened : '')).join('|');
    msgs = list;
    drawChatHead();
    if (sig === msgSig) return;
    msgSig = sig;
    const near = C.scroll.scrollHeight - C.scroll.scrollTop - C.scroll.clientHeight < 120;
    drawMsgs();
    if (toEnd || near) requestAnimationFrame(() => { C.scroll.scrollTop = C.scroll.scrollHeight; });
  }

  function drawMsgs() {
    const c = conv(openId);
    const room = c && c.kind === 'room';
    C.list.innerHTML = '';
    if (!msgs.length) {
      const e = make('div', 'ch-hello');
      e.innerHTML = room ? `<b>${esc(c.name)}</b><span>Messages here last a week. Be nice.</span>` : `<b>Say hi to ${esc(c ? c.name : '')}</b><span>Only you two can read this chat.</span>`;
      C.list.append(e);
      return;
    }
    let lastDay = '', prev = null;
    for (const m of msgs) {
      const d = dayLabel(m.at);
      if (d !== lastDay) { C.list.append(make('div', 'ch-day', d)); lastDay = d; prev = null; }
      const same = prev && prev.from === m.from && m.at - prev.at < 5 * 60000;
      C.list.append(bubble(m, room, same));
      prev = m;
    }
  }

  function bubble(m, room, same) {
    const w = make('div', 'ch-msg' + (m.mine ? ' mine' : '') + (same ? ' same' : ''));
    w.dataset.id = m.id;
    if (room && !m.mine && !same) {
      const n = make('span', 'ch-who', m.nick || 'Someone');
      let h = 0; for (const ch of m.from) h = (h * 31 + ch.charCodeAt(0)) >>> 0;
      n.style.setProperty('--h', String(h % 360));
      w.append(n);
    }
    const b = make('div', 'ch-bubble');
    if (m.deleted) { b.classList.add('gone'); b.textContent = 'Deleted'; w.append(b); return w; }
    if (m.reply) {
      const r = msgs.find(x => x.id === m.reply);
      const quote = make('div', 'ch-quote');
      quote.append(make('b', '', r ? (r.mine ? 'You' : r.nick || 'Them') : ''), make('span', '', r ? (r.pic ? '📷 Picture' : r.text) : 'A message'));
      quote.onclick = () => { const t = C.list.querySelector(`[data-id="${m.reply}"]`); if (t) { t.scrollIntoView({ block: 'center', behavior: 'smooth' }); t.classList.add('flash'); setTimeout(() => t.classList.remove('flash'), 900); } };
      b.append(quote);
    }
    if (m.pic) {
      const p = m.pic;
      const card = make('button', 'ch-pic');
      card.type = 'button';
      const ratio = p.w && p.h ? p.h / p.w : 1;
      card.style.setProperty('--r', String(Math.min(1.6, Math.max(.5, ratio))));
      if (p.once && !m.mine) {
        card.classList.add('once');
        card.innerHTML = p.opened ? '<span class="ch-once">Opened</span>' : '<span class="ch-once">View once · tap to open</span>';
        if (!p.opened) card.onclick = () => viewer(m);
        else card.disabled = true;
      } else {
        const img = make('img');
        img.loading = 'lazy'; img.decoding = 'async'; img.alt = '';
        img.src = media(p.mid);
        card.append(img);
        if (p.once) card.append(make('span', 'ch-once-tag', 'View once'));
        card.onclick = () => viewer(m);
      }
      b.classList.add('has-pic');
      b.append(card);
    }
    if (m.text) {
      const t = make('div', 'ch-text');
      t.textContent = m.text;
      b.append(t);
    }
    const meta = make('span', 'ch-meta');
    meta.append(make('time', '', new Date(m.at).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })));
    if (m.via === 'nearby') meta.append(make('i', 'ch-via', ''));
    if (m.mine) meta.append(make('i', 'ch-state ' + m.state, ''));
    b.append(meta);
    w.append(b);
    if (m.mine && m.state === 'failed') {
      const f = make('button', 'ch-failed', 'Not sent · tap to try again'); f.type = 'button';
      f.onclick = () => api('chatRetry', openId, m.id).then(() => loadMsgs());
      w.append(f);
    }
    const reacts = Object.entries(m.reacts || {});
    if (reacts.length) {
      const rr = make('div', 'ch-reacts');
      const me = S && S.me.id;
      for (const [e, who] of reacts) {
        const r = make('button', 'ch-react' + (who.includes(me) ? ' mine' : ''), e + (who.length > 1 ? ' ' + who.length : ''));
        r.type = 'button';
        r.onclick = () => react(m, e);
        rr.append(r);
      }
      w.append(rr);
    }
    b.oncontextmenu = e => { e.preventDefault(); msgMenu(m, e.clientX, e.clientY); };
    hold(b, (x, y) => msgMenu(m, x, y));
    // a double tap hearts it
    let lastTap = 0;
    b.addEventListener('click', e => { if (e.target.closest('.ch-pic, .ch-quote')) return; const t = Date.now(); if (t - lastTap < 320) { react(m, '❤️'); burstHeart(b); } lastTap = t; });
    return w;
  }

  function burstHeart(el) {
    const h = make('span', 'ch-burst', '♥');
    el.append(h);
    setTimeout(() => h.remove(), 800);
    haptic('light');
  }
  const react = (m, e) => api('chatReact', openId, m.id, e).then(() => loadMsgs()).catch(err => toast(errMsg(err), 'error'));

  function msgMenu(m, x, y) {
    if (m.deleted) return;
    popup(x, y, (pop, close) => {
      const row = make('div', 'ch-pop-reacts');
      for (const e of REACTS) {
        const b = make('button', '', e); b.type = 'button';
        b.onclick = () => { close(); react(m, e); };
        row.append(b);
      }
      pop.append(row);
      item(pop, 'Reply', () => setReply(m));
      if (m.text) item(pop, 'Copy', () => navigator.clipboard.writeText(m.text).then(() => toast('Copied', 'ok')).catch(() => {}));
      if (m.pic && !(m.pic.once && !m.mine)) item(pop, 'Save to my vault', () => saveToVault(m));
      item(pop, 'Delete for me', () => api('chatDelete', openId, m.id, false).then(() => loadMsgs()));
      if (m.mine) item(pop, 'Delete for everyone', () => api('chatDelete', openId, m.id, true).then(() => loadMsgs()), 'danger');
    });
  }

  function setReply(m) {
    replyTo = m;
    if (!C) return;
    C.replybar.classList.toggle('hidden', !m);
    if (m) {
      C.replybar.querySelector('span').textContent = `Replying to ${m.mine ? 'yourself' : (m.nick || 'them')}: ${m.pic ? '📷 Picture' : m.text}`;
      C.text.focus();
    }
  }

  // ---------- pictures: the viewer, saving one, sending one ----------
  function viewer(m) {
    const p = m.pic;
    const v = make('div', 'ch-viewer');
    const img = make('img'); img.src = media(p.mid); img.alt = '';
    const bar = make('div', 'ch-viewer-bar');
    const x = make('button', 'ch-ic ch-x', ''); x.type = 'button'; x.setAttribute('aria-label', 'Close');
    bar.append(x);
    const once = p.once && !m.mine;
    if (once) bar.append(make('span', 'ch-once-tag', 'View once: it goes when you close it'));
    else { const s = make('button', 'btn small', 'Save to my vault'); s.type = 'button'; s.onclick = () => saveToVault(m); bar.append(s); }
    v.append(img, bar);
    if (m.text) v.append(make('div', 'ch-viewer-cap', m.text));
    document.body.append(v);
    requestAnimationFrame(() => v.classList.add('in'));
    const close = () => {
      v.classList.remove('in');
      setTimeout(() => v.remove(), 200);
      if (once) api('chatOpened', openId, m.id).then(() => loadMsgs());
      removeEventListener('keydown', key);
    };
    const key = e => { if (e.key === 'Escape') close(); };
    addEventListener('keydown', key);
    x.onclick = close;
    v.onclick = e => { if (e.target === v) close(); };
    // a screenshot shield for view-once on the phones
    if (once) v.classList.add('protect');
  }

  async function saveToVault(m) {
    try {
      const r = await fetch(media(m.pic.mid));
      if (!r.ok) throw new Error('That picture is gone');
      const blob = await r.blob();
      const file = new File([blob], `chat-${m.id}.jpg`, { type: 'image/jpeg' });
      const input = $('fileInput'), addTab = document.querySelector('header nav button[data-tab="add"]');
      if (!input || typeof DataTransfer === 'undefined') throw new Error('Saving is not available here');
      document.querySelectorAll('.ch-viewer').forEach(v => v.remove());
      if (addTab) addTab.click();
      const dt = new DataTransfer();
      dt.items.add(file);
      input.files = dt.files;
      input.dispatchEvent(new Event('change', { bubbles: true }));
      toast('Tag it, and it is in your vault', 'ok');
    } catch (e) { toast(errMsg(e), 'error'); }
  }

  function attach() {
    const { body, close } = sheet('Send a picture', 'ch-attach');
    body.innerHTML = `
      <div class="ch-attach-top">
        <label class="ch-search"><input type="search" placeholder="Search your vault (tags, names)" spellcheck="false"></label>
        <label class="ch-once-opt"><input type="checkbox"><span>View once</span></label>
      </div>
      <div class="ch-grid"></div>
      <div class="ch-attach-foot"><button class="btn" type="button">From this device…</button><input type="file" accept="image/*" hidden></div>`;
    const grid = body.querySelector('.ch-grid'), search = body.querySelector('input[type=search]'), once = body.querySelector('.ch-once-opt input');
    const file = body.querySelector('input[type=file]');
    body.querySelector('.ch-attach-foot button').onclick = () => file.click();
    file.onchange = async () => {
      const f = file.files[0];
      if (!f) return;
      close();
      const data = await new Promise(res => { const r = new FileReader(); r.onload = () => res(r.result); r.readAsDataURL(f); });
      sendPic(() => api('chatSendData', openId, data, once.checked, ''));
    };
    const pool = (typeof library !== 'undefined' && Array.isArray(library) ? library : []).filter(i => i.kind === 'image' || i.kind === 'gif');
    const draw = () => {
      const q = search.value.trim().toLowerCase();
      let list = pool;
      if (q) list = list.filter(i => `${i.tags || ''} ${i.name || ''} ${i.category || ''} ${i.collection || ''}`.toLowerCase().includes(q));
      list = list.slice().sort((a, b) => String(b.added || '').localeCompare(String(a.added || ''))).slice(0, 120);
      grid.innerHTML = '';
      if (!pool.length) { grid.append(make('p', 'ch-help', 'Your vault has no pictures yet (or the Library hasn\'t loaded: open it once).')); return; }
      for (const it of list) {
        const t = make('button', 'ch-tile'); t.type = 'button';
        const img = make('img'); img.loading = 'lazy'; img.alt = '';
        img.src = it.thumb ? `${vaultOrigin()}/t/${encodeURIComponent(it.thumb)}?w=220` : vaultUrl(it.id);
        t.append(img);
        t.onclick = () => { close(); sendPic(() => api('chatSendItem', openId, it.id, once.checked, '')); };
        grid.append(t);
      }
    };
    search.oninput = draw;
    draw();
  }

  async function sendPic(go) {
    toast('Sending the picture…');
    try { await go(); await loadMsgs(true); haptic('light'); }
    catch (e) { toast(errMsg(e), 'error'); }
  }

  // ---------- Nearby: phones close by, over Bluetooth (like AirDrop) ----------
  // The native side (mobile/android/LvNearby.kt, mobile/ios/Nearby) puts a bridge on the page: on Android an object,
  // window.LvBle (start(helloJson), stop(), send(peer, text)); on iOS a message handler, lvBle. It calls
  // back window.__lvNearby({ type: 'found'|'lost'|'data'|'state', peer, hello, data, on, error }).
  // (looked up when needed: the native side may add it after this file ran)
  const getBridge = () => {
    if (window.LvBle && window.LvBle.start) return { start: h => window.LvBle.start(h), stop: () => window.LvBle.stop(), send: (p, d) => window.LvBle.send(p, d) };
    const wk = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.lvBle;
    if (wk) return { start: h => wk.postMessage({ op: 'start', hello: h }), stop: () => wk.postMessage({ op: 'stop' }), send: (p, d) => wk.postMessage({ op: 'send', peer: p, data: d }) };
    return null;
  };
  const peers = new Map();
  let nearbyUi = null;
  window.__lvNearby = ev => {
    if (!ev) return;
    if (ev.type === 'found' && ev.peer) {
      let hello = {};
      try { hello = typeof ev.hello === 'string' ? JSON.parse(ev.hello) : (ev.hello || {}); } catch (e) {}
      if (hello.id && /^[0-9a-f]{64}$/.test(hello.id) && (!S || hello.id !== S.me.id)) peers.set(ev.peer, { peer: ev.peer, id: hello.id, nick: String(hello.nick || 'Someone').slice(0, 24), seen: Date.now() });
    } else if (ev.type === 'lost') peers.delete(ev.peer);
    else if (ev.type === 'data' && ev.data) {
      // sealed events (one per line), the same as the relays carry
      for (const line of String(ev.data).split('\n')) if (line.trim()) api('chatNearbyTake', line).then(got => { if (got) { haptic('success'); refresh(); } }).catch(() => {});
    } else if (ev.type === 'state' && nearbyUi) nearbyUi.state(ev);
    if (nearbyUi) nearbyUi.draw();
  };

  function nearby() {
    const { body, s } = sheet('Nearby', 'ch-nearby');
    body.innerHTML = `
      <div class="ch-radar"><span class="w1"></span><span class="w2"></span><span class="w3"></span><span class="ch-radar-me"></span><div class="ch-radar-peers"></div></div>
      <p class="ch-nearby-msg"></p>
      <p class="ch-help">While this is open, phones close by with Lewd Vault Chat open on Nearby can see your nickname. Tap someone to send them a message or a picture straight over Bluetooth, sealed for them alone. It works without internet.</p>`;
    const msg = body.querySelector('.ch-nearby-msg'), field = body.querySelector('.ch-radar-peers');
    const bridge = getBridge();
    if (!bridge) {
      msg.textContent = isPhone() ? 'Nearby needs the newest version of the app.' : 'Nearby is on the iPhone and Android apps (it uses Bluetooth). On a PC, chat over the internet.';
      body.querySelector('.ch-radar').classList.add('off');
      return;
    }
    msg.textContent = 'Looking for phones nearby…';
    api('chatNearbyHello').then(h => bridge.start(JSON.stringify(h))).catch(e => { msg.textContent = errMsg(e); });
    nearbyUi = {
      state: ev => { if (ev.error) msg.textContent = ev.error; else if (ev.on === false) msg.textContent = 'Turn on Bluetooth to see who is nearby.'; },
      draw: () => {
        const list = [...peers.values()].filter(p => Date.now() - p.seen < 120000);
        if (list.length) msg.textContent = list.length === 1 ? '1 person nearby' : `${list.length} people nearby`;
        field.innerHTML = '';
        list.forEach((p, i) => {
          const angle = (i * 137.5) * Math.PI / 180, dist = 34 + (i % 3) * 10;
          const b = make('button', 'ch-peer'); b.type = 'button';
          b.style.left = `calc(50% + ${Math.cos(angle) * dist}% - 32px)`;
          b.style.top = `calc(50% + ${Math.sin(angle) * dist}% - 32px)`;
          b.append(avatar(p.id, p.nick, 56, true), make('span', '', p.nick));
          b.onclick = () => sendNearby(p);
          field.append(b);
        });
      },
    };
    nearbyUi.draw();
    s.onclose = () => { try { bridge.stop(); } catch (e) {} nearbyUi = null; };
  }

  function sendNearby(p) {
    const { body, close } = sheet(`Send to ${p.nick}`);
    body.innerHTML = `<textarea class="ch-input" rows="3" placeholder="A message…"></textarea>
      <div class="ch-row-btns"><button class="btn primary" type="button" data-a="text">Send</button><button class="btn" type="button" data-a="pic">A picture from your vault…</button></div>`;
    const text = body.querySelector('textarea');
    const go = async (what, kind) => {
      try {
        const bridge = getBridge();
        if (!bridge) throw new Error('Nearby is not available');
        const events = await api('chatNearbyWrap', p.id, what, kind, p.nick);
        bridge.send(p.peer, events.join('\n'));
        haptic('success');
        toast(`Sent to ${p.nick}`, 'ok');
        close();
        refresh();
      } catch (e) { toast(errMsg(e), 'error'); }
    };
    body.querySelector('[data-a="text"]').onclick = () => { if (text.value.trim()) go(text.value.trim(), 'text'); };
    body.querySelector('[data-a="pic"]').onclick = () => {
      close();
      const prev = openId;
      openId = p.id; // (the picker sends to the open chat: point it at them, through Nearby)
      const pick = sheet('Send a picture', 'ch-attach');
      const pool = (typeof library !== 'undefined' && Array.isArray(library) ? library : []).filter(i => i.kind === 'image');
      const grid = make('div', 'ch-grid');
      for (const it of pool.slice(-90).reverse()) {
        const t = make('button', 'ch-tile'); t.type = 'button';
        const img = make('img'); img.loading = 'lazy'; img.alt = '';
        img.src = it.thumb ? `${vaultOrigin()}/t/${encodeURIComponent(it.thumb)}?w=220` : vaultUrl(it.id);
        t.append(img);
        t.onclick = () => { pick.close(); go(it.id, 'item'); };
        grid.append(t);
      }
      pick.body.append(grid);
      pick.s.onclose = () => { openId = prev; };
    };
  }

  // ---------- keeping up to date ----------
  async function refresh() {
    if (!started) return;
    let r;
    try { r = await api('chatState', rev); } catch (e) { return; }
    if (!r || r.rev === rev && !r.convs) return;
    if (r.convs) { S = r; rev = r.rev; }
    const total = S.convs.filter(c => !c.muted).reduce((n, c) => n + (c.unread || 0), 0);
    badge.textContent = total > 99 ? '99+' : String(total);
    badge.classList.toggle('hidden', !total);
    html.style.setProperty('--ch-unread', `"${total || ''}"`);
    html.classList.toggle('ch-has-unread', total > 0);
    if (!tab.classList.contains('hidden')) {
      drawList();
      if (openId) { if (!conv(openId)) closeChat(); else await loadMsgs(); }
    }
  }
  function schedule() {
    clearTimeout(timer);
    timer = setTimeout(async () => { await refresh(); schedule(); }, visible() ? 1200 : 9000);
  }
  function start() {
    started = true;
    rev = 0;
    refresh().then(() => { if (!tab.classList.contains('hidden')) drawList(); });
    schedule();
  }
  async function enter() {
    if (local(ON) !== '1') {
      // (set up on another device with this vault, or this browser forgot: the backend knows)
      if (await api('chatReady').catch(() => false)) keep(ON, '1');
      else { drawStart(); return; }
    }
    if (!started) start();
    else { rev = 0; refresh(); }
    if (S) drawList();
    schedule();
  }
  function leave() {
    if (openId) api('chatClose').catch(() => {});
    html.classList.remove('ch-chat-open');
    if (F) F.app.classList.remove('in-chat');
    openId = '';
  }
  // the phone's visible height (the keyboard takes part of it)
  if (window.visualViewport) {
    const vv = () => html.style.setProperty('--ch-vh', visualViewport.height + 'px');
    visualViewport.addEventListener('resize', vv);
    vv();
  }
  // already set up: connect at once, so messages (and the badge) arrive while you are elsewhere
  setupReady.then(() => setTimeout(async () => {
    if (local(ON) !== '1' && await api('chatReady').catch(() => false)) keep(ON, '1');
    if (local(ON) === '1' && !started) start();
  }, 1200));

  window.vaultChat = { open: () => navBtn.click(), openChat, refresh, nearby };
})();
