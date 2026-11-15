// The phone layer: what the shared page needs to be a phone app (iPhone, iPad and Android).
//
// Only runs on a phone (the app sets html.phone there, plus html.ios or html.android, before any
// script). The shared page keeps doing all the work; this file re-presents its features the way a
// phone expects them:
//   1. a floating tab bar (Library, Browser, Add, Slideshow, More) and the More tiles,
//   2. the Library laid out like Photos, with search, filters and tags in sheets,
//   3. long-press a picture: it lifts into a preview with its actions under it (stars, level and
//      category as tap strips) in place of the desktop's right-click menu,
//   4. the viewer: the picture full screen, a glass toolbar, details as a pull-up sheet, swipes and
//      double-tap to appreciate; the slideshow takes the same gestures,
//   5. confirm() and prompt(): WKWebView silently drops them (confirm is always "no"), so every
//      "are you sure?" would block its action for good; they become in-app sheets (on Android too,
//      so both look the same),
//   6. privacy: "Hide now" (and a three-finger tap) in place of the emergency key, as a lock screen or
//      a working calculator that opens with your PIN, and a cover while the app is in the background,
//   7. Android's back button: it closes what is open (a sheet, the details, the viewer, the slideshow),
//      then goes back to the Library, and only then leaves the app,
//   8. phone-only extras: the level dial, Stories (rings over the Library) and Sort (swipe the new
//      ones into 1, 2 or 3 stars), and the motion that goes with them.
//   9. the Browser like Safari: an address bar at the bottom, a start page, tabs, a page menu, pull to
//      reload, swipes from the edges, and Customise (where the bar sits, how pictures are laid out...).
// The look is mobile/ui/phone.css.
(() => {
  'use strict';
  const html = document.documentElement;
  if (!html.classList.contains('phone')) return;
  const android = html.classList.contains('android');
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
    chevl: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2.600' stroke-linecap='round' stroke-linejoin='round'><path d='M15 5l-7 7 7 7'/></svg>",
    chevr: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2.600' stroke-linecap='round' stroke-linejoin='round'><path d='M9 5l7 7-7 7'/></svg>",
    sliders: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2.200' stroke-linecap='round'><path d='M4 7h9M17 7h3M4 17h3M11 17h9'/><circle cx='15' cy='7' r='2.200'/><circle cx='9' cy='17' r='2.200'/></svg>",
    reload: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2.400' stroke-linecap='round' stroke-linejoin='round'><path d='M19.500 12a7.500 7.500 0 1 1-2.200-5.300'/><path d='M19.500 4.500v4.200h-4.200'/></svg>",
    tabs: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2.100' stroke-linejoin='round'><rect x='4' y='7' width='13' height='13' rx='3'/><path d='M8 4h9a3 3 0 0 1 3 3v9'/></svg>",
    star: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2.100' stroke-linejoin='round'><path d='M12 3.500l2.600 5.400 5.900.8-4.300 4.100 1 5.800L12 16.900l-5.200 2.700 1-5.800L3.500 9.700l5.900-.8z'/></svg>",
    brush: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M14.500 4.500l5 5L11 18l-5-5z'/><path d='M6 13c-2 0-3 1.500-3 3.500S2 20 2 20s3.500.5 5-1 1-3.500 1-3.500'/></svg>",
    cards: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linejoin='round'><rect x='6' y='3' width='12' height='16' rx='2.500' transform='rotate(-8 12 11)'/><rect x='6' y='5' width='12' height='16' rx='2.500' transform='rotate(7 12 13)'/></svg>",
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
  // (a <div>, not a <nav>: the desktop's sidebar rules are `html.desktop nav …`, and a phone carries both classes)
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
    if (window.vaultPhone) tile('Sort', icon('cards'), () => window.vaultPhone.openSort());
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
    // and a little burst of sparks around it
    for (let k = 0; k < 8; k++) {
      const p = make('i', 'ph-spark');
      p.style.left = x + 'px'; p.style.top = y + 'px';
      p.style.setProperty('--a', (k * 45 + Math.random() * 20) + 'deg');
      p.style.setProperty('--r', (54 + Math.random() * 30) + 'px');
      document.body.append(p);
      setTimeout(() => p.remove(), 800);
    }
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
  // (Drive's browser sign-in: iOS suspends the app while Safari is in front; Android doesn't)
  const settingsTab = $('tab-settings');
  if (settingsTab && !android) new MutationObserver(() => {
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
    if (e.touches.length !== 1 || e.target.closest('input, textarea, select, video, .ios-sheet, .pv-screen, .ios-calc, .ph-story, .ph-sort, .ph-dial-back')) return;
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
    overlays.slice().reverse().forEach(o => o._close()); // (Stories and Sort go at once, not under the cover)
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
    // (the finger is still down when it opens: the tap its lift makes must not land on the app beneath)
    screen.addEventListener('touchstart', e => { clearTimeout(timer); if (e.target.closest('.pv-clock')) timer = setTimeout(() => { swallowTap(); comeBack(); }, 1000); }, { passive: true });
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
    display.addEventListener('touchstart', () => { clearTimeout(timer); if (!pin()) timer = setTimeout(() => { swallowTap(); open(); }, 1000); }, { passive: true });
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

  // ---------- 8. the level dial, Stories and Sort ----------
  // Three phone-only ways in to the same library:
  //   the level dial   NSFW 1-10 on a half circle you slide your thumb along (the viewer's toolbar,
  //                    Stories and Sort open it); it saves the moment you let go
  //   Stories          rings over the Library (New, Favourites, then the biggest categories): each
  //                    plays full screen, one picture after another, tap for the next, hold to pause
  //   Sort             the pictures you haven't sorted yet, as a stack of cards: swipe right to keep
  //                    (2 stars), left for 1 star, up for a favourite (3 stars and a heart)
  // All three write through the same calls the desktop uses (updateImage, recordView).
  const all = () => (typeof library !== 'undefined' && Array.isArray(library) ? library : []);
  const locked = it => typeof isLocked === 'function' && isLocked(it);
  const moving = it => it.kind !== 'image' && !/\.gif$/i.test(it.file || '');
  const fileUrl = it => (typeof vaultUrl === 'function' ? vaultUrl(it.id) : '');
  const thumbOf = it => (typeof thumbUrl === 'function' ? thumbUrl(it).catch(() => '') : Promise.resolve(''));
  const likes = it => (typeof likesOf === 'function' ? likesOf(it.id) : 0);
  const buzz = () => { try { if (navigator.vibrate) navigator.vibrate(8); } catch (e) {} };
  let redraw = null;
  const renderSoon = () => { clearTimeout(redraw); redraw = setTimeout(() => { if (typeof renderLibrary === 'function') renderLibrary(); }, 500); };
  // level and rating, saved the way the detail panel's Save does it (and the file is renamed to match)
  async function setProps(item, f) {
    const want = { category: item.category, nsfw: item.nsfw, quality: item.quality, ...f };
    try {
      const r = await api('updateImage', item.id, want);
      Object.assign(item, want, { uid: r.item.uid, file: r.item.file });
      libraryWritten(r);
      return true;
    } catch (e) { toast(errMsg(e), 'err'); return false; }
  }
  const appreciate = it => { if (typeof recordView === 'function') recordView(it, 'like'); };
  const overlays = [];               // what is open, topmost last (Android's back closes the last)
  const layer = (el, close) => { el._close = close; overlays.push(el); document.body.append(el); };
  const unlayer = el => { const k = overlays.indexOf(el); if (k >= 0) overlays.splice(k, 1); };

  // --- the dial ---
  // A half circle from 1 (left) round the top to 10 (right). Slide anywhere on it; the number follows
  // your thumb with a tick for each step, and letting go saves it.
  function dial(value, onPick) {
    const back = make('div', 'ph-dial-back');
    const box = make('div', 'ph-dial');
    const NS = 'http://www.w3.org/2000/svg';
    const svg = document.createElementNS(NS, 'svg');
    svg.setAttribute('viewBox', '0 0 340 190');
    svg.setAttribute('aria-hidden', 'true');
    const arc = cls => { const p = document.createElementNS(NS, 'path'); p.setAttribute('d', 'M20 170 A150 150 0 0 1 320 170'); p.setAttribute('pathLength', '100'); p.setAttribute('class', cls); svg.append(p); return p; };
    arc('ph-dial-track');
    const fill = arc('ph-dial-fill');
    box.append(svg);
    const ticks = [];
    for (let l = 1; l <= 10; l++) {
      const a = Math.PI * (1 - (l - 1) / 9);
      const t = make('button', 'ph-dial-tick', String(l));
      t.type = 'button';
      t.setAttribute('aria-label', 'Level ' + l);
      t.style.left = (170 + 150 * Math.cos(a)) + 'px';
      t.style.top = (170 - 150 * Math.sin(a)) + 'px';
      t.style.setProperty('--d', (l * 22) + 'ms');
      box.append(t);
      ticks.push(t);
    }
    const mid = make('div', 'ph-dial-mid');
    const num = make('b', 'ph-dial-num');
    mid.append(make('span', 'ph-dial-cap', 'Level'), num, make('span', 'ph-dial-hint', 'slide along the arc'));
    box.append(mid);
    back.append(box);
    let level = Math.min(10, Math.max(1, Math.round(+value || 1))), dragging = false, done = false;
    const draw = (bump) => {
      fill.style.strokeDasharray = `${(level - 1) / 9 * 100} 100`;
      box.style.setProperty('--heat', String((level - 1) / 9));
      num.textContent = level;
      ticks.forEach((t, i) => { t.classList.toggle('on', i + 1 <= level); t.classList.toggle('at', i + 1 === level); });
      if (bump) { num.classList.remove('bump'); void num.offsetWidth; num.classList.add('bump'); }
    };
    const close = () => {
      if (!back.isConnected) return;
      unlayer(back);
      back.classList.remove('in');
      setTimeout(() => back.remove(), 300);
    };
    const pick = l => {
      if (done) return;
      done = true;
      level = l; draw(true);
      box.classList.add('set');
      setTimeout(close, 260);
      if (l !== +value) onPick(l);
    };
    const levelAt = (x, y) => {
      const r = svg.getBoundingClientRect(), k = r.width / 340;
      const ang = Math.atan2((170 * k + r.top) - y, x - (170 * k + r.left)); // (0 at the right, pi at the left)
      const a = Math.min(Math.PI, Math.max(0, ang < -Math.PI / 2 ? Math.PI : ang));
      return Math.min(10, Math.max(1, Math.round(1 + (1 - a / Math.PI) * 9)));
    };
    box.addEventListener('pointerdown', e => {
      // (a drag may start on a number too: the numbers are buttons for keyboards and screen readers)
      dragging = true; box.setPointerCapture(e.pointerId); box.classList.add('drag');
      const l = levelAt(e.clientX, e.clientY);
      if (l !== level) { level = l; draw(true); buzz(); }
    });
    box.addEventListener('pointermove', e => {
      if (!dragging) return;
      const l = levelAt(e.clientX, e.clientY);
      if (l !== level) { level = l; draw(true); buzz(); }
    });
    box.addEventListener('pointerup', () => { if (dragging) { dragging = false; box.classList.remove('drag'); pick(level); } });
    box.addEventListener('pointercancel', () => { dragging = false; box.classList.remove('drag'); });
    ticks.forEach((t, i) => { t.onclick = e => { e.stopPropagation(); pick(i + 1); }; });
    back.addEventListener('click', e => { if (e.target === back) close(); });
    layer(back, close);
    draw(false);
    requestAnimationFrame(() => back.classList.add('in'));
  }

  // the viewer's toolbar gets the dial, as a button showing the level
  if (detail) {
    const bar = detail.querySelector('.ios-viewbar');
    const bin = bar && bar.lastElementChild;
    if (bar) {
      const lv = make('button', 'ios-tool ph-lvtool');
      lv.type = 'button'; lv.title = 'Level'; lv.setAttribute('aria-label', 'Level');
      lv.onclick = e => {
        e.stopPropagation();
        const it = typeof current !== 'undefined' ? current : null;
        if (!it) return;
        dial(it.nsfw, async l => {
          if (!(await setProps(it, { nsfw: l }))) return;
          const field = $('dNsfw'); if (field) field.value = l;
          lv.textContent = l; lv.classList.remove('pop'); void lv.offsetWidth; lv.classList.add('pop');
          renderSoon();
        });
      };
      bar.insertBefore(lv, bin);
      const show = () => { const it = typeof current !== 'undefined' ? current : null; const t = it ? String(it.nsfw) : ''; if (lv.textContent !== t) lv.textContent = t; };
      new MutationObserver(show).observe(detail, { attributes: true, attributeFilter: ['class'] });
      const name = $('dName');
      if (name) new MutationObserver(show).observe(name, { childList: true, characterData: true, subtree: true });
    }
  }

  // --- Stories ---
  const SEEN = 'lv.phone.stories';
  const seen = () => { try { return JSON.parse(localStorage.getItem(SEEN)) || {}; } catch (e) { return {}; } };
  const markSeen = key => { try { const s = seen(); s[key] = Date.now(); localStorage.setItem(SEEN, JSON.stringify(s)); } catch (e) {} };
  const shuffle = a => { for (let i = a.length - 1; i > 0; i--) { const j = Math.floor(Math.random() * (i + 1)); [a[i], a[j]] = [a[j], a[i]]; } return a; };
  const addedAt = it => Date.parse(it.added) || 0;
  function storyGroups() {
    const items = all().filter(it => !locked(it));
    const groups = [];
    const week = Date.now() - 7 * 864e5;
    const fresh = items.filter(it => addedAt(it) >= week).sort((a, b) => addedAt(b) - addedAt(a));
    if (fresh.length) groups.push({ key: 'new', name: 'New', items: fresh.slice(0, 30), newest: addedAt(fresh[0]) });
    const favs = items.filter(it => +it.quality >= 3);
    if (favs.length) groups.push({ key: 'fav', name: 'Favourites', items: shuffle(favs.slice()).slice(0, 24), newest: 0 });
    const by = new Map();
    for (const it of items) { const c = (it.category || '').trim(); if (c) (by.get(c) || by.set(c, []).get(c)).push(it); }
    [...by].sort((a, b) => b[1].length - a[1].length).slice(0, 12).forEach(([name, list]) => {
      groups.push({ key: 'c:' + name, name, items: shuffle(list.slice()).slice(0, 24), newest: Math.max(...list.map(addedAt)) });
    });
    return groups;
  }
  const isNew = g => { const s = seen()[g.key]; return !s || (g.newest && g.newest > s) || Date.now() - s > 20 * 3600e3; };

  // the rings, over the Library's grid
  let rings = null;
  const gridEl = $('grid');
  if (lib && gridEl) {
    rings = make('div', 'ph-rings');
    rings.setAttribute('role', 'list');
    gridEl.before(rings);
    const drawRings = () => {
      const groups = storyGroups();
      const left = toSort().length;
      rings.replaceChildren();
      rings.classList.toggle('hidden', !groups.length && !left);
      let n = 0;
      const ring = (cls, text, onTap, thumbItem) => {
        const b = make('button', 'ph-ring ' + cls);
        b.type = 'button';
        b.setAttribute('role', 'listitem');
        b.style.setProperty('--n', n++);
        const face = make('span', 'ph-ring-face');
        if (thumbItem) thumbOf(thumbItem).then(u => { if (u) face.style.backgroundImage = `url("${u}")`; });
        b.append(face, make('span', 'ph-ring-name', text));
        b.onclick = () => onTap(b);
        rings.append(b);
        return b;
      };
      if (left) {
        const s = ring('sort', 'Sort', () => openSort());
        s.firstChild.style.setProperty('--icon', icon('cards'));
        s.firstChild.append(make('em', 'ph-ring-count', left > 99 ? '99+' : String(left)));
      }
      groups.forEach((g, i) => ring(isNew(g) ? 'fresh' : 'old', g.name, b => openStories(groups, i, b), g.items[0]));
    };
    rings._draw = drawRings;
    // (re)drawn when the library loads or changes size, not on every render
    let sig = '';
    const maybe = () => {
      const items = all();
      const s = items.length + ':' + (items[0] && items[0].id) + ':' + (lib.classList.contains('hidden') ? 0 : 1);
      if (s !== sig) { sig = s; drawRings(); }
    };
    new MutationObserver(() => requestAnimationFrame(maybe)).observe(gridEl, { childList: true });
    if (typeof setupReady !== 'undefined') setupReady.then(() => setTimeout(maybe, 300));
  }

  function openStories(groups, gi, fromEl) {
    const el = make('div', 'ph-story');
    el.setAttribute('role', 'dialog');
    el.setAttribute('aria-label', 'Stories');
    if (fromEl) { const r = fromEl.getBoundingClientRect(); el.style.transformOrigin = `${r.left + r.width / 2}px ${r.top + r.height / 2}px`; }
    const stage = make('div', 'ph-story-stage');
    const bars = make('div', 'ph-story-bars');
    const head = make('div', 'ph-story-head');
    const face = make('span', 'ph-story-face');
    const words = make('div', 'ph-story-words');
    const title = make('b', '', ''), when = make('span', '', '');
    words.append(title, when);
    const x = make('button', 'ph-story-x');
    x.type = 'button'; x.setAttribute('aria-label', 'Close'); x.style.setProperty('--icon', icon('close'));
    head.append(face, words, x);
    const rail = make('div', 'ph-story-rail');
    const railBtn = (cls, label, iconName) => { const b = make('button', 'ph-rail ' + cls); b.type = 'button'; b.setAttribute('aria-label', label); if (iconName) b.style.setProperty('--icon', icon(iconName)); const t = make('span', '', ''); b.append(t); rail.append(b); return b; };
    const heart = railBtn('heart', 'Appreciate', 'heart');
    const lvBtn = railBtn('lv', 'Level', null);
    const openBtn = railBtn('open', 'Open', 'info');
    openBtn.lastChild.textContent = 'Open';
    const cap = make('div', 'ph-story-cap');
    el.append(stage, bars, head, rail, cap);

    let g = gi, i = 0, t0 = 0, paused = false, pausedAt = 0, raf = 0, holdTimer = null, held = false, media = null, closed = false;
    const DUR = 6000;
    const group = () => groups[g];
    const item = () => group().items[i];
    const close = () => {
      if (closed) return;
      closed = true;
      cancelAnimationFrame(raf);
      markSeen(group().key);
      unlayer(el);
      el.classList.add('out');
      if (media && media.pause) try { media.pause(); } catch (e) {}
      setTimeout(() => el.remove(), 320);
      if (rings && rings._draw) rings._draw();
    };
    const drawBars = () => {
      bars.replaceChildren(...group().items.map((_, k) => { const s = make('i', k < i ? 'done' : ''); s.append(make('b')); return s; }));
    };
    const show = async dir => {
      cancelAnimationFrame(raf);
      const it = item();
      if (media && media.pause) try { media.pause(); } catch (e) {}
      const slide = make('div', 'ph-story-slide' + (dir ? ' from-' + dir : ''));
      const blur = make('div', 'ph-story-blur');
      slide.append(blur);
      if (moving(it)) {
        const v = make('video');
        Object.assign(v, { muted: true, playsInline: true, autoplay: true, loop: false, preload: 'auto' });
        v.setAttribute('playsinline', ''); v.setAttribute('muted', '');
        v.src = fileUrl(it);
        media = v;
      } else {
        const img = make('img');
        img.alt = '';
        img.decoding = 'async';
        img.src = fileUrl(it);
        media = img;
      }
      slide.append(media);
      thumbOf(it).then(u => { if (u) blur.style.backgroundImage = `url("${u}")`; });
      const old = [...stage.children];
      stage.append(slide);
      old.forEach(o => { o.classList.add('leave'); setTimeout(() => o.remove(), 380); });
      if (media.play) media.play().catch(() => {});
      // the words
      const g0 = group();
      title.textContent = g0.name;
      when.textContent = `${i + 1} of ${g0.items.length}` + (it.added ? ' · ' + new Date(addedAt(it)).toLocaleDateString(undefined, { day: 'numeric', month: 'short' }) : '');
      thumbOf(g0.items[0]).then(u => { face.style.backgroundImage = u ? `url("${u}")` : ''; });
      cap.replaceChildren();
      cap.append(make('b', 'ph-story-name', it.name || ''));
      const chips = make('div', 'ph-story-chips');
      chips.append(make('span', 'hot', 'Level ' + it.nsfw));
      if (it.category) chips.append(make('span', '', it.category));
      if (+it.quality) chips.append(make('span', 'stars', '★'.repeat(+it.quality)));
      cap.append(chips, make('span', 'ph-story-hint', 'Tap for the next · hold to pause · swipe up to open'));
      const n = likes(it);
      heart.lastChild.textContent = n ? String(n) : '';
      heart.classList.toggle('on', n > 0);
      lvBtn.lastChild.textContent = String(it.nsfw);
      drawBars();
      if (typeof recordView === 'function') recordView(it);
      t0 = performance.now(); paused = false; pausedAt = 0;
      tick();
    };
    const length = () => (media && media.tagName === 'VIDEO' && isFinite(media.duration) && media.duration > 0 ? Math.min(media.duration * 1000, 60000) : DUR);
    const tick = () => {
      raf = requestAnimationFrame(() => {
        if (closed) return;
        const bar = bars.children[i] && bars.children[i].firstChild;
        let f;
        if (media && media.tagName === 'VIDEO' && isFinite(media.duration) && media.duration > 0) f = media.currentTime / Math.min(media.duration, 60);
        else f = paused ? (pausedAt - t0) / DUR : (performance.now() - t0) / DUR;
        if (bar) bar.style.transform = `scaleX(${Math.min(1, f)})`;
        if (!paused && (f >= 1 || (media && media.ended))) return next();
        tick();
      });
    };
    const pause = on => {
      if (paused === on) return;
      paused = on;
      el.classList.toggle('paused', on);
      if (on) { pausedAt = performance.now(); if (media && media.pause && media.tagName === 'VIDEO') media.pause(); }
      else { t0 += performance.now() - pausedAt; if (media && media.play && media.tagName === 'VIDEO') media.play().catch(() => {}); }
    };
    const next = () => {
      if (i < group().items.length - 1) { i++; show('right'); }
      else nextGroup(1);
    };
    const prev = () => {
      if (i > 0) { i--; show('left'); }
      else if (g > 0) nextGroup(-1, true);
      else { t0 = performance.now(); }
    };
    const nextGroup = (d, atEnd) => {
      markSeen(group().key);
      const k = g + d;
      if (k < 0 || k >= groups.length) return close();
      g = k; i = atEnd ? groups[k].items.length - 1 : 0;
      el.classList.remove('cube-l', 'cube-r'); void el.offsetWidth; el.classList.add(d > 0 ? 'cube-r' : 'cube-l');
      show(d > 0 ? 'right' : 'left');
    };
    x.onclick = e => { e.stopPropagation(); close(); };
    heart.onclick = e => {
      e.stopPropagation();
      const it = item();
      appreciate(it);
      heart.lastChild.textContent = String(likes(it));
      heart.classList.add('on'); heart.classList.remove('pop'); void heart.offsetWidth; heart.classList.add('pop');
      const r = heart.getBoundingClientRect(); heartAt(r.left + r.width / 2, r.top);
    };
    lvBtn.onclick = e => {
      e.stopPropagation();
      const it = item();
      pause(true);
      dial(it.nsfw, async l => { if (await setProps(it, { nsfw: l })) { lvBtn.lastChild.textContent = String(l); cap.querySelector('.hot').textContent = 'Level ' + l; renderSoon(); } });
      const wait = setInterval(() => { if (!document.querySelector('.ph-dial-back')) { clearInterval(wait); pause(false); } }, 200);
    };
    const openIt = () => {
      const it = item(), list = group().items;
      close();
      if (typeof openDetail === 'function') openDetail(it, list);
    };
    openBtn.onclick = e => { e.stopPropagation(); openIt(); };
    // touch: tap left/right, hold to pause, swipe sideways for another ring, down to close, up to open
    let from = null;
    stage.addEventListener('touchstart', e => {
      if (e.touches.length !== 1) return;
      from = { x: e.touches[0].clientX, y: e.touches[0].clientY, axis: '' };
      held = false;
      holdTimer = setTimeout(() => { held = true; pause(true); }, 220);
    }, { passive: true });
    stage.addEventListener('touchmove', e => {
      if (!from) return;
      const dx = e.touches[0].clientX - from.x, dy = e.touches[0].clientY - from.y;
      if (!from.axis && Math.hypot(dx, dy) > 12) { from.axis = Math.abs(dx) > Math.abs(dy) ? 'x' : 'y'; clearTimeout(holdTimer); pause(true); }
      if (from.axis === 'y' && dy > 0) { el.style.setProperty('--pull', dy + 'px'); el.classList.add('pulling'); }
      if (from.axis === 'x') stage.style.transform = `translateX(${dx / 3}px)`;
    }, { passive: true });
    const end = e => {
      clearTimeout(holdTimer);
      if (!from) return;
      const t = e.changedTouches[0], dx = t.clientX - from.x, dy = t.clientY - from.y, axis = from.axis;
      from = null;
      el.classList.remove('pulling'); el.style.removeProperty('--pull'); stage.style.transform = '';
      if (axis === 'y' && dy > 110) return close();
      if (axis === 'y' && dy < -80) return openIt();
      if (axis === 'x' && Math.abs(dx) > 70) { pause(false); return nextGroup(dx < 0 ? 1 : -1); }
      if (held || axis) { pause(false); return; }
      if (t.clientX < innerWidth * 0.3) prev(); else next();
    };
    stage.addEventListener('touchend', end, { passive: true });
    stage.addEventListener('touchcancel', () => { clearTimeout(holdTimer); from = null; pause(false); }, { passive: true });
    // a mouse (iPad with a trackpad): a click is a tap
    stage.addEventListener('click', e => { if (e.pointerType === 'mouse' || e.detail && !('ontouchstart' in window)) { if (e.clientX < innerWidth * 0.3) prev(); else next(); } });
    layer(el, close);
    requestAnimationFrame(() => el.classList.add('in'));
    show('');
  }

  // --- Sort ---
  let sortState = null;              // { done: Set of ids } (kept in the vault, so every device agrees)
  const sortReady = (typeof setupReady !== 'undefined' ? setupReady : Promise.resolve()).then(async () => {
    let v = null;
    try { v = await api('kvGet', 'phone-sort'); } catch (e) {}
    sortState = { done: new Set((v && v.done) || []) };
  });
  let sortSave = null;
  const saveSort = () => { clearTimeout(sortSave); sortSave = setTimeout(() => api('kvSet', 'phone-sort', { done: [...sortState.done] }).catch(() => {}), 800); };
  const toSort = () => (sortState ? all().filter(it => !sortState.done.has(it.id) && !locked(it)).sort((a, b) => addedAt(b) - addedAt(a)) : []);
  sortReady.then(() => { if (rings && rings._draw) rings._draw(); });

  async function openSort() {
    await sortReady;
    const queue = toSort();
    if (!queue.length) { toast('Everything is sorted', 'ok'); return; }
    const total = queue.length;
    const el = make('div', 'ph-sort');
    el.setAttribute('role', 'dialog');
    el.setAttribute('aria-label', 'Sort');
    const head = make('div', 'ph-sort-head');
    const words = make('div', '');
    const h = make('h2', '', 'Sort the new ones'), sub = make('span', '', '');
    words.append(h, sub);
    const doneBtn = make('button', 'ios-pill', 'Done');
    doneBtn.type = 'button';
    head.append(words, doneBtn);
    const prog = make('div', 'ph-sort-prog'); const progFill = make('i'); prog.append(progFill);
    const deck = make('div', 'ph-sort-deck');
    const hints = make('div', 'ph-sort-hints');
    hints.append(make('span', '', '← 1 star'), make('span', '', '↑ Favourite'), make('span', '', '2 stars →'));
    const acts = make('div', 'ph-sort-acts');
    const act = (cls, label, text) => { const b = make('button', 'ph-sort-act ' + cls); b.type = 'button'; b.setAttribute('aria-label', label); b.append(make('span', '', text)); acts.append(b); return b; };
    const undoBtn = act('undo', 'Undo', '↶');
    const meh = act('meh', 'One star', '★');
    const fav = act('fav', 'Favourite', '♥');
    const keep = act('keep', 'Two stars', '★★');
    el.append(head, prog, deck, hints, acts);

    let k = 0, sorted = 0, closed = false;
    const history = [];
    const card = it => {
      const c = make('div', 'ph-card');
      const pic = make('div', 'ph-card-pic');
      if (moving(it)) thumbOf(it).then(u => { if (u) pic.style.backgroundImage = `url("${u}")`; });
      else { pic.style.backgroundImage = `url("${fileUrl(it)}")`; thumbOf(it).then(u => { if (u && !c._loaded) c.style.setProperty('--ph', `url("${u}")`); }); }
      c.append(pic);
      const lv = make('button', 'ph-card-lv', 'Level ' + it.nsfw);
      lv.type = 'button';
      lv.onclick = e => { e.stopPropagation(); dial(it.nsfw, async l => { if (await setProps(it, { nsfw: l })) lv.textContent = 'Level ' + l; }); };
      c.append(lv);
      if (moving(it)) c.append(make('span', 'ph-card-play', '▶'));
      const stamp = make('span', 'ph-stamp', '');
      c.append(stamp, make('b', 'ph-card-name', it.name || ''));
      c._item = it; c._stamp = stamp;
      return c;
    };
    const drawDeck = () => {
      deck.replaceChildren();
      const top = queue[k], next = queue[k + 1];
      if (next) deck.append(Object.assign(card(next), { className: 'ph-card under' }));
      if (top) { const c = card(top); c.classList.add('top'); deck.append(c); drag(c); }
      sub.textContent = top ? `${k + 1} of ${total} · ${sorted} sorted` : 'All done';
      progFill.style.transform = `scaleX(${k / total})`;
      undoBtn.disabled = !history.length;
      if (!top) finish();
    };
    const finish = () => {
      deck.replaceChildren();
      const end = make('div', 'ph-sort-end');
      end.append(make('b', '', '✓'), make('p', '', sorted ? `${sorted} sorted. Nice.` : 'Nothing changed.'));
      deck.append(end);
      setTimeout(close, 1200);
    };
    const close = () => {
      if (closed) return;
      closed = true;
      unlayer(el);
      el.classList.remove('in');
      setTimeout(() => el.remove(), 320);
      if (sorted) renderSoon();
      if (rings && rings._draw) setTimeout(() => rings._draw(), 350);
    };
    const STARS = { left: 1, right: 2, up: 3 };
    const decide = async (dir, c) => {
      const it = queue[k];
      if (!it) return;
      const before = { quality: it.quality };
      c = c || deck.querySelector('.ph-card.top');
      if (c) { c.classList.add('fly-' + dir); c._stamp.textContent = dir === 'up' ? '♥ ★★★' : dir === 'right' ? '★★ KEEP' : '★'; c.classList.add('stamped', 's-' + dir); }
      buzz();
      history.push({ it, before, k });
      sortState.done.add(it.id); saveSort();
      sorted++; k++;
      setTimeout(drawDeck, 260);
      if (dir === 'up') appreciate(it);
      if (+it.quality !== STARS[dir]) await setProps(it, { quality: STARS[dir] });
    };
    const undo = async () => {
      const last = history.pop();
      if (!last) return;
      sortState.done.delete(last.it.id); saveSort();
      sorted--; k = last.k;
      if (+last.it.quality !== +last.before.quality) await setProps(last.it, { quality: last.before.quality });
      drawDeck();
      const c = deck.querySelector('.ph-card.top'); if (c) c.classList.add('back');
    };
    // the top card follows your finger: a tilt, and a stamp that says what letting go will do
    function drag(c) {
      let from = null;
      c.addEventListener('pointerdown', e => {
        if (e.target.closest('button')) return;
        from = { x: e.clientX, y: e.clientY, id: e.pointerId };
        c.setPointerCapture(e.pointerId);
        c.classList.add('held');
      });
      c.addEventListener('pointermove', e => {
        if (!from) return;
        const dx = e.clientX - from.x, dy = e.clientY - from.y;
        c.style.transform = `translate(${dx}px, ${dy}px) rotate(${dx / 14}deg)`;
        const dir = -dy > 90 && -dy > Math.abs(dx) ? 'up' : dx > 60 ? 'right' : dx < -60 ? 'left' : '';
        c.classList.remove('s-up', 's-left', 's-right');
        if (dir) { c.classList.add('s-' + dir, 'stamped'); c._stamp.textContent = dir === 'up' ? '♥ ★★★' : dir === 'right' ? '★★ KEEP' : '★'; }
        else c.classList.remove('stamped');
        c.style.setProperty('--tilt', String(Math.min(1, Math.hypot(dx, dy) / 140)));
      });
      const up = e => {
        if (!from) return;
        const dx = e.clientX - from.x, dy = e.clientY - from.y;
        from = null;
        c.classList.remove('held');
        const dir = -dy > 110 && -dy > Math.abs(dx) ? 'up' : dx > 100 ? 'right' : dx < -100 ? 'left' : '';
        if (dir) { c.style.setProperty('--x', dx + 'px'); c.style.setProperty('--y', dy + 'px'); c.style.transform = ''; decide(dir, c); }
        else { c.style.transform = ''; c.classList.remove('stamped', 's-up', 's-left', 's-right'); }
      };
      c.addEventListener('pointerup', up);
      c.addEventListener('pointercancel', () => { from = null; c.style.transform = ''; c.classList.remove('held', 'stamped'); });
    }
    meh.onclick = () => decide('left');
    keep.onclick = () => decide('right');
    fav.onclick = () => { decide('up'); const r = fav.getBoundingClientRect(); heartAt(r.left + r.width / 2, r.top - 20); };
    undoBtn.onclick = undo;
    doneBtn.onclick = close;
    layer(el, close);
    requestAnimationFrame(() => el.classList.add('in'));
    drawDeck();
  }
  window.vaultPhone = { openSort, dial };

  // --- motion ---
  // A freshly drawn grid (a new filter, a new order) ripples in from the top left, diagonal by
  // diagonal. Only whole redraws: a few cards added while scrolling just appear.
  if (gridEl) new MutationObserver(list => {
    const added = [];
    for (const m of list) for (const n of m.addedNodes) if (n.nodeType === 1 && n.classList.contains('card')) added.push(n);
    if (added.length < 6) return;
    const cols = Math.max(1, Math.round(gridEl.clientWidth / (added[0].offsetWidth || 130)));
    added.slice(0, 36).forEach((c, k) => {
      c.style.setProperty('--d', ((Math.floor(k / cols) + k % cols) * 30) + 'ms');
      c.classList.add('ph-in');
      setTimeout(() => c.classList.remove('ph-in'), 900);
    });
  }).observe(gridEl, { childList: true });
  // The viewer grows out of the picture you tapped, and closing it makes that picture land back.
  let lastCard = null;
  if (gridEl) gridEl.addEventListener('click', e => { const c = e.target.closest('.card'); if (c) lastCard = { el: c, r: c.getBoundingClientRect(), at: Date.now() }; }, true);
  if (detail) {
    let wasOpen = false;
    new MutationObserver(() => {
      const open = !detail.classList.contains('hidden');
      if (open === wasOpen) return;
      wasOpen = open;
      const big = detail.querySelector('.big');
      if (open && big && lastCard && Date.now() - lastCard.at < 900 && big.animate && !matchMedia('(prefers-reduced-motion: reduce)').matches) {
        const b = big.getBoundingClientRect(), r = lastCard.r;
        if (b.width && r.width) {
          const k = Math.max(r.width / b.width, r.height / b.height);
          big.animate([
            { transform: `translate(${r.left + r.width / 2 - (b.left + b.width / 2)}px, ${r.top + r.height / 2 - (b.top + b.height / 2)}px) scale(${k})`, opacity: .4, clipPath: `inset(${Math.max(0, (b.height - r.height / k) / 2)}px ${Math.max(0, (b.width - r.width / k) / 2)}px round ${14 / k}px)` },
            { transform: 'none', opacity: 1, clipPath: 'inset(0px 0px round 0px)' }
          ], { duration: 420, easing: 'cubic-bezier(.2, .9, .25, 1)' });
        }
      }
      if (!open && lastCard && lastCard.el.isConnected) {
        const c = lastCard.el;
        c.classList.remove('ph-land'); void c.offsetWidth; c.classList.add('ph-land');
        setTimeout(() => c.classList.remove('ph-land'), 700);
      }
    }).observe(detail, { attributes: true, attributeFilter: ['class'] });
  }

  // ---------- 9. the Browser, like Safari ----------
  // The desktop's Browser keeps its whole toolbar at the top (tabs, search, sort, levels, size,
  // categories), which on a phone is half the screen. Here the pictures get the screen and the
  // controls move where Safari keeps them:
  //   the address bar   at the bottom (or the top: your choice): back, the site and what you searched
  //                     (tap it to search), reload, the tabs and the page menu; it shrinks to a small
  //                     pill while you scroll down, like Safari's
  //   the start page    tapping the address: the search box over your favourite sites, bookmarks and
  //                     recent searches; the site's tag suggestions come up as you type
  //   the tabs          every source as a card, like Safari's tab overview
  //   the page menu     reload, bookmark, forward, filters (sort, levels, tile size, categories),
  //                     sources and keys, and Customise
  // plus pull down to reload and swipes from the screen's edges for back and forward. Every control
  // presses the Browser's own buttons, so nothing about searching or saving is done twice.
  const dtab = $('tab-discover');
  if (dtab) {
    const dq = sel => dtab.querySelector(sel);
    const KV = 'phone-browser';
    const DEF = { bar: 'bottom', layout: 'rows', autohide: true, filters: 'menu', tint: true, edges: true, blur: false, pull: true };
    let opt = { ...DEF };
    const RECENT = 'lv.phone.recent';
    const recent = () => { try { return JSON.parse(localStorage.getItem(RECENT)) || []; } catch (e) { return []; } };
    const remember = text => { const t = String(text || '').trim(); if (!t) return; try { localStorage.setItem(RECENT, JSON.stringify([t, ...recent().filter(x => x !== t)].slice(0, 10))); } catch (e) {} };
    const apply = () => {
      html.dataset.brBar = opt.bar;
      html.dataset.brLayout = opt.layout;
      html.classList.toggle('br-tint', !!opt.tint);
      html.classList.toggle('br-blur', !!opt.blur);
      html.classList.toggle('br-filters-top', opt.filters === 'top');
      placeFilters();
    };
    const saveOpt = () => { apply(); api('kvSet', KV, opt).catch(() => {}); };
    (typeof setupReady !== 'undefined' ? setupReady : Promise.resolve()).then(async () => {
      try { const v = await api('kvGet', KV); if (v && typeof v === 'object') opt = { ...DEF, ...v }; } catch (e) {}
      apply();
    });

    // the bar
    const sbar = make('div', 'ph-sbar');
    const pill = make('div', 'ph-url glass');
    const btn = (cls, label, iconName, onTap) => {
      const b = make('button', 'ph-sb ' + cls);
      b.type = 'button'; b.setAttribute('aria-label', label); b.title = label;
      if (iconName) b.style.setProperty('--icon', icon(iconName));
      b.onclick = e => { e.stopPropagation(); onTap(b); };
      return b;
    };
    const backB = btn('back', 'Back', 'chevl', () => { const b = dq('.dc-back'); if (b && !b.disabled) b.click(); });
    const fwdB = btn('fwd', 'Forward', 'chevr', () => { const b = dq('.dc-fwd'); if (b && !b.disabled) b.click(); });
    const pageB = btn('page', 'Filters', 'sliders', () => openFilters());
    const site = make('button', 'ph-url-site');
    site.type = 'button';
    const fav = make('span', 'ph-url-ico'), words = make('span', 'ph-url-text');
    site.append(fav, words);
    site.onclick = e => { e.stopPropagation(); if (sbar.classList.contains('mini')) return expand(); startPage(); };
    const reload = btn('reload', 'Reload', 'reload', () => doReload());
    const load = make('i', 'ph-url-load');
    pill.append(pageB, site, reload, load);
    const tabsB = btn('tabs', 'Tabs', 'tabs', () => openTabs());
    const tabsN = make('span', 'ph-tabs-n');
    tabsB.append(tabsN);
    const moreB = btn('more', 'Page menu', 'more', () => pageMenu());
    const glassBox = make('div', 'ph-sbar-glass glass');
    glassBox.append(backB, fwdB, pill, tabsB, moreB);
    sbar.append(glassBox);
    document.body.append(sbar);

    // what it shows follows the Browser: the site, its icon and colour, the search, back and forward
    const srcOn = () => dq('.dc-src.on');
    const follow = () => {
      const s = srcOn();
      const name = s ? s.textContent.trim() : 'Browser';
      const query = (dq('.dc-search input') || {}).value || '';
      const sub = dq('.dc-subpill:not(.hidden) b');
      const text = (sub ? sub.textContent : name) + (query ? ' — ' + query : '');
      if (words.textContent !== text) words.textContent = text;
      const img = s && s.querySelector('img.dc-ico');
      const src = img ? img.src : '';
      if (fav.dataset.src !== src) { fav.dataset.src = src; fav.style.backgroundImage = src ? `url("${src}")` : ''; }
      const color = getComputedStyle(dtab).getPropertyValue('--src').trim();
      if (sbar.style.getPropertyValue('--src') !== color) sbar.style.setProperty('--src', color);
      const b = dq('.dc-back'), f = dq('.dc-fwd');
      backB.disabled = !b || b.disabled;
      const canFwd = !!f && !f.disabled;
      if (fwdB.classList.contains('on') !== canFwd) fwdB.classList.toggle('on', canFwd);
      const n = dtab.querySelectorAll('.dc-src').length;
      if (tabsN.textContent !== String(n)) tabsN.textContent = n;
      const st = (dq('.dc-status') || {}).textContent || '';
      const loading = /^Looking on|trying again/.test(st);
      if (pill.classList.contains('loading') !== loading) pill.classList.toggle('loading', loading);
    };
    let fq = false;
    new MutationObserver(() => { if (!fq) { fq = true; requestAnimationFrame(() => { fq = false; follow(); }); } })
      .observe(dtab, { subtree: true, childList: true, characterData: true, attributes: true, attributeFilter: ['class', 'disabled', 'style'] });
    const inp = dq('.dc-search input');
    if (inp) inp.addEventListener('input', follow);
    follow();

    // the filters: in the page menu's sheet (the default), or across the top
    const filterNodes = ['.dc-lens', '.dc-rail', '.dc-marks'].map(dq).filter(Boolean);
    const head = dq('.dc-head');
    const fsheet = make('div', 'ios-dock ph-filters');
    const fback = make('div', 'ios-back ios-dock-back');
    fsheet.append(make('div', 'ios-grab'), make('h2', 'ios-sheet-title', 'Filters'));
    const fbody = make('div', 'ph-filters-body');
    fsheet.append(fbody);
    dtab.append(fback, fsheet);
    const showFilters = on => { fsheet.classList.toggle('in', on); fback.classList.toggle('in', on); html.classList.toggle('ios-dock-open', on); };
    fback.onclick = () => showFilters(false);
    function placeFilters() {
      if (opt.filters === 'top') { if (head) head.append(...filterNodes); }
      else fbody.append(...filterNodes);
    }
    placeFilters();
    function openFilters() { if (opt.filters === 'top') { scrollTo({ top: 0, behavior: 'smooth' }); expand(); return; } showFilters(true); }

    // the start page: the search box (the Browser's own, with its suggestions) over favourites,
    // bookmarks and recent searches
    const row = dq('.dc-searchrow');
    const rowHome = row && row.parentNode, rowNext = row && row.nextSibling;
    function startPage() {
      if (!row) return;
      const el = make('div', 'ph-start');
      el.setAttribute('role', 'dialog'); el.setAttribute('aria-label', 'Search');
      const scroll = make('div', 'ph-start-scroll');
      const section = (title, cls) => { const s = make('section', 'ph-start-sec ' + (cls || '')); s.append(make('h3', '', title)); scroll.append(s); return s; };
      const favs = section('Favourites', 'favs');
      const grid = make('div', 'ph-favs');
      dtab.querySelectorAll('.dc-src').forEach((s, k) => {
        const b = make('button', 'ph-fav' + (s.classList.contains('on') ? ' on' : '') + (s.classList.contains('locked') ? ' locked' : ''));
        b.type = 'button';
        b.style.setProperty('--n', k);
        const ic = make('span', 'ph-fav-ico');
        const img = s.querySelector('img.dc-ico');
        if (img) ic.style.backgroundImage = `url("${img.src}")`; else ic.textContent = s.textContent.trim().slice(0, 1);
        ic.style.setProperty('--src', s.style.getPropertyValue('--src'));
        b.append(ic, make('span', 'ph-fav-name', s.textContent.trim()));
        b.onclick = () => { close(); s.click(); };
        grid.append(b);
      });
      favs.append(grid);
      const marks = [...dtab.querySelectorAll('.dc-mark-go')];
      if (marks.length) {
        const sec = section('Bookmarks', 'marks');
        const list = make('div', 'ios-list');
        for (const m of marks) {
          const r = make('button', 'ios-row plain ph-mark', m.textContent.trim());
          r.type = 'button';
          r.onclick = () => { close(); m.click(); };
          list.append(r);
        }
        sec.append(list);
      }
      const rec = recent();
      if (rec.length) {
        const sec = section('Recent searches', 'recent');
        const chips = make('div', 'ph-recent');
        rec.forEach(t => { const c = make('button', 'ios-chip', t); c.type = 'button'; c.onclick = () => { inp.value = t; go(); }; chips.append(c); });
        const clear = make('button', 'ph-clear', 'Clear');
        clear.type = 'button';
        clear.onclick = () => { try { localStorage.removeItem(RECENT); } catch (e) {} sec.remove(); };
        sec.querySelector('h3').append(clear);
        sec.append(chips);
      }
      const dock = make('div', 'ph-start-dock glass');
      const cancel = make('button', 'ph-cancel', 'Cancel');
      cancel.type = 'button';
      cancel.onclick = () => close();
      dock.append(row, cancel);
      el.append(scroll, dock);
      // The search box goes back into the Browser before the Browser acts on Enter (it redraws its
      // toolbar, the box's buttons included), so this listens first, on the way down to the box.
      const go = () => inp.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }));
      const onKey = e => { if (e.key === 'Enter' && e.target === inp) { remember(inp.value); close(); } };
      el.addEventListener('keydown', onKey, true);
      const sugg = dq('.dc-sugg');
      let closed = false;
      function close() {
        if (closed) return;
        closed = true;
        unlayer(el);
        // (at once: whatever runs next, a favourite's tab or the search, redraws the box's buttons)
        if (rowHome) rowHome.insertBefore(row, rowNext && rowNext.parentNode === rowHome ? rowNext : null);
        inp.blur();
        el.classList.remove('in');
        setTimeout(() => el.remove(), 280);
      }
      layer(el, close);
      requestAnimationFrame(() => { el.classList.add('in'); inp.focus(); inp.select && inp.select(); });
      if (sugg) sugg.classList.add('hidden');
    }

    // the tabs: every source as a card
    function openTabs() {
      const s = sheet('ph-tabsheet');
      const top = make('div', 'ph-tabs-top');
      top.append(make('h2', 'ios-sheet-title', 'Tabs'));
      s.body.append(top);
      const grid = make('div', 'ph-tabgrid');
      dtab.querySelectorAll('.dc-src').forEach((src, k) => {
        const c = make('button', 'ph-tabcard' + (src.classList.contains('on') ? ' on' : '') + (src.classList.contains('locked') ? ' locked' : ''));
        c.type = 'button';
        c.style.setProperty('--src', src.style.getPropertyValue('--src'));
        c.style.setProperty('--n', k);
        const ic = make('span', 'ph-fav-ico');
        const img = src.querySelector('img.dc-ico');
        if (img) ic.style.backgroundImage = `url("${img.src}")`; else ic.textContent = src.textContent.trim().slice(0, 1);
        const dot = src.querySelector('.dc-health');
        const h = make('i', 'ph-health ' + (dot ? dot.className.replace('dc-health', '').trim() : ''));
        c.append(ic, make('b', '', src.textContent.trim()), make('small', '', src.classList.contains('locked') ? 'Needs a key' : src.title.split(' · ')[0].replace(/^https?:\/\//, '')), h);
        c.onclick = () => { s.close(); src.click(); };
        grid.append(c);
      });
      const add = make('button', 'ph-tabcard add');
      add.type = 'button';
      add.append(make('span', 'ph-fav-ico', '+'), make('b', '', 'Sources'), make('small', '', 'add, remove, keys'));
      add.onclick = () => { s.close(); const m = dq('.dc-manage'); if (m) m.click(); };
      grid.append(add);
      s.body.append(grid);
    }

    // the page menu
    function pageMenu() {
      const s = sheet('ph-pagemenu');
      const list = make('div', 'ios-list');
      const row = (text, iconName, onTap, cls) => {
        const r = make('button', 'ios-row' + (cls ? ' ' + cls : ''), text);
        r.type = 'button';
        if (iconName) r.style.setProperty('--icon', icon(iconName));
        r.onclick = () => { s.close(); onTap(); };
        list.append(r);
      };
      const f = dq('.dc-fwd');
      if (f && !f.disabled) row('Forward', 'chevr', () => f.click());
      row('Reload', 'reload', doReload);
      // (it asks for a name: the answer replays the tap, so the tap must be the star's own)
      row('Bookmark this search', 'star', () => { const m = dq('.dc-markbtn'); if (m) { lastTap = m; m.click(); } });
      row('Filters', 'sliders', () => openFilters());
      row('Sources and keys', 'tabs', () => { const m = dq('.dc-manage'); if (m) m.click(); });
      row('Customise the Browser', 'brush', customise);
      s.body.append(make('h2', 'ios-sheet-title', words.textContent || 'Browser'), list);
    }

    // Customise: how the Browser looks and behaves on this phone (kept in the vault)
    function customise() {
      const s = sheet('ph-custom');
      s.body.append(make('h2', 'ios-sheet-title', 'Customise the Browser'));
      const group = (title, choices, key) => {
        const g = make('div', 'ph-opt');
        g.append(make('span', 'ios-peek-label', title));
        const seg = make('div', 'ph-optseg');
        for (const [v, text] of choices) {
          const b = make('button', 'ios-chip' + (opt[key] === v ? ' on' : ''), text);
          b.type = 'button';
          b.onclick = () => { opt[key] = v; seg.querySelectorAll('button').forEach(x => x.classList.toggle('on', x === b)); saveOpt(); };
          seg.append(b);
        }
        g.append(seg);
        s.body.append(g);
      };
      group('Address bar', [['bottom', 'Bottom'], ['top', 'Top']], 'bar');
      group('Pictures', [['rows', 'Whole'], ['grid2', '2 across'], ['grid3', '3 across'], ['feed', 'Feed']], 'layout');
      group('Filters', [['menu', 'In a sheet'], ['top', 'At the top']], 'filters');
      const list = make('div', 'ios-list ph-toggles');
      const toggle = (key, text, help) => {
        const l = make('label', 'ios-row plain ph-toggle');
        const w = make('span', 'ph-toggle-words');
        w.append(make('b', '', text));
        if (help) w.append(make('small', '', help));
        const cb = make('input');
        cb.type = 'checkbox'; cb.checked = !!opt[key];
        cb.onchange = () => { opt[key] = cb.checked; saveOpt(); };
        l.append(w, cb);
        list.append(l);
      };
      toggle('autohide', 'Hide the bars while scrolling', 'Scroll back up, or tap the little pill, to bring them back.');
      toggle('tint', 'Colour the bar like the site', 'Each site\'s own colour, like Safari\'s website tinting.');
      toggle('edges', 'Swipe from the edges', 'From the left edge to go back, from the right to go forward.');
      toggle('pull', 'Pull down to reload');
      toggle('blur', 'Blur the previews', 'Pictures stay blurred in the grid until you open one.');
      s.body.append(list);
      const reset = make('button', 'btn ph-reset', 'Back to the defaults');
      reset.type = 'button';
      reset.onclick = () => { opt = { ...DEF }; saveOpt(); s.close(); setTimeout(customise, 260); };
      s.body.append(reset);
    }

    function doReload() {
      if (!inp) return;
      inp.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }));
      reload.classList.remove('spin'); void reload.offsetWidth; reload.classList.add('spin');
    }

    // shrinking while you scroll down; any scroll up (or a tap on the pill) brings it back
    const on = () => !dtab.classList.contains('hidden');
    const expand = () => { sbar.classList.remove('mini'); html.classList.remove('br-min'); };
    let lastY = scrollY;
    addEventListener('scroll', () => {
      if (!on()) return;
      const y = scrollY, d = y - lastY;
      if (Math.abs(d) < 8) return;
      lastY = y;
      if (!opt.autohide) return;
      const mini = d > 0 && y > 120;
      if (sbar.classList.contains('mini') !== mini) { sbar.classList.toggle('mini', mini); html.classList.toggle('br-min', mini); }
    }, { passive: true });
    new MutationObserver(() => { if (!on()) expand(); html.classList.toggle('br-on', on()); }).observe(dtab, { attributes: true, attributeFilter: ['class'] });
    html.classList.toggle('br-on', on());

    // a post, opened: swipe sideways for the next one, down to close (its own arrows are hidden)
    const view = dq('.dc-view'), stage = dq('.dc-stage');
    if (view && stage) gestures(stage, {
      left: () => { const n = dq('.dc-next'); if (n) n.click(); },
      right: () => { const p = dq('.dc-prev'); if (p) p.click(); },
      down: () => { if (view.scrollTop <= 0) document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })); },
      double: (x, y) => { const s = view.querySelector('.dc-side .btn.primary'); if (s) { s.click(); heartAt(x, y); } },
      drag: (dx, dy, axis) => {
        const m = stage.querySelector('.dc-media');
        if (!m) return;
        m.style.transition = 'none';
        m.style.transform = axis === 'x' ? `translateX(${dx}px) rotate(${dx / 40}deg)` : dy > 0 && view.scrollTop <= 0 ? `translateY(${dy}px) scale(${Math.max(.8, 1 - dy / 900)})` : '';
      },
      settle: () => { const m = stage.querySelector('.dc-media'); if (m) { m.style.transition = 'transform .28s cubic-bezier(.2, .8, .2, 1)'; m.style.transform = ''; } }
    }, 'button, video');

    // pull down to reload, and swipes in from the edges
    const ptr = make('div', 'ph-ptr');
    dtab.prepend(ptr);
    let touch = null;
    dtab.addEventListener('touchstart', e => {
      if (e.touches.length !== 1 || e.target.closest('.dc-view:not(.hidden), .ios-dock, input')) { touch = null; return; }
      const t = e.touches[0];
      touch = { x: t.clientX, y: t.clientY, top: scrollY <= 0, edge: t.clientX < 22 ? 'l' : t.clientX > innerWidth - 22 ? 'r' : '' };
    }, { passive: true });
    dtab.addEventListener('touchmove', e => {
      if (!touch) return;
      const t = e.touches[0], dx = t.clientX - touch.x, dy = t.clientY - touch.y;
      if (opt.pull && touch.top && dy > 0 && Math.abs(dy) > Math.abs(dx) && scrollY <= 0) {
        ptr.style.setProperty('--p', String(Math.min(1, dy / 110)));
        ptr.classList.add('show');
      }
      if (opt.edges && touch.edge) {
        const d = touch.edge === 'l' ? dx : -dx;
        sbar.style.setProperty('--edge', Math.max(0, Math.min(1, d / 120)));
        dtab.style.setProperty('--edge-x', (touch.edge === 'l' ? 1 : -1) * Math.max(0, Math.min(60, d / 3)) + 'px');
        dtab.classList.add('edging');
      }
    }, { passive: true });
    dtab.addEventListener('touchend', e => {
      if (!touch) return;
      const t = e.changedTouches[0], dx = t.clientX - touch.x, dy = t.clientY - touch.y;
      if (ptr.classList.contains('show')) {
        if (dy > 110) { ptr.classList.add('go'); doReload(); setTimeout(() => ptr.classList.remove('go'), 900); }
        ptr.classList.remove('show'); ptr.style.removeProperty('--p');
      }
      if (dtab.classList.contains('edging')) {
        dtab.classList.remove('edging'); dtab.style.removeProperty('--edge-x');
        if (touch.edge === 'l' && dx > 90) backB.click();
        if (touch.edge === 'r' && dx < -90) { const f = dq('.dc-fwd'); if (f && !f.disabled) f.click(); }
      }
      touch = null;
    }, { passive: true });
  }

  // ---------- 7. Android's back button ----------
  // The web view goes back in its history when there is some, and leaves the app when there isn't.
  // So while anything is open, one history entry is kept pushed; back pops it, and the topmost thing
  // closes (and if more is open, the entry goes back on). With nothing open it goes, and the next
  // back leaves the app, as on any Android app.
  if (android) {
    const visible = sel => { const e = document.querySelector(sel); return !!e && !e.classList.contains('hidden'); };
    const libTab = () => document.querySelector('header nav button[data-tab="library"]');
    const onTab = () => { const b = document.querySelector('header nav button.on'); return b ? b.dataset.tab : 'library'; };
    // the topmost open thing, as a function that closes it (or null)
    const top = () => {
      if (document.querySelector('.pv-screen, .ios-calc, .pin-lock:not(.hidden)')) return null; // (never: those are the privacy covers)
      const back = [...document.querySelectorAll('.ios-back.in')].pop();
      if (back) return () => back.click();
      if (overlays.length) { const o = overlays[overlays.length - 1]; return () => o._close(); } // (the dial, Stories, Sort)
      const sheetClose = [...document.querySelectorAll('.dc-sheet:not(.hidden) .dc-sheet-head .btn')].pop(); // (Discover keeps a hidden one)
      if (sheetClose) return () => sheetClose.click();
      if (visible('#detail')) return () => {
        const d = $('detail');
        if (d.classList.contains('ios-info')) d.classList.remove('ios-info'); else $('closeDetail').click();
      };
      if (visible('#show')) return () => { const b = $('sClose'); if (b) b.click(); };
      if (visible('#collage')) return () => document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
      if (document.querySelector('.dc-view:not(.hidden)')) return () => document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
      if (document.querySelector('#tab-profile:not(.hidden)') || onTab() !== 'library') return () => { const b = libTab(); if (b) b.click(); };
      return null;
    };
    let pushed = false, popping = false;
    const sync = () => {
      if (popping) return;
      const open = !!top();
      if (open && !pushed) { history.pushState({ lv: 1 }, ''); pushed = true; }
      else if (!open && pushed) { pushed = false; popping = true; history.back(); setTimeout(() => { popping = false; }, 60); }
    };
    addEventListener('popstate', () => {
      if (popping) { popping = false; return; }
      pushed = false;
      const close = top();
      if (close) close();
      setTimeout(sync, 80);
    });
    let queued = false;
    new MutationObserver(() => { if (!queued) { queued = true; requestAnimationFrame(() => { queued = false; sync(); }); } })
      .observe(document.body, { subtree: true, childList: true, attributes: true, attributeFilter: ['class'] });
  }
})();
