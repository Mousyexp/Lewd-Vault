// Lewd Vault desktop: your own sites, in Discover, Videos and Audio.
//
// A site of your own is a recipe: where its pages are (the front page, a search, a category; {query},
// {page}, {sort}, {category} in the addresses), its orders and categories, and, only when the guesses
// aren't right, where on its pages the items, their pictures and titles, the file and the tags are (CSS
// selectors). Most sites need just the addresses: the items (links with a picture, repeated), the file
// (the page's <video>, <audio> or an .mp4 / .mp3 address in it) and the tags (links to tag or category
// pages) are found by themselves.
//
// The builder (Discover → Sources, Videos → ＋ Your own site, Audio → ＋ Your own site) tries the
// recipe on the real site and shows what it found before it's saved. Recipes can be shared as text
// (Copy / Paste). Discover's recipes are sources (kind "recipe"); Videos' and Audio's are in "sites",
// in the same file (%LOCALAPPDATA%\Image Vault\sources.json), whose addresses the app may then read.
//
// window.vaultSites: { list, random, details, build, sheet, lines, unlines }

(() => {
  'use strict';
  const make = window.vaultUI.make;
  const D = () => window.vaultDiscover;
  const enc = encodeURIComponent;
  const proxy = url => url ? vaultOrigin() + '/web?u=' + enc(url) : '';
  const clean = text => !D().NEVER_TEXT.test(text || '');

  // "Label = value" lines ⇄ [[label, value]]
  const lines = text => String(text || '').split('\n').map(l => l.trim()).filter(Boolean).map(l => {
    const k = l.indexOf('=');
    return k < 0 ? [l, l] : [l.slice(0, k).trim(), l.slice(k + 1).trim()];
  }).filter(([a]) => a); // ("Hot =" is an order with nothing added to the address)
  const unlines = pairs => (pairs || []).map(([a, b]) => a === b ? a : `${a} = ${b}`).join('\n');

  // ---------- reading a site ----------
  // An address from a recipe's pattern. [ ... ] is kept only after the first page (for sites whose
  // first page has no number: https://site.com/new/[page/{page}/]).
  function fill(tpl, { query = '', page = 1, first = 1, sort = '', category = '' }) {
    const q = String(query).trim();
    return String(tpl)
      .replace(/\[([^\]]*)\]/g, (m, inner) => page > first ? inner : '')
      .replace(/\{query\}/g, enc(q)).replace(/\{query\+\}/g, enc(q).replace(/%20/g, '+'))
      .replace(/\{query-\}/g, q.toLowerCase().replace(/[^\p{L}\p{N}]+/gu, '-').replace(/^-+|-+$/g, ''))
      .replace(/\{page\}/g, page).replace(/\{sort\}/g, sort).replace(/\{category\}/g, category);
  }
  const abs = (v, base) => { try { return new URL(String(v).replace(/&amp;/g, '&').replace(/\\\//g, '/').trim(), base).href; } catch (e) { return ''; } };
  async function getPage(url) {
    const html = await api('webJson', url, { Accept: 'text/html' });
    return { html, doc: new DOMParser().parseFromString(html, 'text/html') };
  }
  // a picture's real address (lazy-loading sites keep it in data-*)
  function imgSrc(img, base) {
    if (!img) return '';
    for (const a of ['data-src', 'data-original', 'data-lazy-src', 'data-lazy', 'data-thumb', 'data-webp', 'data-srcset', 'srcset', 'src', 'poster', 'data-poster']) {
      let v = img.getAttribute(a);
      if (!v || /^data:/.test(v) || /^(about:|javascript:)/.test(v)) continue;
      if (/srcset/.test(a)) v = v.split(',').pop().trim().split(/\s+/)[0];
      return abs(v, base);
    }
    const bg = (img.getAttribute('style') || '').match(/url\(["']?([^"')]+)/);
    return bg ? abs(bg[1], base) : '';
  }
  const firstClass = el => (el.getAttribute('class') || '').trim().split(/\s+/).filter(c => /^[\w-]+$/.test(c))[0] || '';
  const selOf = el => el.tagName.toLowerCase() + (firstClass(el) ? '.' + firstClass(el) : '');
  // The items on a list page, guessed: links holding a picture that repeat the same way most often.
  // (audio sites often list plain links, without pictures: picture = false)
  function autoItems(doc, picture = true) {
    const groups = new Map();
    for (const a of doc.querySelectorAll('a[href]')) {
      const href = a.getAttribute('href');
      if ((picture && !a.querySelector('img, [data-src], [style*="background"]')) || /^(#|javascript:|mailto:)/.test(href)) continue;
      if (!picture && (a.closest('nav, header, footer, menu') || a.textContent.trim().length < 3)) continue;
      // (the same markup with different kinds of address apart: /video/… items, /channels/… tiles)
      let shape = '';
      try { shape = new URL(href, 'https://x.invalid/').pathname.split('/').filter(Boolean).slice(0, -1).map(p => /\d/.test(p) ? '#' : p).join('/'); } catch (e) {}
      const key = (a.parentElement ? selOf(a.parentElement) : '') + ' > ' + selOf(a) + ' @' + shape;
      if (!groups.has(key)) groups.set(key, []);
      groups.get(key).push(a);
    }
    const best = [...groups.entries()].sort((x, y) => y[1].length - x[1].length)[0];
    if (!best || best[1].length < 3) return { selector: '', links: [] };
    // the box around each link (one per item), as a selector the recipe can keep
    const box = best[1][0].parentElement;
    const boxSel = box && box !== doc.body ? selOf(box) : '';
    const n = boxSel ? doc.querySelectorAll(boxSel).length : 0;
    return { selector: boxSel && n >= best[1].length && n <= best[1].length * 1.5 ? boxSel : best[0].split(' @')[0], links: best[1] };
  }
  const textOf = el => el ? (el.getAttribute('title') || el.textContent || '').replace(/\s+/g, ' ').trim() : '';
  const durOf = t => { const m = String(t || '').match(/(\d+):(\d\d)(?::(\d\d))?/); return m ? (m[3] ? +m[1] * 3600 + +m[2] * 60 + +m[3] : +m[1] * 60 + +m[2]) : 0; };
  function readItems(r, doc, url) {
    const it = r.item || {};
    let boxes;
    if (it.list) boxes = [...doc.querySelectorAll(it.list)];
    else { let a = autoItems(doc); if (!a.links.length && r.section === 'audio') a = autoItems(doc, false); boxes = a.links.map(l => a.selector.includes(' > ') ? l : l.closest(a.selector) || l); }
    const seen = new Set();
    return boxes.map(el => {
      const link = el.matches('a[href]') ? el : el.querySelector(it.link || 'a[href]');
      const href = link ? abs(link.getAttribute('href'), url) : '';
      const img = it.image ? el.querySelector(it.image) : el.matches('img') ? el : el.querySelector('img') || el.querySelector('[data-src], [style*="background"]');
      const title = it.title ? textOf(el.querySelector(it.title)) : (link && link.getAttribute('title')) || (img && img.getAttribute('alt')) || textOf(el.querySelector('[class*="title"], h2, h3, h4')) || textOf(link);
      const dEl = it.duration ? el.querySelector(it.duration) : el.querySelector('[class*="duration"], [class*="time"], [class*="length"]');
      const clipEl = el.querySelector('[data-preview], [data-trailer], [data-mp4], video source');
      const clip = clipEl ? abs(clipEl.getAttribute('data-preview') || clipEl.getAttribute('data-trailer') || clipEl.getAttribute('data-mp4') || clipEl.getAttribute('src') || '', url) : '';
      return { url: href, id: href, title: title.slice(0, 200), thumb: imgSrc(img, url), duration: durOf(dEl && dEl.textContent), clip: /\.(mp4|webm)(\?|$)/i.test(clip) ? clip : '' };
    }).filter(x => x.url && (x.thumb || r.section === 'audio') && !seen.has(x.url) && seen.add(x.url) && clean(x.title));
  }
  // one page of a recipe's items: the words, or a category, or the front page; in an order; page n (1, 2, …)
  async function list(r, { q = '', sort = '', cat = '', page = 1 } = {}) { // eslint-disable-line prefer-const
    const first = Number.isFinite(+r.firstPage) && r.firstPage !== '' ? +r.firstPage : 1;
    let query = q, tpl;
    if (query) tpl = r.search;
    else if (cat && r.category) tpl = r.category;
    else if (cat) { tpl = r.search; query = cat; }
    else tpl = r.front || r.base;
    if (!tpl) throw new Error(q ? 'This site has no search address in its recipe' : 'This site has no address');
    if (!sort && (r.sorts || []).length) sort = r.sorts[0][1]; // (no order picked: its first)
    const url = fill(tpl, { query, page: first + page - 1, first, sort, category: cat });
    const { doc } = await getPage(url);
    return readItems(r, doc, url);
  }
  // somewhere in the site (or the category): a random page among the first few dozen, shuffled
  async function random(r, { cat = '' } = {}) {
    for (let top = +r.randomPages || 30; top >= 1; top = Math.floor(top / 3)) {
      const got = await list(r, { cat, page: 1 + Math.floor(Math.random() * top) }).catch(() => []);
      if (got.length) return got.sort(() => Math.random() - 0.5);
    }
    return [];
  }
  // An item's page: its file, its tags, its title.
  const MEDIA = { videos: /https?:[^"'\s<>]+?\.(?:mp4|webm|m4v)(?:\?[^"'\s<>]*)?/i, audio: /https?:[^"'\s<>]+?\.(?:mp3|m4a|ogg|opus|wav|flac)(?:\?[^"'\s<>]*)?/i };
  function autoFile(doc, html, section, base) {
    const attr = (sel, a) => { const el = doc.querySelector(sel); return el ? el.getAttribute(a) || '' : ''; };
    const plain = html.replace(/\\\//g, '/');
    if (section === 'audio') return attr('audio source[src]', 'src') || attr('audio[src]', 'src') || attr('meta[property="og:audio"]', 'content') || (plain.match(MEDIA.audio) || [])[0] || '';
    const vid = attr('video source[src]', 'src') || attr('video[src]', 'src') || attr('meta[property="og:video:secure_url"]', 'content') || attr('meta[property="og:video:url"]', 'content')
      || attr('meta[property="og:video"]', 'content') || (plain.match(MEDIA.videos) || [])[0] || '';
    if (vid || section === 'videos') return vid;
    // a picture: the page's own picture, else its biggest
    const og = attr('meta[property="og:image"]', 'content');
    if (og) return og;
    const imgs = [...doc.querySelectorAll('img')].map(i => ({ src: imgSrc(i, base), size: (+i.getAttribute('width') || 0) * (+i.getAttribute('height') || 0) })).filter(x => x.src);
    return (imgs.sort((a, b) => b.size - a.size)[0] || {}).src || '';
  }
  // tags: the page's own keywords, and links to tag / category pages outside its menus
  const NOT_TAGS = /^(categories|tags|see all|show all|more|all|view all|channels|models|pornstars|home|login|sign ?up)$/i;
  function autoTags(doc) {
    const kw = (doc.querySelector('meta[name="keywords"]') || {}).content || '';
    const out = kw.split(',').map(s => s.trim()).filter(s => s && s.length <= 40);
    for (const a of doc.querySelectorAll('a[href]')) {
      const h = a.getAttribute('href'), t = a.textContent.replace(/\s+/g, ' ').trim();
      if (t.length < 2 || t.length > 40 || NOT_TAGS.test(t) || a.closest('nav, header, footer, menu, [class*="menu"], [class*="nav"], [class*="dropdown"], [class*="footer"], [class*="header"]')) continue;
      if (!/\/(tags?|categor(?:y|ies)|keywords?|models?|pornstars?|channels?|genres?|niches?)\/|[?&](tags?|category)=/i.test(h)) continue;
      out.push(t);
    }
    return [...new Set(out.map(t => t.toLowerCase().replace(/^#/, '').replace(/\s+/g, '_')))].slice(0, 30);
  }
  async function details(r, url) {
    const { html, doc } = await getPage(url);
    let file = '';
    if (r.file) {
      if (r.file.startsWith('re:')) { const m = html.replace(/\\\//g, '/').match(new RegExp(r.file.slice(3), 'i')); file = m ? m[1] || m[0] : ''; }
      else { const el = doc.querySelector(r.file); file = el ? el.getAttribute('src') || el.getAttribute('href') || el.getAttribute('content') || el.getAttribute('data-src') || el.getAttribute('data-url') || '' : ''; }
    }
    if (!file) file = autoFile(doc, html, r.section, url);
    file = file ? abs(file, url) : '';
    if (/\.m3u8(\?|$)/i.test(file)) throw new Error('This site streams its videos in pieces (HLS), which the app can\'t play or save');
    if (!file) throw new Error(`No ${r.section === 'audio' ? 'audio' : r.section === 'videos' ? 'video' : 'picture or video'} found on that page. Set "The file" in the site's recipe (Advanced).`);
    const tags = (r.tags ? [...doc.querySelectorAll(r.tags)].map(el => el.textContent.replace(/\s+/g, ' ').trim().toLowerCase().replace(/^#/, '').replace(/\s+/g, '_')).filter(Boolean) : autoTags(doc))
      .filter(t => !D().NEVER.has(t)).slice(0, 30);
    const title = ((doc.querySelector('meta[property="og:title"]') || {}).content || (doc.querySelector('title') || {}).textContent || '').trim();
    if (!clean(title + ' ' + tags.join(' '))) throw new Error('This is tagged with something never shown here');
    const ext = (file.split('?')[0].match(/\.([a-z0-9]{2,4})$/i) || [, ''])[1].toLowerCase();
    return { file, tags, title, ext };
  }

  // ---------- a sheet (the builder, the customise panels) ----------
  function sheet(title, build) {
    const back = make('div', 'dc-sheet st-sheet');
    const card = make('div', 'dc-sheet-card');
    const head = make('div', 'dc-sheet-head');
    const x = make('button', 'btn', 'Close');
    head.append(make('h2', '', title), x);
    const body = make('div', 'dc-sheet-body');
    card.append(head, body);
    back.append(card);
    document.body.append(back);
    let done = null;
    const closed = new Promise(r => { done = r; });
    const close = v => { back.remove(); done(v); };
    x.onclick = () => close(null);
    back.addEventListener('pointerdown', e => { if (e.target === back) close(null); });
    build(body, close, head);
    return closed;
  }
  const field = (label, input, help) => {
    const f = make('label', 'st-field');
    f.append(make('span', 'st-label', label), input);
    if (help) f.append(make('small', 'st-help', help));
    return f;
  };
  const text = (value, placeholder) => { const i = make('input'); Object.assign(i, { type: 'text', value: value || '', placeholder: placeholder || '', spellcheck: false }); return i; };
  const area = (value, placeholder, rows = 4) => { const t = make('textarea'); Object.assign(t, { value: value || '', placeholder: placeholder || '', spellcheck: false, rows }); return t; };

  // ---------- the builder ----------
  const KIND_WORD = { discover: 'pictures and clips', videos: 'videos', audio: 'audio' };
  function build(section, start) {
    const r0 = { section, name: '', base: '', front: '', search: '', category: '', firstPage: 1, sorts: [], cats: [], item: {}, file: '', tags: '', randomPages: 30, ...(start || {}) };
    return sheet(start ? `Edit ${start.name}` : `Your own site: ${KIND_WORD[section]}`, (body, close) => {
      body.append(make('p', 'dc-sheet-lead', `Any site with ${KIND_WORD[section]}: give it a name and its pages' addresses. The app finds the items, the files and the tags itself; Try it shows what it found before you save. In the addresses, {query} is the words searched (or {query+} with + between them, {query-} with - between them), {page} the page number, {sort} the order and {category} the category. [ … ] is left out on the first page.`));
      const name = text(r0.name, 'e.g. My favourite tube');
      const base = text(r0.base, 'https://www.example.com');
      const front = text(r0.front, 'https://www.example.com/latest/[{page}/]');
      const search = text(r0.search, 'https://www.example.com/search/{query-}/[{page}/]');
      const category = text(r0.category, 'https://www.example.com/category/{category}/[{page}/]   (optional)');
      const firstPage = text(String(r0.firstPage ?? 1), '1'); firstPage.inputMode = 'numeric'; firstPage.className = 'st-short';
      const sorts = area(unlines(r0.sorts), 'Newest = latest\nMost viewed = most-viewed\nTop rated = top-rated', 4);
      const cats = area(unlines(r0.cats), 'Amateur = amateur\nCosplay = cosplay\n(or just one word a line: it\'s searched)', 5);
      const randomPages = text(String(r0.randomPages || 30), '30'); randomPages.className = 'st-short';
      // the advanced part: where things are on the pages (CSS selectors), when the guesses are wrong
      const it = r0.item || {};
      const iList = text(it.list, 'auto, e.g. div.video-item'), iLink = text(it.link, 'auto: the first link'), iImage = text(it.image, 'auto: the first picture');
      const iTitle = text(it.title, 'auto'), iDur = text(it.duration, 'auto');
      const file = text(r0.file, `auto: the page's ${section === 'audio' ? '<audio>' : '<video>'} or a file address in it · a selector, or re:regex`);
      const tags = text(r0.tags, 'auto: links to tag and category pages · e.g. .tags a');
      const adv = make('details', 'st-adv');
      adv.append(make('summary', '', 'Advanced: where things are on its pages'),
        field('Each item', iList, 'A CSS selector matching one box per item on a list page.'), field('Its link', iLink), field('Its picture', iImage), field('Its title', iTitle),
        ...(section !== 'discover' ? [field('Its length', iDur)] : []), field('The file (on an item\'s page)', file), field('The tags (on an item\'s page)', tags),
        field('Random picks from the first … pages', randomPages));
      if (it.list || it.link || it.image || it.title || r0.file || r0.tags) adv.open = true;
      const grid = make('div', 'st-grid');
      grid.append(field('Name', name), field('Address', base, 'Its home page.'));
      const pages = make('div', 'st-grid');
      pages.append(field('Front page', front, 'What shows before you search. Leave it empty to use the address above.'), field('Search', search),
        field('A category', category, 'Leave empty to search for the category instead.'), field('First page number', firstPage, 'Usually 1; some sites count from 0.'));
      const opts = make('div', 'st-grid');
      opts.append(field('Orders', sorts, 'One a line: Label = the {sort} word. Shown as buttons; Random is always there.'),
        field('Categories', cats, 'One a line: Label = the {category} word. Shown as chips.'));
      const out = make('div', 'st-out');
      const recipe = () => ({
        ...r0, name: name.value.trim(), base: base.value.trim().replace(/\/+$/, ''), front: front.value.trim(), search: search.value.trim(), category: category.value.trim(),
        firstPage: Math.max(0, parseInt(firstPage.value, 10) || 0), sorts: lines(sorts.value), cats: lines(cats.value), randomPages: Math.max(1, parseInt(randomPages.value, 10) || 30),
        item: Object.fromEntries(Object.entries({ list: iList.value.trim(), link: iLink.value.trim(), image: iImage.value.trim(), title: iTitle.value.trim(), duration: iDur.value.trim() }).filter(([, v]) => v)),
        file: file.value.trim(), tags: tags.value.trim()
      });
      const btns = make('div', 'dc-actions st-actions');
      const tryB = make('button', 'btn', '▶ Try it'), detect = make('button', 'btn', '✨ Find the items for me'), copy = make('button', 'btn', 'Copy recipe'), paste = make('button', 'btn', 'Paste a recipe'), save = make('button', 'btn primary', 'Save');
      tryB.onclick = () => tryIt(recipe(), out);
      detect.onclick = async () => {
        const r = recipe();
        if (!/^https?:\/\//.test(r.front || r.base)) return toast('Give the site\'s address first.', 'err');
        out.replaceChildren(make('div', 'vd-loading', 'Looking at its front page…'));
        try {
          await api('webAllow', r.base || r.front);
          const url = fill(r.front || r.base, { page: r.firstPage || 1, first: r.firstPage || 1 });
          const { doc } = await getPage(url);
          let a = autoItems(doc);
          if (!a.links.length && section === 'audio') a = autoItems(doc, false);
          if (!a.links.length) return out.replaceChildren(make('p', 'st-bad', 'No repeating links with pictures on that page. Try another page address (a list of videos, for example), or set "Each item" yourself.'));
          iList.value = a.selector.includes(' > ') ? '' : a.selector;
          adv.open = true;
          out.replaceChildren(make('p', 'st-good', `Found ${a.links.length} items${iList.value ? ` (each one is ${iList.value})` : ''}. Try it to see them.`));
        } catch (e) { out.replaceChildren(make('p', 'st-bad', errMsg(e))); }
      };
      copy.onclick = () => { navigator.clipboard.writeText(JSON.stringify(recipe(), null, 1)).then(() => toast('Recipe copied: paste it to a friend or into another PC', 'ok'), () => toast('Couldn\'t copy', 'err')); };
      paste.onclick = async () => {
        let r;
        try { r = JSON.parse(await navigator.clipboard.readText()); } catch (e) { r = null; }
        if (!r || typeof r !== 'object' || !r.base) return toast('The clipboard has no site recipe in it', 'err');
        const i = r.item || {};
        [[name, r.name], [base, r.base], [front, r.front], [search, r.search], [category, r.category], [firstPage, r.firstPage ?? 1], [sorts, unlines(r.sorts)], [cats, unlines(r.cats)],
         [randomPages, r.randomPages || 30], [iList, i.list], [iLink, i.link], [iImage, i.image], [iTitle, i.title], [iDur, i.duration], [file, r.file], [tags, r.tags]]
          .forEach(([el, v]) => { el.value = v == null ? '' : String(v); });
        toast(`Pasted the recipe for ${r.name || r.base}: Try it, then Save`, 'ok');
      };
      save.onclick = () => {
        const r = recipe();
        if (!r.name || !/^https?:\/\/[^/]+/.test(r.base)) return toast('Give it a name and its address (https://…).', 'err');
        close({ ...r, id: r.id || 's' + Date.now().toString(36) });
      };
      btns.append(tryB, detect, copy, paste, save);
      body.append(grid, pages, opts, adv, btns, out);
      setTimeout(() => (r0.name ? front : name).focus(), 50);
    });
  }
  // Try it: the front page (and a search), then the first item's page
  async function tryIt(r, out) {
    if (!/^https?:\/\//.test(r.base)) return out.replaceChildren(make('p', 'st-bad', 'Give the site\'s address first.'));
    out.replaceChildren(make('div', 'vd-loading', `Reading ${r.base.replace(/^https?:\/\//, '')}…`));
    try {
      await api('webAllow', r.base);
      for (const t of [r.front, r.search, r.category]) if (t && /^https?:\/\//.test(t)) await api('webAllow', t);
      const items = await list(r, {});
      const report = make('div', 'st-report');
      report.append(make('p', items.length ? 'st-good' : 'st-bad', items.length ? `✓ ${items.length} items on the front page` : '✗ No items on the front page. Try "Find the items for me", or set "Each item" in Advanced.'));
      if (items.length) {
        const shelf = make('div', 'st-shelf');
        shelf.append(...items.slice(0, 8).map(x => { const c = make('div', 'st-thumb'); const im = make('img'); if (x.thumb) im.src = proxy(x.thumb); im.alt = ''; c.append(im, make('span', '', (x.duration ? `${Math.floor(x.duration / 60)}:${String(x.duration % 60).padStart(2, '0')} · ` : '') + x.title)); return c; }));
        report.append(shelf);
      }
      out.replaceChildren(report);
      if (r.search) {
        const word = (r.cats[0] || [])[1] || 'amateur';
        const found = await list(r, { q: word }).catch(e => ({ error: errMsg(e) }));
        report.append(make('p', found.error || !found.length ? 'st-bad' : 'st-good', found.error ? `✗ Search: ${found.error}` : found.length ? `✓ Searching "${word}" finds ${found.length}` : `✗ Searching "${word}" found nothing`));
      }
      if (items.length) {
        report.append(make('p', 'st-wait', 'Opening the first item…'));
        try {
          const d = await details(r, items[0].url);
          report.lastChild.remove();
          report.append(make('p', 'st-good', `✓ Its file: ${d.file.replace(/^https?:\/\//, '').slice(0, 90)}`));
          const kind = /^(mp4|webm|m4v)$/.test(d.ext) ? 'video' : /^(mp3|m4a|ogg|opus|wav|flac)$/.test(d.ext) ? 'audio' : 'img';
          const m = make(kind); m.className = 'st-media';
          if (kind === 'img') m.src = proxy(d.file); else { m.src = vaultOrigin() + '/stream?u=' + enc(d.file); m.controls = true; m.muted = true; m.preload = 'metadata'; }
          report.append(m, make('p', d.tags.length ? 'st-good' : 'st-wait', d.tags.length ? `✓ Its tags: ${d.tags.slice(0, 12).join(', ')}` : 'No tags found on its page (they\'re optional).'));
        } catch (e) { report.lastChild.remove(); report.append(make('p', 'st-bad', `✗ Its page: ${errMsg(e)}`)); }
      }
    } catch (e) { out.replaceChildren(make('p', 'st-bad', `✗ ${errMsg(e)}`)); }
  }

  window.vaultSites = { list, random, details, build, sheet, field, text, area, lines, unlines, fill };
})();
