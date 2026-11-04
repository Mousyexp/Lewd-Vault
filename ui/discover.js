// Lewd Vault: Discover. Browse image sites (boorus like Danbooru, Gelbooru, e621, Derpibooru,
// Paheal; Reddit communities; pictures from the whole web through Bing) inside the app and save what
// you like straight into the vault, with the site's tags, rating and link. About a hundred sources
// are in the catalogue, in categories; their tabs carry the sites' icons and can be dragged into
// any order, and searches can be kept as bookmarks. Right-click a Library picture → Find the source
// asks SauceNAO or IQDB where it came from.
//
// It isn't a web browser: each site's API is read directly (through the app, see web.rs), so no site
// page ever loads: no ads, no popups, no redirects, no trackers. Search with the site's own tag
// suggestions, go back and forward through your searches, look at a post's details and save it in
// one click (or with your own category, scores and tags).
//
// Posts tagged as sexual content involving minors are never shown, whatever the source.

(() => {
  'use strict';
  if (!window.vaultUI || !inTauri) return;
  const { make, tagChip, tagBox, pretty, typeOf, nameOf, cleanTags, showTitle, peek, TAB_KEY, foldChips } = vaultUI;
  const enc = encodeURIComponent;
  const proxy = url => url ? 'http://vault.localhost/web?u=' + enc(url) : '';
  const PAGE = 60;
  // never shown, never searched for: sexual content involving minors
  // (each site's own word for it: e621's young and cub, Derpibooru's foal and filly, Danbooru's child...)
  const NEVER = new Set(['loli', 'lolicon', 'shota', 'shotacon', 'toddlercon', 'toddler', 'child_on_child', 'child', 'children', 'young', 'cub', 'foal', 'filly',
    'foalcon', 'aged_down', 'underage', 'preteen']);

  // ---------- the kinds of sites ----------
  const RATINGS = ['general', 'sensitive', 'questionable', 'explicit'];
  const RATING_LABEL = { general: 'General', sensitive: 'Sensitive', questionable: 'Questionable', explicit: 'Explicit' };
  const RATING_NSFW = { general: 1, sensitive: 4, questionable: 7, explicit: 9 };
  const MIME = { jpg: 'image/jpeg', jpeg: 'image/jpeg', png: 'image/png', gif: 'image/gif', webp: 'image/webp', webm: 'video/webm', mp4: 'video/mp4', avif: 'image/avif' };
  const split = s => String(s || '').split(' ').filter(Boolean);
  const extOf = url => (String(url || '').split('?')[0].match(/\.([a-z0-9]{2,5})$/i) || [, ''])[1].toLowerCase();
  const KINDS = {
    danbooru: {
      label: 'Danbooru-style',
      url: (s, q, page) => `${s.base}/posts.json?limit=${PAGE}&page=${page}&tags=${enc(q)}` + (s.user && s.key ? `&login=${enc(s.user)}&api_key=${enc(s.key)}` : ''),
      list: d => Array.isArray(d) ? d : [],
      map(p, s) {
        if (!p.id || !(p.file_url || p.large_file_url)) return null; // hidden from this account, or deleted
        const vars = (p.media_asset && p.media_asset.variants) || [];
        const v = t => (vars.find(x => x.type === t) || {}).url;
        return {
          id: p.id, preview: v('720x720') || v('360x360') || p.preview_file_url, sample: p.large_file_url || p.file_url, file: p.file_url || p.large_file_url,
          width: p.image_width, height: p.image_height, ext: p.file_ext, score: p.score, created: Date.parse(p.created_at),
          rating: { g: 'general', s: 'sensitive', q: 'questionable', e: 'explicit' }[p.rating] || 'general', source: p.source,
          page: `${s.base}/posts/${p.id}`,
          tags: [...split(p.tag_string_artist).map(t => 'artist:' + t), ...split(p.tag_string_character).map(t => 'character:' + t),
                 ...split(p.tag_string_copyright).map(t => 'series:' + t), ...split(p.tag_string_general), ...split(p.tag_string_meta).map(t => 'meta:' + t)]
        };
      },
      suggest: (s, term) => [`${s.base}/autocomplete.json?search[query]=${enc(term)}&search[type]=tag_query&limit=10`],
      suggestions: d => (Array.isArray(d) ? d : []).map(t => ({ raw: t.value, type: ({ 1: 'artist', 3: 'series', 4: 'character', 5: 'meta' })[t.category] || 'general', count: t.post_count }))
    },
    gelbooru: {
      label: 'Gelbooru-style',
      url: (s, q, page) => `${s.base}/index.php?page=dapi&s=post&q=index&json=1&limit=${PAGE}&pid=${page - 1}&tags=${enc(q)}` + (s.user && s.key ? `&user_id=${enc(s.user)}&api_key=${enc(s.key)}` : ''),
      list: d => Array.isArray(d) ? d : (d && d.post) || [],
      map(p, s) {
        // older Gelbooru (TBIB): no addresses, only the folder and file names
        if (!p.file_url && p.image && p.directory) {
          const site = s.pageBase || s.base;
          p.file_url = `${site}/images/${p.directory}/${p.image}`;
          p.preview_url = p.preview_url || `${site}/thumbnails/${p.directory}/thumbnail_${p.image.replace(/\.[^.]+$/, '')}.jpg`;
          p.sample_url = p.sample_url || (p.sample === true || +p.sample ? `${site}/samples/${p.directory}/sample_${p.image.replace(/\.[^.]+$/, '')}.jpg` : p.file_url);
        }
        if (!p.id || !p.file_url) return null;
        const r = String(p.rating || '').toLowerCase();
        const pic = u => /\.(jpe?g|png|webp|gif)$/i.test(String(u || '').split('?')[0]);
        return {
          id: p.id, preview: pic(p.sample_url) ? p.sample_url : pic(p.file_url) && !/\.gif$/i.test(p.file_url) && +p.width <= 1600 ? p.file_url : p.preview_url,
          sample: p.sample_url || p.file_url, file: p.file_url, width: +p.width, height: +p.height,
          ext: extOf(p.file_url), score: +p.score || 0, created: Date.parse(p.created_at) || (+p.change * 1000) || 0,
          rating: r.startsWith('e') ? 'explicit' : r.startsWith('q') ? 'questionable' : r.startsWith('sensitive') ? 'sensitive' : 'general',
          source: p.source, page: `${s.pageBase || s.base}/index.php?page=post&s=view&id=${p.id}`, tags: split(p.tags)
        };
      },
      suggest: (s, term) => [`${s.base.replace('//api.', '//')}/index.php?page=autocomplete2&term=${enc(term)}&type=tag_query&limit=10`, `${s.base}/autocomplete.php?q=${enc(term)}`],
      suggestions: d => (Array.isArray(d) ? d : []).map(t => ({
        raw: t.value, count: +t.post_count || +(String(t.label || '').match(/\((\d+)\)/) || [])[1] || 0,
        type: ({ artist: 'artist', character: 'character', copyright: 'series', metadata: 'meta' })[t.category] || 'general'
      }))
    },
    moebooru: {
      label: 'Moebooru-style',
      url: (s, q, page) => `${s.base}/post.json?limit=${PAGE}&page=${page}&tags=${enc(q)}`,
      list: d => Array.isArray(d) ? d : [],
      map(p, s) {
        if (!p.id || !p.file_url) return null;
        return {
          id: p.id, preview: p.sample_url || p.actual_preview_url || p.preview_url, sample: p.sample_url || p.jpeg_url || p.file_url, file: p.file_url,
          width: p.width, height: p.height, ext: p.file_ext || extOf(p.file_url), score: p.score, created: p.created_at * 1000,
          rating: { s: 'general', q: 'questionable', e: 'explicit' }[p.rating] || 'general', source: p.source,
          page: `${s.base}/post/show/${p.id}`, tags: split(p.tags)
        };
      },
      suggest: (s, term) => [`${s.base}/tag.json?name=${enc(term)}*&order=count&limit=10`],
      suggestions: d => (Array.isArray(d) ? d : []).map(t => ({ raw: t.name, count: t.count, type: ({ 1: 'artist', 3: 'series', 4: 'character', 5: 'artist', 6: 'meta' })[t.type] || 'general' }))
    },
    e621: {
      label: 'e621-style',
      url: (s, q, page) => `${s.base}/posts.json?limit=${PAGE}&page=${page}&tags=${enc(q)}` + (s.user && s.key ? `&login=${enc(s.user)}&api_key=${enc(s.key)}` : ''),
      list: d => (d && d.posts) || [],
      map(p, s) {
        if (!p.id || !p.file || !p.file.url) return null; // hidden without an account, or deleted
        const t = p.tags || {};
        const sample = p.sample && p.sample.has && p.sample.url ? p.sample.url : p.file.url;
        const picture = /\.(jpe?g|png|webp)$/i.test(sample);
        return {
          id: p.id, preview: picture ? sample : (p.preview && p.preview.url) || sample, sample, file: p.file.url, width: p.file.width, height: p.file.height,
          ext: p.file.ext, score: (p.score && p.score.total) || 0, created: Date.parse(p.created_at),
          rating: { s: 'general', q: 'questionable', e: 'explicit' }[p.rating] || 'general', source: (p.sources || [])[0] || '',
          page: `${s.base}/posts/${p.id}`,
          tags: [...(t.artist || []).filter(a => !/^(conditional_dnp|sound_warning|unknown_artist)$/.test(a)).map(a => 'artist:' + a), ...(t.character || []).map(c => 'character:' + c),
                 ...(t.copyright || []).map(c => 'series:' + c), ...(t.species || []), ...(t.general || []), ...(t.meta || []).map(m => 'meta:' + m)]
        };
      },
      suggest: (s, term) => [`${s.base}/tags/autocomplete.json?search[name_matches]=${enc(term)}&expiry=7`],
      suggestions: d => (Array.isArray(d) ? d : []).map(t => ({ raw: t.name, count: t.post_count, type: ({ 1: 'artist', 3: 'series', 4: 'character', 7: 'meta' })[t.category] || 'general' }))
    },
    philomena: {
      label: 'Philomena (Derpibooru-style)',
      page: 50,
      url(s, q, page) {
        // Philomena's search: tags with spaces, separated by commas; ratings are tags too
        const tags = split(q).filter(t => !t.startsWith('__')).map(t => t.replace(/_/g, ' ').replace(/^-/, '-'));
        const rate = split(q).filter(t => t.startsWith('__rating:')).map(t => t.slice(9));
        const order = (split(q).find(t => t.startsWith('__sort:')) || '__sort:created_at').slice(7);
        const since = (split(q).find(t => t.startsWith('__since:')) || '').slice(8);
        const parts = [...tags, ...(rate.length ? [`(${rate.join(' || ')})`] : []), ...(since ? [`created_at.gte:${since} days ago`] : [])];
        return `${s.base}/api/v1/json/search/images?q=${enc(parts.join(', ') || '*')}&per_page=50&page=${page}&sf=${order}&sd=desc` +
          (s.filter ? `&filter_id=${s.filter}` : '') + (s.key ? `&key=${enc(s.key)}` : '');
      },
      list: d => (d && d.images) || [],
      map(p, s) {
        if (!p.id || !p.representations) return null;
        const r = p.representations;
        const RT = ['safe', 'suggestive', 'questionable', 'explicit', 'semi-grimdark', 'grimdark', 'grotesque'];
        const tags = (p.tags || []).filter(t => !RT.includes(t)).map(t => {
          const m = /^(artist|oc|character|series|editor|photographer|commissioner|prompter|generator|comic|spoiler):(.+)$/.exec(t);
          const name = (m ? m[2] : t).trim().replace(/\s+/g, '_');
          return m ? ({ artist: 'artist:', oc: 'character:', character: 'character:', photographer: 'artist:', comic: 'series:', series: 'series:' }[m[1]] || 'meta:') + name : name;
        });
        const rated = (p.tags || []).find(t => RT.includes(t)) || 'safe';
        return {
          id: p.id, preview: r.medium || r.small || r.thumb, sample: r.large || r.full, file: r.full, width: p.width, height: p.height, ext: p.format,
          score: p.score, created: Date.parse(p.created_at), source: (p.source_urls || [])[0] || p.source_url || '',
          rating: { safe: 'general', suggestive: 'sensitive', questionable: 'questionable', explicit: 'explicit' }[rated] || 'questionable',
          page: `${s.base}/images/${p.id}`, tags
        };
      },
      suggest: (s, term) => [`${s.base}/api/v1/json/search/tags?q=${enc('name:' + term.replace(/_/g, ' ') + '*')}&per_page=10`],
      suggestions: d => ((d && d.tags) || []).map(t => ({ raw: t.name.replace(/\s+/g, '_'), count: t.images, type: ({ origin: 'artist', character: 'character', oc: 'character', 'content-official': 'series' })[t.category] || 'general' }))
    },
    shimmie: {
      label: 'Shimmie (Paheal-style)',
      url: (s, q, page) => `${s.base}/api/danbooru/find_posts?limit=${PAGE}&page=${page}&tags=${enc(q)}`,
      parse: text => [...new DOMParser().parseFromString(text, 'text/xml').querySelectorAll('post')].map(e => Object.fromEntries([...e.attributes].map(a => [a.name, a.value]))),
      list: d => Array.isArray(d) ? d : [],
      map(p, s) {
        if (!p.id || !p.file_url) return null;
        return {
          id: p.id, preview: p.preview_url, sample: p.file_url, file: p.file_url, width: +p.width, height: +p.height, ext: extOf(p.file_url),
          score: +p.score || 0, created: Date.parse(p.date) || 0, rating: 'explicit', source: p.source || '',
          page: `${s.base}/post/view/${p.id}`, tags: split(p.tags).map(t => t.toLowerCase())
        };
      },
    },
    reddit: {
      label: 'Reddit',
      async fetch(s, st, page, after) {
        // without an approved app id of your own: Reddit's public feeds (RSS)
        if (!cfg.reddit.clientId || cfg.reddit.rssOnly) return redditRss(s, st, after);
        let token;
        try { token = await redditToken(); }
        catch (e) { if (/refused|401|403|unauthorized|invalid_client/i.test(errMsg(e))) return redditRss(s, st, after); throw e; }
        const text = st.query.trim(), sub = s.sub || '';
        const sort = st.view || 'hot', t = sort === 'top' ? st.span || 'week' : 'all';
        let path;
        if (text) path = `${sub ? `/r/${sub}` : ''}/search?q=${enc(text)}${sub ? '&restrict_sr=on' : ''}&sort=${{ hot: 'relevance', rising: 'hot' }[sort] || sort}&include_over_18=on&t=${t}`;
        else if (!sub) path = cfg.reddit.refresh ? `/best?` : `/r/popular/${sort}?t=${t}`;
        else path = `/r/${sub}/${sort}?t=${t}`;
        const url = `https://oauth.reddit.com${path}${path.endsWith('?') ? '' : '&'}limit=50&raw_json=1${after ? '&after=' + after : ''}`;
        const d = JSON.parse(await api('webJson', url, { Authorization: 'bearer ' + token }));
        const kids = ((d.data && d.data.children) || []).map(c => c.data);
        return { items: kids, next: d.data && d.data.after };
      },
      map(p, s) {
        if (p && p._mapped) return p; // (from the public feeds: already a post)
        if (!p || !p.id || p.is_self || p.removed_by_category) return null;
        const title = p.title || '';
        if (NEVER_TEXT.test(title) || NEVER_TEXT.test(p.subreddit || '')) return null;
        const pick = (list, max) => { const ok = (list || []).filter(r => r.width <= max); return (ok[ok.length - 1] || (list || [])[0] || {}).url; };
        const img = p.preview && p.preview.images && p.preview.images[0];
        let preview = img ? pick(img.resolutions, 960) || img.source.url : '', sample = img ? pick(img.resolutions, 1280) || img.source.url : '';
        let file = '', ext = '', width = img ? img.source.width : 0, height = img ? img.source.height : 0, gallery = null, resolve = null;
        const url = p.url_overridden_by_dest || p.url || '';
        const video = p.secure_media && p.secure_media.reddit_video || (p.media && p.media.reddit_video);
        if (p.is_gallery && p.media_metadata && p.gallery_data) {
          gallery = p.gallery_data.items.map(it => p.media_metadata[it.media_id]).filter(m => m && m.s && (m.s.u || m.s.mp4 || m.s.gif));
          if (!gallery.length) return null;
          const g = gallery[0];
          file = g.s.u || g.s.mp4 || g.s.gif; width = g.s.x; height = g.s.y;
          preview = preview || ((g.p || []).filter(x => x.x <= 640).pop() || {}).u || file; sample = file;
          gallery = gallery.map(m => m.s.u || m.s.mp4 || m.s.gif);
        } else if (video && video.fallback_url) {
          file = video.fallback_url.split('?')[0]; ext = 'mp4'; width = video.width; height = video.height;
        } else if (/\.(jpe?g|png|webp|gif)(\?|$)/i.test(url)) {
          file = url; sample = sample || url;
        } else if (/imgur\.com\/.+\.gifv$/i.test(url)) {
          file = url.replace(/\.gifv$/i, '.mp4'); ext = 'mp4';
        } else if (/redgifs\.com\/(watch|ifr)\//i.test(url)) {
          const id = url.split('/').pop().split(/[?#-]/)[0].toLowerCase();
          ext = 'mp4';
          resolve = async function () { if (this.file) return; const g = await redgifsGif(id); this.file = g.urls.hd || g.urls.sd; this.width = g.width; this.height = g.height; };
        } else return null;
        if (!preview && !resolve) return null;
        const sub = (p.subreddit || 'reddit').toLowerCase();
        return {
          id: p.id, title, preview: preview || p.thumbnail, sample: sample || file, file, width, height, ext: ext || extOf(file) || 'jpg',
          score: p.score, created: p.created_utc * 1000, rating: p.over_18 ? 'explicit' : 'general', source: url,
          page: 'https://www.reddit.com' + p.permalink, gallery, resolve, sub: p.subreddit,
          tags: ['meta:reddit', 'meta:r_' + sub, ...(p.author && p.author !== '[deleted]' ? ['artist:' + p.author.toLowerCase()] : [])]
        };
      },
    },
    redgifs: {
      label: 'RedGifs',
      async fetch(s, st, page) {
        const q = st.query.trim(), niche = (st.picks || {}).niche, span = st.span || 'week';
        let path;
        if (niche && !q) path = `/niches/${enc(niche)}/gifs?order=${st.view === 'latest' ? 'latest' : { week: 'hot', month: 'best', all: 'top' }[span] || 'hot'}&count=40&page=${page}`;
        else path = `/gifs/search?search_text=${enc(q)}&order=${st.view === 'latest' ? 'latest' : { week: 'top7', month: 'top28', all: 'top' }[span] || 'top7'}&count=40&page=${page}`;
        const d = await rgApi(path);
        return { items: d.gifs || [], next: d.pages && page < d.pages ? page + 1 : null, note: niche && q ? 'A niche and words don\'t mix on RedGifs: these are the words\' results.' : '' };
      },
      map(g) {
        if (!g || !g.id || !g.urls || !(g.urls.hd || g.urls.sd)) return null;
        const tags = (g.tags || []).map(t => t.toLowerCase().trim().replace(/\s+/g, '_'));
        return {
          id: g.id, title: g.description || (g.tags || []).slice(0, 5).join(', '), preview: g.urls.poster || g.urls.thumbnail, sample: g.urls.poster || g.urls.thumbnail,
          file: g.urls.hd || g.urls.sd, clip: g.urls.sd || g.urls.hd, direct: true, width: g.width, height: g.height, ext: 'mp4', duration: g.duration,
          score: g.likes || 0, created: (g.createDate || 0) * 1000, rating: 'explicit', source: '', page: `https://www.redgifs.com/watch/${g.id}`, user: g.userName,
          tags: [...tags, ...(g.userName ? ['artist:' + g.userName.toLowerCase()] : [])]
        };
      }
    },
    erome: {
      // Erome: amateur albums (pictures and videos) people post; read from its pages. An album is a post:
      // its cover in the grid; opened, its first video or picture; saved, all of it as a collection.
      label: 'Erome',
      async fetch(s, st, page) {
        const f = st.picks || {};
        const q = [st.query.trim(), f.topic, f.body, f.look].filter(Boolean).join(' ');
        // (searches sort by Hot or New too: o=new)
        const url = q ? `https://www.erome.com/search?q=${enc(q)}${st.view === 'new' ? '&o=new' : ''}&page=${page}` : `https://www.erome.com/explore${st.view === 'new' ? '/new' : ''}?page=${page}`;
        const doc = new DOMParser().parseFromString(await api('webJson', url, { Accept: 'text/html' }), 'text/html');
        const items = [...doc.querySelectorAll('.album')].map(el => {
          const a = el.querySelector('a.album-link'), img = el.querySelector('img.album-thumbnail, img');
          if (!a || !img) return null;
          const n = sel => +(((el.querySelector(sel) || {}).textContent || '').replace(/\D+/g, '')) || 0;
          return { id: a.getAttribute('href').split('/a/')[1], page: a.getAttribute('href'), thumb: img.getAttribute('src') || img.getAttribute('data-src'),
            title: (((el.querySelector('.album-title') || {}).textContent || img.getAttribute('alt') || '').replace(/\s*#\w+\s*$/, '')).trim(),
            user: ((el.querySelector('.album-user') || {}).textContent || '').trim(), videos: n('.album-videos'), images: n('.album-images') };
        }).filter(Boolean);
        const kept = items.filter(a => f.type === 'videos' ? a.videos > 0 : f.type === 'pictures' ? a.images > 0 && !a.videos : f.type === 'big' ? a.videos + a.images >= 10 : true);
        return { items: kept, next: items.length ? page + 1 : null };
      },
      map(a) {
        if (!a.id || !a.thumb) return null;
        return {
          id: a.id, title: a.title, preview: a.thumb, sample: a.thumb, file: '', width: 0, height: 0, ext: a.videos && !a.images ? 'mp4' : 'jpg', score: 0, created: 0,
          rating: 'explicit', source: '', page: a.page, user: a.user, count: a.videos + a.images,
          tags: ['meta:erome', ...(a.user ? ['artist:' + a.user.toLowerCase().replace(/\s+/g, '_')] : [])],
          // the album's files (its pictures and videos), when it's opened or saved
          resolve: async function () {
            if (this.file) return;
            const html = await api('webJson', this.page, { Accept: 'text/html' });
            // each video with its still (its poster), each picture
            const posters = new Map([...html.matchAll(/<video[^>]*?poster="([^"]+)"[\s\S]*?<source src="([^"]+\.mp4[^"]*)"/g)].map(m => [m[2], m[1]]));
            const vids = [...new Set([...html.matchAll(/<source src="([^"]+\.mp4[^"]*)"/g)].map(m => m[1]))];
            const pics = [...new Set([...html.matchAll(/https:\/\/s\d+\.erome\.com\/[^"' ]+\.(?:jpe?g|png|gif|webp)/g)].map(m => m[0]).filter(u => !/\/thumbs\/|avatar/.test(u)))];
            if (!pics.length && !vids.length) throw new Error('That album is empty or gone');
            // the album's own tags (its page links them to Erome's search)
            const tags = [...new Set([...html.matchAll(/href="https:\/\/www\.erome\.com\/search\?q=([^"&]+)"/g)]
              .map(m => { try { return decodeURIComponent(m[1].replace(/\+/g, ' ')); } catch (e) { return ''; } })
              .map(t => t.trim().toLowerCase().replace(/\s+/g, '_').replace(/[^a-z0-9_+]/g, '')).filter(t => t && t.length <= 40))].slice(0, 40);
            this.tags = [...new Set([...this.tags, ...tags])];
            // what the tile shows comes first (it's what opens, and the collection's cover)
            const first = this.ext === 'mp4' ? vids[0] || pics[0] : pics[0] || vids[0];
            const all = [first, ...pics, ...vids].filter((u, k, a) => a.indexOf(u) === k);
            this.gallery = all.length > 1 ? all : null;
            this.stills = all.map(u => posters.get(u) || (vids.includes(u) ? this.preview : u));
            this.counts = { pics: pics.length, vids: vids.length };
            this.file = first;
            this.sample = pics[0] || this.sample;
            this.ext = extOf(this.file) || 'jpg';
          }
        };
      }
    },
    recipe: {
      // a site of your own, made in the site builder (sites.js): its list pages read, each item's page
      // opened for its file and tags
      label: 'Your own site',
      async fetch(s, st, page) {
        const S = window.vaultSites, r = { ...s.recipe, base: s.base, section: 'discover' }, cat = (st.picks || {}).topic || '';
        const items = st.view === 'random' ? await S.random(r, { cat }) : await S.list(r, { q: st.query, sort: st.view, cat, page });
        return { items, next: items.length && st.view !== 'random' ? page + 1 : st.view === 'random' && page < 5 ? page + 1 : null };
      },
      map(x, src) {
        if (!x.url || !x.thumb) return null;
        return {
          id: x.id, title: x.title, preview: x.thumb, sample: x.thumb, file: '', width: 0, height: 0, ext: x.duration || x.clip ? 'mp4' : 'jpg', score: 0, created: 0,
          rating: 'explicit', source: '', page: x.url, tags: [], _r: { ...src.recipe, base: src.base, section: 'discover' },
          resolve: async function () {
            if (this.file) return;
            const d = await window.vaultSites.details(this._r, this.page);
            this.file = d.file; this.ext = d.ext || this.ext;
            if (!/^(mp4|webm)$/.test(this.ext)) this.sample = d.file;
            this.tags = [...new Set([...this.tags, ...d.tags])];
          }
        };
      }
    },
    bing: {
      label: 'Web images (Bing)',
      async fetch(s, st, page) {
        if (!st.query.trim()) return { items: [], next: null, note: 'Type what to look for. Web images searches the whole web.' };
        const f = st.picks || {};
        const qft = [f.size && `+filterui:imagesize-${f.size}`, f.type && `+filterui:photo-${f.type}`, f.layout && `+filterui:aspect-${f.layout}`,
          f.color && `+filterui:color2-${f.color}`, f.time && `+filterui:age-${f.time}`].filter(Boolean).join('');
        const adlt = ['strict', 'moderate', 'off'].find(l => (st.levels || []).includes(l)) || 'off'; // SafeSearch
        const url = `https://www.bing.com/images/async?q=${enc(st.query.replace(/_/g, ' '))}&first=${(page - 1) * 35}&count=35&adlt=${adlt}&mmasync=1` + (qft ? `&qft=${enc(qft)}` : '');
        const html = await api('webJson', url, { Accept: 'text/html' });
        const doc = new DOMParser().parseFromString(html, 'text/html');
        const items = [...doc.querySelectorAll('a.iusc')].map(a => {
          try {
            const m = JSON.parse(a.getAttribute('m'));
            const li = a.closest('li') || a.parentElement;
            const info = li && li.querySelector('.img_info span, .nowrap');
            const dim = info && /(\d+)\s*[x×]\s*(\d+)/.exec(info.textContent);
            return { ...m, w: dim ? +dim[1] : 0, h: dim ? +dim[2] : 0 };
          } catch (e) { return null; }
        }).filter(Boolean);
        return { items, next: items.length ? page + 1 : null };
      },
      map(p) {
        if (!p.murl || !p.turl) return null;
        return {
          id: (p.mid || p.murl).slice(-16), title: p.t || '', preview: /mm\.bing\.net\/th/.test(p.turl) && !/[&?]w=/.test(p.turl) ? p.turl + '&w=720&h=720&rs=1&p=0' : p.turl, // (sharper, same shape) sample: p.murl, file: p.murl, width: p.w, height: p.h,
          ext: extOf(p.murl) || 'jpg', score: 0, created: 0, rating: 'general', source: p.purl || '', page: p.purl || p.murl, tags: []
        };
      },
    }
  };
  // LENS-START
  // ---------- each source's own browser: its rating levels, its ways of looking, its categories ----------
  // A site's levels are its own rating names: [how its searches write it, its label, the vault's rating].
  const LEVELS = {
    danbooru: [['g', 'General', 'general'], ['s', 'Sensitive', 'sensitive'], ['q', 'Questionable', 'questionable'], ['e', 'Explicit', 'explicit']],
    gelbooru: [['general', 'General', 'general'], ['sensitive', 'Sensitive', 'sensitive'], ['questionable', 'Questionable', 'questionable'], ['explicit', 'Explicit', 'explicit']],
    sqe: [['safe', 'Safe', 'general'], ['questionable', 'Questionable', 'questionable'], ['explicit', 'Explicit', 'explicit']], // older Gelbooru
    moebooru: [['s', 'Safe', 'general'], ['q', 'Questionable', 'questionable'], ['e', 'Explicit', 'explicit']],
    e621: [['s', 'Safe', 'general'], ['q', 'Questionable', 'questionable'], ['e', 'Explicit', 'explicit']],
    philomena: [['safe', 'Safe', 'general'], ['suggestive', 'Suggestive', 'sensitive'], ['questionable', 'Questionable', 'questionable'], ['explicit', 'Explicit', 'explicit']],
    // Reddit's feeds don't say what's NSFW, so its "levels" are what kind of post
    reddit: [['pics', 'Pictures', ''], ['motion', 'GIFs & videos', '']],
    // Bing's SafeSearch: one of three
    bing: [['strict', 'Strict', 'general'], ['moderate', 'Moderate', 'sensitive'], ['off', 'Off', 'explicit']]
  };
  // Ways of looking: [id, label, time spans it offers]. Spans: day, week, month, year, all.
  const VIEWS = {
    danbooru: [['latest', 'Latest'], ['hot', 'Hot'], ['popular', 'Popular', ['day', 'week', 'month']], ['top', 'Top'], ['random', 'Random']],
    gelbooru: [['latest', 'Latest'], ['top', 'Top'], ['random', 'Random']],
    moebooru: [['latest', 'Latest'], ['popular', 'Popular', ['day', 'week', 'month', 'year']], ['top', 'Top'], ['random', 'Random']],
    e621: [['latest', 'Latest'], ['hot', 'Hot'], ['popular', 'Popular', ['day', 'week', 'month']], ['top', 'Top'], ['random', 'Random']],
    philomena: [['latest', 'Latest'], ['trending', 'Trending'], ['popular', 'Popular', ['day', 'week', 'month']], ['top', 'Top'], ['random', 'Random']],
    shimmie: [['latest', 'Latest'], ['top', 'Top']],
    redgifs: [['top', 'Top', ['week', 'month', 'all']], ['latest', 'Latest']],
    erome: [['hot', 'Hot'], ['new', 'New']],
    reddit: [['hot', 'Hot'], ['new', 'New'], ['rising', 'Rising'], ['top', 'Top', ['day', 'week', 'month', 'year', 'all']]],
    bing: []
  };
  const SPAN_LABEL = { day: 'Today', week: 'This week', month: 'This month', year: 'This year', all: 'All time' };
  // Categories: groups of chips, [label, the search it adds]. A group's `key` keeps one choice of its own
  // (so a theme and a resolution can be on together); chips in one group replace each other.
  const grp = (name, chips, extra) => ({ name, chips, ...extra });
  const SERIES = grp('Series', [['Genshin Impact', 'genshin_impact'], ['Star Rail', 'honkai:_star_rail'], ['Zenless Zone Zero', 'zenless_zone_zero'], ['Nikke', 'goddess_of_victory:_nikke'],
    ['Hololive', 'hololive'], ['Fate', 'fate_(series)'], ['Final Fantasy', 'final_fantasy'], ['Overwatch', 'overwatch'], ['League of Legends', 'league_of_legends'], ['Originals', 'original']]);
  const ANIME_THEMES = grp('Themes', [['Big breasts', 'large_breasts'], ['Yuri', 'yuri'], ['Swimsuits', 'swimsuit'], ['Bunny girls', 'playboy_bunny'], ['Maids', 'maid'],
    ['Animal ears', 'animal_ears'], ['Elves', 'elf'], ['Monster girls', 'monster_girl'], ['Lingerie', 'lingerie'], ['Thighhighs', 'thighhighs'], ['Feet', 'feet'],
    ['Futanari', 'futanari'], ['Ahegao', 'ahegao'], ['Bondage', 'bondage'], ['Tentacles', 'tentacles'], ['Animated', 'animated'], ['Comics', 'comic']]);
  const CALM_THEMES = grp('Themes', [['Scenery', 'scenery'], ['Starry skies', 'starry_sky'], ['Flowers', 'flower'], ['Cats', 'cat'], ['Food', 'food'], ['Kimono', 'kimono'],
    ['Dresses', 'dress'], ['Witches', 'witch'], ['Maids', 'maid'], ['Animal ears', 'animal_ears'], ['Fantasy', 'fantasy'], ['Originals', 'original']]);
  const WALLS = grp('Scenes', [['Landscapes', 'landscape'], ['Scenic', 'scenic'], ['Skies', 'sky'], ['Clouds', 'clouds'], ['Night', 'night'], ['Sunsets', 'sunset'],
    ['Water', 'water'], ['Flowers', 'flowers'], ['Snow', 'snow'], ['Cities', 'city'], ['Rain', 'rain'], ['Stars', 'stars']]);
  const RES = grp('Resolution', [['Full HD+', 'width:>=1920'], ['QHD+', 'width:>=2560'], ['4K+', 'width:>=3840']], { key: 'res' });
  const FURRY_SPECIES = grp('Species', [['Canines', 'canine'], ['Foxes', 'fox'], ['Wolves', 'wolf'], ['Cats', 'felid'], ['Dragons', 'dragon'], ['Horses', 'equid'],
    ['Birds', 'avian'], ['Scalies', 'scalie'], ['Bunnies', 'lagomorph']]);
  const SITE = {
    danbooru: { color: '#0075f8', line: 'Anime art, tagged with care since 2005', facts: ['2 tags a search without an account'], trending: true, chips: [SERIES, ANIME_THEMES] },
    aibooru: { color: '#7a5af8', line: 'AI-made anime art', trending: true, chips: [ANIME_THEMES] },
    'safebooru-donmai': { color: '#3fa7f5', line: 'Danbooru\'s all-ages half', only: 'All-ages', trending: true, chips: [CALM_THEMES] },
    safebooru: { color: '#5c9be0', line: 'All-ages anime art', only: 'All-ages', chips: [CALM_THEMES] },
    gelbooru: { color: '#0773fb', line: 'A huge anime and hentai archive', levels: 'gelbooru', chips: [SERIES, ANIME_THEMES] },
    rule34: { color: '#7bc96f', line: 'If it exists, there\'s porn of it', levels: 'sqe', chips: [
      grp('Series', [['Overwatch', 'overwatch'], ['Genshin Impact', 'genshin_impact'], ['League of Legends', 'league_of_legends'], ['Final Fantasy', 'final_fantasy'],
        ['Fortnite', 'fortnite'], ['Marvel', 'marvel'], ['DC', 'dc_comics'], ['Resident Evil', 'resident_evil'], ['Mass Effect', 'mass_effect']]),
      grp('Themes', [['3D', '3d'], ['Animated', 'animated'], ['With sound', 'sound'], ['Big breasts', 'large_breasts'], ['Thick thighs', 'thick_thighs'], ['Futanari', 'futanari'],
        ['Ahegao', 'ahegao'], ['Monster girls', 'monster_girl'], ['Tentacles', 'tentacles'], ['Feet', 'feet']])] },
    tbib: { color: '#8a8f98', line: 'The Big ImageBoard: a mirror of many boorus', levels: 'sqe', chips: [SERIES, ANIME_THEMES] },
    xbooru: { color: '#e0567a', line: 'Hentai', levels: 'sqe', chips: [ANIME_THEMES] },
    hypnohub: { color: '#9a6bff', line: 'Hypnosis and mind-control art', levels: 'sqe', chips: [
      grp('Eyes', [['Spirals', 'spiral_eyes'], ['Rings', 'ring_eyes'], ['Empty', 'empty_eyes'], ['Kaa', 'kaa_eyes'], ['Glowing', 'glowing_eyes']]),
      grp('Roles', [['Femsub', 'femsub'], ['Malesub', 'malesub'], ['Femdom', 'femdom'], ['Maledom', 'maledom']]),
      grp('Themes', [['Pendulum', 'pendulum'], ['Tech', 'tech_control'], ['Collars', 'collar'], ['Animated', 'animated'], ['Comics', 'comic'], ['With text', 'text']])] },
    yandere: { color: '#ee8887', line: 'High-resolution scans from artbooks and magazines', chips: [
      grp('Themes', [['Bikini', 'bikini'], ['Swimsuits', 'swimsuits'], ['Lingerie', 'lingerie'], ['Maids', 'maid'], ['Kimono', 'kimono'], ['Bunny girls', 'bunny_girl'],
        ['Animal ears', 'animal_ears'], ['Thighhighs', 'thighhighs'], ['Cleavage', 'cleavage'], ['Wet', 'wet'], ['Nude', 'naked'], ['Yuri', 'yuri']])] },
    konachan: { color: '#f08a3c', line: 'Anime wallpapers, all-ages', only: 'All-ages', row: 300, chips: [WALLS, RES] },
    'konachan-com': { color: '#f08a3c', line: 'Anime wallpapers, every rating', row: 300, chips: [WALLS, RES] },
    paheal: { color: '#5b8f3a', line: 'Rule 34 of everything', only: 'All explicit', chips: [
      grp('Series', [['Overwatch', 'Overwatch'], ['Genshin Impact', 'Genshin_Impact'], ['League of Legends', 'League_of_Legends'], ['Final Fantasy', 'Final_Fantasy'],
        ['Fortnite', 'Fortnite'], ['Marvel', 'Marvel'], ['DC', 'DC'], ['Resident Evil', 'Resident_Evil'], ['Mass Effect', 'Mass_Effect'], ['Star Wars', 'Star_Wars']]),
      grp('Kinds', [['Animated', 'animated'], ['Comics', 'Comic']])] },
    e621: { color: '#fdba31', line: 'Furry art, every rating', facts: ['Optional account shows more'], chips: [FURRY_SPECIES,
      grp('Bodies', [['Anthro', 'anthro'], ['Feral', 'feral']], { key: 'body' }),
      grp('Pairings', [['Solo', 'solo'], ['Gay', 'male/male'], ['Lesbian', 'female/female'], ['Straight', 'male/female']], { key: 'pair' }),
      grp('Media', [['Animated', 'animated'], ['Comics', 'comic'], ['3D', '3d_(artwork)']], { key: 'media' })] },
    e926: { color: '#fdba31', line: 'Furry art, all-ages', only: 'All-ages', chips: [FURRY_SPECIES,
      grp('Bodies', [['Anthro', 'anthro'], ['Feral', 'feral']], { key: 'body' }), grp('Media', [['Comics', 'comic'], ['Animated', 'animated']], { key: 'media' })] },
    derpibooru: { color: '#618fc3', line: 'My Little Pony fan art', chips: [
      grp('Characters', [['Twilight Sparkle', 'twilight_sparkle'], ['Rainbow Dash', 'rainbow_dash'], ['Fluttershy', 'fluttershy'], ['Pinkie Pie', 'pinkie_pie'], ['Rarity', 'rarity'],
        ['Applejack', 'applejack'], ['Princess Luna', 'princess_luna'], ['Celestia', 'princess_celestia'], ['Sunset Shimmer', 'sunset_shimmer'], ['Trixie', 'trixie']]),
      grp('Styles', [['Anthro', 'anthro'], ['Humanized', 'humanized'], ['Equestria Girls', 'equestria_girls'], ['OCs', 'oc'], ['Comics', 'comic']], { key: 'style' })] },
    furbooru: { color: '#e07b39', line: 'Furry art', chips: [
      grp('Species', [['Canines', 'canine'], ['Foxes', 'fox'], ['Wolves', 'wolf'], ['Cats', 'feline'], ['Dragons', 'dragon'], ['Horses', 'equine'], ['Bunnies', 'rabbit']]),
      grp('Bodies', [['Anthro', 'anthro'], ['Feral', 'feral']], { key: 'body' })] },
    bing: { color: '#13a10e', line: 'Pictures from the whole web', row: 230 },
    redgifs: { color: '#e42744', line: 'Short porn clips and GIFs, in niches', only: 'All explicit', niches: true, row: 300 },
    erome: { color: '#d1355b', line: 'Amateur albums: pictures and videos people post', only: 'All explicit', row: 280, chips: [
      grp('Show', [['Videos', 'videos'], ['Pictures only', 'pictures'], ['Big albums (10+)', 'big']], { key: 'type' }),
      grp('Popular', [['Amateur', 'amateur'], ['Homemade', 'homemade'], ['Couple', 'couple'], ['Cosplay', 'cosplay'], ['Selfie', 'selfie'], ['Lingerie', 'lingerie'],
        ['Shower', 'shower'], ['Outdoor', 'outdoor'], ['Public', 'public'], ['Gym', 'gym'], ['Feet', 'feet'], ['Solo', 'solo'], ['Lesbian', 'lesbian'], ['Threesome', 'threesome'],
        ['Blowjob', 'blowjob'], ['Riding', 'riding'], ['Doggy', 'doggy'], ['Creampie', 'creampie'], ['Squirt', 'squirt'], ['Anal', 'anal']]),
      grp('Body', [['Petite', 'petite'], ['Curvy', 'curvy'], ['Thick', 'thick'], ['PAWG', 'pawg'], ['Big tits', 'big tits'], ['Small tits', 'small tits'], ['Big ass', 'big ass'],
        ['Natural', 'natural'], ['Tattoos', 'tattoo'], ['Piercings', 'piercing'], ['Hairy', 'hairy'], ['Tall', 'tall']], { key: 'body' }),
      grp('Look', [['Blonde', 'blonde'], ['Brunette', 'brunette'], ['Redhead', 'redhead'], ['Asian', 'asian'], ['Latina', 'latina'], ['Ebony', 'ebony'],
        ['Indian', 'indian'], ['MILF', 'milf'], ['Goth', 'goth'], ['Alt', 'alt'], ['Freckles', 'freckles']], { key: 'look' })
    ] },
    reddit: { color: '#ff4500', line: 'Communities of real people, cosplay, art and more', facts: ['One page a minute without an app id'] }
  };
  // a site that isn't in the list gets its kind's controls
  const KIND_SITE = { danbooru: 'danbooru', gelbooru: 'tbib', moebooru: 'yandere', e621: 'e621', philomena: 'furbooru', shimmie: 'paheal', reddit: 'reddit', bing: 'bing', redgifs: 'redgifs', erome: 'erome' };
  // LENS-END
  // "# Group" lines start a group of your own chips, "Label = search words" lines are its chips
  function ownChips(text) {
    const groups = [];
    for (const line of String(text || '').split('\n').map(l => l.trim()).filter(Boolean)) {
      if (line.startsWith('#')) { groups.push(grp(line.replace(/^#+\s*/, '') || 'Yours', [], { key: 'own' + groups.length, own: true })); continue; }
      if (!groups.length) groups.push(grp('Yours', [], { key: 'own0', own: true }));
      const k = line.indexOf('=');
      groups[groups.length - 1].chips.push(k < 0 ? [line, line] : [line.slice(0, k).trim(), line.slice(k + 1).trim()]);
    }
    return groups.filter(g => g.chips.length);
  }
  const siteOf = src => {
    if (!src) return {};
    const base = src.kind === 'recipe'
      ? { color: src.color || '#9b5de5', line: `Your own site · ${String(src.base).replace(/^https?:\/\//, '')}`, row: 260,
          chips: (src.recipe.cats || []).length ? [grp('Categories', src.recipe.cats)] : [] }
      : SITE[src.id] || SITE[KIND_SITE[src.kind]] || {};
    const look = cfg && cfg.look && cfg.look[src.id];
    if (!look) return base;
    return { ...base, row: look.row || base.row, banner: look.banner !== false,
      chips: [...ownChips(look.chips), ...(look.siteChips === false ? [] : base.chips || [])], trending: look.siteChips === false ? false : base.trending, niches: look.siteChips === false ? false : base.niches };
  };
  const levelsOf = src => siteOf(src).only ? [] : (LEVELS[siteOf(src).levels || src.kind] || []).map(([q, label, r]) => ({ q, label, r }));
  const viewsOf = src => src.kind === 'recipe'
    ? [...((src.recipe.sorts || []).length ? src.recipe.sorts.map(([label, id]) => ({ id, label })) : [{ id: '', label: 'Latest' }]), { id: 'random', label: '🎲 Random' }]
    : (VIEWS[src.kind] || []).map(([id, label, spans]) => ({ id, label, spans }));
  // the levels that are on, in the site's own search words (nothing when all are on)
  function levelTags(src, on) {
    const all = levelsOf(src);
    if (!all.length || all.every(l => on.includes(l.q))) return [];
    if (src.kind === 'danbooru') return [`rating:${on.join(',')}`]; // (free: not one of the 2 tags)
    if (src.kind === 'philomena') return on.map(q => '__rating:' + q);
    if (/^(gelbooru|moebooru|e621)$/.test(src.kind)) return all.filter(l => !on.includes(l.q)).map(l => `-rating:${l.q}`);
    return [];
  }
  // the view and its time span, in the site's own search words
  const daysOf = span => ({ day: 1, week: 7, month: 30, year: 365 })[span] || 7;
  const dayIso = days => new Date(Date.now() - days * 864e5).toISOString().slice(0, 10);
  function viewTags(src, view, span) {
    switch (src.kind) {
      case 'danbooru': return { hot: ['order:rank'], popular: ['order:score', `age:<${{ day: '1d', week: '1w', month: '1mo' }[span] || '1w'}`], top: ['order:score'], random: ['order:random'] }[view] || [];
      case 'gelbooru': return { top: ['sort:score'], random: ['sort:random'] }[view] || [];
      case 'moebooru': return { popular: ['order:score', `date:>=${dayIso(daysOf(span))}`], top: ['order:score'], random: ['order:random'] }[view] || [];
      case 'e621': return { hot: ['order:hot'], popular: ['order:score', `date:${{ day: 'day', month: 'month' }[span] || 'week'}`], top: ['order:score'], random: ['order:random'] }[view] || [];
      case 'philomena': return { trending: ['__sort:wilson_score', '__since:3'], popular: ['__sort:score', `__since:${daysOf(span)}`], top: ['__sort:score'], random: ['__sort:random'] }[view] || ['__sort:created_at'];
      case 'shimmie': return { top: ['order:score'] }[view] || [];
    }
    return [];
  }
  // Reddit communities: the catalogue's, by category
  const REDDIT_SUBS = () => CATALOG.filter(p => p.kind === 'reddit' && p.sub);
  const catOfSub = sub => (REDDIT_SUBS().find(p => p.sub.toLowerCase() === String(sub).toLowerCase()) || {}).cat || 'real';

  // posts on sites without tags (Reddit, the web) are kept out by their words too
  const NEVER_TEXT = /\b(loli|lolicon|shota|shotacon|toddler|underage|under[\s-]?age|under\s?18|minors?|child|children|kids?|preteens?|jailbait|cp)\b/i;

  // ---------- Reddit: your own app's client id; your home feed once you log in ----------
  async function redditToken() {
    const r = cfg.reddit;
    if (!r.clientId) throw new Error('Reddit needs your app\'s client id (Sources → Reddit)');
    if (r.token && r.tokenUntil > Date.now() + 60e3) return r.token;
    const form = r.refresh ? { grant_type: 'refresh_token', refresh_token: r.refresh }
      : { grant_type: 'https://oauth.reddit.com/grants/installed_client', device_id: r.device || (r.device = Array.from(crypto.getRandomValues(new Uint8Array(15)), b => (b % 36).toString(36)).join('')) };
    const a = JSON.parse(await api('webPost', 'https://www.reddit.com/api/v1/access_token', form, { Authorization: 'Basic ' + btoa(r.clientId + ':') }));
    if (!a.access_token) throw new Error(a.error === 'invalid_grant' ? 'Your Reddit login expired. Log in again (Sources → Reddit).' : 'Reddit refused the client id: ' + (a.error || a.message || 'unknown'));
    r.token = a.access_token; r.tokenUntil = Date.now() + (a.expires_in || 3600) * 1000;
    saveConfig();
    return r.token;
  }
  // Reddit's public feeds (no app id needed): an Atom feed per listing or search, 50 posts a page.
  // They say nothing about NSFW, so a post's rating follows its source's category; galleries and
  // text posts have no picture in them and are left out; Reddit videos play without sound.
  // Reddit lets them be read once a minute per internet connection (whatever the app), so every
  // feed (Discover's and Audio's) waits its turn in one queue, a minute apart, counting down through
  // onWait(seconds); one nobody wants any more when its turn comes (wanted() false) is dropped
  // without being read, and a feed read in the last 5 minutes comes from memory.
  const RSS_NS = 'http://search.yahoo.com/mrss/';
  const rssCache = new Map();
  let rssQueue = Promise.resolve(), rssNextAt = 0;
  function rssText(url, { onWait = () => {}, wanted = () => true } = {}) {
    const hit = rssCache.get(url);
    if (hit && Date.now() - hit.at < 5 * 60e3) return Promise.resolve(hit.text);
    const turn = rssQueue.then(async () => {
      for (let tries = 1; ; tries++) {
        while (Date.now() < rssNextAt) {
          if (!wanted()) throw new Error('No longer wanted');
          onWait(Math.ceil((rssNextAt - Date.now()) / 1000));
          await new Promise(r => setTimeout(r, Math.min(1000, rssNextAt - Date.now())));
        }
        if (!wanted()) throw new Error('No longer wanted');
        try {
          const text = await api('webJson', url, { Accept: 'application/atom+xml' });
          rssNextAt = Date.now() + 61e3;
          for (const [k, v] of rssCache) if (Date.now() - v.at > 5 * 60e3) rssCache.delete(k);
          rssCache.set(url, { at: Date.now(), text });
          return text;
        } catch (e) {
          const m = /^429(?: \[retry ([\d.]+)\])?/.exec(errMsg(e));
          if (!m) throw e;
          rssNextAt = Date.now() + (Math.min(+m[1] || 60, 600) + 2) * 1000;
          if (tries >= 3) throw new Error('Reddit is turning away its public feeds from this connection for now. Try again in a few minutes, or use an app id of your own (Sources → Reddit).');
        }
      }
    });
    rssQueue = turn.catch(() => {});
    return turn;
  }
  const rssWaitText = s => `Without an app id of your own, Reddit allows one page a minute: next one in ${s} s…`;
  async function redditRss(s, st, after) {
    const my = ticket; // (loadMore's: a newer search makes this one unwanted)
    const text = st.query.trim(), sub = s.sub || ''; // (s is the community being browsed, see current())
    const sort = st.view || 'hot', t = sort === 'top' ? st.span || 'week' : 'all';
    const base = sub ? `https://www.reddit.com/r/${sub}` : 'https://www.reddit.com/r/popular';
    const url = text ? `${base}/search.rss?q=${enc(text)}${sub ? '&restrict_sr=on' : ''}&sort=${{ hot: 'relevance', rising: 'hot' }[sort] || sort}&t=${t}&include_over_18=on&limit=50`
      : `${base}/${sort}/.rss?limit=50${sort === 'top' ? '&t=' + t : ''}`;
    const feed = await rssText(url + (after ? '&after=' + after : ''), {
      wanted: () => my === ticket,
      onWait: sec => { (after ? more : status).textContent = rssWaitText(sec); }
    });
    const xml = new DOMParser().parseFromString(feed, 'text/xml');
    const entries = [...xml.getElementsByTagName('entry')];
    const rating = s.cat === 'art' ? 'general' : 'explicit';
    const items = entries.map(e => rssPost(e, rating)).filter(Boolean);
    const lastId = entries.length ? (entries[entries.length - 1].getElementsByTagName('id')[0] || {}).textContent : '';
    return { items, next: entries.length ? lastId : null };
  }
  function rssPost(e, rating) {
    const get = tag => (e.getElementsByTagName(tag)[0] || {}).textContent || '';
    const id = get('id').replace(/^t3_/, '');
    const title = get('title');
    if (!id || NEVER_TEXT.test(title)) return null;
    const author = get('name').replace(/^\/u\//, '');
    const linkEl = e.getElementsByTagName('link')[0];
    const page = linkEl ? linkEl.getAttribute('href') : '';
    const sub = (e.getElementsByTagName('category')[0] || { getAttribute: () => '' }).getAttribute('term') || '';
    const html = new DOMParser().parseFromString(get('content'), 'text/html');
    const target = ([...html.querySelectorAll('a')].find(a => a.textContent.trim() === '[link]') || {}).href || '';
    const thumbEl = e.getElementsByTagNameNS(RSS_NS, 'thumbnail')[0];
    const thumb = thumbEl ? thumbEl.getAttribute('url') : '';
    let file = '', ext = '', preview = '', resolve = null;
    if (/^https:\/\/i\.redd\.it\/.+\.(jpe?g|png|webp|gif)$/i.test(target) || /^https:\/\/i\.imgur\.com\/.+\.(jpe?g|png|webp|gif)$/i.test(target)) {
      file = target; preview = target; ext = extOf(target);
    } else if (/imgur\.com\/.+\.gifv$/i.test(target)) {
      file = target.replace(/\.gifv$/i, '.mp4'); ext = 'mp4'; preview = thumb;
    } else if (/^https:\/\/v\.redd\.it\/[a-z0-9]+/i.test(target)) {
      // a Reddit video: its video-only file comes in a few sizes; the first that exists is used
      const vid = target.replace(/\/$/, '');
      ext = 'mp4'; preview = thumb;
      resolve = async function () {
        if (this.file) return;
        for (const q of [720, 480, 360, 240]) {
          const u = `${vid}/DASH_${q}.mp4`;
          const r = await fetch(proxy(u)).catch(() => null);
          if (r && r.ok) { this.file = u; return; }
        }
        throw new Error('That Reddit video can\'t be played here');
      };
    } else if (/redgifs\.com\/(watch|ifr)\//i.test(target)) {
      const rid = target.split('/').pop().split(/[?#-]/)[0].toLowerCase();
      ext = 'mp4'; preview = thumb;
      resolve = async function () { if (this.file) return; const g = await redgifsGif(rid); this.file = g.urls.hd || g.urls.sd; this.width = g.width; this.height = g.height; };
    } else return null; // galleries, text and links elsewhere: nothing to show from a feed
    if (!preview) return null;
    return {
      _mapped: true, id, title, preview, sample: file || preview, file, width: 0, height: 0, ext: ext || 'jpg', score: 0, created: Date.parse(get('published')) || 0,
      rating, source: target, page, resolve, sub,
      tags: ['meta:reddit', ...(sub ? ['meta:r_' + sub.toLowerCase()] : []), ...(author ? ['artist:' + author.toLowerCase()] : [])]
    };
  }

  // RedGifs (linked from Reddit): a temporary token, then the clip's addresses
  let rgToken = null;
  async function rgApi(path) {
    if (!rgToken) rgToken = JSON.parse(await api('webJson', 'https://api.redgifs.com/v2/auth/temporary')).token;
    try { return JSON.parse(await api('webJson', 'https://api.redgifs.com/v2' + path, { Authorization: 'Bearer ' + rgToken })); }
    catch (e) { if (/^401/.test(errMsg(e))) { rgToken = null; return rgApi(path); } throw e; }
  }
  async function redgifsGif(id) {
    const d = await rgApi(`/gifs/${enc(id)}`);
    if (!d.gif) throw new Error('That RedGifs clip is gone');
    return d.gif;
  }
  // RedGifs' niches (its own categories), the most followed first, with their pictures
  let rgNiches = null;
  function redgifsNiches() {
    if (!rgNiches) rgNiches = rgApi('/niches?order=subscribers&count=60&page=1').then(d => (d.niches || [])
      .filter(n => n.id && !NEVER_TEXT.test(n.name + ' ' + (n.tags || []).join(' '))).map(n => [n.name.trim(), n.id, n.thumbnail])).catch(() => { rgNiches = null; return []; });
    return rgNiches;
  }

  // ---------- the catalogue: every source you can add, in categories ----------
  // (sites with an API Lewd Vault reads; Reddit communities through your Reddit app. Nothing aimed
  // at anyone under 18 is listed, and the never-show filter covers every one of them.)
  const svg = d => `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">${d}</svg>`;
  const CATEGORIES = [
    ['anime', 'Anime art', svg('<path d="M12 2.5l2.3 6.2 6.2 2.3-6.2 2.3L12 19.5l-2.3-6.2L3.5 11l6.2-2.3z"/>')],
    ['hentai', 'Hentai & rule 34', svg('<path d="M12 20s-7.5-4.7-7.5-10.2A4.3 4.3 0 0 1 12 7.2a4.3 4.3 0 0 1 7.5 2.6C19.5 15.3 12 20 12 20z"/>')],
    ['real', 'Real people', svg('<rect x="3" y="7" width="18" height="13" rx="3"/><circle cx="12" cy="13.5" r="3.5"/><path d="M8.5 7l1.5-3h4l1.5 3"/>')],
    ['cosplay', 'Cosplay', svg('<path d="M3 9c3-2 6-2 9 0 3-2 6-2 9 0 0 5-3 8-6 8-2 0-3-2-3-2s-1 2-3 2c-3 0-6-3-6-8z"/><circle cx="8" cy="11" r="1.2"/><circle cx="16" cy="11" r="1.2"/>')],
    ['femboy', 'Femboys', svg('<path d="M8.5 19.5c-3-1.5-3.6-6-.6-9.5s7.3-5.3 9.3-3.3-.2 6.5-3.6 9.6c-1.9 1.8-3.4 3.9-5.1 3.2z"/><path d="M16.6 6.6l2.4-2.4M15.5 4.8c1.3-.8 3-.6 4 .4"/><path d="M6 6.5l-2-1.2v2.4zM6 6.5l2-1.2v2.4z"/>')],
    ['trans', 'Trans', svg('<circle cx="12" cy="13" r="4.5"/><path d="M15.2 9.8L20 5M17 5h3v3M8.8 9.8L4 5M4 8V5h3M12 17.5V22M9.8 20h4.4"/>')],
    ['men', 'Men & gay', svg('<circle cx="9" cy="14" r="4.5"/><path d="M12.2 10.8L18 5M14.5 5H18v3.5"/><circle cx="15.5" cy="17" r="2.5" opacity=".6"/>')],
    ['lesbian', 'Women & yuri', svg('<circle cx="9" cy="9" r="4.5"/><path d="M9 13.5V21M6.5 18h5"/><circle cx="16" cy="11" r="3.5" opacity=".6"/>')],
    ['furry', 'Furry', svg('<ellipse cx="12" cy="16" rx="4.5" ry="3.8"/><circle cx="6" cy="10.5" r="2"/><circle cx="9.7" cy="6.5" r="2"/><circle cx="14.3" cy="6.5" r="2"/><circle cx="18" cy="10.5" r="2"/>')],
    ['pony', 'Pony', svg('<path d="M6 3.5v8.5a6 6 0 0 0 12 0V3.5"/><path d="M6 7h2M16 7h2M6 11h2M16 11h2"/>')],
    ['feet', 'Feet', svg('<ellipse cx="11" cy="15" rx="4" ry="6"/><circle cx="8.5" cy="6.5" r="1.4"/><circle cx="11.5" cy="5.3" r="1.4"/><circle cx="14.4" cy="6.4" r="1.2"/><circle cx="16.4" cy="8.4" r="1"/>')],
    ['kink', 'Kink & BDSM', svg('<circle cx="7" cy="14" r="4"/><circle cx="17" cy="14" r="4"/><path d="M11 14h2M7 10V7a5 5 0 0 1 10 0v3"/>')],
    ['hypno', 'Hypno', svg('<path d="M12 12a1.5 1.5 0 1 1 1.5 1.5A3 3 0 1 1 10.5 10.5a4.5 4.5 0 1 1-1.5 6.4A6 6 0 1 1 18 12"/>')],
    ['monster', 'Monster girls', svg('<path d="M6 10c-2-3-2-6 0-7 1 2 2 3 4 4M18 10c2-3 2-6 0-7-1 2-2 3-4 4"/><circle cx="12" cy="14" r="6"/><path d="M10 13.5v.5M14 13.5v.5"/>')],
    ['art', 'Art & wallpapers', svg('<path d="M12 3a9 9 0 1 0 0 18c1.2 0 1.8-1 1.3-2-.6-1.2.2-2.5 1.5-2.5H17a4 4 0 0 0 4-4C21 7.2 17 3 12 3z"/><circle cx="7.5" cy="11" r="1.2"/><circle cx="10" cy="7" r="1.2"/><circle cx="15" cy="7.5" r="1.2"/>')],
    ['web', 'The whole web', svg('<circle cx="11" cy="11" r="7"/><path d="M21 21l-4.5-4.5M4 11h14M11 4c2.5 2.5 2.5 11.5 0 14M11 4c-2.5 2.5-2.5 11.5 0 14"/>')]
  ];
  const CAT_ICON = Object.fromEntries(CATEGORIES.map(([id, , icon]) => [id, icon]));
  const REDDIT_ICON = 'https://www.reddit.com/apple-touch-icon-precomposed.png'; // (128 px)
  const R = (sub, cat, name) => ({ id: 'r-' + sub.toLowerCase(), name: name || 'r/' + sub, kind: 'reddit', base: 'https://oauth.reddit.com', sub, cat, icon: REDDIT_ICON });
  const CATALOG = [
    // anime boorus
    { id: 'danbooru', name: 'Danbooru', kind: 'danbooru', base: 'https://danbooru.donmai.us', cat: 'anime', userLabel: 'Login name',
      help: 'Optional. Without an account a search can use 2 tags at most. Your login name and API key are under My Account → API Key.' },
    { id: 'safebooru-donmai', name: 'Safebooru (Danbooru)', kind: 'danbooru', base: 'https://safebooru.donmai.us', cat: 'art', help: 'Danbooru\'s all-ages half.' },
    { id: 'safebooru', name: 'Safebooru', kind: 'gelbooru', base: 'https://safebooru.org', cat: 'art', help: 'All-ages pictures only. No account needed.' },
    { id: 'gelbooru', name: 'Gelbooru', kind: 'gelbooru', base: 'https://gelbooru.com', cat: 'hentai', needsKey: true, userLabel: 'User ID',
      help: 'Gelbooru needs your API key: gelbooru.com → My Account → Options → API Access Credentials (the user_id and api_key in it).' },
    { id: 'yandere', name: 'Yande.re', kind: 'moebooru', base: 'https://yande.re', cat: 'anime', help: 'High-resolution scans. No account needed.' },
    { id: 'konachan', name: 'Konachan.net', kind: 'moebooru', base: 'https://konachan.net', cat: 'art', help: 'Anime wallpapers, all-ages. No account needed.' },
    { id: 'konachan-com', name: 'Konachan.com', kind: 'moebooru', base: 'https://konachan.com', cat: 'anime', help: 'Anime wallpapers, all ratings. No account needed.' },
    { id: 'aibooru', name: 'AIBooru', kind: 'danbooru', base: 'https://aibooru.online', cat: 'anime', help: 'AI-made anime art. No account needed.' },
    { id: 'tbib', name: 'TBIB', kind: 'gelbooru', base: 'https://tbib.org', cat: 'anime', help: 'The Big ImageBoard: a huge mirror of many boorus. No account needed.' },
    { id: 'xbooru', name: 'Xbooru', kind: 'gelbooru', base: 'https://xbooru.com', cat: 'hentai', help: 'Hentai. No account needed.' },
    { id: 'rule34', name: 'Rule34', kind: 'gelbooru', base: 'https://api.rule34.xxx', pageBase: 'https://rule34.xxx', cat: 'hentai', needsKey: true, userLabel: 'User ID',
      help: 'Rule34 needs your API key: rule34.xxx → My Account → Options → API Access Credentials.' },
    { id: 'paheal', name: 'Rule34 Paheal', kind: 'shimmie', base: 'https://rule34.paheal.net', cat: 'hentai', help: 'Rule 34 of everything, all explicit. No account needed.' },
    { id: 'hypnohub', name: 'Hypnohub', kind: 'gelbooru', base: 'https://hypnohub.net', cat: 'hypno', help: 'Hypnosis and mind-control art. No account needed.' },
    // furry and pony
    { id: 'e621', name: 'e621', kind: 'e621', base: 'https://e621.net', cat: 'furry', userLabel: 'Username',
      help: 'Furry art, all ratings. Optional account (username + API key from Account → Manage API Access) shows more.' },
    { id: 'e926', name: 'e926', kind: 'e621', base: 'https://e926.net', cat: 'furry', help: 'e621\'s all-ages half.' },
    { id: 'furbooru', name: 'Furbooru', kind: 'philomena', base: 'https://furbooru.org', cat: 'furry', filter: '2', help: 'Furry art. Optional API key (Account → API key).' },
    { id: 'derpibooru', name: 'Derpibooru', kind: 'philomena', base: 'https://derpibooru.org', cat: 'pony', filter: '56027', help: 'Pony art. Uses the "Everything" filter; optional API key shows your own filters.' },
    // short clips
    { id: 'redgifs', name: 'RedGifs', kind: 'redgifs', base: 'https://api.redgifs.com', cat: 'real', help: 'Short porn clips and GIFs, in RedGifs\' niches. No account needed.' },
    { id: 'erome', name: 'Erome', kind: 'erome', base: 'https://www.erome.com', cat: 'real', help: 'Amateur albums of pictures and videos. No account needed.' },
    // the web
    { id: 'bing', name: 'Web images', kind: 'bing', base: 'https://www.bing.com', cat: 'web', icon: 'https://www.bing.com/favicon.ico',
      help: 'Searches pictures on the whole web (through Bing; Google and DuckDuckGo no longer let apps search, so there are buttons that open them in your browser). Filters: size, type, shape, colour, time.' },
    // Reddit (needs your Reddit app, see Sources → Reddit)
    { ...R('', 'real', 'Reddit'), id: 'reddit', help: 'Every Reddit community in one tab: pick one from the list under the search (or type r/name), pin the ones you like. Your home feed once you log in with an app id.' },
    ...['gonewild', 'RealGirls', 'Amateur', 'nsfw', 'NSFW_GIF', 'nsfw_gifs', 'holdthemoan', 'BustyPetite', 'curvy', 'ass', 'boobs', 'PetiteGoneWild', 'gonewildcurvy',
      'OnOff', 'GirlsFinishingTheJob', 'cumsluts', 'porn', 'collegesluts', 'Blowjobs', 'pussy', 'thick', 'palegirls', 'Hotwife', 'milf', 'AsiansGoneWild', 'latinas',
      'ebony', 'TittyDrop', 'gonewild30plus', 'Nudes', 'GWCouples', 'RealPublicNudity', 'bigasses', 'Boobies'].map(s => R(s, 'real')),
    ...['nsfwcosplay', 'cosplaybutts', 'CosplayLewd', 'cosplaygirls', 'cosplay'].map(s => R(s, 'cosplay')),
    ...['femboy', 'FemBoys', 'femboys', 'feminineboys', 'FemboyThighs'].map(s => R(s, 'femboy')),
    ...['Tgirls', 'GoneWildTrans', 'transporn'].map(s => R(s, 'trans')),
    ...['gaybrosgonewild', 'gayporn', 'ladybonersgw', 'MassiveCock', 'GaybrosGoneWild'].filter((s, k, a) => a.findIndex(x => x.toLowerCase() === s.toLowerCase()) === k).map(s => R(s, 'men')),
    ...['lesbians', 'Lesbian_gifs', 'dykesgonewild', 'yuri'].map(s => R(s, 'lesbian')),
    ...['hentai', 'rule34', 'ecchi', 'HENTAI_GIF', 'AnimeBooty', 'thighdeology', 'OppaiLove', 'Overwatch_Porn'].map(s => R(s, 'hentai')),
    ...['MonsterGirl', 'monstergirls'].map(s => R(s, 'monster')),
    ...['feet', 'FootFetish', 'Soles', 'feetpics'].map(s => R(s, 'feet')),
    ...['yiff', 'furry', 'furryporn'].map(s => R(s, 'furry')),
    ...['BDSM', 'femdom', 'bondage', 'BDSMGW', 'collared'].map(s => R(s, 'kink')),
    ...['HypnoHentai', 'EroticHypnosis'].map(s => R(s, 'hypno')),
    ...['Animewallpaper', 'AnimeART', 'Art', 'wallpaper', 'wallpapers', 'EarthPorn'].map(s => R(s, 'art'))
  ];
  const PRESETS = CATALOG; // (older settings used this name)

  // ---------- settings (on this PC, see web.rs) ----------
  let cfg = null;
  async function loadConfig() {
    cfg = await api('loadSources').catch(() => null);
    if (!cfg || !Array.isArray(cfg.sources)) {
      const first = ['danbooru', 'safebooru', 'gelbooru', 'yandere', 'konachan', 'rule34', 'e621', 'bing'];
      cfg = { sources: CATALOG.filter(p => first.includes(p.id)).map(p => ({ ...p, user: '', key: '', enabled: true })), active: 'danbooru', blacklist: '', ratings: [...RATINGS], categories: {} };
      await saveConfig();
    }
    // what the catalogue says about each source is always the latest (its help, address, category...)
    cfg.sources.forEach(s => {
      const p = CATALOG.find(x => x.id === s.id);
      if (p) Object.assign(s, { name: p.name, kind: p.kind, base: p.base, help: p.help, needsKey: p.needsKey, userLabel: p.userLabel, pageBase: p.pageBase, cat: p.cat, filter: p.filter, icon: p.icon, sub: p.sub });
    });
    cfg.sources = cfg.sources.filter(s => KINDS[s.kind] && (s.kind !== 'recipe' || s.recipe));
    cfg.sites = cfg.sites || []; // (Videos' and Audio's own sites)
    cfg.look = cfg.look || {};
    cfg.reddit = cfg.reddit || { clientId: '' };
    cfg.bookmarks = cfg.bookmarks || [];
    cfg.saucenaoKey = cfg.saucenaoKey || '';
    cfg.ratings = (cfg.ratings || RATINGS).filter(r => RATINGS.includes(r));
    cfg.categories = cfg.categories || {};
    cfg.lens = cfg.lens || {};
    // Reddit: one tab with a community picker; the communities you had as sources become pinned ones
    if (!cfg.redditPins) cfg.redditPins = [...new Set(cfg.sources.filter(s => s.kind === 'reddit' && s.sub).map(s => s.sub))];
    cfg.redditRecent = cfg.redditRecent || [];
    if (!cfg.eromeTab) {
      cfg.eromeTab = true; // (added once for everyone; remove it in Sources if unwanted)
      if (!cfg.sources.some(s => s.id === 'erome')) cfg.sources.push({ ...CATALOG.find(x => x.id === 'erome'), user: '', key: '', enabled: true });
      saveConfig();
    }
    if (!cfg.redgifsTab) {
      cfg.redgifsTab = true; // RedGifs moved here from Videos (short clips belong with the pictures)
      if (!cfg.sources.some(s => s.id === 'redgifs')) cfg.sources.push({ ...CATALOG.find(x => x.id === 'redgifs'), user: '', key: '', enabled: true });
      saveConfig();
    }
    if (!cfg.redditTab) {
      cfg.redditTab = true; // (once: remove it in Sources if you don't want it)
      if (!cfg.sources.some(s => s.kind === 'reddit')) cfg.sources.push({ ...CATALOG.find(x => x.id === 'reddit'), user: '', key: '', enabled: true });
      saveConfig();
    }
  }
  const saveConfig = () => api('saveSources', cfg).catch(e => toast(errMsg(e), 'err'));
  const enabled = () => cfg.sources.filter(s => s.enabled);
  const sourceById = id => cfg.sources.find(s => s.id === id);
  const ready = s => s.kind === 'reddit' ? true : !s.needsKey || (s.user && s.key); // (Reddit works without an app id, through its public feeds)

  // ---------- the page ----------
  const navBtn = make('button');
  navBtn.dataset.tab = 'discover';
  const navLogo = make('img', 'nt-logo');
  navLogo.alt = '';
  navLogo.onload = () => navBtn.classList.add('has-logo');
  navLogo.onerror = () => navBtn.classList.remove('has-logo');
  navBtn.append(make('span', 'nt-title', 'Discover'), make('span', 'nt-sub', 'Gelbooru & more'), make('span', 'nt-art'), navLogo);
  document.querySelector('nav [data-tab="add"]').after(navBtn);
  const tab = make('section', 'hidden');
  tab.id = 'tab-discover';
  tab.innerHTML = `
    <div class="dc-bar">
      <div class="dc-history"><button class="dc-hbtn dc-back" title="Back" disabled>‹</button><button class="dc-hbtn dc-fwd" title="Forward" disabled>›</button></div>
      <div class="dc-sources"></div>
      <button class="dc-manage" type="button" title="Sources, API keys, tags never to show">Sources</button>
    </div>
    <div class="dc-mast"><div class="dc-mast-id"></div></div>
    <div class="dc-searchrow">
      <button class="dc-subpill hidden" type="button" title="Pick a community"></button>
      <div class="searchbar dc-search"><span class="search-icon"></span><input type="search" spellcheck="false" autocomplete="off"><div class="dc-sugg hidden"></div></div>
      <button class="dc-markbtn" type="button" title="Keep this search as a bookmark">☆ Bookmark</button>
    </div>
    <div class="dc-lens"><div class="dc-views"></div><div class="dc-spans"></div><div class="dc-ratings"></div><div class="dc-size"></div><div class="dc-lens-end"></div></div>
    <div class="dc-rail"></div>
    <div class="dc-subpick hidden"></div>
    <div class="dc-marks"></div>
    <div class="dc-status"></div>
    <div class="dc-grid"></div>
    <div class="dc-more"></div>
    <div class="dc-view hidden">
      <div class="dc-stage">
        <button class="nav-arrow dc-prev" title="Previous (←)">‹</button>
        <button class="nav-arrow dc-next" title="Next (→)">›</button>
        <div class="dc-media"></div>
      </div>
      <aside class="dc-side"></aside>
    </div>`;
  document.querySelector('main').append(tab);
  const q = sel => tab.querySelector(sel);
  // the top of the page stays put while you scroll: it shrinks going down (tabs and search only) and
  // opens fully again on any scroll up
  {
    const head = make('div', 'dc-head'), space = make('div', 'dc-headspace');
    tab.querySelector('.dc-bar').before(space, head);
    head.append(...['.dc-bar', '.dc-mast', '.dc-searchrow', '.dc-lens', '.dc-rail', '.dc-marks'].map(sel => tab.querySelector(sel)));
    // (it floats over the page, which keeps a space as tall as the whole top: shrinking moves nothing below)
    // (measured once it has settled at full size: not while it shrinks or grows back)
    let settle = null;
    new ResizeObserver(() => {
      clearTimeout(settle);
      settle = setTimeout(() => { if (!head.classList.contains('compact')) space.style.height = head.offsetHeight + 'px'; }, 400);
    }).observe(head);
    let lastY = scrollY;
    addEventListener('scroll', () => {
      if (tab.classList.contains('hidden')) return;
      const y = scrollY, d = y - lastY;
      if (Math.abs(d) < 6) return;
      head.classList.toggle('compact', d > 0 && y > 160);
      head.classList.toggle('stuck', y > 4);
      lastY = y;
    }, { passive: true });
  }
  const grid = q('.dc-grid'), status = q('.dc-status'), more = q('.dc-more'), input = q('.dc-search input'), suggBox = q('.dc-sugg');

  navBtn.onclick = () => {
    document.querySelectorAll('nav button').forEach(x => { x.classList.toggle('on', x === navBtn); $('tab-' + x.dataset.tab).classList.toggle('hidden', x !== navBtn); });
    stopGrid();
    try { localStorage.setItem(TAB_KEY, 'discover'); } catch (e) {}
    showTitle();
    openDiscover();
  };

  // ---------- state, and going back and forward ----------
  // A search: source, query (the words typed), levels (the site's levels that are on), view and span (how
  // it's ordered, over what time), picks (one category chip per group), sub (on Reddit, the community).
  let state = null, posts = [], pageNo = 0, loading = false, finished = false, ticket = 0, cursor = null, note = '', emptyPages = 0;
  const back = [], fwd = [];
  const snapshot = () => state && ({ state: { ...state }, posts, pageNo, finished, cursor, scroll: scrollY });
  // what a source opens with: its levels, view and span as you last left them there
  function lensState(src) {
    const kept = (cfg.lens || {})[src.id] || {};
    const all = levelsOf(src), views = viewsOf(src);
    let levels = (kept.levels || []).filter(q => all.some(l => l.q === q));
    if (!levels.length) levels = src.kind === 'bing' ? ['off'] : all.filter(l => !l.r || cfg.ratings.includes(l.r)).map(l => l.q);
    if (!levels.length) levels = all.map(l => l.q);
    const view = views.some(v => v.id === kept.view) ? kept.view : views.length ? views[0].id : '';
    const v = views.find(x => x.id === view);
    const span = !v || !v.spans ? '' : v.spans.includes(kept.span) ? kept.span : v.spans.includes('week') ? 'week' : v.spans[0];
    return { levels, view, span, picks: {}, sub: src.kind === 'reddit' ? (cfg.redditLast !== undefined ? cfg.redditLast : src.sub || 'nsfw') : undefined };
  }
  // any state (also older bookmarks, which had ratings and an order) made whole for its source
  function normal(st) {
    const src = sourceById(st.source);
    if (!src) return { levels: [], view: '', span: '', picks: {}, ...st };
    const base = lensState(src);
    const out = { ...base, ...st, picks: { ...(st.picks || st.web || {}) } };
    if (!st.levels && st.ratings) { const lv = levelsOf(src).filter(l => !l.r || st.ratings.includes(l.r)).map(l => l.q); out.levels = lv.length ? lv : base.levels; }
    if (!st.view && st.sort) {
      out.view = src.kind === 'reddit' ? ({ new: 'new', score: 'top' }[st.sort] || 'hot') : ({ score: 'top', random: 'random' }[st.sort] || viewsOf(src)[0]?.id || '');
      if (!viewsOf(src).some(v => v.id === out.view)) out.view = base.view;
      out.span = viewsOf(src).find(v => v.id === out.view)?.spans ? (out.view === 'top' && src.kind === 'reddit' ? 'all' : base.span || 'week') : '';
    }
    if (src.kind === 'reddit') {
      if (st.sub === undefined) out.sub = src.sub || base.sub;
      const m = /^\/?r\/([A-Za-z0-9_]{2,21})\s*(.*)$/.exec(out.query || ''); // (older searches: "r/name words")
      if (m) { out.sub = m[1]; out.query = m[2].trim(); }
    }
    delete out.ratings; delete out.sort; delete out.web;
    return out;
  }
  function go(next, remember = true) {
    if (remember && state) { back.push(snapshot()); fwd.length = 0; }
    state = normal(next);
    keepLens();
    posts = []; pageNo = 0; finished = false; cursor = null; note = ''; emptyPages = 0; enterSeq = 0;
    ticket++; loading = false; // a request still on its way is for the old search: ignored
    closePicker();
    drawChrome();
    grid.replaceChildren();
    window.scrollTo(0, 0);
    loadMore();
  }
  // how a source was left (levels, view, span; on Reddit, the community and the recent ones), for next time
  function keepLens() {
    const src = sourceById(state.source);
    if (!src) return;
    cfg.lens = cfg.lens || {};
    cfg.lens[src.id] = { levels: state.levels, view: state.view, span: state.span };
    if (src.kind === 'reddit') {
      cfg.redditLast = state.sub || '';
      if (state.sub) cfg.redditRecent = [state.sub, ...(cfg.redditRecent || []).filter(x => x.toLowerCase() !== state.sub.toLowerCase())].slice(0, 12);
    }
    saveConfig();
  }
  function restore(snap) {
    state = snap.state; posts = snap.posts; pageNo = snap.pageNo; finished = snap.finished; cursor = snap.cursor || null;
    ticket++; loading = false;
    closePicker();
    drawChrome(); drawGrid(); setTimeout(() => window.scrollTo(0, snap.scroll), 0);
  }
  q('.dc-back').onclick = () => { if (!back.length) return; fwd.push(snapshot()); restore(back.pop()); };
  q('.dc-fwd').onclick = () => { if (!fwd.length) return; back.push(snapshot()); restore(fwd.pop()); };
  // the source being browsed; on Reddit, the community you're in (its own category and save folder)
  function current(st = state) {
    const src = st && sourceById(st.source);
    if (!src || src.kind !== 'reddit') return src;
    const sub = st.sub || '';
    return { ...src, id: sub ? 'r-' + sub.toLowerCase() : 'reddit', name: sub ? 'r/' + sub : 'Reddit', sub, cat: sub ? catOfSub(sub) : 'real' };
  }
  // the label of a chosen category chip
  function pickLabels(src, picks) {
    const groups = src ? railGroups(src) : [];
    return Object.entries(picks || {}).filter(([, v]) => v).map(([key, v]) => {
      const chip = groups.filter(gr => (gr.key || 'topic') === key).flatMap(gr => gr.chips).find(c => c[1] === v);
      return chip ? chip[0] : v.replace(/_/g, ' ');
    });
  }
  const describe = s => {
    const cur = current(s);
    return [cur ? cur.name : '', s.query, ...pickLabels(sourceById(s.source), s.picks)].filter(Boolean).join(' · ');
  };

  // ---------- the bar: the sources; then the source's own header, search, levels, views and categories ----------
  function drawChrome() {
    const src = sourceById(state.source), cur = current(), site = siteOf(src);
    // the tabs; Reddit's communities share one tab, with a picker in it
    let redditTab = false;
    const shown = enabled().filter(s => s.kind !== 'reddit' || (redditTab ? false : (redditTab = true)));
    q('.dc-sources').replaceChildren(...shown.map(s => {
      const on = s.id === state.source || (s.kind === 'reddit' && src && src.kind === 'reddit');
      const b = make('button', 'dc-src' + (on ? ' on' : '') + (ready(s) ? '' : ' locked'), s.kind === 'reddit' ? 'Reddit' : s.name);
      b.type = 'button';
      b.style.setProperty('--src', siteOf(s).color || 'var(--accent)');
      b.title = !ready(s) ? 'Needs your API key (Sources)' : (s.kind === 'reddit' ? 'Reddit\'s communities: click again to pick one' : s.base) + ' · drag to reorder';
      b.prepend(iconEl(s));
      b.onclick = () => {
        if (!ready(s)) return openSources(s.id);
        if (on && s.kind === 'reddit') return openPicker();
        cfg.active = s.id;
        // the words typed come along between tag sites; categories and levels are each site's own
        const keepWords = src && !/^(reddit|bing)$/.test(src.kind) && !/^(reddit|bing)$/.test(s.kind);
        go({ source: s.id, query: keepWords ? state.query : '', ...lensState(s) });
      };
      // drag a tab onto another to put it there
      b.draggable = true;
      b.addEventListener('dragstart', e => { dragId = s.id; e.dataTransfer.effectAllowed = 'move'; b.classList.add('dragging'); });
      b.addEventListener('dragend', () => { dragId = null; b.classList.remove('dragging'); });
      b.addEventListener('dragover', e => { if (dragId && dragId !== s.id) { e.preventDefault(); b.classList.add('drop'); } });
      b.addEventListener('dragleave', () => b.classList.remove('drop'));
      b.addEventListener('drop', e => {
        e.preventDefault(); b.classList.remove('drop');
        const from = cfg.sources.findIndex(x => x.id === dragId), to = cfg.sources.findIndex(x => x.id === s.id);
        if (from < 0 || to < 0) return;
        cfg.sources.splice(to, 0, cfg.sources.splice(from, 1)[0]);
        saveConfig(); drawChrome();
      });
      return b;
    }));
    tab.style.setProperty('--src', site.color || 'var(--accent)');
    navLogo.src = src ? iconOf(src) : '';
    drawMast(src, site);
    // Reddit: the community you're in; click for any other
    const pill = q('.dc-subpill'), onReddit = !!src && src.kind === 'reddit';
    pill.classList.toggle('hidden', !onReddit);
    if (onReddit) pill.replaceChildren(iconEl(cur), make('b', '', state.sub ? 'r/' + state.sub : 'Popular'), make('span', 'dc-caret', '▾'));
    input.value = state.query;
    input.placeholder = placeholderFor(src, cur);
    drawLevels(src, site);
    drawViews(src);
    drawRail(src);
    drawMarks();
    q('.dc-back').disabled = !back.length; q('.dc-fwd').disabled = !fwd.length;
    q('.dc-back').title = back.length ? 'Back to ' + describe(back[back.length - 1].state) : 'Back';
    q('.dc-fwd').title = fwd.length ? 'Forward to ' + describe(fwd[fwd.length - 1].state) : 'Forward';
    navBtn.querySelector('.nt-sub').textContent = cur ? cur.name : 'Gelbooru & more';
  }
  let dragId = null;
  function placeholderFor(src, cur) {
    if (!src) return 'Search…';
    if (src.kind === 'reddit') return `Search ${cur.sub ? cur.name : 'Reddit'}… words in titles, or r/name to go there`;
    if (src.kind === 'bing') return 'Search the whole web for pictures…';
    if (src.kind === 'danbooru' && !(src.user && src.key)) return `Search ${src.name}… up to 2 tags, like blue_hair smile`;
    if (src.kind === 'e621') return `Search ${src.name}… tags like canine solo`;
    if (src.kind === 'philomena') return `Search ${src.name}… tags like ${src.id === 'derpibooru' ? 'twilight_sparkle' : 'fox'} solo`;
    if (src.kind === 'shimmie') return `Search ${src.name}… names like Overwatch`;
    if (src.kind === 'erome') return 'Search Erome… words like cosplay, redhead, couple';
    if (src.kind === 'redgifs') return state.picks.niche ? 'Clips in this niche (or type words to search all of RedGifs)…' : 'Search RedGifs… words like redhead pov';
    return `Search ${src.name}… tags like blue_hair smile`;
  }
  // the source's header: its icon and colour, what it is, what to know about it
  function drawMast(src, site) {
    const box = q('.dc-mast-id');
    q('.dc-mast').classList.toggle('hidden', !src);
    if (!src) return box.replaceChildren();
    const ico = iconEl(src); ico.classList.add('dc-mast-ico');
    const txt = make('div', 'dc-mast-txt');
    const facts = make('span', 'dc-mast-facts');
    const head = make('div', 'dc-mast-name');
    head.append(make('b', '', src.kind === 'reddit' ? 'Reddit' : src.name), facts);
    txt.append(head, make('span', 'dc-mast-line', site.line || src.help || ''));
    const fact = (text, cls) => facts.append(make('span', 'dc-fact-chip' + (cls ? ' ' + cls : ''), text));
    if (site.only) fact(site.only === 'All explicit' ? 'Everything here is explicit' : 'All-ages site', site.only === 'All explicit' ? 'x' : 'safe');
    for (const f of site.facts || []) {
      if (/account/.test(f) && src.user && src.key) continue;
      if (/app id/.test(f) && cfg.reddit.clientId) continue;
      fact(f);
    }
    if (src.kind === 'reddit' && cfg.reddit.clientId) fact(cfg.reddit.refresh ? 'Logged in' : 'Your app id', 'ok');
    if (src.needsKey && src.key) fact('Your key', 'ok');
    if ((src.kind === 'danbooru' || src.kind === 'e621') && src.user && src.key) fact('Your account', 'ok');
    box.replaceChildren(ico, txt);
    box.classList.toggle('hidden', site.banner === false);
  }
  // the site's own levels: its rating names (several at once), Bing's SafeSearch (one), Reddit's kinds of post
  function drawLevels(src, site) {
    const box = q('.dc-ratings');
    const lv = src ? levelsOf(src) : [];
    box.classList.toggle('hidden', !lv.length);
    if (!lv.length) return box.replaceChildren();
    const one = src.kind === 'bing';
    box.title = one ? 'SafeSearch' : src.kind === 'reddit' ? 'Which posts to show' : `${src.name}'s ratings: pick any`;
    box.replaceChildren(...(one ? [make('span', 'dc-rate-h', 'SafeSearch')] : []), ...lv.map(l => {
      const c = make('button', 'dc-rate r-' + (l.r || l.q) + (state.levels.includes(l.q) ? ' on' : ''), l.label);
      c.type = 'button';
      c.onclick = () => {
        const on = one ? [l.q] : state.levels.includes(l.q) ? state.levels.filter(x => x !== l.q) : [...state.levels, l.q];
        if (!on.length || (one && state.levels[0] === l.q)) return;
        if (!one && !/^(reddit)$/.test(src.kind)) cfg.ratings = [...new Set(levelsOf(src).filter(x => on.includes(x.q)).map(x => x.r))];
        go({ ...state, levels: on });
      };
      return c;
    }));
  }
  // the site's ways of looking (Latest, Hot, Popular today...) and, for some, over what time
  function drawViews(src) {
    const views = src ? viewsOf(src) : [];
    q('.dc-lens').classList.toggle('hidden', !src);
    // the site's ways of looking and their time spans, together in one Sort list
    q('.dc-spans').replaceChildren();
    const v = views.find(x => x.id === state.view);
    if (views.length) {
      const btn = make('button', 'dc-sortbtn'); btn.type = 'button'; btn.title = 'How to sort, and over what time';
      btn.append(make('span', 'dc-tool-h', 'Sort'), make('b', '', v ? v.label + (v.spans && state.span ? ' · ' + SPAN_LABEL[state.span] : '') : views[0].label), make('span', 'dc-caret', '▾'));
      btn.onclick = () => sortMenu(btn, views);
      q('.dc-views').replaceChildren(btn);
    } else q('.dc-views').replaceChildren();
    drawSize(src);
    const end = q('.dc-lens-end');
    end.replaceChildren();
    if (src) {
      const tune = make('button', 'dc-ext dc-tune', '⚙ Customise'); tune.type = 'button';
      tune.title = `Your own categories, the tile size and more for ${src.kind === 'reddit' ? 'Reddit' : src.name}`;
      tune.onclick = () => customise(src);
      end.append(tune);
    }
    if (src && src.kind === 'bing') {
      // Google's and DuckDuckGo's image searches no longer let apps in: they open in your browser
      const words = () => enc((state.query || input.value).replace(/_/g, ' '));
      const g1 = make('button', 'dc-ext', 'Google Images ↗'); g1.type = 'button';
      g1.onclick = () => api('openLink', `https://www.google.com/search?tbm=isch&safe=off&q=${words()}`).catch(() => {});
      const d1 = make('button', 'dc-ext', 'DuckDuckGo ↗'); d1.type = 'button';
      d1.onclick = () => api('openLink', `https://duckduckgo.com/?q=${words()}&iax=images&ia=images&kp=-2`).catch(() => {});
      end.append(g1, d1);
    }
    if (src && src.kind === 'danbooru' && !(src.user && src.key) && /^(hot|popular|top|random)$/.test(state.view)) {
      end.append(make('span', 'dc-lens-note', 'Uses one of your 2 tags'));
    }
  }
  // the Sort list: each way of looking, with its time spans right under it
  function sortMenu(btn, views) {
    const old = document.querySelector('.dc-menu');
    if (old) { old.remove(); return; }
    const menu = make('div', 'dc-menu');
    const pick = (view, span) => { close(); if (state.view !== view || state.span !== span) go({ ...state, view, span }); };
    for (const v of views) {
      const span = !v.spans ? '' : v.spans.includes(state.span) ? state.span : v.spans.includes('week') ? 'week' : v.spans[0];
      const b = make('button', 'dc-menu-item' + (state.view === v.id ? ' on' : ''), v.label); b.type = 'button';
      b.onclick = () => pick(v.id, span);
      menu.append(b);
      for (const sp of v.spans || []) {
        const s = make('button', 'dc-menu-item sub' + (state.view === v.id && state.span === sp ? ' on' : ''), SPAN_LABEL[sp]); s.type = 'button';
        s.onclick = () => pick(v.id, sp);
        menu.append(s);
      }
    }
    const r = btn.getBoundingClientRect();
    Object.assign(menu.style, { left: r.left + 'px', top: r.bottom + 6 + 'px' });
    document.body.append(menu);
    const away = e => { if (!menu.contains(e.target) && e.target !== btn && !btn.contains(e.target)) close(); };
    const esc = e => { if (e.key === 'Escape') close(); };
    function close() { menu.remove(); removeEventListener('pointerdown', away, true); removeEventListener('keydown', esc, true); removeEventListener('scroll', close, true); }
    addEventListener('pointerdown', away, true); addEventListener('keydown', esc, true); addEventListener('scroll', close, true);
  }
  // Tile size: right in the toolbar, changes the grid as you drag, kept per source (the same setting as in Customise)
  function drawSize(src) {
    const box = q('.dc-size');
    if (!src) return box.replaceChildren();
    const size = make('input'); Object.assign(size, { type: 'range', min: 140, max: 480, step: 10, value: siteOf(src).row || 260 });
    size.title = 'Tile size';
    let timer = null;
    size.oninput = () => {
      cfg.look = cfg.look || {};
      cfg.look[src.id] = { ...(cfg.look[src.id] || {}), row: +size.value };
      drawGrid();
      clearTimeout(timer); timer = setTimeout(saveConfig, 500);
    };
    box.replaceChildren(make('span', 'dc-tool-h', 'Tile size'), make('span', 'dc-size-ico s', '▣'), size, make('span', 'dc-size-ico l', '▣'));
  }
  // ---------- Customise: a source made yours ----------
  function customise(src) {
    const S = window.vaultSites, look = { ...((cfg.look || {})[src.id] || {}) };
    S.sheet(`Customise ${src.kind === 'reddit' ? 'Reddit' : src.name}`, (body, close) => {
      body.append(make('p', 'dc-sheet-lead', 'Only for this source, and only on this PC. Everything can be changed back here.'));
      const chips = S.area(look.chips || '', '# Moods\nCute = smile blush\nSpicy = nude\n# Artists\nartist_name', 7);
      const siteChips = make('input'); siteChips.type = 'checkbox'; siteChips.checked = look.siteChips !== false;
      const banner = make('input'); banner.type = 'checkbox'; banner.checked = look.banner !== false;
      const size = make('input'); Object.assign(size, { type: 'range', min: 140, max: 480, step: 10, value: look.row || siteOf({ ...src }).row || 260 });
      const sizeOut = make('b', '', size.value + ' px'); size.oninput = () => { sizeOut.textContent = size.value + ' px'; };
      const tickRow = (box, label) => { const l = make('label', 'st-tick'); l.append(box, make('span', '', label)); return l; };
      const sizeRow = make('div', 'st-range'); sizeRow.append(size, sizeOut);
      body.append(S.field('Your own categories', chips, 'Shown as chips above the pictures. "# Name" starts a group; "Label = search words" is a chip (just the words works too).'),
        tickRow(siteChips, `Show ${src.kind === 'reddit' ? 'Reddit' : src.name}'s own categories too`), tickRow(banner, 'Show the header with the site\'s name and notes'),
        S.field('Picture size (row height)', sizeRow));
      const btns = make('div', 'dc-actions st-actions');
      if (src.kind === 'recipe') { const ed = make('button', 'btn', 'Edit the site\'s recipe…'); ed.onclick = () => { close(); editRecipe(src); }; btns.append(ed); }
      const reset = make('button', 'btn', 'Back to how it was'); reset.onclick = () => { delete cfg.look[src.id]; saveConfig(); close(); go({ ...state }, false); };
      const ok = make('button', 'btn primary', 'Done');
      ok.onclick = () => {
        cfg.look = cfg.look || {};
        cfg.look[src.id] = { chips: chips.value.trim(), siteChips: siteChips.checked, banner: banner.checked, row: +size.value };
        saveConfig(); close(); go({ ...state, picks: {} }, false);
      };
      btns.append(reset, ok);
      body.append(btns);
    });
  }
  // a site of your own: new (from Sources) or changed
  async function editRecipe(src) {
    const r = await window.vaultSites.build('discover', src ? { ...src.recipe, name: src.name, base: src.base, id: src.id } : null);
    if (!r) return;
    const entry = { id: src ? src.id : 'r' + r.id, name: r.name, kind: 'recipe', base: r.base, user: '', key: '', enabled: true, custom: true, cat: 'real', recipe: r };
    if (src) Object.assign(src, entry); else cfg.sources.push(entry);
    await saveConfig();
    toast(src ? `${r.name} changed` : `${r.name} added to Discover`, 'ok');
    if (!sheet.classList.contains('hidden')) openSources();
    go({ source: entry.id, query: '', ...lensState(entry) });
  }

  // ---------- categories: chips that suit each site (series, themes, species, scenes, resolution...) ----------
  // web images (Bing): size, type, shape, colour (as swatches), time
  const WEB_OPTS = [
    ['size', 'Size', [['small', 'Small'], ['medium', 'Medium'], ['large', 'Large'], ['wallpaper', 'Wallpaper']]],
    ['type', 'Type', [['photo', 'Photos'], ['clipart', 'Clip art'], ['linedrawing', 'Drawings'], ['animatedgif', 'GIFs'], ['transparent', 'Transparent']]],
    ['layout', 'Shape', [['square', 'Square'], ['wide', 'Wide'], ['tall', 'Tall']]],
    ['color', 'Colour', [['color', 'Colour only'], ['bw', 'Black & white'], ...['Red', 'Orange', 'Yellow', 'Green', 'Teal', 'Blue', 'Purple', 'Pink', 'Brown', 'Black', 'Gray', 'White'].map(c => ['FGcls_' + c.toUpperCase(), c])]],
    ['time', 'Time', [['lt1440', 'Past day'], ['lt10080', 'Past week'], ['lt43200', 'Past month'], ['lt525600', 'Past year']]]
  ];
  const SWATCH = { color: 'conic-gradient(#f2555a, #f5d000, #2fbf4a, #2f7bf6, #8b4fd8, #f2555a)', bw: 'linear-gradient(135deg, #111 50%, #f4f4f4 50%)',
    FGcls_RED: '#e5383b', FGcls_ORANGE: '#f28c28', FGcls_YELLOW: '#f5d000', FGcls_GREEN: '#2fbf4a', FGcls_TEAL: '#1fb5a8', FGcls_BLUE: '#2f7bf6',
    FGcls_PURPLE: '#8b4fd8', FGcls_PINK: '#f06aa5', FGcls_BROWN: '#8b5a2b', FGcls_BLACK: '#151515', FGcls_GRAY: '#8a8a8a', FGcls_WHITE: '#f4f4f4' };
  function railGroups(src) {
    if (src.kind === 'reddit') return redditGroups();
    if (src.kind === 'bing') return WEB_OPTS.map(([key, name, opts]) => ({ name, key, chips: opts.map(([v, l]) => [l, v]), swatch: key === 'color' }));
    return siteOf(src).chips || [];
  }
  // Reddit's rail: the communities you pinned, the ones you were in lately, more like this one
  function redditGroups() {
    const pins = cfg.redditPins || [], here = (state.sub || '').toLowerCase();
    const pinned = s => pins.some(p => p.toLowerCase() === s.toLowerCase());
    const recent = (cfg.redditRecent || []).filter(s => !pinned(s) && s.toLowerCase() !== here).slice(0, 6);
    const cat = state.sub ? catOfSub(state.sub) : 'real';
    const same = REDDIT_SUBS().filter(p => p.cat === cat && p.sub.toLowerCase() !== here && !pinned(p.sub)).map(p => p.sub);
    const chips = list => list.map(s => ['r/' + s, s]);
    return [
      pins.length && { name: 'Pinned', nav: true, chips: chips(pins) },
      recent.length && { name: 'Recent', nav: true, chips: chips(recent) },
      same.length && { name: 'More ' + catName(cat).toLowerCase(), nav: true, chips: chips(same) }
    ].filter(Boolean);
  }
  // today's most-searched tags, from the site itself (Danbooru and its kin)
  const trendCache = new Map();
  async function trending(src) {
    if (trendCache.has(src.base)) return trendCache.get(src.base);
    let chips = [];
    try {
      const d = JSON.parse(await api('webJson', `${src.base}/explore/posts/searches.json`));
      chips = (Array.isArray(d) ? d : []).map(x => Array.isArray(x) ? x[0] : x && x.query).filter(t => t && !/\s/.test(t) && !NEVER.has(t.replace(/^-/, '').toLowerCase()))
        .slice(0, 14).map(t => [t.replace(/_/g, ' ').replace(/\b\w/g, c => c.toUpperCase()), t]);
    } catch (e) { /* no trends: the rail goes without */ }
    trendCache.set(src.base, chips);
    return chips;
  }
  q('.dc-rail').addEventListener('wheel', e => {
    const rail = e.currentTarget;
    if (rail.scrollWidth <= rail.clientWidth || Math.abs(e.deltaX) > Math.abs(e.deltaY)) return;
    e.preventDefault(); rail.scrollLeft += e.deltaY;
  }, { passive: false });
  function drawRail(src) {
    const rail = q('.dc-rail');
    rail.replaceChildren();
    if (!src) return rail.classList.add('hidden');
    const group = gr => {
      const key = gr.key || 'topic';
      const box = make('div', 'dc-rail-g' + (gr.hot ? ' hot' : ''));
      box.append(make('span', 'dc-rail-h', gr.name));
      for (const [label, value, pic] of gr.chips) {
        const on = gr.nav ? (state.sub || '').toLowerCase() === value.toLowerCase() : state.picks[key] === value;
        const c = make('button', 'dc-chip' + (on ? ' on' : '') + (gr.swatch ? ' sw' : '') + (gr.nav ? ' nav' : ''));
        c.type = 'button';
        if (gr.swatch) { const dot = make('i'); dot.style.background = SWATCH[value] || 'var(--panel)'; c.append(dot, make('span', '', label)); }
        else if (pic) { const im = make('img', 'dc-chip-pic'); im.alt = ''; im.loading = 'lazy'; im.src = proxy(pic); im.onerror = () => im.remove(); c.append(im, make('span', '', label)); c.classList.add('pic'); }
        else c.textContent = label;
        c.title = gr.nav ? (on ? `You're in ${label}` : `Go to ${label}`) : on ? 'Click again to take it off' : `Only ${label.toLowerCase()}`;
        c.onclick = () => gr.nav ? openSub(value) : go({ ...state, picks: { ...state.picks, [key]: on ? '' : value } });
        box.append(c);
      }
      return box;
    };
    const groups = railGroups(src);
    rail.append(...groups.map(group));
    const site = siteOf(src), my = state;
    rail.classList.toggle('hidden', !groups.length && !site.trending && !site.niches);
    if (site.trending) trending(src).then(chips => {
      if (state !== my || !chips.length) return;
      rail.prepend(group({ name: 'Trending today', hot: true, chips }));
      rail.classList.remove('hidden');
    });
    if (site.niches) redgifsNiches().then(chips => {
      if (state !== my || !chips.length) return;
      rail.prepend(group({ name: 'Niches', key: 'niche', chips }));
      rail.classList.remove('hidden');
    });
  }

  // ---------- Reddit: the community picker ----------
  const picker = q('.dc-subpick');
  q('.dc-subpill').onclick = () => picker.classList.contains('hidden') ? openPicker() : closePicker();
  function closePicker() { picker.classList.add('hidden'); }
  document.addEventListener('pointerdown', e => { if (!picker.classList.contains('hidden') && !picker.contains(e.target) && !e.target.closest('.dc-subpill, .dc-src')) closePicker(); });
  function openSub(sub) { closePicker(); go({ ...state, sub, query: '', picks: {} }); }
  function openPicker() {
    const src = sourceById(state.source);
    if (!src || src.kind !== 'reddit') return;
    const find = make('input', 'dc-subpick-find');
    Object.assign(find, { type: 'search', placeholder: 'Find a community, or type any r/name', spellcheck: false });
    const list = make('div', 'dc-subpick-list');
    const pinned = s => (cfg.redditPins || []).some(p => p.toLowerCase() === s.toLowerCase());
    const row = (sub, label) => {
      const r = make('div', 'dc-subrow' + ((state.sub || '') .toLowerCase() === sub.toLowerCase() ? ' on' : ''));
      const open = make('button', 'dc-subrow-go');
      open.type = 'button';
      open.append(iconEl({ icon: REDDIT_ICON, cat: sub ? catOfSub(sub) : 'real' }), make('b', '', label || 'r/' + sub));
      open.onclick = () => openSub(sub);
      r.append(open);
      if (sub) {
        const star = make('button', 'dc-subrow-pin' + (pinned(sub) ? ' on' : ''), pinned(sub) ? '★' : '☆');
        star.type = 'button';
        star.title = pinned(sub) ? 'Unpin' : 'Pin it to the bar under the search';
        star.onclick = () => {
          cfg.redditPins = pinned(sub) ? cfg.redditPins.filter(p => p.toLowerCase() !== sub.toLowerCase()) : [...(cfg.redditPins || []), sub];
          saveConfig(); draw(); drawRail(src);
        };
        r.append(star);
      }
      return r;
    };
    const draw = () => {
      const typed = find.value.trim().replace(/^\/?r\//i, ''), t = typed.toLowerCase();
      const match = s => !t || s.toLowerCase().includes(t);
      list.replaceChildren();
      const section = (title, icon, subs) => {
        if (!subs.length) return;
        const h = make('div', 'dc-subpick-h');
        const i = make('span', 'dc-cat-ico'); i.innerHTML = icon; h.append(i, make('span', '', title));
        list.append(h, ...subs.map(s => row(s)));
      };
      const known = REDDIT_SUBS().map(p => p.sub.toLowerCase());
      if (/^[A-Za-z0-9_]{2,21}$/.test(typed) && !known.includes(t)) list.append(row(typed, `Go to r/${typed}`));
      if (!t) list.append(row('', 'Popular: all of Reddit'));
      section('Pinned', '★', (cfg.redditPins || []).filter(match));
      section('Recent', '◷', (cfg.redditRecent || []).filter(s => !pinned(s) && match(s)).slice(0, 8));
      for (const [id, name, icon] of CATEGORIES) section(name, icon, REDDIT_SUBS().filter(p => p.cat === id && !pinned(p.sub) && match(p.sub)).map(p => p.sub));
      if (!list.children.length) list.append(make('div', 'dc-subpick-none', 'Community names have 2–21 letters, digits or _.'));
    };
    find.oninput = draw;
    find.onkeydown = e => {
      if (e.key === 'Escape') return closePicker();
      if (e.key !== 'Enter') return;
      const v = find.value.trim().replace(/^\/?r\//i, '');
      const hit = REDDIT_SUBS().find(p => p.sub.toLowerCase() === v.toLowerCase()) || REDDIT_SUBS().find(p => p.sub.toLowerCase().includes(v.toLowerCase()));
      if (/^[A-Za-z0-9_]{2,21}$/.test(v)) openSub(hit && hit.sub.toLowerCase() === v.toLowerCase() ? hit.sub : v);
    };
    picker.replaceChildren(find, list);
    draw();
    const at = q('.dc-subpill').getBoundingClientRect(); // (just under the community's button)
    picker.style.top = Math.round(at.bottom + 6) + 'px'; picker.style.left = Math.round(at.left) + 'px';
    picker.classList.remove('hidden');
    find.focus();
  }

  // bookmarks: searches kept to come back to
  function drawMarks() {
    const box = q('.dc-marks');
    box.replaceChildren(...cfg.bookmarks.map((m, k) => {
      const chip = make('span', 'dc-mark');
      const go2 = make('button', 'dc-mark-go');
      go2.type = 'button';
      const s = sourceById(m.state.source);
      if (s) go2.append(iconEl(s));
      go2.append(document.createTextNode(m.name));
      go2.title = describe(m.state);
      go2.onclick = () => { if (s && !s.enabled) { s.enabled = true; saveConfig(); } go({ ...m.state }); };
      const x = make('button', 'dc-mark-x', '×'); x.type = 'button'; x.title = 'Remove this bookmark';
      x.onclick = () => { cfg.bookmarks.splice(k, 1); saveConfig(); drawMarks(); };
      chip.append(go2, x);
      return chip;
    }));
    box.classList.toggle('hidden', !cfg.bookmarks.length);
  }
  q('.dc-markbtn').onclick = () => {
    const name = prompt('Name this bookmark', describe(state));
    if (!name || !name.trim()) return;
    cfg.bookmarks.push({ name: name.trim().slice(0, 40), state: { source: state.source, query: state.query, levels: [...state.levels], view: state.view, span: state.span, picks: { ...state.picks }, sub: state.sub } });
    saveConfig(); drawMarks();
    toast('Bookmarked', 'ok');
  };
  const cleanQuery = text => text.trim().split(/\s+/).filter(t => t && !NEVER.has(t.replace(/^[-~]/, '').toLowerCase())).join(' ');
  function search() {
    hideSugg();
    const src = sourceById(state.source);
    // on Reddit, "r/name" (and words after it) goes to that community
    const m = src && src.kind === 'reddit' && /^\/?r\/([A-Za-z0-9_]{2,21})\s*(.*)$/.exec(input.value.trim());
    if (m) return go({ ...state, sub: m[1], query: cleanQuery(m[2]), picks: {} });
    go({ ...state, query: cleanQuery(input.value) });
  }

  // the site's own tag suggestions for the word being typed
  let suggTimer = null, suggRows = [], suggAt = -1, suggTicket = 0;
  input.addEventListener('input', () => { clearTimeout(suggTimer); suggTimer = setTimeout(suggest, 180); });
  input.addEventListener('keydown', e => {
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { if (!suggRows.length) return; e.preventDefault(); suggAt = (suggAt + (e.key === 'ArrowDown' ? 1 : -1) + suggRows.length) % suggRows.length; drawSugg(); }
    else if (e.key === 'Tab' && suggRows.length) { e.preventDefault(); pick(suggRows[Math.max(0, suggAt)]); }
    else if (e.key === 'Enter') { e.preventDefault(); if (suggAt >= 0 && suggRows.length) pick(suggRows[suggAt]); else search(); }
    else if (e.key === 'Escape') hideSugg();
  });
  input.addEventListener('blur', () => setTimeout(hideSugg, 120));
  async function suggest() {
    const m = /(^|\s)(-?)([^\s-][^\s]*)$/.exec(input.value);
    const term = m ? m[3] : '';
    if (term.length < 2) return hideSugg();
    const src = sourceById(state.source), kind = KINDS[src.kind], my = ++suggTicket;
    if (!kind.suggest) return hideSugg();
    for (const url of kind.suggest(src, term)) {
      try {
        const rows = kind.suggestions(JSON.parse(await api('webJson', url))).filter(r => r.raw && !NEVER.has(r.raw));
        if (my !== suggTicket) return;
        suggRows = rows.slice(0, 10); suggAt = -1; drawSugg();
        return;
      } catch (e) { /* the next way of asking, if any */ }
    }
    hideSugg();
  }
  function drawSugg() {
    suggBox.replaceChildren(...suggRows.map((r, k) => {
      const row = make('div', 'tsuggest-row t-' + r.type + (k === suggAt ? ' on' : ''));
      row.append(make('span', 'tdot'), make('span', 'tsuggest-name', r.raw.replace(/_/g, ' ')), make('span', 'tsuggest-type', r.type === 'general' ? '' : r.type), make('span', 'tsuggest-count', r.count ? fmtN(r.count) : ''));
      row.onmousedown = e => { e.preventDefault(); pick(r); };
      return row;
    }));
    suggBox.classList.toggle('hidden', !suggRows.length);
  }
  function hideSugg() { suggRows = []; suggAt = -1; suggBox.classList.add('hidden'); }
  function pick(r) {
    input.value = input.value.replace(/(^|\s)(-?)[^\s]*$/, (m0, sp, neg) => sp + neg + r.raw) + ' ';
    hideSugg(); input.focus();
  }

  // ---------- loading posts ----------
  const blacklist = () => new Set(split(cfg.blacklist).map(t => t.toLowerCase()));
  let retried = false;
  async function loadMore() {
    if (loading || finished || !state || tab.classList.contains('hidden')) return; // paused while another tab shows
    const src = sourceById(state.source);
    if (!src || !ready(src)) { status.replaceChildren(needKeyNote(src)); return; }
    const kind = KINDS[src.kind], cur = current(), my = ticket = ticket + 1;
    loading = true;
    more.textContent = pageNo ? 'Loading more…' : '';
    if (!pageNo) status.textContent = `Looking on ${cur.name}…`;
    try {
      let raw, last;
      if (kind.fetch) {
        const own = Object.entries(state.picks || {}).filter(([k, v]) => /^own/.test(k) && v).map(([, v]) => v);
        const r = await kind.fetch(cur, own.length ? { ...state, query: [state.query, ...own].filter(Boolean).join(' ') } : state, pageNo + 1, cursor);
        if (my !== ticket) return;
        raw = r.items; cursor = r.next; last = !r.next || !raw.length; note = r.note || '';
      } else {
        // the words typed, the categories picked, the levels that are on, the view: in the site's own search words
        const query = [state.query, ...Object.values(state.picks || {}).filter(Boolean), ...levelTags(src, state.levels), ...viewTags(src, state.view, state.span)].filter(Boolean).join(' ');
        const text = await api('webJson', kind.url(src, query, pageNo + 1));
        if (my !== ticket) return;
        raw = !String(text).trim() ? [] : kind.list(kind.parse ? kind.parse(text) : JSON.parse(text)); // (some answer nothing at all for no results)
        last = raw.length < (kind.page || PAGE);
      }
      const hide = blacklist();
      const fresh = raw.map(p => kind.map(p, cur)).filter(Boolean)
        .filter(p => !p.tags.some(t => NEVER.has(nameOf(t)) || hide.has(nameOf(t)) || hide.has(t)))
        .filter(p => !p.title || !NEVER_TEXT.test(p.title))
        .filter(p => src.kind !== 'reddit' || state.levels.includes(/^(gif|mp4|webm)$/.test(p.ext) ? 'motion' : 'pics')); // (Reddit: pictures, GIFs & videos)
      pageNo++; retried = false;
      finished = last || (state.view === 'random' && pageNo >= 5 && !kind.fetch);
      posts.push(...fresh);
      drawGrid();
      // a page that was all filtered out: the next one (5 at most in a row)
      emptyPages = fresh.length ? 0 : emptyPages + 1;
      if (emptyPages >= 5) { finished = true; drawGrid(); }
      else if (!fresh.length && !finished) setTimeout(() => { if (my === ticket) loadMore(); }, 0);
      // a short page leaves the end in reach, and the observer only speaks when that changes: asked again
      else if (!finished) setTimeout(() => { if (my === ticket) { moreObserver.unobserve(more); moreObserver.observe(more); } }, 0);
    } catch (e) {
      if (my !== ticket) return;
      // Danbooru answers 500 QueryCanceled (and others 502/503/504) when a search takes too long:
      // try once more after a moment before saying anything
      if (!retried && /^(500|502|503|504)\b|QueryCanceled|timed? ?out|Couldn't reach/i.test(errMsg(e))) {
        retried = true; loading = false;
        status.textContent = `${cur.name} is slow right now, trying again…`;
        setTimeout(() => { if (my === ticket) loadMore(); }, 3000);
        return;
      }
      finished = true;
      status.replaceChildren(errorNote(cur, errMsg(e)));
      more.textContent = '';
    } finally {
      if (my === ticket) loading = false;
    }
  }
  function errorNote(src, msg) {
    const box = make('div', 'dc-note err');
    // (a site's error page is HTML: never shown as it is)
    let text = `${src.name} said: ${msg.replace(/<[^>]*>/g, ' ').replace(/\s+/g, ' ').trim().slice(0, 160)}`;
    if (/QueryCanceled|timed? ?out|^\s*504/i.test(msg)) text = `${src.name} took too long on this search. Try fewer or more common tags, or try again in a minute.`;
    else if (/^\s*50[0-3]\b|<html|<!doctype|downbooru|maintenance/i.test(msg)) text = `${src.name} is down or too busy right now (its own servers, not the app). Try again in a few minutes, or pick another source.`;
    if (/^401|^403|authentication|api key/i.test(msg)) text = `${src.name} wants your API key.`;
    if (/more than 2 tags|tag limit/i.test(msg)) text = `${src.name} allows 2 tags per search without an account, and a chosen category or Hot, Popular, Top and Random each count as one. Use fewer tags or Latest, or add your API key.`;
    box.append(make('span', '', text));
    if (/key|2 tags|401|403/i.test(text)) { const b = make('button', 'btn', 'Add the key'); b.onclick = () => openSources(src.id); box.append(b); }
    return box;
  }
  function needKeyNote(src) {
    const box = make('div', 'dc-note');
    box.append(make('span', '', src ? `${src.name} needs your API key before it can show anything.` : 'Pick a source.'));
    const b = make('button', 'btn primary', 'Add the key');
    b.onclick = () => openSources(src && src.id);
    box.append(b);
    return box;
  }
  const moreObserver = new IntersectionObserver(es => { if (es[0].isIntersecting) loadMore(); }, { rootMargin: '1400px' });
  moreObserver.observe(more);

  // ---------- the grid: edge to edge, whole pictures in rows ----------
  let saved = new Set();
  const pending = new Set(); // saves on their way into the vault
  const refreshSaved = () => { saved = new Set((library || []).map(i => i.source).filter(Boolean)); pending.forEach(u => saved.add(u)); };
  function drawGrid() {
    const src = sourceById(state.source);
    status.replaceChildren();
    const line = make('span', '', `${describe(state)} · ${fmtN(posts.length)}${finished ? '' : '+'} shown`);
    const calm = make('span', 'dc-calm', 'No ads · no popups · read straight from the API');
    status.append(line, calm);
    if (!posts.length) {
      grid.replaceChildren(make('div', 'empty', finished ? (note || 'Nothing found. Try other tags or ratings.') : ''));
      more.textContent = '';
      return;
    }
    const width = grid.clientWidth || 900;
    const rowH = siteOf(src).row || Math.min(320, Math.max(180, width / 4.5)); // (wallpapers bigger, web thumbnails smaller)
    const rows = justifyRows(posts.map(p => ({ ...p, width: p.width || 300, height: p.height || 400, post: p })), width, rowH);
    grid.replaceChildren(...rows.map(row => {
      const div = make('div', 'jrow');
      for (const { item, ar } of row.cells) {
        const tile = tileFor(item.post);
        tile.style.flex = row.last ? `0 1 ${ar * row.h}px` : `${ar * 100} 1 0`;
        tile.style.aspectRatio = ar;
        div.append(tile);
      }
      return div;
    }));
    more.textContent = finished ? (posts.length ? 'That\'s all.' : '') : '';
  }
  let enterSeq = 0;
  function tileFor(p) {
    const t = make('button', 'dc-tile' + (saved.has(p.page) ? ' saved' : ''));
    // a tile rises in the first time it shows (one after another), not on every redraw
    if (!p._shown) { p._shown = true; t.classList.add('enter'); t.style.setProperty('--k', enterSeq++ % 24); }
    t.type = 'button';
    const img = make('img');
    img.loading = 'lazy'; img.decoding = 'async'; img.alt = ''; img.src = proxy(p.preview);
    img.onload = () => {
      img.classList.add('in');
      if (!p.width && img.naturalWidth) { p.width = img.naturalWidth; p.height = img.naturalHeight; reshape(); }
    };
    t.append(img);
    if (p.clip) {
      let timer = null, vid = null;
      t.addEventListener('pointerenter', () => {
        timer = setTimeout(() => {
          vid = make('video', 'dc-tile-clip');
          Object.assign(vid, { muted: true, loop: true, autoplay: true, playsInline: true, src: p.clip });
          vid.addEventListener('playing', () => vid.classList.add('in'), { once: true });
          img.after(vid);
        }, 220);
      });
      t.addEventListener('pointerleave', () => { clearTimeout(timer); if (vid) { vid.remove(); vid = null; } });
    }
    if (/^(webm|mp4|gif)$/.test(p.ext)) t.append(make('span', 'tag badge', p.ext === 'gif' ? 'GIF' : p.duration ? '▶ ' + Math.round(p.duration) + ' s' : '▶'));
    if (p.gallery && p.gallery.length > 1) t.append(make('span', 'tag badge dc-gal', `▣ ${p.gallery.length}`));
    else if (p.count > 1) t.append(make('span', 'tag badge dc-gal', `▣ ${p.count}`));
    if (p.title) t.title = p.title;
    t.append(captionFor(p));
    const heart = make('span', 'dc-save', '♥');
    heart.title = 'Save to your vault';
    heart.onclick = e => { e.stopPropagation(); savePost(p).catch(err => toast(errMsg(err), 'err')); };
    if (pending.has(p.page)) t.classList.add('pending');
    const badge = make('span', 'dc-saved');
    badge.append(make('i', 'dc-spin'), make('b', 'dc-check', '✓'), make('span', '', 'Added'));
    t.append(heart, badge);
    t.onclick = () => openView(p);
    p._tile = t;
    return t;
  }
  // what a tile says on hover: a Reddit post's title and community, a web picture's site and size,
  // a booru post's score and rating
  function captionFor(p) {
    const cap = make('span', 'dc-cap');
    const kind = (sourceById(state.source) || {}).kind;
    if (kind === 'reddit') cap.append(make('b', '', p.title || ''), make('span', '', p.sub ? 'r/' + p.sub : ''));
    else if (kind === 'redgifs') cap.append(make('b', '', p.title || ''), make('span', '', p.user ? '@' + p.user : ''), make('span', '', p.score ? `♥ ${fmtN(p.score)}` : ''));
    else if (kind === 'bing') cap.append(make('b', '', (p.page || '').replace(/^https?:\/\/(www\.)?/, '').split('/')[0]), make('span', '', p.width ? `${p.width} × ${p.height}` : ''));
    else {
      const r = make('span', 'dc-cap-r r-' + p.rating); r.title = RATING_LABEL[p.rating] || '';
      cap.append(r, make('span', '', p.score ? `▲ ${fmtN(p.score)}` : ''), make('span', '', p.width ? `${p.width} × ${p.height}` : ''));
    }
    return cap;
  }
  let reshapeTimer = null;
  const reshape = () => { clearTimeout(reshapeTimer); reshapeTimer = setTimeout(() => { const y = scrollY; drawGrid(); scrollTo(0, y); }, 700); };
  let gridWidth = 0;
  window.addEventListener('resize', () => { if (!tab.classList.contains('hidden') && posts.length && Math.abs(grid.clientWidth - gridWidth) > 30) { gridWidth = grid.clientWidth; drawGrid(); } });


  // ---------- asking before anything goes into the vault ----------
  // Every save from Discover, Videos and Audio asks for the main category, the NSFW level and how good it
  // is. Suggestions show (the categories your similar items are in, the level the site's rating hints at),
  // but none is filled in for you: only the tags and the source are.
  const GUESS_CAT = { anime: 'Anime', hentai: 'Hentai', real: 'Real', cosplay: 'Cosplay', femboy: 'Femboys', trans: 'Trans', men: 'Men', lesbian: 'Lesbian',
    furry: 'Furry', pony: 'Pony', feet: 'Feet', kink: 'Kink', hypno: 'Hypno', monster: 'Monster girls', art: 'Art' };
  const LEVEL_HINT = { general: 2, sensitive: 4, questionable: 7, explicit: 9 };
  let lastAnswer = null;
  // (a category named after a site or a community isn't a suggestion: categories are about what's in it)
  const sourceNamed = c => /^r-|^r\//i.test(c) || CATALOG.some(x => x.name.toLowerCase() === c.toLowerCase() || x.id === c.toLowerCase());
  function suggestCategories(tags, srcCat) {
    const want = new Set(tags.filter(t => !/^(meta|artist):/.test(t)));
    const score = new Map();
    if (want.size) for (const i of library || []) {
      let n = 0;
      for (const t of vaultUI.tagsOf(i)) if (want.has(t)) n++;
      if (n && i.category) score.set(i.category, (score.get(i.category) || 0) + n);
    }
    const out = [...score.entries()].sort((a, b) => b[1] - a[1]).map(e => e[0]).filter(c => !sourceNamed(c)).slice(0, 4);
    const guess = GUESS_CAT[srcCat];
    if (guess && !out.some(c => c.toLowerCase() === guess.toLowerCase())) out.push(guess);
    if (lastAnswer && lastAnswer.category && !out.includes(lastAnswer.category)) out.unshift(lastAnswer.category);
    return out.slice(0, 6);
  }
  // o: { title, thumb, source, tags, rating, cat, audio: [the audio categories] } → the answers, or null
  function ask(o) {
    return new Promise(done => {
      const back = make('div', 'ask-back');
      const card = make('div', 'ask-card');
      back.append(card);
      const audio = !!o.audio;
      const ans = { category: '', nsfw: 0, quality: 0 };
      // the item: its picture, title and source
      const head = make('div', 'ask-head');
      const pic = make('div', 'ask-pic');
      if (o.thumb) { const im = make('img'); im.alt = ''; im.src = o.thumb; pic.append(im); }
      const who = make('div', 'ask-who');
      who.append(make('b', '', audio ? 'Add to your audio' : 'Add to your vault'), make('span', 'ask-title', o.title || ''));
      if (o.source) {
        const src = make('button', 'linkish ask-src', 'Source: ' + o.source.replace(/^https?:\/\/(www\.)?/, '').slice(0, 70));
        src.type = 'button'; src.title = o.source;
        src.onclick = () => api('openLink', o.source).catch(() => {});
        who.append(src);
      }
      head.append(pic, who);
      card.append(head);
      const section = (label, el, extra) => { const box = make('div', 'ask-row'); box.append(make('label', 'ask-label', label), el); if (extra) box.append(extra); card.append(box); return box; };
      // the main category: typed or picked, with suggestions
      const cat = make('input', 'ask-cat'); Object.assign(cat, { type: 'text', placeholder: audio ? 'JOI, Hypno, ASMR…' : 'Type one, or pick a suggestion' }); cat.setAttribute('list', 'catList');
      const sugg = make('div', 'ask-sugg');
      const ideas = audio ? o.audio : suggestCategories(o.tags || [], o.cat);
      if (ideas.length) sugg.append(make('span', 'ask-hint', audio ? 'Categories' : 'Suggested'));
      for (const c of ideas) {
        const b = make('button', 'ask-chip', c); b.type = 'button';
        if (lastAnswer && c === lastAnswer.category && !audio) b.title = 'The one you used last';
        b.onclick = () => { cat.value = c; cat.dispatchEvent(new Event('input')); };
        sugg.append(b);
      }
      cat.addEventListener('input', () => { ans.category = cat.value.trim(); sugg.querySelectorAll('.ask-chip').forEach(b => b.classList.toggle('on', b.textContent === ans.category)); check(); });
      section('Main category', cat, sugg);
      // the NSFW level: ten faces; the one the site's rating hints at is marked, not chosen
      let levels = null, stars = null;
      if (!audio) {
        const F = window.vaultFaces;
        levels = make('div', 'ask-levels');
        const hint = LEVEL_HINT[o.rating];
        for (let n = 1; n <= 10; n++) {
          const b = make('button', 'ask-lv' + (n === hint ? ' suggested' : ''));
          b.type = 'button'; b.dataset.n = n;
          b.style.setProperty('--h', n);
          if (F) { const f = make('span', 'ask-face'); f.innerHTML = F.faceFor ? F.faceFor(n) : F.face(F.tierOf(n)); b.append(f); }
          b.append(make('b', '', String(n)));
          b.title = `NSFW ${n}` + (F ? ': ' + (F.levelName ? F.levelName(n) : F.TIERS[F.tierOf(n)][2]) : '') + (n === hint ? ' (what the site\'s rating suggests)' : '');
          b.onclick = () => pickLevel(n);
          levels.append(b);
        }
        section('NSFW level', levels, hint ? make('span', 'ask-hint small', `The dotted one is what ${o.siteName || 'the site'}'s rating suggests`) : null);
        stars = make('div', 'ask-stars');
        [[1, '★', 'okay'], [2, '★★', 'good'], [3, '★★★', 'great']].forEach(([q, st, word]) => {
          const b = make('button', 'ask-star q' + q); b.type = 'button'; b.dataset.q = q;
          b.append(make('span', 'ask-star-s', st), make('small', '', word));
          b.onclick = () => pickStars(q);
          stars.append(b);
        });
        section('How good is it?', stars);
      }
      function pickLevel(n) { ans.nsfw = n; levels.querySelectorAll('.ask-lv').forEach(b => b.classList.toggle('on', +b.dataset.n === n)); check(); }
      function pickStars(q) { ans.quality = q; stars.querySelectorAll('.ask-star').forEach(b => b.classList.toggle('on', +b.dataset.q === q)); check(); }
      // the tags (filled in from the site) and a collection
      const box = vaultUI.tagBox({ placeholder: 'Tags…' });
      box.set(o.tags || []);
      section('Tags', box.el);
      let col = null;
      if (!audio) { col = make('input'); Object.assign(col, { type: 'text', placeholder: 'Optional: groups items into one card' }); col.setAttribute('list', 'colList'); section('Collection', col); }
      // the buttons
      const foot = make('div', 'ask-foot');
      const cancel = make('button', 'btn', 'Cancel'); cancel.type = 'button';
      const go2 = make('button', 'btn primary ask-go', audio ? '♥ Add to audio' : '♥ Add to vault'); go2.type = 'button';
      const need = make('span', 'ask-need');
      foot.append(need, cancel, go2);
      card.append(foot);
      function check() {
        const missing = [!ans.category && 'a category', !audio && !ans.nsfw && 'an NSFW level', !audio && !ans.quality && 'stars'].filter(Boolean);
        go2.disabled = !!missing.length;
        need.textContent = missing.length ? 'Pick ' + missing.join(', ').replace(/, ([^,]*)$/, ' and $1') : '';
      }
      check();
      const finish = value => { document.removeEventListener('keydown', keys, true); back.classList.add('out'); setTimeout(() => back.remove(), 160); done(value); };
      cancel.onclick = () => finish(null);
      back.addEventListener('pointerdown', e => { if (e.target === back) finish(null); });
      go2.onclick = () => {
        if (go2.disabled) return;
        box.flush();
        const a = { category: ans.category, nsfw: ans.nsfw, quality: ans.quality, tags: box.get(), collection: col ? col.value.trim() : '' };
        if (!audio) lastAnswer = a;
        finish(a);
      };
      // keys: 1–9 and 0 the level, Q W E the stars, Enter adds, Esc cancels
      function keys(e) {
        const typing = /INPUT|TEXTAREA|SELECT/.test(e.target.tagName);
        if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); return finish(null); }
        if (e.key === 'Enter' && !(e.target.closest && e.target.closest('.tag-box') && e.target.value)) { e.preventDefault(); e.stopPropagation(); return go2.click(); }
        if (!back.contains(e.target)) e.stopPropagation();
        if (typing || audio) return;
        if (/^[0-9]$/.test(e.key)) { e.preventDefault(); e.stopPropagation(); pickLevel(e.key === '0' ? 10 : +e.key); }
        else if (/^[qwe]$/i.test(e.key)) { e.preventDefault(); e.stopPropagation(); pickStars({ q: 1, w: 2, e: 3 }[e.key.toLowerCase()]); }
      }
      document.addEventListener('keydown', keys, true);
      back.addEventListener('keydown', e => e.stopPropagation()); // (nothing behind the dialog hears its keys)
      document.body.append(back);
      // (the dialog itself has the focus, so 1–0 and Q W E work at once; the category list opens when clicked)
      card.tabIndex = -1;
      setTimeout(() => card.focus({ preventScroll: true }), 30);
    });
  }
  // "Imported!": a note at the bottom right when something you saved arrives and it isn't on screen any more
  const importStack = make('div', 'imp-stack');
  document.body.append(importStack);
  function imported({ thumb, text, sub }) {
    const n = make('div', 'imp-note');
    if (thumb) { const im = make('img'); im.alt = ''; im.src = thumb; n.append(im); }
    const t = make('div', 'imp-text'); t.append(make('b', '', text || 'Imported!'), make('span', '', sub || '')); n.append(t, make('span', 'imp-tick', '✓'));
    importStack.append(n);
    setTimeout(() => vaultUI.burst(n.querySelector('.imp-tick') || n, 12), 250);
    setTimeout(() => n.classList.add('out'), 3600);
    setTimeout(() => n.remove(), 4100);
  }
  // a big file (a video, an hour of audio) downloaded to this PC first, then read as one blob: the
  // picture proxy gives up after 40 s, this doesn't. onProgress({ done, total }).
  async function downloadBlob(url, onProgress) {
    const id = await api('downloadStart', url);
    try {
      for (;;) {
        const p = await api('downloadProgress', id);
        if (p.state === 'done') break;
        if (p.state) throw new Error(p.state);
        if (onProgress) onProgress(p);
        await new Promise(r => setTimeout(r, 500));
      }
      return await (await fetch('http://vault.localhost/dl/' + id)).blob();
    } finally { api('downloadDrop', id).catch(() => {}); }
  }
  // the little round spinner that becomes a tick
  const spinTick = () => '<span class="sp-tick"><svg viewBox="0 0 24 24"><circle class="sp-c" cx="12" cy="12" r="9"/><path class="sp-t" d="M7 12.5l3.2 3.2L17 9"/></svg></span>';

  // ---------- saving into the vault ----------
  const saving = new Set();
  // a category is a folder, so it can't hold / or \: r/gonewild's posts go into "r-gonewild"
  const safeCategory = c => c.replace(/\s*[\\/]+\s*/g, '-').trim();
  const categoryFor = src => safeCategory(cfg.categories[src.id] || src.name);
  async function savePost(p) {
    if (saving.has(p.page) || saved.has(p.page)) return;
    const src = current();
    saving.add(p.page);
    let opts;
    try {
      if (p.resolve && !p.file) await p.resolve.call(p); // (an album: its files and tags, before asking)
      opts = await ask({ title: p.title || `${src.name} #${p.id}`, thumb: proxy(p.preview), source: p.page, tags: cleanTags(p.tags.filter(t => !NEVER.has(nameOf(t)))),
        rating: p.rating, cat: src.cat, siteName: src.name });
    } finally { if (!opts) saving.delete(p.page); }
    if (!opts) return;
    const mark = cls => { p._tile && p._tile.classList.toggle(cls, true); if (viewing === p) drawSide(p); };
    saved.add(p.page); pending.add(p.page);
    mark('saved'); mark('pending');
    try {
      if (p.resolve) await p.resolve.call(p);
      const urls = p.gallery && p.gallery.length ? p.gallery : [p.file];
      const category = safeCategory(opts.category);
      const tags = cleanTags(opts.tags).join(' ');
      let job = null;
      for (let k = 0; k < urls.length; k++) {
        const ext = extOf(urls[k]) || (k ? 'jpg' : p.ext) || 'jpg'; // (each file's own: an album mixes pictures and videos)
        let blob;
        if (/^(mp4|webm)$/.test(ext)) blob = await downloadBlob(urls[k]);
        else {
          const res = await fetch(proxy(urls[k]));
          if (!res.ok) throw new Error(`Couldn't download it (${res.status})`);
          blob = await res.blob();
        }
        const type = blob.type && blob.type !== 'application/octet-stream' ? blob.type : MIME[ext] || 'image/jpeg';
        const file = new File([blob], `${src.id}-${p.id}${urls.length > 1 ? '-' + (k + 1) : ''}.${ext}`, { type });
        // a gallery's pictures go into one collection, named after the post
        const collection = opts.collection || (urls.length > 1 ? (p.title || `${src.name} ${p.id}`).replace(/[\\/:*?"<>|]+/g, ' ').trim().slice(0, 60) : '');
        jobs.push({ file, category, nsfw: opts.nsfw, quality: opts.quality, collection, tags, source: k ? `${p.page}#${k + 1}` : p.page });
        if (!k) job = jobs[jobs.length - 1];
        if (k === 0) pumpUploads();
      }
      pumpUploads();
      addCategory(category);
      // done when the library has it (or the upload failed)
      const until = Date.now() + 10 * 60e3;
      const check = () => {
        if ((library || []).some(i => i.source === p.page)) return settle(true);
        if (failed.includes(job) || Date.now() > until) return settle(false);
        setTimeout(check, 600);
      };
      check();
    } catch (e) {
      settle(false);
      throw e;
    } finally {
      saving.delete(p.page);
    }
    function settle(ok) {
      pending.delete(p.page);
      if (!ok) saved.delete(p.page);
      if (p._tile) { p._tile.classList.remove('pending'); p._tile.classList.toggle('saved', ok); if (ok) p._tile.classList.add('just'); }
      if (viewing === p) { drawSide(p); if (ok) setTimeout(() => vaultUI.burst(q('.dc-save-big'), 18), 30); }
      else if (ok) imported({ thumb: proxy(p.preview), text: 'Imported!', sub: `Into ${safeCategory(opts.category)}` });
    }
  }

  // ---------- a post, large, with its details ----------
  let viewing = null;
  const view = q('.dc-view');
  async function openView(p) {
    if (p.resolve && !p.file) { try { await p.resolve.call(p); } catch (e) { toast(errMsg(e), 'err'); return; } }
    viewing = p;
    p._at = 0;
    view.classList.remove('hidden');
    showMedia(p.file, p.ext, p.preview, p.sample);
    view.style.setProperty('--ambient', `url("${proxy(p.preview)}")`);
    drawSide(p);
    // the ones either side, ready
    const k = posts.indexOf(p);
    [posts[k + 1], posts[k - 1]].filter(Boolean).forEach(n => { if (!/^(webm|mp4)$/.test(n.ext) && n.sample) { const im = new Image(); im.src = proxy(n.sample); } });
    if (k >= posts.length - 6) loadMore();
  }
  function showMedia(file, ext, preview, sample) {
    const media = q('.dc-media');
    media.replaceChildren();
    if (/^(webm|mp4)$/.test(ext)) {
      const v = make('video');
      // (through vault://stream, a piece at a time: the picture proxy sends whole files, which can't be jumped into)
      Object.assign(v, { autoplay: true, loop: true, playsInline: true, crossOrigin: 'anonymous', src: 'http://vault.localhost/stream?u=' + enc(file) });
      media.append(v);
      window.vaultPlayer.attach(v, media);
    } else {
      const small = make('img', 'dc-low'); small.src = proxy(preview); small.alt = '';
      const big = make('img', 'dc-full'); big.alt = ''; big.src = proxy(sample);
      big.onload = () => { big.classList.add('in'); small.remove(); };
      media.append(small, big);
    }
  }
  // one file of the open album
  function showInAlbum(p, k) {
    const url = p.gallery[k], ext = extOf(url) || 'jpg';
    p._at = k;
    showMedia(url, ext, (p.stills && p.stills[k]) || p.preview, url);
    q('.dc-side').querySelectorAll('.dc-album-it').forEach((b, i) => b.classList.toggle('on', i === k));
  }
  const closeView = () => { viewing = null; view.classList.add('hidden'); q('.dc-media').replaceChildren(); };
  const step = d => { if (!viewing) return; const k = posts.indexOf(viewing); const n = posts[k + d]; if (n) openView(n); };
  q('.dc-prev').onclick = () => step(-1);
  q('.dc-next').onclick = () => step(1);
  document.addEventListener('keydown', e => {
    if (!viewing || tab.classList.contains('hidden') || /INPUT|SELECT|TEXTAREA/.test(e.target.tagName)) return;
    if (e.key === 'Escape') closeView();
    else if (e.key === 'ArrowLeft') step(-1);
    else if (e.key === 'ArrowRight') step(1);
    else if (e.key.toLowerCase() === 's') savePost(viewing).catch(err => toast(errMsg(err), 'err'));
  });

  function drawSide(p) {
    const src = current();
    const side = q('.dc-side');
    side.replaceChildren();
    const head = make('div', 'dc-side-head');
    const close = make('button', 'btn dc-close', 'Close');
    close.onclick = closeView;
    head.append(make('div', 'dc-side-title', p.title ? p.title.slice(0, 90) : `${src.name} #${p.id}`), close);
    side.append(head);

    const actions = make('div', 'dc-actions');
    const isSaved = saved.has(p.page), isPending = pending.has(p.page);
    const saveBtn = make('button', 'btn primary dc-save-big' + (isSaved ? ' added' + (isPending ? ' busy' : ' done') : ''));
    saveBtn.innerHTML = isSaved ? `${spinTick()}<span>Added to vault</span>` : '♥ Add to vault';
    saveBtn.disabled = isSaved;
    saveBtn.title = isSaved ? (isPending ? 'Going into your vault…' : 'In your vault') : 'Asks for its category, NSFW level and stars (S)';
    saveBtn.onclick = () => savePost(p).catch(err => toast(errMsg(err), 'err'));
    const siteBtn = make('button', 'btn', 'Open on site ↗');
    siteBtn.onclick = () => api('openLink', p.page).catch(err => toast(errMsg(err), 'err'));
    actions.append(saveBtn, siteBtn);
    side.append(actions);
    if (p.gallery && p.gallery.length > 1) {
      const c = p.counts;
      side.append(make('h4', 'dc-h', `In this album · ${c ? [c.pics && `${c.pics} ${c.pics === 1 ? 'picture' : 'pictures'}`, c.vids && `${c.vids} ${c.vids === 1 ? 'video' : 'videos'}`].filter(Boolean).join(', ') : p.gallery.length}`));
      const strip = make('div', 'dc-album');
      p.gallery.forEach((url, k) => {
        const b = make('button', 'dc-album-it' + (k === (p._at || 0) ? ' on' : '') + (/^(mp4|webm)$/.test(extOf(url)) ? ' vid' : ''));
        b.type = 'button';
        b.title = `${k + 1} of ${p.gallery.length}`;
        const im = make('img'); im.loading = 'lazy'; im.alt = ''; im.src = proxy((p.stills && p.stills[k]) || url); im.onerror = () => im.remove();
        b.append(im);
        b.onclick = () => showInAlbum(p, k);
        strip.append(b);
      });
      side.append(strip);
      side.append(make('p', 'dc-album-note', 'Adding it to your vault saves the whole album as one collection.'));
    }

    const info = make('div', 'dc-info');
    const fact = (k, v) => { if (v === undefined || v === '' || v === null) return; const d = make('div', 'dc-fact'); d.append(make('span', '', k), make('b', '', String(v))); info.append(d); };
    fact('Size', p.width && p.height ? `${p.width} × ${p.height}` : '');
    fact('Type', (p.ext || '').toUpperCase());
    fact('Rating', RATING_LABEL[p.rating]);
    fact('Score', p.score);
    fact('Posted', p.created ? new Date(p.created).toLocaleDateString() : '');
    side.append(info);
    if (p.source && /^https?:\/\//.test(p.source)) {
      const s = make('button', 'linkish dc-source', 'Original source: ' + p.source.replace(/^https?:\/\/(www\.)?/, '').slice(0, 48));
      s.onclick = () => api('openLink', p.source).catch(() => {});
      side.append(s);
    }

    // more like this: the artist, the characters
    const like = p.tags.filter(t => /^(artist|character|series):/.test(t)).slice(0, 4);
    if (like.length) {
      side.append(make('h4', 'dc-h', 'More like this'));
      const wrap = make('div', 'dc-like');
      like.forEach(t => wrap.append(tagChip(t, { onClick: () => { closeView(); go({ ...state, query: nameOf(t) }); } })));
      side.append(wrap);
    }
    // the tags, by type; click one to search for it, + to add it to the search
    const ORDER = ['artist', 'character', 'series', 'general', 'meta'], TITLES = { artist: 'Artists', character: 'Characters', series: 'Series', general: 'Tags', meta: 'Meta' };
    for (const kind of ORDER) {
      const list = p.tags.filter(t => typeOf(t) === kind && !NEVER.has(nameOf(t)));
      if (!list.length) continue;
      side.append(make('h4', 'dc-h t-' + kind, TITLES[kind]));
      const wrap = make('div', 'dc-tags');
      for (const t of list) {
        const chip = tagChip(t, { onClick: () => { closeView(); go({ ...state, query: nameOf(t) }); } });
        const plus = make('button', 'dc-plus', '+');
        plus.title = 'Add to the search';
        plus.onclick = () => { closeView(); go({ ...state, query: cleanQuery(`${state.query} ${nameOf(t)}`) }); };
        chip.append(plus);
        wrap.append(chip);
      }
      foldChips(wrap);
      side.append(wrap);
    }
  }

  // ---------- sources, keys and tags never to show ----------
  const sheet = make('div', 'dc-sheet hidden');
  sheet.innerHTML = '<div class="dc-sheet-card"><div class="dc-sheet-head"><h2>Sources</h2><button class="btn dc-sheet-close" type="button">Done</button></div><div class="dc-sheet-body"></div></div>';
  document.body.append(sheet);
  sheet.querySelector('.dc-sheet-close').onclick = () => { sheet.classList.add('hidden'); saveConfig().then(() => { drawChrome(); if (!posts.length) go({ ...state }, false); }); };
  q('.dc-manage').onclick = () => openSources();
  let sheetCat = 'all', sheetFind = '';
  const originOf = url => String(url || '').split('/').slice(0, 3).join('/');
  // sharp icons: the sites' own big ones (SVG, or 128–512 px) where they have them; where a site only has
  // a 16–32 px favicon, a badge in its colour drawn here instead of a blurry enlargement
  const HI_ICON = {
    danbooru: 'https://danbooru.donmai.us/favicon.svg', 'safebooru-donmai': 'https://safebooru.donmai.us/favicon.svg', aibooru: 'https://aibooru.online/favicon.svg',
    e621: 'https://e621.net/favicon.svg', e926: 'https://e621.net/favicon.svg', furbooru: 'https://furbooru.org/favicon.svg', derpibooru: 'https://derpicdn.net/favicon.svg',
    rule34: 'https://rule34.xxx/apple-touch-icon.png', hypnohub: 'https://hypnohub.net/apple-touch-icon.png', xbooru: 'https://xbooru.com/apple-touch-icon-152x152-precomposed.png',
    reddit: REDDIT_ICON
  };
  const MONO = {
    gelbooru: ['G', '#0773fb'], safebooru: ['S', '#5c9be0'], yandere: ['Y', '#e2707a'], konachan: ['K', '#f08a3c'], 'konachan-com': ['K', '#e0632a'], tbib: ['TB', '#6b7280'],
    paheal: ['34', '#4f8a2e'], bing: ['b', '#0f9d8a'], redgifs: ['RG', '#e42744'], erome: ['E', '#d1355b']
  };
  const badgeSvg = (text, color) => 'data:image/svg+xml,' + enc(`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"><defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff" stop-opacity=".28"/><stop offset=".55" stop-color="#fff" stop-opacity="0"/></linearGradient></defs><rect width="64" height="64" rx="16" fill="${color}"/><rect width="64" height="64" rx="16" fill="url(#g)"/><text x="32" y="${text.length > 1 ? 42 : 45}" font-family="Segoe UI, Arial, sans-serif" font-weight="800" font-size="${text.length > 1 ? 27 : 36}" fill="#fff" text-anchor="middle">${text}</text></svg>`);
  const iconOf = s => {
    if (s.kind === 'reddit') return proxy(REDDIT_ICON);
    if (MONO[s.id]) return badgeSvg(...MONO[s.id]);
    if (HI_ICON[s.id]) return proxy(HI_ICON[s.id]);
    return proxy(s.icon || originOf(s.pageBase || s.base) + '/favicon.ico');
  };
  // a source's little picture: its site's icon, or its category's when the site has none
  function iconEl(s) {
    const img = make('img', 'dc-ico');
    img.alt = ''; img.src = iconOf(s); img.loading = 'lazy';
    img.onerror = () => { const f = make('span', 'dc-ico cat'); f.innerHTML = CAT_ICON[s.cat] || CAT_ICON.web; img.replaceWith(f); };
    return img;
  }
  const catName = id => (CATEGORIES.find(c => c[0] === id) || [, ''])[1];
  function openSources(focusId) {
    const body = sheet.querySelector('.dc-sheet-body');
    body.replaceChildren();
    body.append(make('p', 'dc-sheet-lead', 'Lewd Vault reads these sites\' APIs directly: no ads, no popups, no redirects. Keys and logins stay on this PC.'));

    // Reddit: your own app id, and a login for your home feed
    const rd = make('div', 'dc-srccard dc-reddit' + (cfg.reddit.clientId ? ' on' : ''));
    const rdHead = make('div', 'dc-srctop');
    rdHead.append(iconEl({ icon: REDDIT_ICON, cat: 'real' }), make('b', '', 'Reddit'), make('span', 'dc-srckind', cfg.reddit.refresh ? 'Logged in' : cfg.reddit.clientId ? 'Your app id' : 'Public feeds'));
    const rdHelp = make('p', 'dc-srchelp');
    rdHelp.innerHTML = 'Every r/ source works as it is, through Reddit\'s public feeds: pictures and videos (no galleries, no sound on Reddit videos), about 30 posts a page and one page a minute (Reddit\'s limit, shared with Audio). <b>Your own app id</b> makes it much faster, adds galleries and sound, and <b>Log in</b> adds your home feed. Since November 2025 Reddit approves app ids by request: create one at <button class="linkish rd-apps" type="button">reddit.com/prefs/apps</button> (<b>installed app</b>, redirect uri <code>http://127.0.0.1:65010/reddit</code>), ask Reddit for access, and paste the id here once it\'s approved.';
    rdHelp.querySelector('.rd-apps').onclick = () => api('openLink', 'https://www.reddit.com/prefs/apps').catch(() => {});
    const rdRow = make('div', 'dc-keys');
    const rdId = make('input'); Object.assign(rdId, { type: 'text', value: cfg.reddit.clientId || '', placeholder: 'Your Reddit app\'s client id', spellcheck: false });
    rdId.oninput = () => { if (rdId.value.trim() !== cfg.reddit.clientId) { cfg.reddit = { clientId: rdId.value.trim() }; } };
    const rdLogin = make('button', 'btn', cfg.reddit.refresh ? 'Log out' : 'Log in for your home feed');
    rdLogin.type = 'button';
    rdLogin.onclick = async () => {
      if (cfg.reddit.refresh) { cfg.reddit = { clientId: cfg.reddit.clientId }; await saveConfig(); return openSources(); }
      if (!cfg.reddit.clientId) return toast('Paste your Reddit app\'s client id first.', 'err');
      rdLogin.disabled = true; rdLogin.textContent = 'Allow it in your browser…';
      try {
        const t = await api('redditLogin', cfg.reddit.clientId);
        if (!t.access_token) throw new Error(t.error || 'Reddit didn\'t log you in');
        Object.assign(cfg.reddit, { token: t.access_token, tokenUntil: Date.now() + (t.expires_in || 3600) * 1000, refresh: t.refresh_token || '' });
        await saveConfig();
        toast('Logged in to Reddit', 'ok');
      } catch (e) { toast(errMsg(e), 'err'); }
      openSources();
    };
    rdRow.append(rdId, rdLogin);
    rd.append(rdHead, rdHelp, rdRow);
    body.append(rd);

    // Find the source (reverse search from the Library)
    const sn = make('div', 'dc-srccard' + (cfg.saucenaoKey ? ' on' : ''));
    const snTop = make('div', 'dc-srctop');
    snTop.append(iconEl({ icon: 'https://saucenao.com/favicon.ico', cat: 'web' }), make('b', '', 'Find the source (SauceNAO)'), make('span', 'dc-srckind', cfg.saucenaoKey ? 'Ready' : 'Optional'));
    const snHelp = make('p', 'dc-srchelp');
    snHelp.innerHTML = 'Right-click a picture in the Library → <b>Find the source</b>. Without a key it asks IQDB (anime art); with your free SauceNAO key (<button class="linkish sn-key" type="button">saucenao.com → Account → API</button>) it also finds real photos, pixiv, Twitter and more, 200 searches a day.';
    snHelp.querySelector('.sn-key').onclick = () => api('openLink', 'https://saucenao.com/user.php?page=search-api').catch(() => {});
    const snKey = make('input'); Object.assign(snKey, { type: 'password', value: cfg.saucenaoKey || '', placeholder: 'SauceNAO API key' });
    snKey.oninput = () => { cfg.saucenaoKey = snKey.value.trim(); };
    sn.append(snTop, snHelp, snKey);
    body.append(sn);

    // your sources: on/off, keys, remove; drag the tabs above the grid to reorder them
    body.append(make('h3', 'dc-sheet-h', 'Your sources'));
    for (const s of cfg.sources) {
      const card = make('div', 'dc-srccard' + (s.enabled ? ' on' : ''));
      const top = make('label', 'dc-srctop');
      const tick = make('input'); tick.type = 'checkbox'; tick.checked = s.enabled;
      tick.onchange = () => { s.enabled = tick.checked; card.classList.toggle('on', s.enabled); };
      top.append(tick, iconEl(s), make('b', '', s.name), make('span', 'dc-srcbase', s.kind === 'reddit' ? '' : s.base.replace('https://', '')), make('span', 'dc-srckind', (KINDS[s.kind] || {}).label || s.kind));
      card.append(top);
      if (s.help) card.append(make('p', 'dc-srchelp', s.help));
      if (s.needsKey || /^(danbooru|e621|philomena)$/.test(s.kind) || s.custom) {
        const keys = make('div', 'dc-keys');
        const user = make('input'); Object.assign(user, { type: 'text', value: s.user || '', placeholder: s.userLabel || (s.kind === 'philomena' ? '(not needed)' : 'User ID') });
        const key = make('input'); Object.assign(key, { type: 'password', value: s.key || '', placeholder: 'API key' });
        user.oninput = () => { s.user = user.value.trim(); };
        key.oninput = () => { s.key = key.value.trim(); };
        keys.append(user, key);
        card.append(keys);
        if (s.id === focusId) setTimeout(() => (s.user ? key : user).focus(), 50);
      }
      const rm = make('button', 'linkish', 'Remove');
      rm.onclick = () => { cfg.sources = cfg.sources.filter(x => x !== s); openSources(); };
      if (s.kind === 'recipe') { const ed = make('button', 'linkish', 'Edit its recipe'); ed.onclick = () => editRecipe(s); const row = make('div', 'st-linkrow'); row.append(ed, rm); card.append(row); }
      else card.append(rm);
      body.append(card);
    }

    // the catalogue: more sources, by category
    body.append(make('h3', 'dc-sheet-h', `Add more sources (${CATALOG.length})`));
    const cats = make('div', 'dc-cats');
    for (const [id, name, icon] of [['all', 'All', CAT_ICON.web], ...CATEGORIES]) {
      const c = make('button', 'dc-cat' + (sheetCat === id ? ' on' : ''));
      c.type = 'button';
      const i = make('span', 'dc-cat-ico'); i.innerHTML = icon;
      c.append(i, make('span', '', name));
      c.onclick = () => { sheetCat = id; openSources(); };
      cats.append(c);
    }
    const find = make('input', 'dc-cat-find'); Object.assign(find, { type: 'search', placeholder: 'Find a site or community…', value: sheetFind });
    const list = make('div', 'dc-catalog');
    // the catalogue's rows; Reddit's communities sit together in one group that opens when clicked
    // (or by itself when a search finds some of them), so they don't crowd the sites
    let redditOpen = false;
    const drawList = () => {
      const t = find.value.trim().toLowerCase();
      sheetFind = find.value;
      const shown = CATALOG.filter(p => (sheetCat === 'all' || p.cat === sheetCat) && (!t || (p.name + ' ' + (p.sub || '') + (p.kind === 'reddit' ? ' reddit' : '')).toLowerCase().includes(t)));
      const rowFor = p => {
        const pinnedSub = p.kind === 'reddit' && p.sub;
        const have = pinnedSub ? (cfg.redditPins || []).some(x => x.toLowerCase() === p.sub.toLowerCase()) : cfg.sources.some(s => s.id === p.id);
        const row = make('div', 'dc-catrow' + (have ? ' have' : '') + (pinnedSub ? ' rd' : ''));
        const ci = make('span', 'dc-cat-ico'); ci.innerHTML = CAT_ICON[p.cat] || '';
        const add = make('button', 'btn' + (have ? '' : ' primary'), have ? (pinnedSub ? 'Pinned ★' : 'Added ✓') : pinnedSub ? 'Pin' : 'Add');
        add.type = 'button';
        add.title = pinnedSub ? 'Pin it in the Reddit tab (every community is in its picker)' : '';
        add.onclick = () => {
          if (pinnedSub) {
            cfg.redditPins = have ? cfg.redditPins.filter(x => x.toLowerCase() !== p.sub.toLowerCase()) : [...(cfg.redditPins || []), p.sub];
            const tabSrc = cfg.sources.find(s => s.kind === 'reddit');
            if (!have && !tabSrc) cfg.sources.push({ ...CATALOG.find(x => x.id === 'reddit'), user: '', key: '', enabled: true });
            else if (!have && !tabSrc.enabled) tabSrc.enabled = true;
          } else if (have) cfg.sources = cfg.sources.filter(s => s.id !== p.id);
          else cfg.sources.push({ ...p, user: '', key: '', enabled: true });
          drawList();
        };
        const kindTag = make('span', 'dc-srckind' + (p.kind === 'reddit' ? ' rd-kind' : ''), KINDS[p.kind].label);
        row.append(iconEl(p), make('b', '', p.name), ci, make('span', 'dc-srckind', catName(p.cat)), kindTag, add);
        return row;
      };
      const sites = shown.filter(p => !(p.kind === 'reddit' && p.sub)), subs = shown.filter(p => p.kind === 'reddit' && p.sub);
      const rows = sites.map(rowFor);
      if (subs.length) {
        const open = redditOpen || !!t;
        const head = make('button', 'dc-rdgroup' + (open ? ' open' : ''));
        head.type = 'button';
        const pinned = subs.filter(p => (cfg.redditPins || []).some(x => x.toLowerCase() === p.sub.toLowerCase())).length;
        head.append(iconEl({ kind: 'reddit', icon: REDDIT_ICON, cat: 'real' }), make('b', '', 'Reddit communities'),
          make('span', 'dc-srckind rd-kind', `${subs.length} ${t ? 'found' : 'communities'}${pinned ? ` · ${pinned} pinned` : ''}`), make('span', 'dc-rdgroup-chev', open ? '▾' : '▸'));
        head.title = open ? 'Fold them away' : 'Show them (they all live in the Reddit tab picker; pin the ones you like)';
        head.onclick = () => { redditOpen = !open; drawList(); };
        rows.push(head);
        if (open) { const box = make('div', 'dc-rdlist'); box.append(...subs.map(rowFor)); rows.push(box); }
      }
      list.replaceChildren(...rows);
    };
    find.oninput = drawList;
    drawList();
    body.append(cats, find, list);

    // add another site of one of the kinds
    const add = make('div', 'dc-srccard dc-addsrc');
    const name = make('input'); Object.assign(name, { type: 'text', placeholder: 'Name' });
    const base = make('input'); Object.assign(base, { type: 'text', placeholder: 'https://… (the site\'s address)' });
    const kind = make('select'); Object.entries(KINDS).filter(([k]) => !/^(reddit|bing)$/.test(k)).forEach(([k, v]) => kind.add(new Option(v.label, k)));
    const addBtn = make('button', 'btn', 'Add source');
    addBtn.onclick = () => {
      const url = base.value.trim().replace(/\/+$/, '');
      if (!name.value.trim() || !/^https:\/\/[^/]+$/.test(url)) return toast('Give it a name and its https:// address.', 'err');
      cfg.sources.push({ id: 'c' + Date.now(), name: name.value.trim(), kind: kind.value, base: url, user: '', key: '', enabled: true, custom: true, cat: 'anime' });
      openSources();
    };
    add.append(make('b', '', 'Add a site that isn\'t listed'), name, base, kind, addBtn);
    body.append(add);
    // any other site: a recipe (sites.js)
    const own = make('div', 'dc-srccard dc-addsrc st-own');
    const ownBtn = make('button', 'btn primary', '✨ Build your own site');
    ownBtn.onclick = () => editRecipe(null);
    own.append(make('b', '', 'Any other site, with its own pages'), make('p', 'dc-srchelp', 'Give its addresses (front page, search, categories) and the app reads its pictures and clips, with your own orders, categories and Random. Try it before saving; recipes can be copied and shared.'), ownBtn);
    body.append(own);
    // tags never to show (on top of the built-in ones)
    const bl = make('div', 'dc-srccard');
    const area = make('textarea'); area.value = cfg.blacklist || ''; area.placeholder = 'e.g. guro scat comic';
    area.oninput = () => { cfg.blacklist = area.value; };
    bl.append(make('b', '', 'Never show posts with these tags'), area);
    body.append(bl);
    sheet.classList.remove('hidden');
  }

  // ---------- Find the source of a Library item (right-click → Find the source) ----------
  // SauceNAO (with your key) or IQDB get the item's thumbnail; each match can be opened, or its
  // tags (artist, characters, series...) added to the item.
  const BOORU_POST = [
    [/danbooru\.donmai\.us\/posts\/(\d+)/, 'danbooru'], [/gelbooru\.com\/index\.php\?.*id=(\d+)/, 'gelbooru'], [/yande\.re\/post\/show\/(\d+)/, 'yandere'],
    [/konachan\.com\/post\/show\/(\d+)/, 'konachan-com'], [/e621\.net\/posts\/(\d+)/, 'e621'], [/safebooru\.org\/index\.php\?.*id=(\d+)/, 'safebooru']
  ];
  async function postTags(sourceId, id) {
    const s = sourceById(sourceId) || CATALOG.find(p => p.id === sourceId);
    const k = KINDS[s.kind];
    const url = s.kind === 'danbooru' ? `${s.base}/posts/${id}.json` : s.kind === 'e621' ? `${s.base}/posts/${id}.json`
      : s.kind === 'moebooru' ? `${s.base}/post.json?tags=id:${id}` : `${s.base}/index.php?page=dapi&s=post&q=index&json=1&id=${id}`;
    let d = JSON.parse(await api('webJson', url));
    if (s.kind === 'e621') d = [d.post]; else if (!Array.isArray(d)) d = k.list(d).length ? k.list(d) : [d];
    const p = k.map(d[0], s);
    return p ? p.tags.filter(t => !NEVER.has(nameOf(t))) : [];
  }
  async function findSource(item) {
    const back = make('div', 'dc-sheet');
    back.innerHTML = '<div class="dc-sheet-card"><div class="dc-sheet-head"><h2>Find the source</h2><button class="btn" type="button">Close</button></div><div class="dc-sheet-body"></div></div>';
    back.querySelector('.dc-sheet-head .btn').onclick = () => back.remove();
    back.addEventListener('pointerdown', e => { if (e.target === back) back.remove(); });
    document.body.append(back);
    const body = back.querySelector('.dc-sheet-body');
    const me = make('div', 'dc-rs-me');
    const thumb = make('img'); thumb.src = vaultUI.thumbSrc(item, 240); thumb.alt = '';
    const say = make('p', 'dc-sheet-lead', cfg && cfg.saucenaoKey ? 'Asking SauceNAO…' : 'Asking IQDB…');
    me.append(thumb, say);
    body.append(me);
    if (!cfg) await loadConfig();
    const list = make('div', 'dc-rs-list');
    body.append(list);
    let matches = [];
    try {
      if (cfg.saucenaoKey) {
        const d = JSON.parse(await api('reverseSearch', 'saucenao', item.id, cfg.saucenaoKey));
        if (d.header && d.header.status < 0) throw new Error(d.header.message || 'SauceNAO said no');
        matches = (d.results || []).map(r => ({
          sim: parseFloat(r.header.similarity), thumb: r.header.thumbnail, site: r.header.index_name.replace(/^Index #\d+:\s*/, '').replace(/\s*-.*$/, ''),
          url: (r.data.ext_urls || [])[0] || r.data.source || '', title: r.data.title || r.data.source || '',
          tags: [...String(r.data.creator || r.data.member_name || r.data.author_name || '').split(/,\s*/).filter(Boolean).map(a => 'artist:' + a),
                 ...String(r.data.characters || '').split(/,\s*/).filter(Boolean).map(c => 'character:' + c),
                 ...String(r.data.material || '').split(/,\s*/).filter(m => m && m !== 'original').map(m => 'series:' + m)]
        }));
      } else {
        const doc = new DOMParser().parseFromString(await api('reverseSearch', 'iqdb', item.id, ''), 'text/html');
        matches = [...doc.querySelectorAll('#pages > div')].map(div => {
          const a = div.querySelector('td.image a'), img = div.querySelector('td.image img');
          const text = div.textContent;
          if (!a || /Your image/i.test(text)) return null;
          const href = a.getAttribute('href');
          return { sim: parseFloat((/(\d+)% similarity/.exec(text) || [])[1] || '0'), thumb: img ? 'https://iqdb.org' + img.getAttribute('src') : '',
            site: (div.querySelector('td img[alt]') && div.querySelector('img.service-icon') ? '' : '') + (href.match(/\/\/(?:www\.)?([^/]+)/) || [, ''])[1],
            url: href.startsWith('//') ? 'https:' + href : href, title: (div.querySelector('td.image img') || {}).alt || '', tags: [] };
        }).filter(Boolean);
      }
    } catch (e) {
      say.textContent = errMsg(e);
      return;
    }
    matches = matches.filter(m => m.url && m.sim >= 45).sort((a, b) => b.sim - a.sim);
    say.textContent = matches.length ? `${matches.length} possible ${matches.length === 1 ? 'match' : 'matches'}, best first.` : 'Nothing similar found.';
    for (const m of matches) {
      const row = make('div', 'dc-rs-row');
      const im = make('img'); im.alt = ''; if (m.thumb) im.src = proxy(m.thumb);
      const info = make('div', 'dc-rs-info');
      info.append(make('b', '', `${Math.round(m.sim)}% · ${m.site}`), make('span', 'dc-srchelp', m.title.slice(0, 120)));
      const btns = make('div', 'dc-rs-btns');
      const open = make('button', 'btn', 'Open ↗'); open.onclick = () => api('openLink', m.url).catch(err => toast(errMsg(err), 'err'));
      btns.append(open);
      const hit = BOORU_POST.find(([re]) => re.test(m.url));
      if (hit || m.tags.length) {
        const use = make('button', 'btn primary', 'Add its tags');
        use.onclick = async () => {
          use.disabled = true;
          try {
            let tags = m.tags;
            if (hit) tags = [...tags, ...await postTags(hit[1], hit[0].exec(m.url)[1])];
            tags = cleanTags(tags.map(t => t.toLowerCase().replace(/\s+/g, '_')));
            if (!tags.length) throw new Error('That match has no tags');
            const r = await api('setTags', [item.id], tags.join(' '), 'add');
            item.tags = cleanTags([...vaultUI.tagsOf(item), ...tags]).join(' ');
            libraryWritten(r); vaultUI.tagsChanged();
            use.textContent = `✓ ${tags.length} tags added`;
          } catch (e) { use.disabled = false; toast(errMsg(e).replace(/isn't one of your sources/, 'isn\'t one of your sources: add it in Discover → Sources first'), 'err'); }
        };
        btns.append(use);
      }
      row.append(im, info, btns);
      list.append(row);
    }
  }
  // ---------- Add → paste links: one or many addresses become files to tag ----------
  // Direct links to pictures and videos, Imgur gifv, RedGifs, Danbooru / e621 / Yande.re / Konachan posts,
  // Eporner and Rule34Video pages. They go through the Add page's usual questions (category, NSFW,
  // stars), one by one, and keep where they came from.
  async function resolveLink(u) {
    if (!/^https?:\/\/\S+$/i.test(u)) throw new Error('Not a web address');
    const ext = extOf(u);
    const name = decodeURIComponent(u.split('/').pop().split('?')[0]) || 'link';
    let m;
    if (/^(jpe?g|png|gif|webp|avif|mp4|webm|mov|m4v)$/.test(ext)) return { file: u, name, source: u, video: /^(mp4|webm|mov|m4v)$/.test(ext) };
    if ((m = /imgur\.com\/(\w+)\.gifv/i.exec(u))) return { file: `https://i.imgur.com/${m[1]}.mp4`, name: m[1] + '.mp4', source: u, video: true };
    if ((m = /redgifs\.com\/(?:watch|ifr)\/([a-z0-9]+)/i.exec(u))) { const g = await redgifsGif(m[1].toLowerCase()); return { file: g.urls.hd || g.urls.sd, name: `redgifs-${g.id}.mp4`, source: u, video: true }; }
    if ((m = /^(https:\/\/(?:danbooru|safebooru)\.donmai\.us|https:\/\/aibooru\.online)\/posts\/(\d+)/.exec(u))) {
      const p = JSON.parse(await api('webJson', `${m[1]}/posts/${m[2]}.json`));
      if (!p.file_url && !p.large_file_url) throw new Error('That post\'s file is hidden without an account');
      return { file: p.file_url || p.large_file_url, name: `danbooru-${p.id}.${p.file_ext}`, source: u, video: /^(mp4|webm)$/.test(p.file_ext) };
    }
    if ((m = /^(https:\/\/e(?:621|926)\.net)\/posts\/(\d+)/.exec(u))) {
      const p = JSON.parse(await api('webJson', `${m[1]}/posts/${m[2]}.json`)).post;
      if (!p || !p.file || !p.file.url) throw new Error('That post\'s file is hidden without an account');
      return { file: p.file.url, name: `e621-${p.id}.${p.file.ext}`, source: u, video: /^(mp4|webm)$/.test(p.file.ext) };
    }
    if ((m = /^(https:\/\/(?:yande\.re|konachan\.(?:com|net)))\/post\/show\/(\d+)/.exec(u))) {
      const p = JSON.parse(await api('webJson', `${m[1]}/post.json?tags=id:${m[2]}`))[0];
      if (!p) throw new Error('That post is gone');
      return { file: p.file_url, name: `${m[1].split('//')[1].split('.')[0]}-${p.id}.${extOf(p.file_url) || 'jpg'}`, source: u, video: false };
    }
    if (/eporner\.com\/|rule34video\.com\/video\/|hypnotube\.com\/video\//.test(u) && window.vaultVideos && window.vaultVideos.resolveUrl) return { ...(await window.vaultVideos.resolveUrl(u)), source: u, video: true };
    throw new Error('No picture or video found at that address');
  }
  {
    const add = document.getElementById('tab-add'), drop = document.getElementById('drop');
    const box = make('aside', 'add-links');
    box.innerHTML = `<b>Paste links</b><span class="add-links-sub">One or many, one per line: pictures, GIFs and videos, RedGifs, Imgur, booru posts, Eporner, Rule34Video and Hypnotube pages.</span>
      <textarea spellcheck="false" placeholder="https://…&#10;https://…"></textarea><button class="btn primary" type="button">＋ Add these</button><div class="add-links-list"></div>`;
    drop.after(box);
    const area = box.querySelector('textarea'), go = box.querySelector('.btn'), list = box.querySelector('.add-links-list');
    area.addEventListener('keydown', e => { if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) go.click(); });
    // (pasting into the drop zone works too)
    drop.addEventListener('paste', e => { const t = e.clipboardData && e.clipboardData.getData('text'); if (t && /https?:\/\//.test(t)) { area.value = t; go.click(); } });
    go.onclick = async () => {
      const urls = [...new Set(area.value.split(/\s+/).map(x => x.trim()).filter(x => /^https?:\/\//i.test(x)))];
      if (!urls.length) return toast('Paste one or more web addresses first', 'err');
      go.disabled = true;
      list.replaceChildren();
      const files = [];
      for (const u of urls) {
        const row = make('div', 'add-link-row busy');
        row.innerHTML = spinTick();
        row.append(make('span', 'add-link-url', u.replace(/^https?:\/\/(www\.)?/, '').slice(0, 60)));
        list.append(row);
        try {
          const r = await resolveLink(u);
          const blob = r.video ? await downloadBlob(r.file) : await (async () => { const res = await fetch(proxy(r.file)); if (!res.ok) throw new Error(`Couldn't download it (${res.status})`); return res.blob(); })();
          const ext = extOf(r.name) || extOf(r.file) || (r.video ? 'mp4' : 'jpg');
          const type = blob.type && blob.type !== 'application/octet-stream' ? blob.type : MIME[ext] || (r.video ? 'video/mp4' : 'image/jpeg');
          const file = new File([blob], /\.\w{2,5}$/.test(r.name) ? r.name : `${r.name}.${ext}`, { type });
          file._source = r.source;
          files.push(file);
          row.classList.replace('busy', 'done');
        } catch (e) { row.classList.replace('busy', 'bad'); row.append(make('span', 'add-link-err', errMsg(e))); }
      }
      go.disabled = false;
      if (files.length) { area.value = ''; startQueue(files); }
    };
  }

  // shared with Videos and Audio: the never-show words, Reddit (through your app), the Sources sheet
  window.vaultDiscover = {
    findSource, NEVER, NEVER_TEXT, proxy, ask, imported, spinTick, rgApi, downloadBlob,
    config: async () => { if (!cfg) await loadConfig(); return cfg; },
    saveConfig: () => saveConfig(),
    redditReady: async () => true, // (the public feeds need nothing)
    redditFeed: async (sub, words, opts) => {
      // Reddit posts as {title, author, page, sub, html (the post's text, as HTML), url}, from the public
      // feed; opts: {onWait(seconds), wanted()} as for rssText (it shares Discover's one-a-minute queue)
      if (!cfg) await loadConfig();
      const base = `https://www.reddit.com/r/${sub}`;
      const url = words ? `${base}/search.rss?q=${enc(words)}&restrict_sr=on&sort=top&t=all&include_over_18=on&limit=50` : `${base}/hot/.rss?limit=50`;
      const xml = new DOMParser().parseFromString(await rssText(url, opts), 'text/xml');
      return [...xml.getElementsByTagName('entry')].map(e => {
        const get = tag => (e.getElementsByTagName(tag)[0] || {}).textContent || '';
        const link = e.getElementsByTagName('link')[0];
        return { title: get('title'), author: get('name').replace(/^\/u\//, ''), page: link ? link.getAttribute('href') : '', sub, html: get('content') };
      });
    },
    reddit: async path => { if (!cfg) await loadConfig(); return JSON.parse(await api('webJson', 'https://oauth.reddit.com' + path, { Authorization: 'bearer ' + await redditToken() })); },
    openSources: async id => { if (!cfg) await loadConfig(); openSources(id); }
  };

  // ---------- opening the page ----------
  let started = false;
  async function openDiscover() {
    if (!cfg) await loadConfig();
    refreshSaved();
    if (!started) {
      started = true;
      const first = sourceById(cfg.active) && sourceById(cfg.active).enabled ? cfg.active : (enabled().find(ready) || enabled()[0] || {}).id;
      go({ source: first, query: '' }, false); // (with the levels, view and community you left it with)
      peek(q('.dc-bar'), 'sonico', { at: 26 });
      drawMarks();
    } else if (posts.length) {
      posts.forEach(p => p._tile && p._tile.classList.toggle('saved', saved.has(p.page)));
    }
  }
})();
