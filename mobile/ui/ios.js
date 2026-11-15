// The iOS layer: what the shared page needs to be a phone app.
//
// Only runs on iOS (the app sets html.ios there, before any script). The shared page keeps doing all
// the work; this file re-presents its features the way a phone expects them:
//   1. a floating tab bar (Library, Browser, Add, Slideshow, More) and the More tiles,
//   2. the Library laid out like Photos, with search, filters and tags in sheets,
//   3. long-press a picture: it lifts into a preview with its actions under it (stars, level and
//      category as tap strips) in place of the desktop's right-click menu,
//   4. the viewer: the picture full screen, a glass toolbar, details as a pull-up sheet, swipes and
//      double-tap to appreciate; the slideshow takes the same gestures,
//   5. confirm() and prompt(): WKWebView silently drops them (confirm is always "no"), so every
//      "are you sure?" would block its action for good; they become in-app sheets,
//   6. privacy: "Hide now" (and a three-finger tap) in place of the emergency key, as a lock screen or
//      a working calculator that opens with your PIN, and a cover while the app is in the background.
// The look is mobile/ui/ios.css.
(() => {
  'use strict';
  const html = document.documentElement;
  if (!html.classList.contains('ios')) return;
  const $ = id => document.getElementById(id);
  const make = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };
  const ICON = {
    more: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='black'><circle cx='5' cy='12' r='2.2'/><circle cx='12' cy='12' r='2.2'/><circle cx='19' cy='12' r='2.2'/></svg>",
    close: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2.4' stroke-linecap='round'><path d='M6 6l12 12M18 6L6 18'/></svg>",
    you: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round'><circle cx='12' cy='8' r='4'/><path d='M4.5 20c1.2-3.6 4-5.5 7.5-5.5s6.3 1.9 7.5 5.5'/></svg>",
    coach: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M12 20s-7-4.4-7-10a4 4 0 0 1 7-2.6A4 4 0 0 1 19 10c0 5.600-7 10-7 10z'/></svg>",
    hide: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M3 3l18 18M10.6 6.200A9.700 9.700 0 0 1 12 6c5 0 8.500 4 9.500 6-.4.800-1.200 2-2.400 3.100M6.500 7.600C4.500 9 3.100 10.800 2.500 12c1 2 4.500 6 9.500 6 1.300 0 2.500-.3 3.600-.7M9.900 9.900a3 3 0 0 0 4.200 4.200'/></svg>",
    heart: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='black'><path d='M12 20.500s-7.500-4.600-7.500-10.300A4.200 4.200 0 0 1 12 7.600a4.200 4.200 0 0 1 7.500 2.600c0 5.700-7.500 10.300-7.500 10.300z'/></svg>",
    info: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round'><circle cx='12' cy='12' r='8.500'/><path d='M12 11v5.500'/><circle cx='12' cy='7.800' r='.6' fill='black'/></svg>",
    edit: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M4 20l1-4L16.500 4.500a2.100 2.100 0 0 1 3 3L8 19z'/></svg>",
    bin: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M4 6.500h16M9 6.500V4h6v2.500M6 6.500l1 13.500h10l1-13.500M10 10.500v6M14 10.500v6'/></svg>",
    down: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2.400' stroke-linecap='round' stroke-linejoin='round'><path d='M6 9l6 6 6-6'/></svg>",
    torch: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='1.800' stroke-linejoin='round'><path d='M8 3h8v3l-2 3v11a1 1 0 0 1-1 1h-2a1 1 0 0 1-1-1V9L8 6z'/><path d='M12 12v3' stroke-linecap='round'/></svg>",
    camera: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='1.800' stroke-linejoin='round'><path d='M3 8.500A1.500 1.500 0 0 1 4.500 7H8l1.500-2h5L16 7h3.500A1.500 1.500 0 0 1 21 8.500v9a1.500 1.500 0 0 1-1.500 1.500h-15A1.500 1.500 0 0 1 3 17.500z'/><circle cx='12' cy='13' r='3.500'/></svg>",
    search: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2.2' stroke-linecap='round'><circle cx='11' cy='11' r='6.5'/><path d='M16 16l4.5 4.5'/></svg>",
    tags: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M3 12V4h8l10 10-8 8z'/><circle cx='7.500' cy='8.500' r='1.300' fill='black'/></svg>"
  };
  const icon = name => `url("data:image/svg+xml;utf8,${ICON[name].replace(/"/g, "'").replace(/#/g, '%23')}")`;

  // ---------- sheets (the More list, confirm/prompt, the Tags panel's backdrop) ----------
  // One at a time, from the bottom, closed by its backdrop. Returns { el, body, close }.
  function sheet(cls, onClose) {
    const back = make('div', 'ios-back');
    const el = make('div', 'ios-sheet glass ' + (cls || ''));
    el.setAttribute('role', 'dialog');
    el.append(make('div', 'ios-grab'));
    const body = make('div', 'ios-sheet-body');
    el.append(body);
    back.append(el);
    let open = true;
    const close = result => {
      if (!open) return;
      open = false;
      back.classList.remove('in');
      setTimeout(() => back.remove(), 220);
      if (onClose) onClose(result);
    };
    back.addEventListener('click', e => { if (e.target === back) close(); });
    document.body.append(back);
    requestAnimationFrame(() => back.classList.add('in'));
    return { el, body, close };
  }

  // ---------- 1. the tab bar ----------
  const nav = document.querySelector('header nav');
  const sideTabs = nav ? [...nav.querySelectorAll('button[data-tab]')] : [];
  const side = tab => sideTabs.find(b => b.dataset.tab === tab);
  const label = b => (b.querySelector('.nt-title') || b).textContent.trim();
  const iconOf = b => getComputedStyle(b).getPropertyValue('--icon').trim(); // (the sidebar's own icon)
  const PRIMARY = ['library', 'discover', 'add', 'slideshow'].filter(side);
  // (a <div>, not a <nav>: the desktop's sidebar rules are `html.desktop nav …`, and iOS carries both classes)
  const tabbar = make('div', 'ios-tabbar glass');
  tabbar.setAttribute('role', 'navigation');
  tabbar.setAttribute('aria-label', 'Sections');
  const tabButton = (text, iconValue, onTap) => {
    const t = make('button', '', text);
    t.type = 'button';
    if (iconValue) t.style.setProperty('--icon', iconValue);
    t.onclick = onTap;
    tabbar.append(t);
    return t;
  };
  for (const tab of PRIMARY) tabButton(label(side(tab)), iconOf(side(tab)), () => side(tab).click()).dataset.tab = tab;
  const moreBtn = tabButton('More', icon('more'), openMore);
  moreBtn.dataset.tab = 'more';
  document.body.append(tabbar);

  // More: big tiles (Control Centre's shape), Hide now across the bottom
  function openMore() {
    const s = sheet('ios-more');
    s.body.append(make('h2', 'ios-sheet-title', 'More'));
    const tiles = make('div', 'ios-tiles');
    const tile = (text, iconValue, onTap, cls) => {
      const r = make('button', 'ios-tile' + (cls ? ' ' + cls : ''));
      r.type = 'button';
      if (iconValue) r.style.setProperty('--icon', iconValue);
      r.append(make('span', '', text));
      r.onclick = () => { s.close(); onTap(); };
      tiles.append(r);
      return r;
    };
    for (const b of sideTabs) if (!PRIMARY.includes(b.dataset.tab)) tile(label(b), iconOf(b), () => b.click());
    if (window.vaultProfile) tile('You', icon('you'), () => window.vaultProfile.open());
    if (window.vaultCoach) tile('Coach', icon('coach'), () => window.vaultCoach.open());
    tile('Hide now', icon('hide'), hideNow, 'wide danger').append(make('small', '', 'or tap anywhere with three fingers'));
    s.body.append(tiles);
  }

  // the bar follows whichever section is showing (More lights up for everything it holds)
  const syncTabs = () => {
    const on = nav && nav.querySelector('button.on');
    const tab = on ? on.dataset.tab : '';
    const inMore = !PRIMARY.includes(tab);
    tabbar.querySelectorAll('button').forEach(t => t.classList.toggle('on', t.dataset.tab === tab || (t === moreBtn && inMore)));
  };
  if (nav) new MutationObserver(syncTabs).observe(nav, { subtree: true, attributes: true, attributeFilter: ['class'] });
  syncTabs();
  // a new section starts at its top
  tabbar.addEventListener('click', () => scrollTo(0, 0));

  // ---------- 2. the Library, laid out like Photos ----------
  // The grid runs edge to edge under everything. Over it float: the title and count (top left), Select
  // and two round buttons (top right: search & filters, tags), and a pill switcher above the tab bar
  // (All / Pictures / GIFs / Videos). Search, sort and filters live in one sheet, Tags in another.
  const lib = $('tab-library');
  if (lib) {
    const head = make('div', 'ios-libhead');
    const title = make('h1', 'ios-title', 'Library');
    const sub = make('span', 'ios-sub');
    const words = make('div', 'ios-libwords');
    words.append(title, sub);
    const tools = make('div', 'ios-libtools');
    const round = (name, label, onTap) => {
      const b = make('button', 'ios-round');
      b.type = 'button';
      b.title = label;
      b.setAttribute('aria-label', label);
      b.style.setProperty('--icon', icon(name));
      b.onclick = onTap;
      return b;
    };
    const select = make('button', 'ios-pill', 'Select');
    select.type = 'button';
    head.append(words, tools);
    lib.insertBefore(head, lib.firstChild);
    // the count under the title is the page's own ("5 of 5 items")
    const count = $('count');
    const showCount = () => { sub.textContent = count ? count.textContent.replace(/^(\d[\d,.]*) of \1 /, '$1 ') : ''; };
    if (count) new MutationObserver(showCount).observe(count, { childList: true, characterData: true, subtree: true });
    showCount();
    // scrolled: the title shrinks (the head stays, like Photos' date header)
    let ticking = false;
    addEventListener('scroll', () => {
      if (ticking) return;
      ticking = true;
      requestAnimationFrame(() => { head.classList.toggle('small', scrollY > 24); ticking = false; });
    }, { passive: true });

    // a sheet made of the page's own controls: `nodes` move into it once and stay there
    const dock = (cls, nodes) => {
      const panel = make('div', 'ios-dock ' + cls);
      panel.append(make('div', 'ios-grab'), ...nodes.filter(Boolean));
      const back = make('div', 'ios-back ios-dock-back');
      lib.append(back, panel);
      const show = on => { panel.classList.toggle('in', on); back.classList.toggle('in', on); html.classList.toggle('ios-dock-open', on); };
      back.onclick = () => show(false);
      return show;
    };
    const showFilters = dock('ios-filters', [make('h2', 'ios-sheet-title', 'Search & filters'), lib.querySelector('.searchbar'), lib.querySelector('.lib-filterrow')]);
    const panel = lib.querySelector('.tag-panel');
    let showTags = () => {};
    if (panel) {
      const back = make('div', 'ios-back ios-dock-back');
      panel.before(back); // (beside the panel, so the panel stacks above it)
      panel.prepend(make('div', 'ios-grab'));
      showTags = on => { html.classList.toggle('ios-tags-open', on); back.classList.toggle('in', on); html.classList.toggle('ios-dock-open', on); };
      back.onclick = () => showTags(false);
      // picking a tag filters the grid behind the sheet: close it so the result shows
      panel.addEventListener('click', e => { if (e.target.closest('.tl-name')) setTimeout(() => showTags(false), 120); });
    }
    tools.append(select, round('search', 'Search and filters', () => showFilters(true)), round('tags', 'Tags', () => showTags(true)));

    // Select: the desktop picks with Ctrl+click; here Select makes the next taps pick instead of open
    // (after the first pick the page's own selection takes over: its bar has Done)
    const grid = $('grid');
    let selecting = false;
    const setSelecting = on => { selecting = on; select.textContent = on ? 'Cancel' : 'Select'; select.classList.toggle('on', on); };
    select.onclick = () => {
      if (selecting) dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })); // (clears the page's selection)
      setSelecting(!selecting);
    };
    if (grid) grid.addEventListener('click', e => {
      if (!selecting || e.ctrlKey || !e.isTrusted) return;
      const card = e.target.closest('.card');
      if (!card) return;
      e.stopImmediatePropagation(); e.preventDefault();
      card.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true, ctrlKey: true }));
    }, true);
    const bar = document.querySelector('.ms-bar');
    if (bar) new MutationObserver(() => { if (bar.classList.contains('hidden') && selecting && !grid.querySelector('.card.picked-many')) setSelecting(false); })
      .observe(bar, { attributes: true, attributeFilter: ['class'] });

    // the switcher above the tab bar: the page's own Type filter, as a segmented control
    const kind = $('fKind');
    if (kind) {
      const seg = make('div', 'ios-seg glass');
      const thumb = make('i', 'ios-seg-thumb');
      seg.append(thumb);
      const opts = [['', 'All'], ['image', 'Pictures'], ['gif', 'GIFs'], ['video', 'Videos']];
      const draw = () => {
        const k = Math.max(0, opts.findIndex(([v]) => v === kind.value));
        seg.style.setProperty('--at', k);
        seg.querySelectorAll('button').forEach((b, i) => b.classList.toggle('on', i === k));
      };
      for (const [v, text] of opts) {
        const b = make('button', '', text);
        b.type = 'button';
        b.onclick = () => { kind.value = v; kind.dispatchEvent(new Event('change', { bubbles: true })); draw(); scrollTo({ top: 0, behavior: 'smooth' }); };
        seg.append(b);
      }
      seg.style.setProperty('--n', opts.length);
      kind.addEventListener('change', draw);
      lib.append(seg);
      draw();
    }

    // Photos shows squares: unless a layout was chosen, the Library starts as the even grid here
    if (typeof setupReady !== 'undefined') setupReady.then(s => {
      if (s && !(s.settings && 'libraryLayout' in s.settings) && matchMedia('(max-width: 820px)').matches) {
        settings.libraryLayout = 'grid';
        const pick = $('fLayout');
        if (pick) pick.value = 'grid';
        setTimeout(() => { if (typeof renderLibrary === 'function') renderLibrary(); }, 0);
      }
    });
  }

  // the Add screen's words: there is no dropping files on a phone
  const drop = $('drop');
  if (drop && drop.querySelector('strong')) {
    drop.querySelector('strong').textContent = 'Tap to choose pictures, GIFs or videos';
    for (const n of [...drop.childNodes]) if (n.nodeType === 3 && n.textContent.trim()) n.textContent = ' From Photos or Files. You\'ll tag them one by one: category, NSFW 1–10, rating 1–3. ';
  }

  // ---------- hearts: a double tap appreciates, and says so ----------
  const heartAt = (x, y) => {
    const h = make('span', 'ios-heart');
    h.style.left = x + 'px'; h.style.top = y + 'px';
    h.style.setProperty('--icon', icon('heart'));
    document.body.append(h);
    setTimeout(() => h.remove(), 900);
  };
  // taps and swipes on `el`: { tap, double, left, right, up, down, drag(dx, dy, axis), settle() }
  function gestures(el, on, skip) {
    let from = null, tapAt = 0, tapTimer = null;
    el.addEventListener('touchstart', e => {
      from = e.touches.length === 1 && !(skip && e.target.closest(skip)) ? { x: e.touches[0].clientX, y: e.touches[0].clientY, axis: '', t: Date.now() } : null;
    }, { passive: true });
    el.addEventListener('touchmove', e => {
      if (!from) return;
      const dx = e.touches[0].clientX - from.x, dy = e.touches[0].clientY - from.y;
      if (!from.axis && Math.hypot(dx, dy) > 12) from.axis = Math.abs(dx) > Math.abs(dy) ? 'x' : 'y';
      if (from.axis && on.drag) on.drag(dx, dy, from.axis);
    }, { passive: true });
    const end = e => {
      if (!from) return;
      const t = e.changedTouches[0], dx = t.clientX - from.x, dy = t.clientY - from.y, axis = from.axis, quick = Date.now() - from.t < 350;
      from = null;
      if (on.settle) on.settle();
      if (!axis && quick) {
        // a tap; a second one soon after makes it a double tap (and the single tap never fires)
        if (Date.now() - tapAt < 300) { clearTimeout(tapTimer); tapAt = 0; if (on.double) on.double(t.clientX, t.clientY); }
        else { tapAt = Date.now(); tapTimer = setTimeout(() => { if (on.tap) on.tap(t.clientX, t.clientY); }, on.double ? 280 : 0); }
        return;
      }
      if (axis === 'x' && Math.abs(dx) > 64) { const f = dx < 0 ? on.left : on.right; if (f) f(); }
      if (axis === 'y' && Math.abs(dy) > 90) { const f = dy < 0 ? on.up : on.down; if (f) f(); }
    };
    el.addEventListener('touchend', end, { passive: true });
    el.addEventListener('touchcancel', () => { from = null; if (on.settle) on.settle(); }, { passive: true });
  }

  // ---------- 4. the viewer, like Photos ----------
  // The picture has the whole screen. Over it: a round close button, the "3 / 12" count, and a glass
  // toolbar (appreciate, details, edit, delete). A tap hides all of it. The details are a sheet you
  // pull up. Swipe sideways for the next one, down to close, double-tap to appreciate.
  const detail = $('detail');
  if (detail) {
    const big = detail.querySelector('.big');
    const media = () => big && big.querySelector('img:not(.hidden), video:not(.hidden)');
    const press = id => { const b = $(id); if (b) { lastTap = b; b.click(); } };
    const step = d => {
      const on = detail.querySelector('.filmstrip:not(.hidden) .fs-item.on');
      const near = on && (d > 0 ? on.nextElementSibling : on.previousElementSibling);
      if (near && near.classList.contains('fs-item')) { near.click(); return true; }
      const b = $(d > 0 ? 'dNext' : 'dPrev');
      if (b && !b.classList.contains('hidden')) { b.click(); return true; }
      return false;
    };
    const info = on => { if (detail.classList.contains('ios-info') !== !!on) detail.classList.toggle('ios-info', !!on); };
    const top = make('div', 'ios-viewtop');
    const close = make('button', 'ios-round');
    close.type = 'button'; close.title = 'Close';
    close.style.setProperty('--icon', icon('down'));
    close.onclick = e => { e.stopPropagation(); info(false); press('closeDetail'); };
    const where = make('span', 'ios-viewcount');
    top.append(close, where);
    const bar = make('div', 'ios-viewbar glass');
    const tool = (name, label, onTap) => {
      const b = make('button', 'ios-tool');
      b.type = 'button'; b.title = label; b.setAttribute('aria-label', label);
      b.style.setProperty('--icon', icon(name));
      b.onclick = e => { e.stopPropagation(); onTap(b); };
      bar.append(b);
      return b;
    };
    const like = tool('heart', 'Appreciate', b => { press('dLike'); b.classList.remove('pop'); void b.offsetWidth; b.classList.add('pop'); });
    tool('info', 'Details', () => info(!detail.classList.contains('ios-info')));
    const edit = tool('edit', 'Edit picture', () => { const b = big.querySelector('.lv-edit-btn'); if (b) b.click(); });
    tool('bin', 'Delete', () => press('dTrash'));
    const back = make('div', 'ios-infoback');
    back.onclick = () => info(false);
    const aside = detail.querySelector('aside');
    if (aside) aside.prepend(make('div', 'ios-grab'));
    detail.append(top, bar, back);
    // what the bar shows follows the picture: the count from the filmstrip, Edit only for pictures
    const follow = () => {
      const c = detail.querySelector('.fs-count');
      const text = c && !c.closest('.hidden') ? c.textContent : '';
      if (where.textContent !== text) where.textContent = text; // (only on a change: this runs inside an observer of the same tree)
      const e = big.querySelector('.lv-edit-btn');
      edit.classList.toggle('hidden', !e || !!big.querySelector('video:not(.hidden)'));
      const likes = $('dLikes');
      like.classList.toggle('on', !!likes && /[1-9]/.test(likes.textContent));
    };
    new MutationObserver(follow).observe(detail, { subtree: true, childList: true, characterData: true, attributes: true, attributeFilter: ['class'] });
    // closed: back to its starting state for next time. (Only real changes: removing a class that isn't
    // there still rewrites the attribute, which would call this observer again, for ever.)
    new MutationObserver(() => {
      if (!detail.classList.contains('hidden')) return;
      for (const c of ['ios-info', 'ios-clean']) if (detail.classList.contains(c)) detail.classList.remove(c);
    }).observe(detail, { attributes: true, attributeFilter: ['class'] });
    if (big) gestures(big, {
      tap: () => { if (detail.classList.contains('ios-info')) info(false); else detail.classList.toggle('ios-clean'); },
      double: (x, y) => { press('dLike'); heartAt(x, y); },
      left: () => step(1), right: () => step(-1),
      up: () => info(true),
      down: () => { if (detail.classList.contains('ios-info')) info(false); else press('closeDetail'); },
      drag: (dx, dy, axis) => {
        const m = media();
        if (!m || detail.classList.contains('ios-info')) return;
        m.style.transition = 'none';
        m.style.transform = axis === 'x' ? `translateX(${dx}px)` : dy > 0 ? `translateY(${dy}px) scale(${Math.max(.8, 1 - dy / 900)})` : `translateY(${dy / 4}px)`;
        if (axis === 'y' && dy > 0) detail.style.setProperty('--fade', String(Math.max(.35, 1 - dy / 500)));
      },
      settle: () => { const m = media(); if (m) { m.style.transition = 'transform .28s cubic-bezier(.2, .8, .2, 1)'; m.style.transform = ''; } detail.style.removeProperty('--fade'); }
    }, 'button, input, .filmstrip, video');
    // the sheet itself: pull it down to put it away
    if (aside) gestures(aside, { down: () => { if (aside.scrollTop <= 0) info(false); } }, 'input, textarea, select, .tagbox');
  }

  // ---------- the slideshow: the same gestures ----------
  const show = $('show');
  if (show) gestures(show, {
    double: (x, y) => { const b = $('sLike'); if (b) b.click(); heartAt(x, y); },
    left: () => { const b = $('sNext'); if (b) b.click(); },
    right: () => { const b = $('sPrev'); if (b) b.click(); },
    down: () => { const b = $('sClose'); if (b) b.click(); }
  }, '.show-hud, button');

  // the viewer's "Open" (in another app): no such thing on a phone ("Show in folder" is #dOpen, in the CSS)
  const openInApp = [...document.querySelectorAll('#detail aside button')].find(b => b.textContent.trim() === 'Open');
  if (openInApp) openInApp.classList.add('ios-no');

  // Settings' jump chips are made from its headings: drop the one for the section hidden on iOS
  const settingsTab = $('tab-settings');
  if (settingsTab) new MutationObserver(() => {
    for (const b of settingsTab.querySelectorAll('.st-jump')) if (/Google Drive/.test(b.textContent)) b.remove();
  }).observe(settingsTab, { childList: true, subtree: true });

  // the same two in the long-press menu (it is rebuilt each time it opens)
  const lvMenu = document.querySelector('.lv-menu');
  if (lvMenu) new MutationObserver(() => {
    for (const b of lvMenu.querySelectorAll(':scope > .lv-item')) if (/Open with its program|Show in folder/.test(b.textContent)) b.remove();
  }).observe(lvMenu, { childList: true });

  // ---------- 3. confirm() and prompt() ----------
  // WKWebView's defaults are confirm -> false and prompt -> "" with nothing shown, and the page calls
  // them synchronously ("if (!confirm(...)) return;"), which no sheet can answer in time. So: the
  // first call shows the sheet and answers "no" (the action stops, as if cancelled); when you choose
  // OK, the tap that started it is replayed and the same question is answered at once with what you
  // chose. Every one of these questions comes from a tap, so the replay is exact.
  let lastTap = null;           // what was last tapped (the thing to tap again)
  let answered = null;          // { kind, text, value }: the answer waiting for the replayed question
  addEventListener('click', e => { if (e.isTrusted) lastTap = e.target; }, true);
  const replay = target => {
    if (!target || !target.isConnected) return;
    if (typeof target.click === 'function') target.click();
    else target.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true }));
  };
  function ask(kind, text, def) {
    const key = String(text);
    if (answered && answered.kind === kind && answered.text === key) {
      const v = answered.value;
      answered = null;
      return v;
    }
    const target = lastTap;
    const s = sheet('ios-ask');
    s.body.append(make('p', 'ios-ask-text', key));
    let input = null;
    if (kind === 'prompt') {
      input = make('input', 'ios-ask-input');
      Object.assign(input, { type: 'text', value: def == null ? '' : String(def), autocapitalize: 'off', spellcheck: false });
      s.body.append(input);
      setTimeout(() => input.focus(), 260);
    }
    const acts = make('div', 'ios-ask-acts');
    const no = make('button', 'btn', 'Cancel'), yes = make('button', 'btn primary', 'OK');
    no.type = yes.type = 'button';
    no.onclick = () => s.close();
    yes.onclick = () => {
      s.close();
      answered = { kind, text: key, value: kind === 'prompt' ? input.value : true };
      replay(target);
      // (if the replayed tap asked nothing after all, don't let the answer leak into a later question)
      setTimeout(() => { answered = null; }, 400);
    };
    if (input) input.addEventListener('keydown', e => { if (e.key === 'Enter') yes.click(); });
    acts.append(no, yes);
    s.body.append(acts);
    return kind === 'prompt' ? null : false;
  }
  window.confirm = text => ask('confirm', text);
  window.prompt = (text, def) => ask('prompt', text, def);
  window.alert = text => { const s = sheet('ios-ask'); s.body.append(make('p', 'ios-ask-text', String(text))); const ok = make('button', 'btn primary', 'OK'); ok.type = 'button'; ok.onclick = () => s.close(); const a = make('div', 'ios-ask-acts'); a.append(ok); s.body.append(a); };

  // ---------- 3. long-press: the picture lifts, its actions under it ----------
  // The desktop's right-click menu is a column of sub-menus. Here the page still builds that menu
  // (out of sight), and this sheet re-presents it: the picture large on top, each sub-menu as a strip
  // you tap (stars for Rating, 1-10 for the level, chips for categories and collections), the plain
  // actions as a list. Every control presses the menu's own button, so nothing is reimplemented.
  const menuEl = document.querySelector('.lv-menu');
  function peek(card, x, y) {
    html.classList.add('ios-peeking'); // (keeps the desktop menu invisible while it is "open")
    card.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true, clientX: x, clientY: y }));
    const items = menuEl ? [...menuEl.children].filter(e => e.classList.contains('lv-item')) : [];
    if (!items.length) { html.classList.remove('ios-peeking'); return false; }
    const s = sheet('ios-peek', () => { html.classList.remove('ios-peeking'); menuEl.classList.add('hidden'); });
    const through = b => { s.close(); lastTap = b; b.click(); };
    const img = card.querySelector('img');
    if (img && (img.currentSrc || img.src)) {
      const pic = make('img', 'ios-peek-pic');
      pic.src = img.currentSrc || img.src; pic.alt = '';
      s.el.before(pic); // (above the sheet, on the dimmed backdrop)
    }
    const name = card._item && (card._item.name || card._item.file);
    if (name) s.body.append(make('p', 'ios-peek-name', name));
    const list = make('div', 'ios-list');
    for (const it of items) {
      const text = (it.querySelector(':scope > .lv-label') || it).textContent.trim();
      if (/^(Open|Open with its program|Show in folder)$/.test(text)) continue; // (a tap opens it; the other two need a PC)
      const sub = it.querySelector(':scope > .lv-sub');
      if (!sub) {
        const r = make('button', 'ios-row plain' + (it.classList.contains('danger') ? ' danger' : ''), text.replace(/…$/, ''));
        r.type = 'button';
        r.onclick = () => through(it);
        list.append(r);
        continue;
      }
      // a sub-menu: a labelled strip
      const kind = /^Rating/.test(text) ? 'k-stars' : /^NSFW/.test(text) ? 'k-level' : 'k-chips';
      const strip = make('div', 'ios-strip ' + kind);
      for (const o of sub.querySelectorAll(':scope > .lv-item')) {
        const t = (o.querySelector('.lv-label') || o).textContent.trim();
        const c = make('button', 'ios-chip' + (o.classList.contains('on') ? ' on' : ''), kind === 'k-stars' ? t.replace(/[^★]/g, '') : t.replace(/…$/, ''));
        c.type = 'button';
        c.onclick = () => through(o);
        strip.append(c);
      }
      const block = make('div', 'ios-peek-block');
      block.append(make('span', 'ios-peek-label', text), strip);
      s.body.append(block);
      requestAnimationFrame(() => { const on = strip.querySelector('.on'); if (on && kind === 'k-chips') strip.scrollLeft = Math.max(0, on.offsetLeft - 60); });
    }
    s.body.append(list);
    return true;
  }
  let hold = null;
  const cancelHold = () => { if (hold) { clearTimeout(hold.timer); hold = null; } };
  // the tap that a long press would otherwise end in is swallowed, but only that one: if the browser
  // sends none, the swallow must not wait around for the next real tap
  let eat = null;
  const swallowTap = () => { eat = c => { c.preventDefault(); c.stopPropagation(); }; addEventListener('click', eat, { capture: true, once: true }); };
  const dropSwallow = () => { if (eat) { const e = eat; eat = null; setTimeout(() => removeEventListener('click', e, true), 120); } };
  addEventListener('touchstart', e => {
    cancelHold();
    if (e.touches.length !== 1 || e.target.closest('input, textarea, select, video, .ios-sheet, .pv-screen, .ios-calc')) return;
    const t = e.touches[0], target = e.target;
    hold = { x: t.clientX, y: t.clientY, timer: setTimeout(() => {
      hold = null;
      const card = target.closest('#grid .card');
      if (card && !html.classList.contains('ios-dock-open')) { if (peek(card, t.clientX, t.clientY)) swallowTap(); return; }
      // anywhere else the desktop listens for a right-click, long-press is that right-click
      const ev = new MouseEvent('contextmenu', { bubbles: true, cancelable: true, clientX: t.clientX, clientY: t.clientY });
      if (!target.dispatchEvent(ev)) swallowTap();
    }, 420) };
  }, { passive: true });
  addEventListener('touchmove', e => { const t = e.touches[0]; if (hold && t && Math.hypot(t.clientX - hold.x, t.clientY - hold.y) > 10) cancelHold(); }, { passive: true });
  addEventListener('touchend', () => { cancelHold(); dropSwallow(); }, { passive: true });
  addEventListener('touchcancel', () => { cancelHold(); dropSwallow(); }, { passive: true });

  // ---------- 6. privacy ----------
  // "Hide now" (More, or a three-finger tap anywhere) is the emergency key. What it shows is the same
  // setting as on the PC (Settings → Privacy), with a phone's choices:
  //   a lock screen   the date and a big clock; swipe up like any lock screen and it asks your PIN
  //   a cat video     the PC's (ui/privacy.js)
  //   a calculator    it really calculates; type your PIN and press = to get back in
  // With no PIN set, the way back is to hold a finger down for a second (on the lock screen's clock,
  // on the calculator's display): something nobody tries by accident.
  const events = window.__TAURI__ && window.__TAURI__.event;
  const emit = payload => { if (events && events.emit) events.emit('panic', payload); };
  const pin = () => (typeof settings !== 'undefined' && settings.pin && settings.pin.hash ? settings.pin : null);
  const pinOk = async digits => {
    const p = pin();
    if (!p || !window.crypto || !crypto.subtle) return false;
    const key = await crypto.subtle.importKey('raw', new TextEncoder().encode(digits), 'PBKDF2', false, ['deriveBits']);
    const bits = await crypto.subtle.deriveBits({ name: 'PBKDF2', hash: 'SHA-256', salt: new TextEncoder().encode(p.salt), iterations: 150000 }, key, 256);
    return [...new Uint8Array(bits)].map(b => b.toString(16).padStart(2, '0')).join('') === p.hash; // (as ui/extras.js keeps it)
  };
  // (the PIN lock listens for Ctrl+L, see ui/extras.js; it does nothing when no PIN is set)
  const lockIfPin = () => dispatchEvent(new KeyboardEvent('keydown', { key: 'l', ctrlKey: true, bubbles: true }));
  const mode = () => (typeof S === 'function' && ['video', 'hide'].includes(S('panicMode')) ? S('panicMode') : 'screensaver');
  // the setting's own words, for a phone (the list is ui/privacy.js's; "hide" has no window to hide here)
  const WORDS = { screensaver: 'A lock screen', video: 'A cat video (or a video you pick)', hide: 'A calculator that opens with your PIN' };
  if (typeof SETTINGS !== 'undefined') {
    const spec = SETTINGS.find(x => x.key === 'panicMode');
    if (spec) { spec.label = 'Hide now shows'; spec.options = spec.options.map(([v]) => [v, WORDS[v] || v]); }
    const key = SETTINGS.find(x => x.key === 'panicOn');
    if (key) { key.label = 'Three-finger tap hides the app'; key.help = 'Tap anywhere with three fingers and Lewd Vault turns into something harmless at once. More → Hide now does the same.'; }
  }
  document.querySelectorAll('.pv-moderow option').forEach(o => { if (WORDS[o.value]) o.textContent = WORDS[o.value]; });

  function hideNow() {
    if (document.querySelector('.pv-screen, .ios-calc')) return;
    if (mode() === 'hide') { emit({ on: true, mode: 'hide' }); calculator(); } // (the message pauses everything that plays)
    else emit({ on: true, mode: mode() });
  }
  const comeBack = () => { emit({ on: false, mode: 'cover' }); lockIfPin(); };
  addEventListener('touchstart', e => {
    if (e.touches.length === 3 && (typeof S !== 'function' || S('panicOn') !== false)) hideNow();
  }, { passive: true, capture: true });

  // the lock screen: the PC's screensaver (the clock on soft colours), given a lock screen's furniture
  new MutationObserver(() => {
    const screen = document.querySelector('.pv-screen:not(.ios-dressed)');
    if (!screen) return;
    screen.classList.add('ios-dressed');
    const foot = make('div', 'ios-lockfoot');
    for (const name of ['torch', 'camera']) { const b = make('span', 'ios-lockbtn'); b.style.setProperty('--icon', icon(name)); foot.append(b); }
    screen.append(foot, make('span', 'ios-lockhint', pin() ? 'Swipe up to open' : ''), make('i', 'ios-homebar'));
    // a lock screen shows 11:15, not 11:15 AM (the PC's clock writes the time every second)
    const time = screen.querySelector('.pv-clock b');
    const trim = () => { const t = time.textContent.replace(/\s*[AP]\.?M\.?$/i, '').replace(/^0(\d:)/, '$1'); if (t !== time.textContent) time.textContent = t; };
    if (time) { new MutationObserver(trim).observe(time, { childList: true, characterData: true, subtree: true }); trim(); }
    let timer = null;
    gestures(screen, {
      up: () => { if (pin()) comeBack(); else { screen.classList.remove('nudge'); void screen.offsetWidth; screen.classList.add('nudge'); } },
      drag: (dx, dy, axis) => { if (axis === 'y' && dy < 0) screen.style.setProperty('--lift', Math.max(-90, dy / 3) + 'px'); },
      settle: () => screen.style.removeProperty('--lift')
    });
    screen.addEventListener('touchstart', e => { clearTimeout(timer); if (e.target.closest('.pv-clock')) timer = setTimeout(comeBack, 1000); }, { passive: true });
    for (const ev of ['touchend', 'touchcancel', 'touchmove']) screen.addEventListener(ev, () => clearTimeout(timer), { passive: true });
  }).observe(document.body, { childList: true });

  // the calculator: a working one. Your PIN then "=" opens the vault (the display never shows why);
  // with no PIN set, hold the display for a second.
  function calculator() {
    const calc = make('div', 'ios-calc');
    const display = make('div', 'ios-calc-display', '0');
    const keys = make('div', 'ios-calc-keys');
    calc.append(display, keys);
    let cur = '0', prev = null, op = null, fresh = true, typed = '';
    const fmt = v => { const n = Number(v); if (!isFinite(n)) return 'Error'; const t = String(+n.toPrecision(10)); return t.length > 11 ? n.toExponential(5) : t; };
    const draw = () => { display.textContent = fmt(cur).replace('.', ','); display.style.fontSize = display.textContent.length > 7 ? '56px' : ''; };
    const apply = () => {
      const a = Number(prev), b = Number(cur);
      cur = String(op === '+' ? a + b : op === '−' ? a - b : op === '×' ? a * b : b === 0 ? NaN : a / b);
      prev = null; op = null; fresh = true;
    };
    const open = () => { calc.remove(); emit({ on: false, mode: 'cover' }); };
    const press = async k => {
      if (/^\d$/.test(k)) { typed += k; cur = fresh || cur === '0' ? k : (cur + k).slice(0, 12); fresh = false; }
      else if (k === ',') { if (fresh) { cur = '0'; fresh = false; } if (!cur.includes('.')) cur += '.'; typed = ''; }
      else if (k === 'AC') { cur = '0'; prev = null; op = null; fresh = true; typed = ''; }
      else if (k === '±') { cur = String(-Number(cur)); typed = ''; }
      else if (k === '%') { cur = String(Number(cur) / 100); typed = ''; }
      else if (k === '=') {
        const digits = typed; typed = '';
        if (digits && await pinOk(digits)) return open();
        if (op) apply();
      } else { if (op && !fresh) apply(); prev = cur; op = k; fresh = true; typed = ''; }
      draw();
    };
    for (const k of ['AC', '±', '%', '÷', '7', '8', '9', '×', '4', '5', '6', '−', '1', '2', '3', '+', '0', ',', '=']) {
      const b = make('button', 'ios-calc-key' + (/[÷×−+=]/.test(k) ? ' op' : /AC|±|%/.test(k) ? ' fn' : '') + (k === '0' ? ' zero' : ''), k);
      b.type = 'button';
      b.onclick = () => press(k);
      keys.append(b);
    }
    let timer = null;
    display.addEventListener('touchstart', () => { clearTimeout(timer); if (!pin()) timer = setTimeout(open, 1000); }, { passive: true });
    for (const ev of ['touchend', 'touchcancel', 'touchmove']) display.addEventListener(ev, () => clearTimeout(timer), { passive: true });
    document.body.append(calc);
  }

  // in the background (the app switcher, a notification pulled down): a plain cover over everything,
  // so the switcher's snapshot shows nothing; back in front it lifts, to the PIN lock if there is one
  const cover = make('div', 'ios-cover');
  document.body.append(cover);
  let coveredAt = 0;
  const setCover = on => {
    if (on) { coveredAt = Date.now(); cover.classList.add('on'); document.querySelectorAll('video, audio').forEach(m => { try { m.pause(); } catch (e) {} }); }
    else { cover.classList.remove('on'); if (coveredAt && Date.now() - coveredAt > 1500) lockIfPin(); coveredAt = 0; }
  };
  document.addEventListener('visibilitychange', () => setCover(document.visibilityState !== 'visible'));
  addEventListener('pagehide', () => setCover(true));
  addEventListener('pageshow', () => { if (document.visibilityState === 'visible') setCover(false); });
})();
