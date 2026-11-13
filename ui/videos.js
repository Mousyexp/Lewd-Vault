// Lewd Vault desktop: Videos (Phase 5 of PLAN.md), a sidebar section of its own.
//   - Yours: the long videos in your library (10 minutes or more) in rows like a streaming service:
//     Continue watching (it remembers where you stopped, kept with the vault), Newest, Most watched
//     and one row per category. They open in the full view, with the themed player, at that spot.
//   - Browse: long-video sites, in the same rows. Eporner and Rule34Video (fan-made 3D and 2D hentai)
//     play in the app's own player (streamed through the app a piece at a time, so it can jump
//     anywhere) and can be saved into the vault (downloaded to this PC first, with its progress);
//     Pornhub only hands its videos to its own player, so it plays embedded in a sandbox (no popups,
//     no redirects). Searching searches the site. Every title and tag goes through Discover's
//     never-show filter. (RedGifs' short clips moved to Discover.)
//   - Long videos (10 minutes or more) live here, not in the Library.
//   - Watch later: what you kept from the sites.

(() => {
  'use strict';
  if (!window.vaultUI || !inTauri) return;
  const { make, showTitle, TAB_KEY, thumbSrc } = window.vaultUI;
  const enc = encodeURIComponent;
  const wait = ms => new Promise(r => setTimeout(r, ms));
  const fmt = s => { s = Math.max(0, Math.round(s || 0)); const h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60, x = s % 60; return (h ? h + ':' + String(m).padStart(2, '0') : m) + ':' + String(x).padStart(2, '0'); };
  const LONG = 600;
  const D = () => window.vaultDiscover;
  const proxy = url => url ? vaultOrigin() + '/web?u=' + enc(url) : '';
  const clean = text => !D() || !D().NEVER_TEXT.test(text || '');

  // ---------- what's kept with the vault: where you stopped, and what you kept for later ----------
  let resume = {}, later = [];
  const loadKV = async () => {
    resume = (await api('kvGet', 'resume').catch(() => null)) || {};
    later = (await api('kvGet', 'watchlater').catch(() => null)) || [];
  };
  let saveTimer = null;
  const saveResume = () => { clearTimeout(saveTimer); saveTimer = setTimeout(() => api('kvSet', 'resume', resume).catch(() => {}), 1500); };
  const saveLater = () => api('kvSet', 'watchlater', later).catch(e => toast(errMsg(e), 'err'));
  const kvReady = setupReady.then(loadKV);
  libraryOk = i => !(i.kind === 'video' && i.duration >= LONG);
  const stream = url => vaultOrigin() + '/stream?u=' + enc(url);
  const secs = t => String(t || '').split(':').reduce((a, b) => a * 60 + (+b || 0), 0);

  // the full view remembers where a long video was left, and starts there next time
  const dv = document.getElementById('detailVideo');
  let lastSaved = 0;
  dv.addEventListener('timeupdate', () => {
    if (!current || !(current.duration >= LONG) || !dv.currentTime) return;
    if (Math.abs(dv.currentTime - lastSaved) < 5) return;
    lastSaved = dv.currentTime;
    if (dv.currentTime > current.duration * 0.95) delete resume[current.id];
    else if (dv.currentTime > 20) resume[current.id] = { t: Math.round(dv.currentTime), at: Date.now() };
    saveResume();
  });
  function playFrom(item, list) {
    openDetail(item, list);
    const r = resume[item.id];
    if (r && r.t) {
      const go = () => { if (current === item && dv.duration > r.t) dv.currentTime = r.t; };
      dv.addEventListener('loadedmetadata', go, { once: true });
    }
  }

  // ---------- the sidebar tile and the page ----------
  const navBtn = make('button');
  navBtn.dataset.tab = 'videos';
  navBtn.append(make('span', 'nt-title', 'Videos'), make('span', 'nt-sub', 'Long-form & sites'), make('span', 'nt-art'));
  const discoverBtn = document.querySelector('nav [data-tab="discover"]');
  (discoverBtn || document.querySelector('nav [data-tab="add"]')).after(navBtn);
  const tab = make('section', 'hidden');
  tab.id = 'tab-videos';
  tab.innerHTML = `
    <div class="vd-top">
      <div class="vd-modes"><button data-mode="yours" class="on">Yours</button><button data-mode="browse">Browse sites</button><button data-mode="later">Watch later</button></div>
      <div class="vd-sites hidden"></div>
      <div class="searchbar vd-search hidden"><span class="search-icon"></span><input type="search" spellcheck="false" placeholder="Search…"></div>
      <div class="vd-lens hidden"></div>
    </div>
    <div class="vd-body"></div>
    <div class="vd-theater hidden">
      <div class="vd-stage"></div>
      <aside class="vd-side"></aside>
    </div>`;
  document.querySelector('main').append(tab);
  const q = s => tab.querySelector(s);
  const body = q('.vd-body');
  let mode = 'yours', site = 'eporner';
  try { site = localStorage.getItem('imageVault.videoSite') || 'eporner'; } catch (e) {}
  // (RedGifs' short clips moved to Discover)

  navBtn.onclick = async () => {
    document.querySelectorAll('nav button').forEach(x => { x.classList.toggle('on', x === navBtn); const t = $('tab-' + x.dataset.tab); if (t) t.classList.toggle('hidden', x !== navBtn); });
    stopGrid();
    try { localStorage.setItem(TAB_KEY, 'videos'); } catch (e) {}
    showTitle();
    await kvReady;
    draw();
  };
  tab.querySelectorAll('.vd-modes button').forEach(b => b.onclick = () => {
    mode = b.dataset.mode;
    tab.querySelectorAll('.vd-modes button').forEach(x => x.classList.toggle('on', x === b));
    draw();
  });
  const input = q('.vd-search input');
  input.addEventListener('keydown', e => { if (e.key === 'Enter') { if (lens[site]) lens[site].random = false; draw(); } });

  // ---------- rows, cards, the hero ----------
  function row(title, cards, opts = {}) {
    if (!cards.length) return null;
    const r = make('section', 'vd-row');
    const head = make('div', 'vd-row-head');
    head.append(make('h3', '', title));
    if (opts.more) { const m = make('button', 'linkish', 'More'); m.onclick = opts.more; head.append(m); }
    const strip = make('div', 'vd-strip');
    strip.append(...cards);
    const left = make('button', 'vd-arrow left', '‹'), right = make('button', 'vd-arrow right', '›');
    left.onclick = () => strip.scrollBy({ left: -strip.clientWidth * 0.85, behavior: 'smooth' });
    right.onclick = () => strip.scrollBy({ left: strip.clientWidth * 0.85, behavior: 'smooth' });
    r.append(head, left, strip, right);
    return r;
  }
  // v: { title, thumb, frames?, clip?, duration, progress?, badge?, sub?, open() }
  function card(v) {
    const c = make('button', 'vd-card');
    c.type = 'button';
    const pic = make('div', 'vd-pic');
    const img = make('img'); img.alt = ''; img.loading = 'lazy'; img.decoding = 'async'; img.src = v.thumb;
    img.onerror = () => img.classList.add('broken');
    pic.append(img);
    if (v.duration) pic.append(make('span', 'vd-dur', fmt(v.duration)));
    if (v.duration >= LONG) pic.append(make('span', 'vd-long', 'LONG'));
    if (v.badge) pic.append(make('span', 'vd-badge', v.badge));
    if (v.progress) { const bar = make('span', 'vd-progress'); bar.append(make('i')); bar.firstChild.style.width = Math.min(100, v.progress * 100) + '%'; pic.append(bar); }
    c.append(pic, make('span', 'vd-title', v.title || ''));
    if (v.sub) c.append(make('span', 'vd-sub', v.sub));
    c.title = v.title || '';
    // a moving preview on hover: the clip itself, or the site's frames one after another
    let timer = null, vid = null;
    c.addEventListener('pointerenter', () => {
      timer = setTimeout(() => {
        if (v.clip) {
          vid = make('video'); Object.assign(vid, { muted: true, loop: true, autoplay: true, playsInline: true, src: v.clip });
          vid.addEventListener('playing', () => vid.classList.add('in'), { once: true });
          pic.append(vid);
        } else if (v.frames && v.frames.length > 1) {
          let k = 0;
          const tick = () => { k = (k + 1) % v.frames.length; img.src = v.frames[k]; timer = setTimeout(tick, 650); };
          tick();
        }
      }, 350);
    });
    c.addEventListener('pointerleave', () => { clearTimeout(timer); if (vid) { vid.remove(); vid = null; } if (v.frames) img.src = v.thumb; });
    c.onclick = () => v.open();
    return c;
  }
  function hero(v, actions) {
    const h = make('section', 'vd-hero');
    const bg = make('div', 'vd-hero-bg');
    if (v.clip) { const vid = make('video'); Object.assign(vid, { muted: true, loop: true, autoplay: true, playsInline: true, src: v.clip }); bg.append(vid); }
    else { const im = make('img'); im.src = v.thumb; im.alt = ''; bg.append(im); }
    const text = make('div', 'vd-hero-text');
    text.append(make('h2', '', v.title || ''), make('p', '', v.sub || ''));
    const btns = make('div', 'vd-hero-btns');
    for (const [label, run, primary] of actions) { const b = make('button', 'btn' + (primary ? ' primary' : ''), label); b.onclick = run; btns.append(b); }
    text.append(btns);
    h.append(bg, text);
    return h;
  }
  const empty = (text, btn) => { const e = make('div', 'empty vd-empty', text); if (btn) e.append(btn); return e; };

  // ---------- Yours: the long videos in the library ----------
  function drawYours() {
    const all = (library || []).filter(i => i.kind === 'video' && i.duration >= LONG);
    if (!all.length) return body.replaceChildren(empty('No long videos yet. Videos of 10 minutes or more show here, with Continue watching.'));
    const asCard = i => card({
      title: i.name, thumb: thumbSrc(i, 480), clip: i.previewId ? vaultUrl(i.previewId) : '', duration: i.duration,
      progress: resume[i.id] ? resume[i.id].t / i.duration : 0, sub: `${i.category} · ${fmtN(viewsOf(i.id))} ${viewsOf(i.id) === 1 ? 'view' : 'views'}`,
      open: () => playFrom(i, all)
    });
    const going = all.filter(i => resume[i.id]).sort((a, b) => resume[b.id].at - resume[a.id].at);
    const newest = [...all].sort((a, b) => b.added.localeCompare(a.added));
    const watched = [...all].sort((a, b) => viewsOf(b.id) - viewsOf(a.id) || timeOf(b.id) - timeOf(a.id)).filter(i => viewsOf(i.id) > 0);
    const cats = [...new Set(all.map(i => i.category))].sort((a, b) => all.filter(i => i.category === b).length - all.filter(i => i.category === a).length).slice(0, 6);
    const top = going[0] || newest[Math.floor(Math.random() * Math.min(5, newest.length))];
    const r = resume[top.id];
    body.replaceChildren(...[
      hero({ title: top.name, sub: `${top.category} · ${fmt(top.duration)}${r ? ' · stopped at ' + fmt(r.t) : ''}`, clip: top.previewId ? vaultUrl(top.previewId) : '', thumb: thumbSrc(top, 960) },
        [[r ? `▶ Resume at ${fmt(r.t)}` : '▶ Play', () => playFrom(top, all), true], ...(r ? [['Start over', () => { delete resume[top.id]; saveResume(); playFrom(top, all); }]] : [])]),
      row('Continue watching', going.map(asCard)),
      row('Newest', newest.slice(0, 40).map(asCard)),
      row('Most watched', watched.slice(0, 40).map(asCard)),
      ...cats.map(c => row(c, newest.filter(i => i.category === c).slice(0, 40).map(asCard)))
    ].filter(Boolean));
  }

  // ---------- Browse: the sites ----------
  const SITES = {
    eporner: {
      name: 'Eporner', icon: 'https://www.eporner.com/favicon.ico', own: true,
      rows: q => q ? [['Results', `query=${enc(q)}&order=most-popular`], ['Long ones', `query=${enc(q)}&order=longest`]] :
        [['Top this week', 'query=&order=top-weekly'], ['Most popular', 'query=&order=most-popular'], ['Top rated', 'query=&order=top-rated'], ['Newest', 'query=&order=latest'], ['Long ones', 'query=&order=longest']],
      async load(params) {
        const d = JSON.parse(await api('webJson', `https://www.eporner.com/api/v2/video/search/?${params}&per_page=30&thumbsize=big&format=json`));
        return (d.videos || []).filter(v => clean(v.title + ' ' + v.keywords)).map(v => ({
          site: 'eporner', id: v.id, title: v.title, thumb: proxy(v.default_thumb && v.default_thumb.src), frames: (v.thumbs || []).map(t => proxy(t.src)),
          duration: v.length_sec, sub: `${fmtN(v.views)} views · ★ ${v.rate}`, url: v.url, embed: v.embed, tags: String(v.keywords || '').split(',').map(s => s.trim()).filter(Boolean).slice(0, 12)
        }));
      },
      // the video's file: its page lists one per quality (the best up to 1080p, not AV1)
      async resolve(v) {
        const html = await api('webJson', v.url, { Accept: 'text/html' });
        const q = l => +(l.match(/\/(\d{3,4})\//) || [])[1] || 0;
        const links = [...html.matchAll(/href="(\/dload\/[^"]+\.mp4)"/g)].map(m => m[1]).filter(l => !/-av1\.mp4$/.test(l)).sort((x, y) => q(y) - q(x));
        const best = links.find(l => q(l) <= 1080) || links[0];
        if (!best) throw new Error('Eporner has no file for this video');
        return { file: 'https://www.eporner.com' + best, quality: q(best) + 'p' };
      }
    },
    rule34video: {
      name: 'Rule34Video', icon: 'https://rule34video.com/apple-touch-icon.png', own: true, hentai: true,
      rows: q => q ? [['Results', `search/${enc(q.trim().replace(/\s+/g, '-'))}/`]] :
        [['Most popular', 'most-popular/'], ['Top rated', 'top-rated/'], ['Newest', 'latest-updates/'], ['Newest, page 2', 'latest-updates/2/']],
      async load(path) {
        const html = await api('webJson', 'https://rule34video.com/' + path, { Accept: 'text/html' });
        const doc = new DOMParser().parseFromString(html, 'text/html');
        return [...doc.querySelectorAll('.item.thumb[data-video-card-id]')].map(el => {
          const a = el.querySelector('a.th'), img = el.querySelector('img.thumb'), prev = el.querySelector('[data-preview]');
          const title = (a && a.getAttribute('title')) || ((el.querySelector('.thumb_title') || {}).textContent || '').trim();
          const views = ((el.querySelector('.video-views-count') || {}).textContent || '').trim(), rate = ((el.querySelector('.video-card-meta__rating') || {}).textContent || '').trim().split(' ')[0];
          return {
            site: 'rule34video', id: el.dataset.videoCardId, title, thumb: img ? proxy(img.getAttribute('src')) : '', clip: prev ? prev.dataset.preview : '',
            duration: secs(((el.querySelector('.time') || {}).textContent || '').trim()), sub: [views && views + ' views', rate].filter(Boolean).join(' · '),
            url: a ? a.getAttribute('href') : '', tags: []
          };
        }).filter(v => v.url && clean(v.title));
      },
      // the video's file (720p when there's one) and its tags
      async resolve(v) {
        const html = await api('webJson', v.url, { Accept: 'text/html' });
        const files = [...html.matchAll(/href="(https:\/\/rule34video\.com\/get_file\/[^"]+)"/g)].map(m => m[1].replace(/&amp;/g, '&'));
        const q = l => +(l.match(/_(\d{3,4})p?\.mp4/) || [])[1] || 0;
        files.sort((x, y) => q(y) - q(x));
        const best = files.find(l => q(l) <= 1080) || files[0] || (html.match(/video_url:\s*'([^']+)'/) || [])[1];
        if (!best) throw new Error('Rule34Video has no file for this video');
        const tags = [...html.matchAll(/href="https:\/\/rule34video\.com\/tags\/([^/"]+)\/"/g)].map(m => decodeURIComponent(m[1]).replace(/-/g, '_')).filter((t, k, all) => all.indexOf(t) === k).slice(0, 24);
        if (!clean(tags.join(' '))) throw new Error('This video is tagged with something never shown here');
        return { file: best.replace(/^function\/0\//, ''), quality: (q(best) || 360) + 'p', tags: tags.length ? tags : v.tags };
      }
    },
    hypnotube: {
      // Hypnotube: hypno, sissy, gooning and audio videos people make and upload. Read from its pages; its
      // search answers only through a session (the form is sent first: webFormThenGet).
      name: 'Hypnotube', icon: 'https://hypnotube.com/apple-touch-icon.png', own: true,
      sorts: [['', 'Newest'], ['views', 'Most viewed'], ['rating', 'Top rated'], ['longest', 'Longest'], ['discussed', 'Most discussed']],
      rows: q => q ? [['Results', { q }], ['Top rated', { q, sort: 'rating' }], ['Most viewed', { q, sort: 'views' }]] :
        [['Most viewed this month', 'most-viewed/month/'], ['Top rated this month', 'top-rated/month/'], ['New releases', 'most-recent/'], ['Hypnosis', 'channels/11/hypnosis/views/'],
         ['Audio', 'channels/37/audio/views/'], ['Longest', 'longest/']],
      async cats() {
        const doc = new DOMParser().parseFromString(await api('webJson', 'https://hypnotube.com/channels/', { Accept: 'text/html' }), 'text/html');
        const seen = new Set();
        return [...doc.querySelectorAll('.item--channel a[href*="/channels/"]')].map(a => {
          const m = a.getAttribute('href').match(/\/channels\/(\d+)\/([^/]+)\//);
          const name = (a.getAttribute('title') || a.textContent || '').trim();
          if (!m || !name || seen.has(m[1]) || !clean(name)) return null;
          seen.add(m[1]);
          return [name, `${m[1]}/${m[2]}`, a.querySelector('img') ? proxy(a.querySelector('img').getAttribute('src')) : ''];
        }).filter(Boolean);
      },
      // a page of videos: the words (searched), or a category, or everything; in an order; page n
      async list({ q, sort = '', cat = '', page = 1 }) {
        const pg = page > 1 ? `page${page}.html` : '';
        if (q) {
          const slug = q.trim().toLowerCase().replace(/[^a-z0-9 ]+/g, ' ').trim().replace(/\s+/g, '-');
          const order = { '': 'newest/', views: 'views/', rating: 'rating/', longest: 'longest/' }[sort] || '';
          return this.load(await api('webFormThenGet', 'https://hypnotube.com/searchgate.php', { q: q.trim(), type: 'videos' }, `https://hypnotube.com/search/videos/${slug}/${order}${pg}`));
        }
        if (cat) return this.load(`channels/${cat}/${{ views: 'views/', rating: 'rating/', longest: 'longest/' }[sort] || ''}${pg}`);
        return this.load(`${{ '': 'most-recent/', views: 'most-viewed/', rating: 'top-rated/', longest: 'longest/', discussed: 'most-discussed/' }[sort]}${pg}`);
      },
      // somewhere in the whole catalogue (or the category): a random page, shuffled
      async random({ cat = '' } = {}) {
        for (let top = cat ? 20 : 500; top >= 1; top = Math.floor(top / 3)) {
          const list = await this.list({ cat, page: 1 + Math.floor(Math.random() * top) });
          if (list.length) return list.sort(() => Math.random() - 0.5);
        }
        return [];
      },
      async load(pathOrHtml) {
        const html = pathOrHtml.includes('<') ? pathOrHtml : await api('webJson', 'https://hypnotube.com/' + pathOrHtml, { Accept: 'text/html' });
        const doc = new DOMParser().parseFromString(html, 'text/html');
        return [...doc.querySelectorAll('.item-col:not(.item--channel) a[href*="/video/"]')].map(a => {
          const img = a.querySelector('img'), thumb = img ? img.getAttribute('src') || '' : '';
          const title = (a.getAttribute('title') || ((a.querySelector('.title') || {}).textContent) || '').trim();
          const stat = cls => ((a.querySelector(cls + ' .sub-desc') || {}).textContent || '').trim();
          const id = (a.getAttribute('href').match(/-(\d+)\.html/) || [])[1];
          return {
            site: 'hypnotube', id, title, thumb: proxy(thumb),
            // (its hover frames: the same picture numbered 1 to 10)
            frames: /-1\.jpg$/.test(thumb) ? Array.from({ length: 8 }, (_, k) => proxy(thumb.replace(/-1\.jpg$/, `-${k + 1}.jpg`))) : [],
            duration: secs(((a.querySelector('.time') || {}).textContent || '').trim()), url: a.getAttribute('href'),
            sub: [stat('.s-e-views') && fmtN(+stat('.s-e-views')) + ' views', stat('.s-e-rate')].filter(Boolean).join(' · '), tags: []
          };
        }).filter(v => v.id && v.url && clean(v.title));
      },
      // the video's file and its tags (the page's keywords)
      async resolve(v) {
        const html = await api('webJson', v.url, { Accept: 'text/html' });
        const m = html.match(/<source src="([^"]+\.mp4[^"]*)"[^>]*?(?:sizes='(\d+)')?/);
        if (!m) throw new Error('Hypnotube has no file for this video (it may be for members only)');
        const tags = ((html.match(/<meta name="keywords" content="([^"]*)"/) || [])[1] || '').split(',').map(t => t.trim().toLowerCase().replace(/\s+/g, '_')).filter(Boolean).slice(0, 24);
        if (!clean(tags.join(' '))) throw new Error('This video is tagged with something never shown here');
        return { file: m[1].replace(/&amp;/g, '&'), quality: m[2] ? m[2] + 'p' : '', tags };
      }
    },
    pornhub: {
      // (Pornhub gives its video files only to its own player: an empty list and refused playlists to
      // anything else, so it plays embedded and can't be saved)
      name: 'Pornhub', icon: 'https://www.pornhub.com/apple-touch-icon.png',
      rows: q => q ? [['Results', `search=${enc(q)}&ordering=mostrelevant`], ['Most viewed', `search=${enc(q)}&ordering=mostviewed`]] :
        // (its API finds nothing for some category + period pairs, e.g. weekly: these all answer)
        [['Popular amateurs', 'category=amateur&ordering=mostviewed'], ['Newest amateurs', 'category=amateur&ordering=newest'], ['Hentai', 'category=hentai&ordering=mostviewed&period=monthly'],
         ['Cosplay', 'category=cosplay&ordering=mostviewed&period=monthly'], ['Lesbian', 'category=lesbian&ordering=mostviewed'], ['Female friendly', 'category=popular-with-women&ordering=mostviewed'],
         ['Verified amateurs', 'category=verified-amateurs&ordering=newest']],
      async load(params) {
        const d = JSON.parse(await api('webJson', `https://www.pornhub.com/webmasters/search?${params}&thumbsize=large_hd&page=1`));
        return (d.videos || []).filter(v => clean(v.title + ' ' + (v.tags || []).map(t => t.tag_name).join(' '))).map(v => ({
          site: 'pornhub', id: v.video_id, title: v.title, thumb: proxy(v.default_thumb || v.thumb), frames: (v.thumbs || []).map(t => proxy(t.src)),
          duration: secs(v.duration), sub: `${fmtN(v.views)} views · ${Math.round(v.rating)}%`, url: v.url, embed: `https://www.pornhub.com/embed/${v.video_id}`,
          tags: (v.tags || []).map(t => t.tag_name).slice(0, 12)
        }));
      }
    }
  };
  // ---------- your own sites (sites.js) and how the sites look (cfg.videos: order, hidden, look) ----------
  const BUILT_IN = Object.keys(SITES);
  let vcfg = null; // Discover's config (sources.json): its "sites" and "videos"
  const host = url => String(url || '').replace(/^https?:\/\/(www\.)?/, '').split('/')[0];
  function recipeSite(r) {
    const S = window.vaultSites, R = { ...r, section: 'videos' };
    const asVideo = x => ({ site: r.id, id: x.id, title: x.title, thumb: proxy(x.thumb), clip: x.clip ? proxy(x.clip) : '', duration: x.duration, url: x.url, sub: host(r.base), tags: [] });
    return {
      name: r.name, icon: `${r.base}/favicon.ico`, own: true, custom: r,
      sorts: (r.sorts || []).length ? r.sorts.map(([label, v]) => [v, label]) : [['', 'Latest']],
      rows: q => q ? [['Results', { q }]] : [[r.sorts && r.sorts[0] ? r.sorts[0][0] : 'Latest', {}], ...(r.cats || []).slice(0, 5).map(([label, v]) => [label, { cat: v }])],
      cats: async () => (r.cats || []).map(([label, v]) => [label, v, '']),
      async list(o) { return (await S.list(R, o)).map(asVideo); },
      async random(o) { return (await S.random(R, o)).map(asVideo); },
      async resolve(v) { const d = await S.details(R, v.url); return { file: d.file, quality: '', tags: d.tags }; }
    };
  }
  async function loadOwn() {
    vcfg = await window.vaultDiscover.config();
    vcfg.videos = vcfg.videos || { order: [], hidden: [], look: {} };
    for (const id of Object.keys(SITES)) if (SITES[id].custom) delete SITES[id];
    for (const r of (vcfg.sites || []).filter(x => x.section === 'videos')) SITES[r.id] = recipeSite(r);
    if (!SITES[site]) site = BUILT_IN[0];
  }
  const lookOf = id => (vcfg && vcfg.videos.look[id]) || {};
  const shownSites = () => {
    const order = vcfg ? vcfg.videos.order : [];
    const ids = Object.keys(SITES).sort((a, b) => (order.indexOf(a) + 1 || 999) - (order.indexOf(b) + 1 || 999));
    return ids.filter(id => !(vcfg && vcfg.videos.hidden.includes(id)));
  };
  async function editOwnSite(r) {
    const made = await window.vaultSites.build('videos', r);
    if (!made) return;
    const list = vcfg.sites;
    const k = list.findIndex(x => x.id === made.id);
    if (k >= 0) list[k] = { ...made, section: 'videos' }; else list.push({ ...made, section: 'videos' });
    await window.vaultDiscover.saveConfig();
    await loadOwn();
    site = made.id; try { localStorage.setItem('imageVault.videoSite', site); } catch (e) {}
    toast(k >= 0 ? `${made.name} changed` : `${made.name} added to Videos`, 'ok');
    draw();
  }
  function customise() {
    const S = window.vaultSites, V = vcfg.videos, cur = SITES[site], look = { ...lookOf(site) };
    S.sheet('Customise Videos', (body, close) => {
      // the sites: shown or not, in what order; yours can be changed or removed
      body.append(make('h3', 'dc-sheet-h', 'Sites'));
      const list = make('div', 'st-sites');
      const order = () => Object.keys(SITES).sort((a, b) => (V.order.indexOf(a) + 1 || 999) - (V.order.indexOf(b) + 1 || 999));
      const drawList = () => {
        const ids = order();
        list.replaceChildren(...ids.map((id, k) => {
          const s2 = SITES[id], row = make('div', 'st-site');
          const tick = make('input'); tick.type = 'checkbox'; tick.checked = !V.hidden.includes(id); tick.title = 'Shown';
          tick.onchange = () => { V.hidden = tick.checked ? V.hidden.filter(x => x !== id) : [...V.hidden, id]; };
          const up = make('button', 'btn sm', '↑'), down = make('button', 'btn sm', '↓');
          const move = d => { const all = order(); const j = all.indexOf(id); if (all[j + d] === undefined) return; [all[j], all[j + d]] = [all[j + d], all[j]]; V.order = all; drawList(); };
          up.onclick = () => move(-1); down.onclick = () => move(1); up.disabled = !k; down.disabled = k === ids.length - 1;
          row.append(tick, make('b', '', s2.name), make('span', 'dc-srckind', s2.custom ? 'Your own · ' + host(s2.custom.base) : 'Built in'), up, down);
          if (s2.custom) {
            const ed = make('button', 'btn sm', 'Edit'); ed.onclick = () => { close(); editOwnSite(s2.custom); };
            const rm = make('button', 'btn sm', 'Remove'); rm.onclick = () => { if (!confirm(`Remove ${s2.name}?`)) return; vcfg.sites = vcfg.sites.filter(x => x.id !== id); delete SITES[id]; drawList(); };
            row.append(ed, rm);
          }
          return row;
        }));
      };
      drawList();
      const addOwn = make('button', 'btn', '✨ Build your own site'); addOwn.onclick = () => { close(); editOwnSite(null); };
      body.append(list, addOwn);
      // this site's front page
      body.append(make('h3', 'dc-sheet-h', `${cur.name}'s front page`));
      const rows = S.area(look.rows || '', 'Cosplay = cosplay\nLong ones = sort:longest\nMy category = category:amateur', 5);
      const siteRows = make('input'); siteRows.type = 'checkbox'; siteRows.checked = look.siteRows !== false;
      const heroT = make('input'); heroT.type = 'checkbox'; heroT.checked = look.hero !== false;
      const tickRow = (box, label) => { const l = make('label', 'st-tick'); l.append(box, make('span', '', label)); return l; };
      body.append(S.field('Your own rows', rows, `One a line: Title = words to search${cur.list ? ', or sort:… (one of its orders), or category:…' : ''}. They come first.`),
        tickRow(siteRows, `Show ${cur.name}'s own rows too`), tickRow(heroT, 'Show the big video at the top'));
      // every site: card size
      const size = make('input'); Object.assign(size, { type: 'range', min: 180, max: 460, step: 10, value: V.card || 260 });
      const sizeOut = make('b', '', size.value + ' px'); size.oninput = () => { sizeOut.textContent = size.value + ' px'; };
      const sizeRow = make('div', 'st-range'); sizeRow.append(size, sizeOut);
      body.append(S.field('Video card width (every site)', sizeRow));
      const btns = make('div', 'dc-actions st-actions');
      const ok = make('button', 'btn primary', 'Done');
      ok.onclick = async () => {
        V.order = order(); V.card = +size.value;
        V.look[site] = { rows: rows.value.trim(), siteRows: siteRows.checked, hero: heroT.checked };
        await window.vaultDiscover.saveConfig(); await loadOwn();
        close(); applyCard(); draw();
      };
      btns.append(ok);
      body.append(btns);
    });
  }
  const applyCard = () => { if (vcfg && vcfg.videos.card) tab.style.setProperty('--vd-card', vcfg.videos.card + 'px'); };
  // your rows for a site: [title, what to load]
  function ownRows(s, id) {
    return window.vaultSites.lines(lookOf(id).rows).map(([title, v]) => {
      const m = /^(sort|category):\s*(.*)$/i.exec(v);
      if (s.list) return [title, m ? (m[1].toLowerCase() === 'sort' ? { sort: m[2] } : { cat: m[2] }) : { q: v }];
      return [title, (s.rows(v)[0] || [])[1]];
    }).filter(([, p]) => p !== undefined);
  }
  function drawSites() {
    const box = q('.vd-sites');
    box.replaceChildren(...shownSites().map(id => [id, SITES[id]]).map(([id, s]) => {
      const b = make('button', 'vd-site' + (id === site ? ' on' : ''));
      const ico = make('img'); ico.src = proxy(s.icon); ico.alt = ''; ico.onerror = () => ico.remove();
      b.append(ico, document.createTextNode(s.name));
      if (s.own) { b.title = 'Plays in the app\'s player, and can be saved into your vault'; b.append(make('span', 'vd-site-own', '♥')); }
      else b.title = 'Plays in its own player (it can\'t be saved)';
      b.onclick = () => { site = id; try { localStorage.setItem('imageVault.videoSite', id); } catch (e) {} input.value = ''; draw(); };
      return b;
    }));
    // like a browser's new-tab button: + adds your own site; the gear customises the row
    const own = make('button', 'vd-site vd-site-add vd-tabicon', '＋'); own.title = 'Add your own site: any other site with videos (give its addresses and the app reads it)';
    own.onclick = () => editOwnSite(null);
    const tune = make('button', 'vd-site vd-site-add vd-tabicon'); tune.innerHTML = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><path d="M12 2v3M12 19v3M4.2 4.2l2.1 2.1M17.7 17.7l2.1 2.1M2 12h3M19 12h3M4.2 19.8l2.1-2.1M17.7 6.3l2.1-2.1"/></svg>'; tune.title = 'Customise: which sites, in what order, your own rows, card size';
    tune.onclick = () => customise();
    box.append(own, tune);
  }
  // ---------- a site's own controls: its orders, Random, its categories ----------
  const lens = {}; // site → { sort, cat, random }
  const lensOf = id => lens[id] || (lens[id] = { sort: '', cat: '', random: false });
  const catCache = {};
  function drawLens() {
    const box = q('.vd-lens'), s = SITES[site];
    box.classList.toggle('hidden', mode !== 'browse' || !s.sorts);
    if (!s.sorts) return box.replaceChildren();
    const L = lensOf(site);
    const pills = make('div', 'vd-sorts');
    for (const [id, label] of s.sorts) {
      const b = make('button', 'vd-sort' + (!L.random && L.sort === id && (id || L.cat || input.value.trim()) ? ' on' : ''), label);
      b.type = 'button';
      b.onclick = () => { L.sort = id; L.random = false; draw(); };
      pills.append(b);
    }
    const dice = make('button', 'btn vd-random' + (L.random ? ' on' : ''), '🎲 Random');
    dice.title = L.cat ? 'Random videos from this category' : 'Random videos from the whole site';
    dice.onclick = () => { L.random = true; input.value = ''; draw(); };
    const reset = make('button', 'linkish vd-lens-reset', 'Back to the front page');
    reset.onclick = () => { Object.assign(L, { sort: '', cat: '', random: false }); input.value = ''; draw(); };
    const top = make('div', 'vd-lens-top');
    top.append(pills, dice);
    if (L.sort || L.cat || L.random || input.value.trim()) top.append(reset);
    const rail = make('div', 'vd-cats');
    box.replaceChildren(top, rail);
    if (s.cats) (catCache[site] || (catCache[site] = s.cats().catch(() => []))).then(list => {
      rail.replaceChildren(make('span', 'dc-rail-h', 'Categories'), ...list.map(([label, id, pic]) => {
        const on = L.cat === id;
        const c = make('button', 'dc-chip' + (on ? ' on' : '') + (pic ? ' pic' : ''));
        c.type = 'button';
        if (pic) { const im = make('img', 'dc-chip-pic'); im.alt = ''; im.loading = 'lazy'; im.src = pic; im.onerror = () => im.remove(); c.append(im); }
        c.append(make('span', '', label));
        c.title = on ? 'Click again to take it off' : `Only ${label}`;
        c.onclick = () => { L.cat = on ? '' : id; L.catName = on ? '' : label; L.random = false; input.value = ''; draw(); };
        return c;
      }));
      const onChip = rail.querySelector('.dc-chip.on');
      if (onChip) onChip.scrollIntoView({ inline: 'center', block: 'nearest' });
    });
  }
  {
    // (a mouse wheel scrolls the categories sideways)
    q('.vd-lens').addEventListener('wheel', e => {
      const rail = e.target.closest('.vd-cats');
      if (!rail || rail.scrollWidth <= rail.clientWidth || Math.abs(e.deltaX) > Math.abs(e.deltaY)) return;
      e.preventDefault(); rail.scrollLeft += e.deltaY;
    }, { passive: false });
  }
  // a chosen order, category, search or Random: one grid, with more on request
  async function drawPicked(s, my) {
    const L = lensOf(site), words = input.value.trim();
    const title = L.random ? `Random from ${L.cat ? L.catName : 'all of ' + s.name}` : [words ? `"${words}"` : L.cat && L.catName, (s.sorts.find(x => x[0] === L.sort) || [])[1]].filter(Boolean).join(' · ');
    body.replaceChildren(make('div', 'vd-loading', `Loading ${s.name}…`));
    let page = 1, all = [];
    const head = make('div', 'vd-row-head vd-picked-head'), grid = make('div', 'vd-grid'), more = make('button', 'btn vd-more', L.random ? '🎲 More random' : 'Load more');
    head.append(make('h3', '', title));
    const fetchPage = () => L.random ? s.random({ cat: L.cat }) : s.list({ q: words, sort: L.sort, cat: L.cat, page });
    const add = list => {
      const fresh = list.filter(v => !all.some(x => x.id === v.id));
      all.push(...fresh);
      grid.append(...fresh.map(v => card({ ...v, open: () => openTheater(v, all) })));
      more.classList.toggle('hidden', !list.length);
    };
    try {
      const first = await fetchPage();
      if (my !== browseTicket) return;
      if (!first.length) return body.replaceChildren(empty('Nothing found here.'));
      body.replaceChildren(head, grid, more);
      add(first);
    } catch (e) { if (my === browseTicket) body.replaceChildren(empty(`${s.name} didn't answer: ${errMsg(e)}`)); return; }
    more.onclick = async () => {
      more.disabled = true; more.textContent = 'Loading…';
      page++;
      try { const list = await fetchPage(); if (my === browseTicket) add(list); }
      catch (e) { toast(errMsg(e), 'err'); page--; }
      more.disabled = false; more.textContent = L.random ? '🎲 More random' : 'Load more';
    };
  }
  let browseTicket = 0;
  async function drawBrowse() {
    const s = SITES[site], my = ++browseTicket, words = input.value.trim();
    input.placeholder = `Search ${s.name}…`;
    drawLens();
    if (s.list) {
      const L = lensOf(site);
      if (L.sort || L.cat || L.random || words) return drawPicked(s, my);
    }
    body.replaceChildren(make('div', 'vd-loading', `Loading ${s.name}…`));
    const look = lookOf(site);
    const defs = words ? s.rows(words) : [...ownRows(s, site), ...(look.siteRows === false ? [] : s.rows(words))];
    if (!defs.length) return body.replaceChildren(empty('No rows: add some of your own in ⚙ Customise, or show the site\'s own.'));
    const results = await Promise.all(defs.map(([, p]) => (typeof p === 'object' ? s.list(p) : s.load(p)).catch(e => ({ error: errMsg(e) }))));
    if (my !== browseTicket) return;
    const errors = results.filter(r => r.error);
    const rows = defs.map(([title], k) => Array.isArray(results[k]) ? row(title, results[k].map(v => card({ ...v, open: () => openTheater(v, results[k]) }))) : null).filter(Boolean);
    if (!rows.length) return body.replaceChildren(empty(errors.length ? `${s.name} didn't answer: ${errors[0].error}` : 'Nothing found.'));
    const firstList = results.find(r => Array.isArray(r) && r.length), first = firstList[0];
    body.replaceChildren(...(look.hero === false ? [] : [hero({ ...first, sub: `${s.name} · ${first.duration ? fmt(first.duration) : ''} ${first.sub ? '· ' + first.sub : ''}` },
      [['▶ Play', () => openTheater(first, firstList), true], ['＋ Watch later', () => keep(first)]])]), ...rows);
  }

  // ---------- the theater: a site's video, big ----------
  const theater = q('.vd-theater'), stage = q('.vd-stage'), side = q('.vd-side');
  let showing = null, showList = [];
  async function openTheater(v, list) {
    showing = v; showList = list || [v];
    theater.classList.remove('hidden');
    stage.replaceChildren();
    const site = SITES[v.site] || {};
    if (site.resolve || v.file) {
      // the app's own player, the file streamed through the app (it can jump anywhere)
      const vid = make('video');
      Object.assign(vid, { autoplay: true, playsInline: true, crossOrigin: 'anonymous' });
      stage.append(vid);
      window.vaultPlayer.attach(vid, stage);
      const wait1 = make('div', 'vd-loading vd-stage-wait', 'Loading the video…');
      stage.append(wait1);
      vid.addEventListener('loadeddata', () => wait1.remove(), { once: true });
      try {
        if (!v.file) Object.assign(v, await site.resolve(v));
        if (showing !== v) return;
        vid.src = stream(v.file);
        drawSide(v);
      } catch (e) { if (showing === v) wait1.textContent = errMsg(e); }
    } else if (v.embed) {
      const f = make('iframe');
      f.src = v.embed;
      // their player, without popups, new windows or leaving the app
      f.setAttribute('sandbox', 'allow-scripts allow-same-origin allow-presentation');
      f.setAttribute('allow', 'autoplay; fullscreen; encrypted-media; picture-in-picture');
      f.setAttribute('allowfullscreen', '');
      f.referrerPolicy = 'no-referrer';
      stage.append(f);
    }
    drawSide(v);
  }
  function drawSide(v) {
    if (showing !== v) return;
    const site = SITES[v.site] || {};
    side.replaceChildren();
    const head = make('div', 'dc-side-head');
    const close = make('button', 'btn', 'Close'); close.onclick = closeTheater;
    head.append(make('div', 'dc-side-title', v.title), close);
    side.append(head, make('p', 'vd-side-sub', `${site.name || v.site}${v.duration ? ' · ' + fmt(v.duration) : ''}${v.quality ? ' · ' + v.quality : ''} · ${v.sub || ''}`));
    const btns = make('div', 'dc-actions');
    if (site.resolve || v.file) {
      const s1 = make('button', 'btn primary dc-save-big');
      const job = saving.get(v.url);
      if (job) { s1.classList.add('added', job.done ? 'done' : 'busy'); s1.innerHTML = `${window.vaultDiscover.spinTick()}<span>Added to vault</span><span class="vd-dlprog">${job.done ? '' : job.text || ''}</span>`; s1.disabled = true; job.btn = s1; }
      else if ((library || []).some(i => i.source === v.url)) { s1.classList.add('added', 'done'); s1.innerHTML = `${window.vaultDiscover.spinTick()}<span>In your vault</span>`; s1.disabled = true; }
      else { s1.textContent = '♥ Add to vault'; s1.title = 'Asks for its category, NSFW level and stars, then downloads it into your vault'; s1.onclick = () => saveVideo(v); }
      btns.append(s1);
    }
    const k = make('button', 'btn', later.some(x => x.site === v.site && x.id === v.id) ? '✓ In Watch later' : '＋ Watch later'); k.onclick = () => { keep(v); k.textContent = '✓ In Watch later'; };
    const o = make('button', 'btn', 'Open on site ↗'); o.onclick = () => api('openLink', v.url).catch(() => {});
    btns.append(k, o);
    side.append(btns);
    if (v.tags && v.tags.length) {
      side.append(make('h4', 'dc-h', 'Tags'));
      const wrap = make('div', 'dc-tags');
      for (const t of v.tags) { const b = make('button', 'vd-tag', t.replace(/_/g, ' ')); b.onclick = () => { closeTheater(); mode = 'browse'; input.value = t.replace(/_/g, ' '); syncModes(); draw(); }; wrap.append(b); }
      side.append(wrap);
    }
    // the next ones
    const k2 = showList.indexOf(v);
    const next = showList.slice(k2 + 1, k2 + 7);
    if (next.length) {
      side.append(make('h4', 'dc-h', 'Up next'));
      const up = make('div', 'vd-upnext');
      next.forEach(n => up.append(card({ ...n, open: () => openTheater(n, showList) })));
      side.append(up);
    }
  }
  function closeTheater() { showing = null; theater.classList.add('hidden'); stage.querySelectorAll('video').forEach(x => { x.pause(); x.removeAttribute('src'); x.load(); }); stage.replaceChildren(); }
  document.addEventListener('keydown', e => { if (showing && e.key === 'Escape' && !tab.classList.contains('hidden')) { e.stopPropagation(); closeTheater(); } }, true);
  function keep(v) {
    if (later.some(x => x.site === v.site && x.id === v.id)) return toast('Already in Watch later', 'ok');
    later.unshift({ site: v.site, id: v.id, title: v.title, thumb: v.thumb, frames: (v.frames || []).slice(0, 8), clip: v.clip || '', file: v.file || '', embed: v.embed || '', duration: v.duration || 0, url: v.url, tags: v.tags || [], at: Date.now() });
    saveLater();
    toast('Kept in Watch later', 'ok');
  }
  const saving = new Map(); // url → { text, done, btn }
  const mb = n => n >= 1e9 ? (n / 1e9).toFixed(2) + ' GB' : Math.round(n / 1e6) + ' MB';
  async function saveVideo(v) {
    const D = window.vaultDiscover, site = SITES[v.site] || {};
    if (saving.has(v.url)) return;
    let answer;
    try {
      if (!v.file && site.resolve) Object.assign(v, await site.resolve(v)); // (its tags, for the question)
      answer = await D.ask({ title: v.title, thumb: v.thumb, source: v.url, tags: window.vaultUI.cleanTags((v.tags || []).map(t => t.toLowerCase().replace(/\s+/g, '_'))),
        rating: 'explicit', cat: site.hentai ? 'hentai' : 'real', siteName: site.name });
    } catch (e) { return toast(errMsg(e), 'err'); }
    if (!answer) return;
    const job = { text: 'starting…', done: false, btn: null };
    saving.set(v.url, job);
    drawSide(v);
    const dl = window.vaultDownloads ? window.vaultDownloads.add({ title: v.title, thumb: v.thumb, from: 'Videos · ' + (site.name || v.site), retry: () => saveVideo(v) }) : null;
    const say = text => { job.text = text; const p = job.btn && job.btn.querySelector('.vd-dlprog'); if (p) p.textContent = text; };
    try {
      const blob = await D.downloadBlob(v.file, p => {
        say(p.total ? `${Math.floor(p.done / p.total * 100)}% of ${mb(p.total)}` : mb(p.done));
        if (dl) dl.progress(p.total ? p.done / p.total : null, p.total ? `${mb(p.done)} of ${mb(p.total)}` : mb(p.done));
      });
      if (dl) dl.progress(null, 'Adding to the vault');
      say('adding…');
      const file = new File([blob], `${v.site}-${v.id}.mp4`, { type: 'video/mp4' });
      const category = answer.category.replace(/\s*[\\/]+\s*/g, '-').trim();
      jobs.push({ file, category, nsfw: answer.nsfw, quality: answer.quality, collection: answer.collection, tags: answer.tags.join(' '), source: v.url });
      pumpUploads(); addCategory(category);
      const until = Date.now() + 30 * 60e3;
      while (!(library || []).some(i => i.source === v.url)) {
        if (Date.now() > until || failed.some(j => j.source === v.url)) throw new Error('It downloaded but couldn\'t be added to the vault');
        await wait(800);
      }
      job.done = true; say(''); if (dl) dl.done();
      if (showing === v) { drawSide(v); setTimeout(() => window.vaultUI.burst(side.querySelector('.dc-save-big'), 18), 30); }
      else D.imported({ thumb: v.thumb, text: 'Imported!', sub: `Into ${category} · in Videos` });
    } catch (e) {
      saving.delete(v.url);
      if (dl) dl.fail(errMsg(e));
      if (showing === v) drawSide(v);
      toast(`${v.title.slice(0, 40)} not saved: ${errMsg(e)}`, 'err');
    }
  }

  // ---------- Watch later ----------
  function drawLater() {
    if (!later.length) return body.replaceChildren(empty('Nothing kept yet. "＋ Watch later" on any site video keeps it here.'));
    const cards = later.map(v => {
      const c = card({ ...v, sub: `${(SITES[v.site] || {}).name || v.site} · kept ${new Date(v.at).toLocaleDateString()}`, open: () => openTheater(v, later) });
      const x = make('span', 'vd-x', '×'); x.title = 'Remove';
      x.onclick = e => { e.stopPropagation(); later = later.filter(y => y !== v); saveLater(); drawLater(); };
      c.querySelector('.vd-pic').append(x);
      return c;
    });
    body.replaceChildren(make('div', 'vd-grid'));
    body.firstChild.append(...cards);
  }

  function syncModes() { tab.querySelectorAll('.vd-modes button').forEach(x => x.classList.toggle('on', x.dataset.mode === mode)); }
  function draw() {
    if (!vcfg) return loadOwn().then(() => { applyCard(); draw(); }, () => { vcfg = { sites: [], videos: { order: [], hidden: [], look: {} } }; draw(); });
    syncModes();
    q('.vd-sites').classList.toggle('hidden', mode !== 'browse');
    q('.vd-search').classList.toggle('hidden', mode !== 'browse');
    if (mode !== 'browse') q('.vd-lens').classList.add('hidden');
    if (mode === 'browse') { drawSites(); drawBrowse(); }
    else if (mode === 'later') drawLater();
    else drawYours();
  }
  // the tile's line: how many long videos, or what's left to finish
  setupReady.then(async () => {
    await kvReady;
    while (!library) await wait(300);
    const n = library.filter(i => i.kind === 'video' && i.duration >= LONG).length, going = Object.keys(resume).length;
    navBtn.querySelector('.nt-sub').textContent = going ? `${going} to finish` : n ? `${fmtN(n)} long ${n === 1 ? 'video' : 'videos'}` : 'Long-form & sites';
  });
  // a page address pasted on the Add page: its file (Eporner, Rule34Video)
  async function resolveUrl(url) {
    const site = /eporner\.com/.test(url) ? 'eporner' : /rule34video\.com/.test(url) ? 'rule34video' : /hypnotube\.com\/video\//.test(url) ? 'hypnotube' : '';
    if (!site) {
      if (!vcfg) await loadOwn();
      const mine = Object.values(SITES).find(x => x.custom && host(url) === host(x.custom.base));
      if (mine) { const r = await mine.resolve({ url }); return { file: r.file, name: `${host(url).split('.')[0]}-${Date.now()}.mp4` }; }
      throw new Error('Not a video page of a site the app reads');
    }
    const r = await SITES[site].resolve({ url, tags: [] });
    return { file: r.file, name: `${site}-${(url.match(/(\d{5,}|video-([\w-]+))/) || [])[0] || Date.now()}.mp4`.replace(/[^\w.-]+/g, '-') };
  }
  window.vaultVideos = { openTheater, draw, resolveUrl };
})();
